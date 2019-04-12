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

#include "relaxed_atomic.h"
#include <folly/CachelinePadded.h>
#include <platform/cb_arena_malloc_client.h>
#include <platform/corestore.h>
#include <platform/je_arena_corelocal_tracker.h>
#include <platform/non_negative_counter.h>

#include <jemalloc/jemalloc.h>

#include <algorithm>
#include <array>

namespace cb {
// Per client estimated memory
// This is a signed variable as depending on how/when the core-local
// counters merge their info, this could be negative
static std::array<folly::CachelinePadded<cb::RelaxedAtomic<int64_t>>,
                  ArenaMallocMaxClients>
        clientEstimatedMemory;

// Per client CoreLocal tracking
struct CoreLocalStats {
    using Counter=cb::RelaxedAtomic<int64_t>;
    Counter memUsed;
        //! Total size of stored objects.
    Counter currentSize;

    //! Total number of blob objects
    Counter numBlob;

    //! Total size of blob memory overhead
    Counter blobOverhead;

    //! Total memory overhead to store values for resident keys.
    Counter totalValueSize;

    //! The number of storedVal object
    Counter numStoredVal;

    //! Total memory for stored values
    Counter totalStoredValSize;

    //! Amount of memory used to track items and what-not.
    Counter memOverhead;

    //! Total number of Item objects
    Counter numItem;
};

static std::array<CoreStore<folly::CachelinePadded<CoreLocalStats>>,
                  ArenaMallocMaxClients>
        coreAllocated;

// This method exists only to give some stability to C.V. in theory we
// shouldn't need this and more investigation would be required to remove
// this.  If you no-op this method failures occur. Suspect an issue where
// something was freed incorrectly against the engine causing a permanent
// negative memUsed which results in 0. This function resets the memUsed each
// time an engine is created (which matches the behaviour when ep-engine
// stats.cc did the tracking)
// MB-abcd tracks this issue
void JEArenaCoreLocalTracker::clientRegistered(
        const ArenaMallocClient& client) {
    clientEstimatedMemory[client.index]->store(0);
    for (auto& core : coreAllocated[client.index]) {
        core.get()->memUsed.exchange(0);
    }
}

size_t JEArenaCoreLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client) {
    auto& clientEstimate = clientEstimatedMemory[client.index];
    for (auto& core : coreAllocated[client.index]) {
        clientEstimate.get()->fetch_add(core.get()->memUsed.exchange(0));
    }
    // This still could become negative, e.g. core 0 allocated X after we
    // read it, then core n deallocated X and we read -X.
    return size_t(std::max(int64_t(0), clientEstimate.get()->load()));
}

size_t JEArenaCoreLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client) {
    return size_t(std::max(int64_t(0),
                           clientEstimatedMemory[client.index].get()->load()));
}

inline void maybeUpdateEstimatedTotalMemUsed(const ArenaMallocClient& client,
                                      cb::RelaxedAtomic<int64_t>& coreMemory,
                                      int64_t value) {
    if (std::abs(value) > client.estimateUpdateThreshold) {
        // Swap the core's value to 0 and update total with whatever we got
        clientEstimatedMemory[client.index].get()->fetch_add(
                coreMemory.exchange(0));
    }
}

void JEArenaCoreLocalTracker::memAllocated(const ArenaMallocClient& client,
                                           size_t size) {
    if (client.index != NoClientIndex) {
        size = je_nallocx(size, 0 /* flags aren't read in  this call*/);
        auto& coreLocal = *coreAllocated[client.index].get();
        auto newSize = coreLocal.memUsed.fetch_add(size) + size;
        maybeUpdateEstimatedTotalMemUsed(client, coreLocal.memUsed, newSize);
    }
}

void JEArenaCoreLocalTracker::memDeallocated(const ArenaMallocClient& client,
                                             void* ptr) {
    if (client.index != NoClientIndex) {
        auto size = je_sallocx(ptr, 0 /* flags aren't read in  this call*/);
        auto& coreLocal = *coreAllocated[client.index].get();
        auto newSize = coreLocal.memUsed.fetch_sub(size) - size;
        maybeUpdateEstimatedTotalMemUsed(client, coreLocal.memUsed, newSize);
    }
}

} // end namespace cb