/*
 *     Copyright 2019-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once

#include <platform/cb_arena_malloc_client.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace cb {

template <class T>
class RelaxedAtomic;

struct ArenaMallocClient;

/**
 * "plugin" Tracker for JEArenaMalloc, used in production (NDEBUG) builds.
 *
 * Each thread counts the bytes it allocates and deallocates for each client
 * in counters only it writes, so accounting for an allocation costs a few
 * unlocked instructions and writes no shared memory. The precise value is
 * the sum over all threads; the estimate is a single counter per client and
 * domain which threads update when their contribution has changed by more
 * than a threshold. See je_arena_corelocal_tracker.cc for the design.
 *
 * The name is historical: earlier versions kept the counters per core.
 */
class JEArenaCoreLocalTracker {
public:
    static void clientRegistered(const ArenaMallocClient& client,
                                 bool arenaDebugChecksEnabled);

    static void initialiseForNewThread(const ArenaMallocClient& client);

    /**
     * Update the threshold (the client's estimateUpdateThreshold) which
     * bounds how far the client's estimate may lag the precise value.
     */
    static void setAllocatedThreshold(const ArenaMallocClient& client);

    static bool isTrackingAlwaysPrecise() {
        return false;
    }

    /**
     * Choose whether the threshold each thread applies is scaled down when
     * more threads than cpus are allocating. Enabled (the default), the lag
     * of the estimate is bounded by estimateUpdateThreshold multiplied by
     * the cpu count; disabled, every thread applies estimateUpdateThreshold
     * itself and the bound is that multiplied by the number of threads.
     *
     * Exists so the two can be compared by platform-arena_tracking_bench on
     * the deployment platforms; expected to be removed once one is chosen.
     *
     * @param enabled true to scale the thresholds
     * @return the previous setting
     */
    static bool setThresholdScalingEnabled(bool enabled);

    /**
     * Return the precise memory used by the client. This sums the counters
     * of every thread and brings the value returned by getEstimatedAllocated
     * up to date as a side effect.
     */
    static size_t getPreciseAllocated(const ArenaMallocClient& client);

    /**
     * Return the estimated memory used by the client, this is an efficient
     * read of a single counter per domain but can be ahead or behind the
     * 'precise' value. The lag is bounded by the client's configured
     * estimateUpdateThreshold multiplied by the number of cores returned by
     * cb::get_cpu_count
     */
    static size_t getEstimatedAllocated(const ArenaMallocClient& client);

    /// as above, but lookup for the requested domain only
    static size_t getPreciseAllocated(const ArenaMallocClient& client,
                                      MemoryDomain domain);

    /// as above, but lookup for the requested domain only
    static size_t getEstimatedAllocated(const ArenaMallocClient& client,
                                        MemoryDomain domain);

    /**
     * Notify that memory was allocated to the client
     * @param index The index for the client who did the allocation
     * @param size The requested size of the allocation (i.e. size parameter to
     *        malloc)
     * @param alignment Alignment required for the allocation; if no additional
     *        alignment needed (over system default), specify 0.
     */
    static void memAllocated(uint8_t index,
                             MemoryDomain domain,
                             size_t size,
                             std::align_val_t alignment = std::align_val_t{0});

    /**
     * Notify that memory was de-allocated by the client
     * @param index The index for the client who did the deallocation
     * @param ptr The allocation being deallocated
     * @return the usable size of the allocation (as looked up for the
     *         accounting) if the client is tracked, so the caller can use a
     *         sized deallocation and avoid a second lookup. 0 if the client
     *         is not tracked and no lookup was made.
     */
    static size_t memDeallocated(uint8_t index, MemoryDomain domain, void* ptr);

    /**
     * Notify that memory was de-allocated by the client
     * @param index The index for the client who did the deallocation
     * @param size The size of the deallocation
     */
    static void memDeallocated(uint8_t index, MemoryDomain domain, size_t size);
};
} // end namespace cb
