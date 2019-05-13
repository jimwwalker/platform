#include <iostream>

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
#include <folly/ThreadCachedInt.h>
#include <platform/cb_arena_malloc_client.h>
#include <platform/je_arena_threadlocal_tracker.h>
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

// Per client ThreadLocal tracking
static std::array<folly::ThreadCachedInt<int64_t>, ArenaMallocMaxClients>
        threadAllocated;

// This method exists only to give some stability to C.V. in theory we
// shouldn't need this and more investigation would be required to remove
// this.  If you no-op this method failures occur. Suspect an issue where
// something was freed incorrectly against the engine causing a permanent
// negative memUsed which results in 0. This function resets the memUsed each
// time an engine is created (which matches the behaviour when ep-engine
// stats.cc did the tracking)
// MB-abcd tracks this issue
void JEArenaThreadLocalTracker::clientRegistered(
        const ArenaMallocClient& client) {
    clientEstimatedMemory[client.index]->store(0);

    // Very important to write to the ThreadCachedInt here to initialise it
    // outside of the malloc path, otherwise we will deadlock
    threadAllocated[client.index].increment(0);
    threadAllocated[client.index].readFullAndReset();
}

void JEArenaThreadLocalTracker::threadUp(const ArenaMallocClient& client) {
    threadAllocated[client.index].increment(0);
    // threadAllocated[client.index].increment(-1);
    // threadAllocated[client.index].readFastAndReset();
}

size_t JEArenaThreadLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client) {
    clientEstimatedMemory[client.index]->fetch_add(
            threadAllocated[client.index].readFullAndReset());

    // This still could become negative, e.g. core 0 allocated X after we
    // read it, then core n deallocated X and we read -X.
    return size_t(
            std::max(int64_t(0), clientEstimatedMemory[client.index]->load()));
}

size_t JEArenaThreadLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client) {
    return size_t(std::max(int64_t(0),
                           clientEstimatedMemory[client.index].get()->load()));
}

void maybeUpdateEstimatedTotalMemUsed(
        const ArenaMallocClient& client,
        folly::ThreadCachedInt<int64_t>& threadTracker,
        int64_t value) {
    if (std::abs(value) > 1024) { // client.estimateUpdateThreshold) {
        // Reset the thread value and update total with whatever we got
        clientEstimatedMemory[client.index]->fetch_add(
                threadTracker.readFastAndReset());
    }
}

void JEArenaThreadLocalTracker::memAllocated(const ArenaMallocClient& client,
                                             size_t size) {
    if (client.index != NoClientIndex) {
        size = je_nallocx(size, 0 /* flags aren't read in  this call*/);
        auto& threadCounter = threadAllocated[client.index];
        auto& newSize = threadCounter += size;
        maybeUpdateEstimatedTotalMemUsed(
                client, threadCounter, newSize.readFast());
    }
}

void JEArenaThreadLocalTracker::memDeallocated(const ArenaMallocClient& client,
                                               void* ptr) {
    if (client.index != NoClientIndex) {
        auto size = je_sallocx(ptr, 0 /* flags aren't read in  this call*/);
        auto& threadCounter = threadAllocated[client.index];
        auto& newSize = threadCounter -= size;
        maybeUpdateEstimatedTotalMemUsed(
                client, threadCounter, newSize.readFast());
    }
}

} // end namespace cb