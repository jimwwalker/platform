/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <platform/cb_arena_malloc.h>

#include <type_traits>

//
// NOTE: The size of this structure is deliberately <= 64-bits so that the
// SystemArenaMalloc can ideally store an entire client in a single 64-bit TLS.
// This isn't a hard requirement, but a nice to have.
//
static_assert(sizeof(cb::ArenaMallocClient) <= 8,
              "sizeof cb::ArenaMallocClient should ideally be <= 64-bits");

namespace cb {

ArenaMallocAutoSwitchFrom::~ArenaMallocAutoSwitchFrom() {
    ArenaMalloc::switchFromClient();
}
} // namespace cb
