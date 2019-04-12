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
static  folly::CachelinePadded<cb::RelaxedAtomic<int64_t>>
        clientEstimatedMemory;

// Per client CoreLocal tracking
static  CoreStore<folly::CachelinePadded<cb::RelaxedAtomic<int64_t>>>
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
    clientEstimatedMemory->store(0);
    for (auto& core : coreAllocated) {
        core.get()->exchange(0);
    }
}

size_t JEArenaCoreLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client) {
    auto& clientEstimate = clientEstimatedMemory;
    for (auto& core : coreAllocated) {
        clientEstimate.get()->fetch_add(core.get()->exchange(0));
    }
    // This still could become negative, e.g. core 0 allocated X after we
    // read it, then core n deallocated X and we read -X.
    return size_t(std::max(int64_t(0), clientEstimate.get()->load()));
}

size_t JEArenaCoreLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client) {
    return size_t(std::max(int64_t(0),
                           clientEstimatedMemory.get()->load()));
}

void JEArenaCoreLocalTracker::memAllocated(const ArenaMallocClient& client,
                                           int flags,
                                           size_t size) {
    if (client.index != NoClientIndex) {
        size = je_nallocx(size, flags);
        auto& coreLocal = *coreAllocated.get();
        auto newSize = coreLocal.fetch_add(size) + size;
        maybeUpdateEstimatedTotalMemUsed(client, coreLocal, newSize);
    }
}

void JEArenaCoreLocalTracker::memDeallocated(const ArenaMallocClient& client,
                                             int flags,
                                             void* ptr) {
    if (client.index != NoClientIndex) {
        auto size = je_sallocx(ptr, flags);
        auto& coreLocal = *coreAllocated.get();
        auto newSize = coreLocal.fetch_sub(size) - size;
        maybeUpdateEstimatedTotalMemUsed(client, coreLocal, newSize);
    }
}

void JEArenaCoreLocalTracker::maybeUpdateEstimatedTotalMemUsed(
        const ArenaMallocClient& client,
        cb::RelaxedAtomic<int64_t>& coreMemory,
        int64_t value) {
    if (std::abs(value) > client.estimateUpdateThreshold) {
        // Swap the core's value to 0 and update total with whatever we got
        clientEstimatedMemory.get()->fetch_add(
                coreMemory.exchange(0));
    }
}
} // end namespace cb