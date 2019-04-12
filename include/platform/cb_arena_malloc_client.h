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

#include "relaxed_atomic.h"
#include <platform/visibility.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cb {

/// The maximum number of concurrently registered clients
const int ArenaMallocMaxClients = 100;

/// Define a special value to denote that no client is selected
const uint8_t NoClientIndex = ArenaMallocMaxClients + 1;

/**
 * The cb::ArenaMallocClient is an object that any client of the cb::ArenaMalloc
 * class must keep for use with cb::ArenaMalloc class.
 *
 * The client will receive a cb::ArenaMallocClient object when they call
 * cb::ArenaMalloc::registerClient and it must be kept until they call
 * cb::ArenaMalloc::unregister.
 */
struct PLATFORM_PUBLIC_API ArenaMallocClient {
    ArenaMallocClient() {
    }

    ArenaMallocClient(int arena, uint8_t index, bool threadCache)
        : arena(arena), index(index), threadCache(threadCache) {
    }

    /**
     * Set the per-core threshold at which the estimated memory counter is
     * updated from all core counters.
     *
     * % of maxDataSize spread over each core
     *
     * @param maxDataSize the client (bucket's) maximum permitted memory usage
     * @param percentage the percentage (0.0 to 100.0) of maxDataSize to spread
     *        per core.
     */
    void setEstimateUpdateThreshold(size_t maxDataSize, float percentage);

    /// How many bytes a thread can alloc or dealloc before the arena's
    /// estimated memory is update.
    cb::RelaxedAtomic<uint32_t> estimateUpdateThreshold{100 * 1024};
    uint16_t arena{0}; // uniquely identifies the arena assigned to the client
    uint8_t index{NoClientIndex}; // uniquely identifies the registered client
    bool threadCache{true}; // should thread caching be used
};

} // namespace cb