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

// For each client we store the following three pieces of information.
//
// 1) A folly::ThreadCachedInt which is used to accumulate the thread's
//    allocation total.
// 2) A thread allocation threshold. A signed 64-bit counter for how much we
//    will allow 1) to accumulate before a) updating the estimate 3) and b)
//    clearing the thread count.
// 3) Estimated memory - a signed 64-bit counter of how much the client has
//    allocated. This value is updated by a) a thread reaching the per thread
//    allocation threshold, or b) a call to getPreciseAllocated. This counter
//    is signed so we can safely handle the counter validly being negative (see
//    comments in getPreciseAllocated and getEstimatedAllocated).

static std::array<folly::ThreadCachedInt<int64_t>, ArenaMallocMaxClients>
        threadAllocated;

static std::array<cb::RelaxedAtomic<int64_t>, ArenaMallocMaxClients>
        threadThreshold;

static std::array<folly::CachelinePadded<cb::RelaxedAtomic<int64_t>>,
                  ArenaMallocMaxClients>
        clientEstimatedMemory;

void JEArenaThreadLocalTracker::clientRegistered(
        const ArenaMallocClient& client) {
    clientEstimatedMemory[client.index]->store(0);

    // Very important to write to the ThreadCachedInt here to initialise it
    // outside of the malloc path, otherwise we will deadlock
    threadAllocated[client.index].increment(0);
    threadAllocated[client.index].readFullAndReset();
    updateClientThreshold(client);
}

void JEArenaThreadLocalTracker::threadUp(uint8_t index) {
    threadAllocated[index].increment(0);
}

size_t JEArenaThreadLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client) {
    clientEstimatedMemory[client.index]->fetch_add(
            threadAllocated[client.index].readFullAndReset());

    // See the comment in getEstimatedAllocated regarding negative counts, even
    // in this case where we are summing up all thread counters there is still
    // the possibility of seeing a negative value based. After we've observed
    // a threads counter and summed it into the global count, it's not
    // impossible for an allocation to occur on that thread and then be
    // deallocated on the next thread, so our summation observes more
    // deallocations than allocations.
    return size_t(
            std::max(int64_t(0), clientEstimatedMemory[client.index]->load()));
}

size_t JEArenaThreadLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client) {
    // The client's memory counter could become negative.
    // For example if Thread1 deallocates something it didn't allocate and the
    // deallocation triggers a sync of the thread local counter into the global
    // counter, this can lead to a negative value. In the negative case we
    // return zero so that we don't end up with a large unsigned value.
    return size_t(std::max(int64_t(0),
                           clientEstimatedMemory[client.index].get()->load()));
}

void JEArenaThreadLocalTracker::updateClientThreshold(
        const ArenaMallocClient& client) {
    threadThreshold[client.index] = client.estimateUpdateThreshold;
}

void maybeUpdateEstimatedTotalMemUsed(
        uint8_t index,
        folly::ThreadCachedInt<int64_t>& threadTracker,
        int64_t value) {
    if (std::abs(value) > threadThreshold[index]) {
        // Reset the thread value and update total with whatever we got
        clientEstimatedMemory[index]->fetch_add(
                threadTracker.readFastAndReset());
    }
}

void JEArenaThreadLocalTracker::memAllocated(uint8_t index, size_t size) {
    if (index != NoClientIndex) {
        size = je_nallocx(size, 0 /* flags aren't read in  this call*/);
        auto& threadCounter = threadAllocated[index];
        threadCounter.increment(size);
        maybeUpdateEstimatedTotalMemUsed(
                index, threadCounter, threadCounter.readFast());
    }
}

void JEArenaThreadLocalTracker::memDeallocated(uint8_t index, void* ptr) {
    if (index != NoClientIndex) {
        auto size = je_sallocx(ptr, 0 /* flags aren't read in  this call*/);
        auto& threadCounter = threadAllocated[index];
        threadCounter.increment(-int64_t(size));
        maybeUpdateEstimatedTotalMemUsed(
                index, threadCounter, threadCounter.readFast());
    }
}

} // end namespace cb