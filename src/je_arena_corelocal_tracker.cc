/*
 *     Copyright 2019-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include <platform/je_arena_corelocal_tracker.h>
#include <platform/unshared.h>

#include <jemalloc/jemalloc.h>

#include <algorithm>
#include <array>

namespace cb {

struct ClientData {
    Unshared<MemoryDomain> allocated;
};

// The client counters, one per client. Unshared is itself cache line aligned
// (and sized) so adjacent clients never share a line.
static_assert(alignof(ClientData) == CachelineAlign);
static std::array<ClientData, ArenaMallocMaxClients> clientData;

void JEArenaCoreLocalTracker::clientRegistered(const ArenaMallocClient& client,
                                               bool arenaDebugChecksEnabled) {
    // CoreLocalTracker doesn't support debug checks.
    (void)arenaDebugChecksEnabled;
    clientData[client.index].allocated.reset();
    setAllocatedThreshold(client);
}

size_t JEArenaCoreLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client) {
    return clientData[client.index].allocated.getPreciseSum();
}

size_t JEArenaCoreLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client) {
    return clientData[client.index].allocated.getEstimateSum();
}

size_t JEArenaCoreLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client, MemoryDomain domain) {
    return clientData[client.index].allocated.getPrecise(domain);
}

size_t JEArenaCoreLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client, MemoryDomain domain) {
    return clientData[client.index].allocated.getEstimate(domain);
}

void JEArenaCoreLocalTracker::setAllocatedThreshold(
        const ArenaMallocClient& client) {
    clientData[client.index].allocated.setCoreThreshold(
            client.estimateUpdateThreshold);
}

void JEArenaCoreLocalTracker::memAllocated(uint8_t index,
                                           MemoryDomain domain,
                                           size_t size,
                                           std::align_val_t alignment) {
    if (index != NoClientIndex) [[likely]] {
        const int flags = (alignment > std::align_val_t{0})
                                  ? MALLOCX_ALIGN(alignment)
                                  : 0;
        size = je_nallocx(size, flags);
        clientData[index].allocated.add(size, domain);
    }
}

size_t JEArenaCoreLocalTracker::memDeallocated(uint8_t index,
                                               MemoryDomain domain,
                                               void* ptr) {
    if (index != NoClientIndex) [[likely]] {
        auto size = je_sallocx(ptr, 0 /* flags aren't read in this call*/);
        clientData[index].allocated.sub(size, domain);
        return size;
    }
    return 0;
}

void JEArenaCoreLocalTracker::memDeallocated(uint8_t index,
                                             MemoryDomain domain,
                                             size_t size) {
    if (index != NoClientIndex) [[likely]] {
        size = je_nallocx(size, 0 /* flags aren't read in this call*/);
        clientData[index].allocated.sub(size, domain);
    }
}

} // end namespace cb
