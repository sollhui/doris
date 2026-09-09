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

#include <memory>
#include <mutex>
#include <vector>

#include "common/status.h"

namespace doris {

class ThreadPoolToken;

// A shared cancellation signal for related tokens. Registration and cancellation
// do not require locks belonging to the tokens' task owners.
class ThreadPoolTokenCancellation {
public:
    bool ok() const { return _status.ok(); }
    Status status() const { return _status.ok() ? Status::OK() : _status.status(); }

    void register_token(const std::shared_ptr<ThreadPoolToken>& token);

    // Discard queued tasks and wait only for running tasks. Repeated calls retain
    // the first status and still wait for outstanding work.
    void cancel(const Status& reason);

private:
    AtomicStatus _status;
    std::mutex _lock;
    // Weak references avoid retaining tokens or their pools after writers finish.
    std::vector<std::weak_ptr<ThreadPoolToken>> _tokens;
};

} // namespace doris
