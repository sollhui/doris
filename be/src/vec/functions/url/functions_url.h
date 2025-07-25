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
// This file is copied from
// https://github.com/ClickHouse/ClickHouse/blob/master/src/Functions/URL/FunctionsURL.h
// and modified by Doris

#pragma once

#include "vec/columns/column_string.h"
#include "vec/common/memcpy_small.h"

namespace doris::vectorized {
#include "common/compile_check_begin.h"
/** URL processing functions. See implementation in separate .cpp files.
  * All functions are not strictly follow RFC, instead they are maximally simplified for performance reasons.
  *
  * Functions for extraction parts of URL.
  * If URL has nothing like, then empty string is returned.
  *
  *  domain
  *  domainWithoutWWW
  *  topLevelDomain
  *  protocol
  *  path
  *  queryString
  *  fragment
  *  queryStringAndFragment
  *  netloc
  *
  * Functions, removing parts from URL.
  * If URL has nothing like, then it is returned unchanged.
  *
  *  cutWWW
  *  cutFragment
  *  cutQueryString
  *  cutQueryStringAndFragment
  *
  * Extract value of parameter in query string or in fragment identifier. Return empty string, if URL has no such parameter.
  * If there are many parameters with same name - return value of first one. Value is not %-decoded.
  *
  *  extractURLParameter(URL, name)
  *
  * Extract all parameters from URL in form of array of strings name=value.
  *  extractURLParameters(URL)
  *
  * Extract names of all parameters from URL in form of array of strings.
  *  extractURLParameterNames(URL)
  *
  * Remove specified parameter from URL.
  *  cutURLParameter(URL, name)
  *
  * Get array of URL 'hierarchy' as in web-analytics tree-like reports. See the docs.
  *  URLHierarchy(URL)
  */

using Pos = const char*;

/** Select part of string using the Extractor.
  */
template <typename Extractor>
struct ExtractSubstringImpl {
    static Status vector(const ColumnString::Chars& data, const ColumnString::Offsets& offsets,
                         ColumnString::Chars& res_data, ColumnString::Offsets& res_offsets) {
        size_t size = offsets.size();
        res_offsets.resize(size);
        res_data.reserve(size * Extractor::get_reserve_length_for_element());

        size_t prev_offset = 0;
        size_t res_offset = 0;

        /// Matched part.
        Pos start;
        size_t length;

        for (size_t i = 0; i < size; ++i) {
            Extractor::execute(reinterpret_cast<const char*>(&data[prev_offset]),
                               offsets[i] - prev_offset, start, length);
            res_data.resize(res_data.size() + length);
            memcpy_small_allow_read_write_overflow15(&res_data[res_offset], start, length);
            res_offset += length;

            res_offsets[i] = (ColumnString::Offset)res_offset;
            prev_offset = offsets[i];
        }
        return Status::OK();
    }

    static void constant(const std::string& data, std::string& res_data) {
        Pos start;
        size_t length;
        Extractor::execute(data.data(), data.size(), start, length);
        res_data.assign(start, length);
    }
};

/** Delete part of string using the Extractor.
  */
template <typename Extractor>
struct CutSubstringImpl {
    static void vector(const ColumnString::Chars& data, const ColumnString::Offsets& offsets,
                       ColumnString::Chars& res_data, ColumnString::Offsets& res_offsets) {
        res_data.reserve(data.size());
        size_t size = offsets.size();
        res_offsets.resize(size);

        size_t prev_offset = 0;
        size_t res_offset = 0;

        /// Matched part.
        Pos start;
        size_t length;

        for (size_t i = 0; i < size; ++i) {
            const char* current = reinterpret_cast<const char*>(&data[prev_offset]);
            Extractor::execute(current, offsets[i] - prev_offset, start, length);
            size_t start_index = start - reinterpret_cast<const char*>(data.data());

            res_data.resize(res_data.size() + offsets[i] - prev_offset - length);
            memcpy_small_allow_read_write_overflow15(&res_data[res_offset], current,
                                                     start - current);
            memcpy_small_allow_read_write_overflow15(&res_data[res_offset + start - current],
                                                     start + length,
                                                     offsets[i] - start_index - length);
            res_offset += offsets[i] - prev_offset - length;

            res_offsets[i] = res_offset;
            prev_offset = offsets[i];
        }
    }

    static void constant(const std::string& data, std::string& res_data) {
        Pos start;
        size_t length;
        Extractor::execute(data.data(), data.size(), start, length);
        res_data.reserve(data.size() - length);
        res_data.append(data.data(), start);
        res_data.append(start + length, data.data() + data.size());
    }
};
#include "common/compile_check_end.h"
} // namespace doris::vectorized
