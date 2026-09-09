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

#include "util/threadpool_token_cancellation.h"

#include <utility>

#include "common/logging.h"
#include "util/threadpool.h"

namespace doris {

void ThreadPoolTokenCancellation::register_token(const std::shared_ptr<ThreadPoolToken>& token) {
    {
        std::lock_guard lock(_lock);
        if (_status.ok()) {
            _tokens.emplace_back(token);
            return;
        }
    }
    // A token created after cancellation must reject submissions too.
    token->shutdown();
}

void ThreadPoolTokenCancellation::cancel(const Status& reason) {
    DCHECK(!reason.ok());
    std::vector<std::shared_ptr<ThreadPoolToken>> tokens;
    {
        std::lock_guard lock(_lock);
        _status.update(reason);
        for (const auto& weak_token : _tokens) {
            if (auto token = weak_token.lock()) {
                tokens.push_back(std::move(token));
            }
        }
    }
    // Publish to all tasks before waiting. Neither registration nor task completion
    // needs to wait for this lock while shutdown waits for running tasks.
    for (const auto& token : tokens) {
        token->shutdown();
    }
}

} // namespace doris
