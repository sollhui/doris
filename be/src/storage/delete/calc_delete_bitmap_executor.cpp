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

#include "storage/delete/calc_delete_bitmap_executor.h"

#include <gen_cpp/olap_file.pb.h>

#include <ostream>

#include "common/logging.h"
#include "load/memtable/memtable.h"
#include "storage/tablet/base_tablet.h"
#include "util/time.h"

namespace doris {
using namespace ErrorCode;

DeleteBitmapCancellation::~DeleteBitmapCancellation() {
    if (!_status.ok()) {
        // All token wrappers have released their shared state. Include late
        // submissions and owner shutdowns that happened after cancel returned.
        _log_stats("final", -1);
    }
}

void DeleteBitmapCancellation::_log_stats(const char* stage, int64_t elapsed_us) const {
    for (size_t i = 0; i < _phase_counters.size(); ++i) {
        const auto& c = _phase_counters[i];
        LOG(INFO) << "delete bitmap cancel stats: load_id=" << _load_id
                  << ", phase=" << (i == 0 ? "for_load" : "rowset_builder") << ", stage=" << stage
                  << ", elapsed_us=" << elapsed_us
                  << ", cancel_calls=" << _cancel_calls.load(std::memory_order_relaxed)
                  << ", tokens=" << c.tokens.load(std::memory_order_relaxed)
                  << ", late_tokens=" << c.late_tokens.load(std::memory_order_relaxed)
                  << ", snapshot_queued=" << c.snapshot_queued.load(std::memory_order_relaxed)
                  << ", snapshot_running=" << c.snapshot_running.load(std::memory_order_relaxed)
                  << ", discarded_by_load=" << c.discarded_by_load.load(std::memory_order_relaxed)
                  << ", discarded_by_owner=" << c.discarded_by_owner.load(std::memory_order_relaxed)
                  << ", submitted=" << c.submitted.load(std::memory_order_relaxed)
                  << ", body_started=" << c.body_started.load(std::memory_order_relaxed)
                  << ", body_finished=" << c.body_finished.load(std::memory_order_relaxed)
                  << ", rejected_after_cancel="
                  << c.rejected_after_cancel.load(std::memory_order_relaxed)
                  << ", skipped_after_cancel="
                  << c.skipped_after_cancel.load(std::memory_order_relaxed);
    }
}

void DeleteBitmapCancellation::_register_token(const std::shared_ptr<ThreadPoolToken>& token,
                                               DeleteBitmapPhase phase) {
    auto& counters = _counters(phase);
    counters.tokens.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(_lock);
        if (_status.ok()) {
            _tokens.push_back({token, phase});
            return;
        }
    }
    // A token created after cancellation must reject submissions too.
    counters.late_tokens.fetch_add(1, std::memory_order_relaxed);
    token->shutdown();
}

void DeleteBitmapCancellation::cancel(const Status& reason) {
    DCHECK(!reason.ok());
    const auto start_us = MonotonicMicros();
    _cancel_calls.fetch_add(1, std::memory_order_relaxed);
    bool first_cancel;
    std::vector<std::pair<std::shared_ptr<ThreadPoolToken>, DeleteBitmapPhase>> tokens;
    {
        std::lock_guard lock(_lock);
        first_cancel = _status.update(reason);
        for (const auto& entry : _tokens) {
            if (auto token = entry.token.lock()) {
                tokens.emplace_back(std::move(token), entry.phase);
            }
        }
    }
    if (first_cancel) {
        // Per-token snapshots after publication, before this caller starts
        // draining tokens. Other workers/owners can change queues concurrently.
        for (const auto& [token, phase] : tokens) {
            const auto snapshot = token->task_stats();
            auto& counters = _counters(phase);
            counters.snapshot_queued.fetch_add(static_cast<int64_t>(snapshot.queued),
                                               std::memory_order_relaxed);
            counters.snapshot_running.fetch_add(static_cast<int64_t>(snapshot.running),
                                                std::memory_order_relaxed);
        }
        _log_stats("begin", MonotonicMicros() - start_us);
    }
    // Publish to all tasks before waiting. Neither registration nor task completion
    // needs to wait for this lock while shutdown waits for running tasks.
    for (const auto& [token, phase] : tokens) {
        ThreadPoolToken::TaskStats removed;
        token->shutdown(&removed);
        _counters(phase).discarded_by_load.fetch_add(static_cast<int64_t>(removed.queued),
                                                     std::memory_order_relaxed);
    }
    if (first_cancel) {
        _log_stats("end", MonotonicMicros() - start_us);
    }
}

CalcDeleteBitmapToken::CalcDeleteBitmapToken(
        std::unique_ptr<ThreadPoolToken> thread_token,
        std::shared_ptr<DeleteBitmapCancellation> load_cancel_status, DeleteBitmapPhase phase)
        : _thread_token(std::move(thread_token)),
          _status(Status::OK()),
          _load_cancel_status(std::move(load_cancel_status)),
          _phase(phase) {
    if (_load_cancel_status) {
        _load_cancel_status->_register_token(_thread_token, _phase);
    }
}

CalcDeleteBitmapToken::~CalcDeleteBitmapToken() {
    // A concurrent cancellation can retain the underlying token. Finish callbacks
    // that capture this before destroying the wrapper's status and lock.
    _shutdown();
}

void CalcDeleteBitmapToken::_shutdown() {
    ThreadPoolToken::TaskStats removed;
    _thread_token->shutdown(&removed);
    if (_load_cancel_status) {
        _load_cancel_status->_counters(_phase).discarded_by_owner.fetch_add(
                static_cast<int64_t>(removed.queued), std::memory_order_relaxed);
    }
}

void CalcDeleteBitmapToken::_record_rejected_submission() {
    if (_load_cancel_status && !_load_cancel_status->ok()) {
        _load_cancel_status->_counters(_phase).rejected_after_cancel.fetch_add(
                1, std::memory_order_relaxed);
    }
}

Status CalcDeleteBitmapToken::submit(BaseTabletSPtr tablet, RowsetSharedPtr cur_rowset,
                                     const segment_v2::SegmentSharedPtr& cur_segment,
                                     const std::vector<RowsetSharedPtr>& target_rowsets,
                                     int64_t end_version, DeleteBitmapPtr delete_bitmap,
                                     RowsetWriter* rowset_writer,
                                     DeleteBitmapPtr tablet_delete_bitmap) {
    const auto submit_time_us = MonotonicMicros();
    return submit_func([=]() {
        const auto queue_time_us = MonotonicMicros() - submit_time_us;
        auto st = tablet->calc_segment_delete_bitmap(cur_rowset, cur_segment, target_rowsets,
                                                     delete_bitmap, end_version, rowset_writer,
                                                     tablet_delete_bitmap, queue_time_us);
        if (!st.ok()) {
            LOG(WARNING) << "failed to calc segment delete bitmap, tablet_id: "
                         << tablet->tablet_id() << " rowset: " << cur_rowset->rowset_id()
                         << " seg_id: " << cur_segment->id() << " version: " << end_version
                         << " error: " << st;
        }
        return st;
    });
}

Status CalcDeleteBitmapToken::submit(BaseTabletSPtr tablet, TabletSchemaSPtr schema,
                                     RowsetId rowset_id,
                                     const std::vector<segment_v2::SegmentSharedPtr>& segments,
                                     DeleteBitmapPtr delete_bitmap) {
    const auto submit_time_us = MonotonicMicros();
    return submit_func([=]() {
        const auto queue_time_us = MonotonicMicros() - submit_time_us;
        auto st = tablet->calc_delete_bitmap_between_segments(schema, rowset_id, segments,
                                                              delete_bitmap, queue_time_us);
        if (!st.ok()) {
            LOG(WARNING) << "failed to calc delete bitmap between segments, tablet_id: "
                         << tablet->tablet_id() << " rowset: " << rowset_id
                         << " segments num: " << segments.size() << " error: " << st;
        }
        return st;
    });
}

Status CalcDeleteBitmapToken::_get_status() {
    std::shared_lock rlock(_lock);
    RETURN_IF_ERROR(_status);
    return _load_cancel_status && !_load_cancel_status->ok() ? _load_cancel_status->status()
                                                             : Status::OK();
}

Status CalcDeleteBitmapToken::wait() {
    _thread_token->wait();
    return _get_status();
}

void CalcDeleteBitmapToken::cancel(const Status& st) {
    DCHECK(!st.ok());
    {
        std::lock_guard wlock(_lock);
        if (_status.ok()) {
            _status = st;
        }
    }
    // Do not hold _lock while waiting: running tasks may need it to report an error.
    _shutdown();
}

void CalcDeleteBitmapExecutor::init(const std::string& name, int max_threads) {
    static_cast<void>(ThreadPoolBuilder(name)
                              .set_min_threads(1)
                              .set_max_threads(max_threads)
                              .build(&_thread_pool));
}

std::unique_ptr<CalcDeleteBitmapToken> CalcDeleteBitmapExecutor::create_token(
        std::shared_ptr<DeleteBitmapCancellation> load_cancel_status, DeleteBitmapPhase phase) {
    return std::make_unique<CalcDeleteBitmapToken>(
            _thread_pool->new_token(ThreadPool::ExecutionMode::CONCURRENT),
            std::move(load_cancel_status), phase);
}

} // namespace doris
