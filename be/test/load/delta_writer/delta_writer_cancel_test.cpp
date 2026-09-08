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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "cloud/cloud_delta_writer.h"
#include "cloud/cloud_rowset_builder.h"
#include "cloud/cloud_rowset_writer.h"
#include "cloud/cloud_storage_engine.h"
#include "load/delta_writer/delta_writer.h"
#include "runtime/memory/mem_tracker_limiter.h"
#include "runtime/thread_context.h"
#include "storage/delete/calc_delete_bitmap_executor.h"
#include "storage/options.h"
#include "storage/rowset/beta_rowset_writer.h"
#include "storage/rowset/group_rowset_writer.h"
#include "storage/rowset_builder.h"
#include "storage/storage_engine.h"
#include "util/countdown_latch.h"

namespace doris {

// Exercise both local/cloud writers, with and without row binlog, without tablet I/O.
class DeltaWriterCancelTest : public testing::TestWithParam<int> {
protected:
    void SetUp() override {
        auto tracker = MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::OTHER,
                                                        "DeltaWriterCancelTest");
        _attach_task = std::make_unique<AttachTask>(tracker);
        ASSERT_TRUE(ThreadPoolBuilder("DeltaWriterCancelTest")
                            .set_min_threads(1)
                            .set_max_threads(1)
                            .build(&_pool)
                            .ok());
        WriteRequest data_req;
        WriteRequest group_req;
        group_req.write_req_type = WriteRequestType::GROUP;
        WriteRequest binlog_req;
        binlog_req.write_req_type = WriteRequestType::ROW_BINLOG;
        if (is_cloud()) {
            _cloud_engine = std::make_unique<CloudStorageEngine>(EngineOptions {});
            if (is_group()) {
                _writer = std::make_unique<CloudDeltaWriter>(*_cloud_engine, group_req, data_req,
                                                             binlog_req, nullptr, UniqueId {});
                auto* group = static_cast<CloudGroupRowsetBuilder*>(_writer->_rowset_builder.get());
                _builders = {group->data_builder(), group->row_binlog_builder()};
            } else {
                _writer = std::make_unique<CloudDeltaWriter>(*_cloud_engine, data_req, nullptr,
                                                             UniqueId {});
            }
        } else {
            _local_engine = std::make_unique<StorageEngine>(EngineOptions {});
            if (is_group()) {
                _writer = std::make_unique<DeltaWriter>(*_local_engine, group_req, data_req,
                                                        binlog_req, nullptr, UniqueId {});
                auto* group = static_cast<GroupRowsetBuilder*>(_writer->_rowset_builder.get());
                _builders = {group->txn_rowset_builder(), group->row_binlog_builder()};
            } else {
                _writer = std::make_unique<DeltaWriter>(*_local_engine, data_req, nullptr,
                                                        UniqueId {});
            }
        }
        if (!is_group()) {
            _builders = {_writer->_rowset_builder.get()};
        }
    }

    void TearDown() override {
        // Release the unrelated task before destroying writers, even after a failed assertion.
        _release_worker.count_down();
        if (_pool) {
            _pool->wait();
        }
        _writer.reset();
        _tokens.clear();
        _pool.reset();
        _cloud_engine.reset();
        _local_engine.reset();
        _attach_task.reset();
    }

    bool is_cloud() const { return GetParam() & 1; }
    bool is_group() const { return GetParam() & 2; }

    void install_tokens() {
        for (auto* builder : _builders) {
            std::shared_ptr<BaseBetaRowsetWriter> rowset_writer;
            if (is_cloud()) {
                rowset_writer = std::make_shared<CloudRowsetWriter>(*_cloud_engine);
            } else {
                rowset_writer = std::make_shared<BetaRowsetWriter>(*_local_engine);
            }
            // Set up only the state needed by cancellation and destruction. No files are created.
            rowset_writer->_rowset_meta = std::make_shared<RowsetMeta>();
            rowset_writer->_calc_delete_bitmap_token = make_token();
            builder->_calc_delete_bitmap_token = make_token();
            builder->_rowset_writer = rowset_writer;
            _tokens.push_back(rowset_writer->_calc_delete_bitmap_token.get());
            _tokens.push_back(builder->_calc_delete_bitmap_token.get());
        }
        if (is_group()) {
            auto group_writer = std::make_shared<GroupRowsetWriter>();
            group_writer->set_data_writer(_builders[0]->rowset_writer());
            group_writer->set_row_binlog_writer(_builders[1]->rowset_writer());
            _writer->_rowset_builder->_rowset_writer = std::move(group_writer);
        }
    }

    std::unique_ptr<CalcDeleteBitmapToken> make_token() {
        return std::make_unique<CalcDeleteBitmapToken>(
                _pool->new_token(ThreadPool::ExecutionMode::CONCURRENT));
    }

    std::unique_ptr<AttachTask> _attach_task;
    std::unique_ptr<StorageEngine> _local_engine;
    std::unique_ptr<CloudStorageEngine> _cloud_engine;
    std::unique_ptr<ThreadPool> _pool;
    std::unique_ptr<BaseDeltaWriter> _writer;
    std::vector<BaseRowsetBuilder*> _builders;
    std::vector<CalcDeleteBitmapToken*> _tokens;
    CountDownLatch _worker_started {1};
    CountDownLatch _release_worker {1};
    std::atomic<int> _executed {0};
};

TEST_P(DeltaWriterCancelTest, CancelBeforeInit) {
    ASSERT_TRUE(_writer->cancel().ok());
    EXPECT_TRUE(_writer->_is_cancelled);
    EXPECT_TRUE(_writer->_memtable_writer->_is_cancelled);
    for (auto* builder : _builders) {
        EXPECT_TRUE(builder->_is_cancelled);
    }
    EXPECT_TRUE(_writer->cancel().ok());
}

TEST_P(DeltaWriterCancelTest, CancelRemovesBothPhasesBeforeDestruction) {
    install_tokens();
    ASSERT_TRUE(_pool->submit_func([this] {
                         _worker_started.count_down();
                         _release_worker.wait();
                     }).ok());
    ASSERT_TRUE(_worker_started.wait_for(std::chrono::seconds(10)));
    for (auto* token : _tokens) {
        ASSERT_TRUE(token->submit_func([this] {
                             ++_executed;
                             return Status::OK();
                         }).ok());
    }
    ASSERT_EQ(_pool->get_queue_size(), _tokens.size());

    const auto cancelled = Status::Cancelled("load cancelled by sender");
    ASSERT_TRUE(_writer->cancel_with_status(cancelled).ok());
    EXPECT_EQ(_pool->get_queue_size(), 0);
    EXPECT_EQ(_executed.load(), 0);
    EXPECT_EQ(_writer->_memtable_writer->_cancel_status, cancelled);
    for (auto* token : _tokens) {
        EXPECT_EQ(token->wait(), cancelled);
        EXPECT_EQ(token->submit_func([] { return Status::OK(); }), cancelled);
    }
    ASSERT_TRUE(_writer->cancel_with_status(Status::Cancelled("second cancel")).ok());
    for (auto* token : _tokens) {
        EXPECT_EQ(token->wait(), cancelled);
    }

    _release_worker.count_down();
    _pool->wait();
    EXPECT_EQ(_executed.load(), 0);
}

TEST_P(DeltaWriterCancelTest, PreserveEarlierCalculationFailure) {
    install_tokens();
    const auto failure = Status::InternalError("delete bitmap calculation failed");
    ASSERT_TRUE(_tokens.front()->submit_func([failure] { return failure; }).ok());
    ASSERT_EQ(_tokens.front()->wait(), failure);
    ASSERT_TRUE(_writer->cancel().ok());
    EXPECT_EQ(_tokens.front()->wait(), failure);
    EXPECT_EQ(_tokens.front()->submit_func([] { return Status::OK(); }), failure);
    for (size_t i = 1; i < _tokens.size(); ++i) {
        EXPECT_TRUE(_tokens[i]->wait().is<ErrorCode::CANCELLED>());
    }
}

INSTANTIATE_TEST_SUITE_P(LocalAndCloud, DeltaWriterCancelTest, testing::Values(0, 1, 2, 3));

} // namespace doris
