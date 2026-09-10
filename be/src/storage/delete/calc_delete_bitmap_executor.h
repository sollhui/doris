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

#include <stdint.h>

#include <array>
#include <atomic>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/status.h"
#include "storage/rowset/rowset.h"
#include "storage/rowset/rowset_writer.h"
#include "storage/segment/segment.h"
#include "storage/tablet/tablet_fwd.h"
#include "util/threadpool.h"

namespace doris {

class DataDir;
class Tablet;
enum RowsetTypePB : int;

enum class DeleteBitmapPhase { FOR_LOAD, ROWSET_BUILDER };

// Per-load cancellation shared by for-load and rowset-builder bitmap tokens.
class DeleteBitmapCancellation {
public:
    explicit DeleteBitmapCancellation(std::string load_id = "") : _load_id(std::move(load_id)) {}
    ~DeleteBitmapCancellation();

    bool ok() const { return _status.ok(); }
    Status status() const { return _status.ok() ? Status::OK() : _status.status(); }

    // Discard queued bitmap work and wait only for running tasks. No owner lock
    // is needed; repeated cancellation retains the first error.
    void cancel(const Status& reason);

private:
    friend class CalcDeleteBitmapToken;
    struct Counters {
        std::atomic<int64_t> tokens {0};
        std::atomic<int64_t> late_tokens {0};
        std::atomic<int64_t> snapshot_queued {0};
        std::atomic<int64_t> snapshot_running {0};
        std::atomic<int64_t> discarded_by_load {0};
        std::atomic<int64_t> discarded_by_owner {0};
        std::atomic<int64_t> submitted {0};
        std::atomic<int64_t> body_started {0};
        std::atomic<int64_t> body_finished {0};
        std::atomic<int64_t> rejected_after_cancel {0};
        std::atomic<int64_t> skipped_after_cancel {0};
    };
    struct RegisteredToken {
        std::weak_ptr<ThreadPoolToken> token;
        DeleteBitmapPhase phase;
    };
    Counters& _counters(DeleteBitmapPhase phase) {
        return _phase_counters[static_cast<size_t>(phase)];
    }
    void _register_token(const std::shared_ptr<ThreadPoolToken>& token, DeleteBitmapPhase phase);
    void _log_stats(const char* stage, int64_t elapsed_us) const;

    const std::string _load_id;
    std::array<Counters, 2> _phase_counters;
    std::atomic<int64_t> _cancel_calls {0};
    AtomicStatus _status;
    std::mutex _lock;
    // Do not keep finished writers' tokens or their thread pools alive.
    std::vector<RegisteredToken> _tokens;
};

// A thin wrapper of ThreadPoolToken to submit calc delete bitmap task.
// Usage:
// 1. create a token
// 2. submit delete bitmap calculate tasks
// 3. wait all tasks complete
// 4. call `get_delete_bitmap()` to get the result of all tasks
class CalcDeleteBitmapToken {
public:
    explicit CalcDeleteBitmapToken(
            std::unique_ptr<ThreadPoolToken> thread_token,
            std::shared_ptr<DeleteBitmapCancellation> load_cancel_status = nullptr,
            DeleteBitmapPhase phase = DeleteBitmapPhase::ROWSET_BUILDER);
    ~CalcDeleteBitmapToken();

    // calculate delete bitmap of `cur_segment` to historical `target_rowsets`
    Status submit(BaseTabletSPtr tablet, RowsetSharedPtr cur_rowset,
                  const segment_v2::SegmentSharedPtr& cur_segment,
                  const std::vector<RowsetSharedPtr>& target_rowsets, int64_t end_version,
                  DeleteBitmapPtr delete_bitmap, RowsetWriter* rowset_writer,
                  DeleteBitmapPtr tablet_delete_bitmap);

    // calculate delete bitmap between `segments`
    Status submit(BaseTabletSPtr tablet, TabletSchemaSPtr schema, RowsetId rowset_id,
                  const std::vector<segment_v2::SegmentSharedPtr>& segments,
                  DeleteBitmapPtr delete_bitmap);

    // submit a generic function to the thread pool
    template <typename Func>
    Status submit_func(Func&& func) {
        auto status = _get_status();
        if (!status.ok()) {
            _record_rejected_submission();
            return status;
        }
        auto resource_ctx = thread_context()->resource_ctx();
        status = _thread_token->submit_func(
                [this, resource_ctx = std::move(resource_ctx), func = std::forward<Func>(func)]() {
                    SCOPED_ATTACH_TASK(resource_ctx);
                    // Cancellation can arrive while the owning channel is blocked in close().
                    auto st = _get_status();
                    if (st.ok()) {
                        if (_load_cancel_status) {
                            _load_cancel_status->_counters(_phase).body_started.fetch_add(
                                    1, std::memory_order_relaxed);
                        }
                        st = func();
                        if (_load_cancel_status) {
                            _load_cancel_status->_counters(_phase).body_finished.fetch_add(
                                    1, std::memory_order_relaxed);
                        }
                    } else if (_load_cancel_status && !_load_cancel_status->ok()) {
                        _load_cancel_status->_counters(_phase).skipped_after_cancel.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                    if (!st.ok()) {
                        std::lock_guard wlock(_lock);
                        if (_status.ok()) {
                            _status = st;
                        }
                    }
                });
        if (status.ok()) {
            if (_load_cancel_status) {
                _load_cancel_status->_counters(_phase).submitted.fetch_add(
                        1, std::memory_order_relaxed);
            }
        } else {
            _record_rejected_submission();
        }
        return status;
    }

    // wait all tasks in token to be completed.
    Status wait();

    void cancel(const Status& st = Status::Cancelled("delete bitmap calculation cancelled"));

private:
    Status _get_status();
    void _record_rejected_submission();
    void _shutdown();

    std::shared_ptr<ThreadPoolToken> _thread_token;

    std::shared_mutex _lock;
    // Records the current status of the calc delete bitmap job.
    // Note: Once its value is set to Failed, it cannot return to SUCCESS.
    Status _status;
    const std::shared_ptr<DeleteBitmapCancellation> _load_cancel_status;
    const DeleteBitmapPhase _phase;
};

// CalcDeleteBitmapExecutor is responsible for calc delete bitmap concurrently.
// It encapsulate a ThreadPool to handle all tasks.
class CalcDeleteBitmapExecutor {
public:
    CalcDeleteBitmapExecutor() = default;
    ~CalcDeleteBitmapExecutor() { _thread_pool->shutdown(); }

    // init should be called after storage engine is opened,
    void init(const std::string& name, int max_threads);

    std::unique_ptr<CalcDeleteBitmapToken> create_token(
            std::shared_ptr<DeleteBitmapCancellation> load_cancel_status = nullptr,
            DeleteBitmapPhase phase = DeleteBitmapPhase::ROWSET_BUILDER);

private:
    std::unique_ptr<ThreadPool> _thread_pool;
};

} // namespace doris
