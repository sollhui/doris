// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/adaptive_thread_controller.h"

#include <algorithm>
#include <chrono>

#include "cloud/config.h"
#include "common/config.h"
#include "common/logging.h"
#include "common/status.h"
#include "util/system_metrics.h"
#include "util/threadpool.h"
#include "util/time.h"

namespace doris {

int AdaptiveThreadController::PoolGroup::get_max_threads() const {
    int num_cpus = std::thread::hardware_concurrency();
    if (num_cpus <= 0) {
        num_cpus = 1;
    }
    return static_cast<int>(num_cpus * max_threads_per_cpu);
}

int AdaptiveThreadController::PoolGroup::get_min_threads() const {
    int num_cpus = std::thread::hardware_concurrency();
    if (num_cpus <= 0) {
        num_cpus = 1;
    }
    return std::max(1, static_cast<int>(num_cpus * min_threads_per_cpu));
}

AdaptiveThreadController::~AdaptiveThreadController() {
    stop();
}

void AdaptiveThreadController::init(SystemMetrics* system_metrics,
                                     ThreadPool* s3_file_upload_pool, int num_disk) {
    _system_metrics = system_metrics;
    _s3_file_upload_pool = s3_file_upload_pool;
    _num_disk = std::max(1, num_disk);
}

void AdaptiveThreadController::register_pool_group(std::string name,
                                                    std::vector<ThreadPool*> pools,
                                                    AdjustFunc adjust_func,
                                                    double max_threads_per_cpu,
                                                    double min_threads_per_cpu) {
    PoolGroup group;
    group.name = std::move(name);
    group.pools = std::move(pools);
    group.adjust_func = std::move(adjust_func);
    group.max_threads_per_cpu = max_threads_per_cpu;
    group.min_threads_per_cpu = min_threads_per_cpu;
    group.current_threads = group.get_max_threads();

    std::lock_guard<std::mutex> lock(_groups_mutex);
    _pool_groups.push_back(std::move(group));
    LOG(INFO) << "Adaptive: registered pool group '" << _pool_groups.back().name
              << "' with " << _pool_groups.back().pools.size() << " pools"
              << ", max_threads=" << _pool_groups.back().get_max_threads()
              << ", min_threads=" << _pool_groups.back().get_min_threads();
}

void AdaptiveThreadController::start() {
    if (!config::enable_adaptive_flush_threads) {
        LOG(INFO) << "Adaptive thread controller is disabled";
        return;
    }

    _stopped.store(false);
    _adjustment_thread = std::thread(&AdaptiveThreadController::_adjustment_loop, this);
    LOG(INFO) << "Adaptive thread controller started";
}

void AdaptiveThreadController::stop() {
    if (_stopped.load()) {
        return;
    }

    _stopped.store(true);
    if (_adjustment_thread.joinable()) {
        _adjustment_thread.join();
    }
    LOG(INFO) << "Adaptive thread controller stopped";
}

int AdaptiveThreadController::get_current_threads(const std::string& name) const {
    std::lock_guard<std::mutex> lock(_groups_mutex);
    for (const auto& group : _pool_groups) {
        if (group.name == name) {
            return group.current_threads;
        }
    }
    return 0;
}

void AdaptiveThreadController::_adjustment_loop() {
    while (!_stopped.load()) {
        if (!config::enable_adaptive_flush_threads) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
            continue;
        }

        adjust_once();

        std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
    }
}

void AdaptiveThreadController::adjust_once() {
    std::lock_guard<std::mutex> lock(_groups_mutex);
    for (auto& group : _pool_groups) {
        if (group.pools.empty() || group.pools[0] == nullptr || !group.adjust_func) {
            continue;
        }

        int min_threads = group.get_min_threads();
        int max_threads = group.get_max_threads();
        int target = group.adjust_func(group.current_threads, min_threads, max_threads);

        _apply_thread_count(group, target);
    }
}

bool AdaptiveThreadController::is_io_busy() {
    // For cloud mode (compute-storage separated), check S3 upload queue length
    if (config::is_cloud_mode()) {
        if (_s3_file_upload_pool == nullptr) {
            return false;
        }
        int queue_size = _s3_file_upload_pool->get_queue_size();
        VLOG(2) << "Adaptive: s3 upload queue size=" << queue_size
                << ", threshold=" << kS3QueueBusyThreshold;
        return queue_size > kS3QueueBusyThreshold;
    }

    // For compute-storage integrated mode, check disk IO util
    if (_system_metrics == nullptr) {
        return false;
    }

    int64_t current_time_sec = MonotonicSeconds();
    int64_t interval_sec = current_time_sec - _last_check_time_sec;

    if (interval_sec <= 0) {
        _system_metrics->get_disks_io_time(&_last_disk_io_time);
        _last_check_time_sec = current_time_sec;
        return false;
    }

    int64_t max_io_util = _system_metrics->get_max_io_util(_last_disk_io_time, interval_sec);

    _system_metrics->get_disks_io_time(&_last_disk_io_time);
    _last_check_time_sec = current_time_sec;

    VLOG(2) << "Adaptive: disk io util=" << max_io_util
            << ", threshold=" << kIOBusyThresholdPercent;

    return max_io_util > kIOBusyThresholdPercent;
}

bool AdaptiveThreadController::is_cpu_busy() {
    if (_system_metrics == nullptr) {
        return false;
    }

    double load_avg = _system_metrics->get_load_average_1_min();
    int num_cpus = std::thread::hardware_concurrency();

    if (num_cpus <= 0) {
        return false;
    }

    double cpu_usage_percent = (load_avg / num_cpus) * 100.0;

    VLOG(2) << "Adaptive: cpu load_avg=" << load_avg << ", num_cpus=" << num_cpus
            << ", usage_percent=" << cpu_usage_percent
            << ", threshold=" << kCPUBusyThresholdPercent;

    return cpu_usage_percent > kCPUBusyThresholdPercent;
}

void AdaptiveThreadController::_apply_thread_count(PoolGroup& group, int target_threads) {
    int max_threads = group.get_max_threads();
    int min_threads = group.get_min_threads();
    target_threads = std::max(min_threads, std::min(max_threads, target_threads));
    if (target_threads == group.current_threads) {
        return;
    }

    LOG(INFO) << "Adaptive[" << group.name << "]: adjusting threads from " << group.current_threads
              << " to " << target_threads << " (min=" << min_threads << ", max=" << max_threads
              << ")";

    for (auto* pool : group.pools) {
        if (pool == nullptr) {
            continue;
        }
        Status st = pool->set_max_threads(target_threads);
        if (!st.ok()) {
            LOG(WARNING) << "Adaptive[" << group.name
                         << "]: failed to set max threads: " << st;
        }
    }

    group.current_threads = target_threads;
}

} // namespace doris
