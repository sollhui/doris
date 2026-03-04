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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace doris {

class ThreadPool;
class SystemMetrics;

// AdaptiveThreadController dynamically adjusts thread pool sizes based on
// system load. It manages multiple pool groups, each with its own adjustment
// logic. Different subsystems (memtable flush, delete bitmap, etc.) can
// register their pools via register_pool_group() with custom adjust functions.
//
// The controller provides is_io_busy() and is_cpu_busy() as public utilities
// that adjust functions can use to make decisions.
//
// Usage:
//   controller.init(system_metrics, s3_pool, num_disk);
//   controller.register_pool_group("flush", {pool1, pool2},
//       [&](int cur, int min, int max) {
//           int target = cur;
//           if (memory_pressure) target = std::min(max, target + 1);
//           if (controller.is_io_busy()) target = std::max(min, target - 1);
//           return target;
//       }, max_per_cpu, min_per_cpu);
//   controller.start();
class AdaptiveThreadController {
public:
    // AdjustFunc: given (current_threads, min_threads, max_threads),
    // return the desired target thread count.
    // The controller will clamp the result to [min, max] automatically.
    using AdjustFunc = std::function<int(int current, int min_threads, int max_threads)>;

    static constexpr int kCheckIntervalMs = 60000; // Check every 60 seconds

    // Common thresholds exposed as constants for use by adjust functions
    static constexpr int kQueueThreshold = 10;
    static constexpr int kIOBusyThresholdPercent = 90;
    static constexpr int kCPUBusyThresholdPercent = 90;
    static constexpr int kS3QueueBusyThreshold = 100;

    AdaptiveThreadController() = default;
    ~AdaptiveThreadController();

    // Initialize with system-level dependencies. Must be called before start().
    void init(SystemMetrics* system_metrics, ThreadPool* s3_file_upload_pool, int num_disk);

    // Register a group of thread pools to be managed together.
    // Can be called before or after start().
    // @param name: identifier for logging (e.g., "flush", "delete_bitmap")
    // @param pools: thread pools to manage (first pool used for queue size)
    // @param adjust_func: custom function to calculate target thread count
    // @param max_threads_per_cpu: maximum threads per CPU core
    // @param min_threads_per_cpu: minimum threads per CPU core
    void register_pool_group(std::string name, std::vector<ThreadPool*> pools,
                             AdjustFunc adjust_func, double max_threads_per_cpu,
                             double min_threads_per_cpu);

    void start();
    void stop();

    // Get current thread count for a named pool group (for testing/debugging)
    int get_current_threads(const std::string& name) const;

    // Manually trigger one adjustment cycle for all groups (for testing)
    void adjust_once();

    // Public utilities for use by adjust functions
    bool is_io_busy();
    bool is_cpu_busy();

private:
    struct PoolGroup {
        std::string name;
        std::vector<ThreadPool*> pools;
        AdjustFunc adjust_func;
        double max_threads_per_cpu = 4;
        double min_threads_per_cpu = 0.5;
        int current_threads = 0;

        int get_max_threads() const;
        int get_min_threads() const;
    };

    void _adjustment_loop();
    void _apply_thread_count(PoolGroup& group, int target_threads);

private:
    SystemMetrics* _system_metrics = nullptr;
    ThreadPool* _s3_file_upload_pool = nullptr;
    int _num_disk = 1;

    mutable std::mutex _groups_mutex;
    std::vector<PoolGroup> _pool_groups;

    // For disk IO util calculation
    std::map<std::string, int64_t> _last_disk_io_time;
    int64_t _last_check_time_sec = 0;

    // Per-cycle cache: computed once per adjust_once() so all groups see the same value.
    // -1 means "not yet computed this cycle".
    int _cached_io_busy = -1;
    int _cached_cpu_busy = -1;

    // Background thread control
    std::atomic<bool> _stopped {true};
    std::thread _adjustment_thread;
};

} // namespace doris
