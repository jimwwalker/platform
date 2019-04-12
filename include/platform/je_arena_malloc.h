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

#include <folly/Synchronized.h>
#include <platform/cb_arena_malloc_client.h>
#include <platform/je_arena_threadlocal_tracker.h>
#include <platform/non_negative_counter.h>
#include <platform/visibility.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace cb {

// Defined in .cc, MSVC won't allow thread_local in the DLL exported API
struct ThreadLocalData;

/**
 * JEArenaMalloc implements the ArenaMalloc class providing memory allocation
 * via je_malloc - https://github.com/jemalloc/jemalloc.
 *
 * registering for an arena gives the client a je_malloc arena to encapsulate
 * their allocation activity and providing better memory per bucket memory stats
 *
 */
template <class trackingImpl>
class PLATFORM_PUBLIC_API _JEArenaMalloc {
public:
    static ArenaMallocClient registerClient(bool threadCache);
    static void unregisterClient(const ArenaMallocClient& client);
    static void unregisterCurrentClient();
    static void switchToClient(const ArenaMallocClient& client);
    static void switchFromClient();
    static void updateClientThreshold(const ArenaMallocClient& client) {
        trackingImpl::updateClientThreshold(client);
    }
    static size_t getPreciseAllocated(const ArenaMallocClient& client) {
        return trackingImpl::getPreciseAllocated(client);
    }
    static size_t getEstimatedAllocated(const ArenaMallocClient& client) {
        return trackingImpl::getEstimatedAllocated(client);
    }

    static void* malloc(size_t size);
    static void* calloc(size_t nmemb, size_t size);
    static void* realloc(void* ptr, size_t size);
    static void free(void* ptr);
    static void sized_free(void* ptr, size_t size);
    static size_t malloc_usable_size(void* ptr);
    static constexpr bool canTrackAllocations() {
        return true;
    }
    static void setTCacheEnabled(bool value);

private:
    static void clientRegistered(const ArenaMallocClient& client) {
        trackingImpl::clientRegistered(client);
    }

    static void threadUp(uint8_t index) {
        trackingImpl::threadUp(index);
    }

    /// @return the allocated bytes for the given arena
    static size_t getAllocated(int arena);

    /**
     * Called when memory is allocated
     *
     * @param client The client making the allocation
     * @param size The clients requested allocation size
     */
    static void memAllocated(uint8_t index, size_t size) {
        trackingImpl::memAllocated(index, size);
    }

    /**
     * Called when memory is deallocated
     *
     * @param client The client making the allocation
     * @param ptr The allocation being de-allocated
     */
    static void memDeallocated(uint8_t index, void* ptr) {
        trackingImpl::memDeallocated(index, ptr);
    }

    /// @return make and return a new arena index from jemalloc
    static int makeArena();

    /// @return make and return a new tcache ID from jemalloc
    static int makeTCache();
};

using JEArenaMalloc = _JEArenaMalloc<JEArenaThreadLocalTracker>;

} // namespace cb