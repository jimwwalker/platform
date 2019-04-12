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

#pragma once

#include <cstdint>

namespace cb {

/// The maximum number of concurrently registered clients
const int ArenaMallocMaxClients = 100;

/**
 * The cb::ArenaMallocClient is an object that any client of the cb::ArenaMalloc
 * class must keep for use with cb::ArenaMalloc class.
 *
 * The client will receive a cb::ArenaMallocClient object when they call
 * cb::ArenaMalloc::registerClient and it must be kept until they call
 * cb::ArenaMalloc::unregister.
 * Many of the cb::ArenaMalloc methods require a const reference to this object
 * to perform functions on behalf of the client.
 */
struct ArenaMallocClient {
    uint8_t index{0}; // uniquely identifies the registered client
    bool threadCache{true}; // should thread caching be used
    int arena{0}; // uniquely identifies the arena assigned to the client
};

} // namespace cb