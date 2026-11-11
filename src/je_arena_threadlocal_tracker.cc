/*
 *     Copyright 2026-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

/*
 * Design
 * ======
 *
 * Every thread which allocates against a client owns a ThreadCounters block
 * holding, for each client and domain, the net number of bytes that thread has
 * allocated ("total"). Only the owning thread ever writes a total, so the
 * update on every allocation and deallocation is a plain load, add and store
 * to memory which no other thread writes: no locked instruction, no lookup of
 * the current cpu and no shared cache line is written.
 *
 * - Precise read: the sum of every thread's total. The blocks are linked into
 *   a registry and are never freed (a block is recycled when its thread
 *   exits, keeping its totals), so the registry can be walked without a lock.
 *
 * - Estimate read: one shared counter per client and domain. A thread adds
 *   the part of its total which the estimate has not yet seen ("total" minus
 *   "flushed") once that exceeds the threshold. Precise reads do the same on
 *   behalf of every thread, so (as before) a precise read brings the estimate
 *   up to date. "flushed" is therefore written by its owner and by precise
 *   readers; both use compare-exchange and whoever wins applies the
 *   difference to the estimate, so nothing is counted twice or lost.
 *
 * The lag of JEArenaCoreLocalTracker's estimate is bounded by the client's
 * threshold multiplied by the cpu count. As the unflushed amounts are here
 * per thread rather than per core, the threshold applied to each thread is
 * scaled down when more threads than cpus own a block so that bound still holds
 * (a thread flushes everything when it exits, so only live threads count).
 *
 * The remaining cost of tracking is finding the size jemalloc will really
 * allocate for a request (je_nallocx). The small size classes are a simple
 * function of the request, so for those it is computed inline once that has
 * been verified against je_nallocx (see verifySizeClasses).
 */

#include <platform/cacheline_aligned.h>
#include <platform/je_arena_threadlocal_tracker.h>
#include <platform/sysinfo.h>

#include <jemalloc/jemalloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <mutex>
#include <new>

namespace cb {
namespace {

using Counter = std::atomic<int64_t>;

constexpr auto NumDomains = static_cast<size_t>(MemoryDomain::Count);

// All of the orderings are relaxed: the counters are independent statistics
// and nothing is published through them.
constexpr auto relaxed = std::memory_order_relaxed;

/**
 * One thread's counters for one client.
 */
struct ClientCell {
    /**
     * Net bytes allocated by the owning thread, per domain. Written only by
     * the owning thread (see ThreadCounters::shared for the one exception).
     */
    std::array<Counter, NumDomains> total{};

    /**
     * The value of total which has been applied to the client's estimate.
     * Written (compare-exchange) by the owning thread and by precise readers.
     */
    std::array<Counter, NumDomains> flushed{};
};

/**
 * The counters of one thread for every client. Cache line aligned (and so a
 * multiple of a line in size) so that no two threads' counters share a line.
 * Blocks are allocated on demand, linked into the registry and never freed.
 */
struct alignas(CachelineAlign) ThreadCounters {
    constexpr ThreadCounters() = default;

    constexpr explicit ThreadCounters(bool shared) : shared(shared) {
    }

    std::array<ClientCell, ArenaMallocMaxClients> clients{};

    /// The next block in the registry; never changes once the block is linked
    ThreadCounters* next{nullptr};

    /// True whilst a thread owns this block
    std::atomic<bool> inUse{true};

    /**
     * True for the one block which has no owner and is updated with atomic
     * read-modify-writes (see orphanCounters).
     */
    const bool shared{false};
};

/**
 * State of one client which is shared by all threads.
 */
struct alignas(CachelineAlign) ClientState {
    /**
     * A thread applies its unflushed bytes to the estimate when they exceed
     * this. Read on every allocation and deallocation and rarely written, so
     * kept on a different line from the estimates.
     */
    Counter threadThreshold{0};

    /// The threshold requested by the client (estimateUpdateThreshold)
    Counter clientThreshold{0};

    /// The estimated bytes allocated, per domain.
    alignas(CachelineAlign) std::array<Counter, NumDomains> estimates{};
};

constinit std::array<ClientState, ArenaMallocMaxClients> clientStates{};

/**
 * Counters used by threads which no longer have (or can't obtain) a block of
 * their own: allocations made whilst a thread is being destroyed, after its
 * block was released. Always the last element of the registry.
 */
constinit ThreadCounters orphanCounters{true};

/// Head of the list of every ThreadCounters
constinit std::atomic<ThreadCounters*> registry{&orphanCounters};

/// Guards activeBlocks, thresholdDivisor and ClientState::threadThreshold
constinit std::mutex thresholdMutex;

/// Number of ThreadCounters owned by a thread, plus one for orphanCounters
constinit size_t activeBlocks{1};

/**
 * The scaling currently applied to each client's threshold to give the
 * threshold used by each thread: threshold x cpus / blocks, or no scaling
 * when blocks is 0 (no more active blocks than cpus). blocks is the number
 * of active blocks rounded up to a multiple of a quarter of the cpu count so
 * that the thresholds, which every allocation reads, are not rewritten each
 * time a thread starts or exits.
 */
struct ThresholdScale {
    size_t cpus{0};
    size_t blocks{0};
};
constinit ThresholdScale thresholdScale{};

/// See JEArenaThreadLocalTracker::setThresholdScalingEnabled
constinit bool thresholdScalingEnabled{true};

/**
 * True once the inline calculation of jemalloc's size classes has been
 * checked against jemalloc. Read on every allocation, written once.
 */
alignas(CachelineAlign) constinit std::atomic<bool> sizeClassesVerified{false};

/**
 * Largest request for which the size class is calculated inline. Limited to
 * jemalloc's small size classes: these cover most allocations, their cost is
 * low enough for a call to je_nallocx to matter and (unlike the large
 * classes, which depend on the page size and jemalloc options) they can be
 * verified exhaustively.
 */
constexpr size_t MaxInlineSize = 4096;

/**
 * jemalloc's small size classes for a 16 byte quantum: 8, multiples of 16 up
 * to 128 and from there four evenly spaced classes per doubling of size.
 * @param size the requested size, 1 to MaxInlineSize
 * @return the size of the allocation which would be made
 */
constexpr size_t sizeClassFor(size_t size) {
    if (size <= 8) {
        return 8;
    }
    // ceil(log2(size))
    const auto lgCeil = static_cast<size_t>(std::bit_width(size - 1));
    const auto lgDelta = lgCeil < 7 ? size_t{4} : lgCeil - 3;
    const auto mask = (size_t{1} << lgDelta) - 1;
    return (size + mask) & ~mask;
}

/**
 * Check sizeClassFor against je_nallocx for every size it is used for. If
 * any differ (a jemalloc built with a different quantum for example) the
 * inline calculation stays disabled and je_nallocx is called for every
 * allocation.
 */
void verifySizeClasses() {
    static std::once_flag once;
    std::call_once(once, []() {
        for (size_t size = 1; size <= MaxInlineSize; size++) {
            if (sizeClassFor(size) != je_nallocx(size, 0)) {
                return;
            }
        }
        sizeClassesVerified.store(true, relaxed);
    });
}

/**
 * @return the size jemalloc allocates for a request of size bytes with no
 *         alignment requirement, as je_nallocx(size, 0)
 */
inline size_t allocationSize(size_t size) {
    if (size - 1 < MaxInlineSize && sizeClassesVerified.load(relaxed))
            [[likely]] {
        return sizeClassFor(size);
    }
    return je_nallocx(size, 0);
}

/**
 * The calling thread's block. Null until the thread first accounts for memory
 * and again once the thread's block has been released at thread exit.
 * Trivially constructed and destroyed so access needs no guard.
 */
thread_local constinit ThreadCounters* threadCounters{nullptr};

/// Set once the calling thread has released its block (it is exiting)
thread_local constinit bool threadCountersReleased{false};

/**
 * Calculate the threshold each thread applies so that the sum of all threads'
 * unflushed bytes stays within clientThreshold x cpu count.
 * Caller must hold thresholdMutex.
 */
void updateThreadThreshold(ClientState& state) {
    auto threshold = state.clientThreshold.load(relaxed);
    if (thresholdScale.blocks != 0) {
        threshold = threshold * int64_t(thresholdScale.cpus) /
                    int64_t(thresholdScale.blocks);
    }
    state.threadThreshold.store(threshold, relaxed);
}

/**
 * Note that the number of blocks owned by threads changed, and update every
 * client's thread threshold if the scaling changes as a result.
 * @param change +1 or -1
 * @param cpus the cpu count
 */
void activeBlocksChanged(int change, size_t cpus) {
    std::lock_guard<std::mutex> guard(thresholdMutex);
    activeBlocks += change;
    ThresholdScale scale{};
    if (thresholdScalingEnabled && activeBlocks > cpus) {
        const auto step = std::max<size_t>(1, cpus / 4);
        scale = {.cpus = cpus,
                 .blocks = ((activeBlocks + step - 1) / step) * step};
    }
    if (scale.blocks != thresholdScale.blocks ||
        scale.cpus != thresholdScale.cpus) {
        thresholdScale = scale;
        for (auto& state : clientStates) {
            updateThreadThreshold(state);
        }
    }
}

/**
 * Bring the estimate up to date with the given total of one thread.
 * @param flushed the value last read from cell.flushed[domain]
 * @return the bytes added to the estimate (0 if another thread did so first)
 */
int64_t flush(ClientCell& cell, size_t domain, int64_t total, int64_t flushed) {
    if (cell.flushed[domain].compare_exchange_strong(
                flushed, total, relaxed, relaxed)) {
        return total - flushed;
    }
    return 0;
}

/**
 * Apply everything the block has not yet flushed to the estimates.
 */
void flushAll(ThreadCounters& counters) {
    for (size_t index = 0; index < counters.clients.size(); index++) {
        auto& cell = counters.clients[index];
        for (size_t domain = 0; domain < NumDomains; domain++) {
            const auto total = cell.total[domain].load(relaxed);
            const auto flushed = cell.flushed[domain].load(relaxed);
            if (total == flushed) {
                continue;
            }
            const auto moved = flush(cell, domain, total, flushed);
            if (moved != 0) {
                clientStates[index].estimates[domain].fetch_add(moved, relaxed);
            }
        }
    }
}

/**
 * Give the calling thread a ThreadCounters: a block released by a thread
 * which has exited or else a new one.
 * @return the block, or nullptr if one could not be allocated
 */
ThreadCounters* acquireThreadCounters() {
    ThreadCounters* block = nullptr;
    for (auto* candidate = registry.load(std::memory_order_acquire);
         candidate != nullptr;
         candidate = candidate->next) {
        bool expected = false;
        if (candidate->inUse.load(relaxed) ||
            !candidate->inUse.compare_exchange_strong(
                    expected, true, std::memory_order_acquire, relaxed)) {
            continue;
        }
        block = candidate;
        break;
    }

    if (block == nullptr) {
        // Allocate directly from jemalloc's default arena: this is called
        // from within an allocation and must not be tracked.
        static_assert(sizeof(ThreadCounters) % CachelineAlign == 0);
        void* memory = je_mallocx(sizeof(ThreadCounters),
                                  MALLOCX_ALIGN(CachelineAlign));
        if (memory == nullptr) {
            return nullptr;
        }
        block = new (memory) ThreadCounters();
        block->next = registry.load(relaxed);
        while (!registry.compare_exchange_weak(
                block->next, block, std::memory_order_release, relaxed)) {
        }
    }

    // Must be set before anything below: registering the thread_local's
    // destructor and reading the cpu count can allocate, which re-enters the
    // tracker.
    threadCounters = block;

    /**
     * Returns the thread's block for reuse when the thread exits. The totals
     * stay in the block (and so in every precise read); what the thread had
     * not flushed is applied to the estimates so that only live threads
     * contribute to the estimates' lag.
     */
    struct Release {
        ~Release() {
            threadCounters = nullptr;
            threadCountersReleased = true;
            flushAll(*block);
            block->inUse.store(false, std::memory_order_release);
            activeBlocksChanged(-1, cpus);
        }
        ThreadCounters* block;
        size_t cpus;
    };
    const auto cpus = get_cpu_count();
    thread_local Release release{block, cpus};

    activeBlocksChanged(1, cpus);
    return block;
}

/**
 * Account for delta bytes using atomic read-modify-writes, for blocks which
 * may be written by many threads.
 */
void accountShared(ThreadCounters& counters,
                   uint8_t index,
                   size_t domain,
                   int64_t delta) {
    auto& cell = counters.clients[index];
    auto& state = clientStates[index];
    const auto total = cell.total[domain].fetch_add(delta, relaxed) + delta;
    const auto flushed = cell.flushed[domain].load(relaxed);
    if (std::abs(total - flushed) > state.threadThreshold.load(relaxed)) {
        const auto moved = flush(cell, domain, total, flushed);
        if (moved != 0) {
            state.estimates[domain].fetch_add(moved, relaxed);
        }
    }
}

/**
 * Account for delta bytes on the calling thread's own block.
 */
inline void accountOwned(ThreadCounters& counters,
                         uint8_t index,
                         size_t domain,
                         int64_t delta) {
    auto& cell = counters.clients[index];
    // Single writer, so no atomic read-modify-write is needed.
    const auto total = cell.total[domain].load(relaxed) + delta;
    cell.total[domain].store(total, relaxed);

    const auto flushed = cell.flushed[domain].load(relaxed);
    auto& state = clientStates[index];
    if (std::abs(total - flushed) > state.threadThreshold.load(relaxed))
            [[unlikely]] {
        const auto moved = flush(cell, domain, total, flushed);
        if (moved != 0) {
            state.estimates[domain].fetch_add(moved, relaxed);
        }
    }
}

/**
 * Account for delta bytes when the calling thread has no block.
 */
[[gnu::noinline]] void accountNoBlock(uint8_t index,
                                      size_t domain,
                                      int64_t delta) {
    if (!threadCountersReleased) {
        if (auto* counters = acquireThreadCounters()) {
            accountOwned(*counters, index, domain, delta);
            return;
        }
    }
    accountShared(orphanCounters, index, domain, delta);
}

inline void account(uint8_t index, MemoryDomain domain, int64_t delta) {
    auto* counters = threadCounters;
    if (counters == nullptr) [[unlikely]] {
        accountNoBlock(index, static_cast<size_t>(domain), delta);
        return;
    }
    accountOwned(*counters, index, static_cast<size_t>(domain), delta);
}

/**
 * Sum every thread's totals for the client, bringing the estimates up to
 * date as a side effect. All domains are summed in one walk as one thread's
 * counters for a client share a cache line.
 * @return the sum for each domain, which is negative if the domain has had
 *         more deallocated from it than was allocated to it.
 */
std::array<int64_t, NumDomains> preciseAllocated(uint8_t index) {
    std::array<int64_t, NumDomains> sums{};
    std::array<int64_t, NumDomains> moved{};
    for (auto* counters = registry.load(std::memory_order_acquire);
         counters != nullptr;
         counters = counters->next) {
        auto& cell = counters->clients[index];
        for (size_t domain = 0; domain < NumDomains; domain++) {
            const auto total = cell.total[domain].load(relaxed);
            const auto flushed = cell.flushed[domain].load(relaxed);
            sums[domain] += total;
            // Threads with nothing to flush are only read, so their cache
            // line is not taken from them.
            if (total != flushed) {
                moved[domain] += flush(cell, domain, total, flushed);
            }
        }
    }
    for (size_t domain = 0; domain < NumDomains; domain++) {
        if (moved[domain] != 0) {
            clientStates[index].estimates[domain].fetch_add(moved[domain],
                                                            relaxed);
        }
    }
    return sums;
}

size_t clampToZero(int64_t value) {
    return static_cast<size_t>(std::max(int64_t{0}, value));
}

} // namespace

void JEArenaThreadLocalTracker::clientRegistered(
        const ArenaMallocClient& client, bool arenaDebugChecksEnabled) {
    // This tracker doesn't support debug checks.
    (void)arenaDebugChecksEnabled;
    verifySizeClasses();

    // Zero the counters left by the previous user of this index. No thread
    // should be allocating against the index at this point; if one is the
    // counters remain valid but the result is unspecified, as it would be
    // for any reset which races with updates.
    for (auto* counters = registry.load(std::memory_order_acquire);
         counters != nullptr;
         counters = counters->next) {
        auto& cell = counters->clients[client.index];
        for (size_t domain = 0; domain < NumDomains; domain++) {
            cell.total[domain].store(0, relaxed);
            cell.flushed[domain].store(0, relaxed);
        }
    }
    for (auto& estimate : clientStates[client.index].estimates) {
        estimate.store(0, relaxed);
    }
    setAllocatedThreshold(client);
}

size_t JEArenaThreadLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client) {
    size_t sum = 0;
    for (const auto allocated : preciseAllocated(client.index)) {
        sum += clampToZero(allocated);
    }
    return sum;
}

size_t JEArenaThreadLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client) {
    int64_t sum = 0;
    for (const auto& estimate : clientStates[client.index].estimates) {
        sum += estimate.load(relaxed);
    }
    return clampToZero(sum);
}

size_t JEArenaThreadLocalTracker::getPreciseAllocated(
        const ArenaMallocClient& client, MemoryDomain domain) {
    return clampToZero(
            preciseAllocated(client.index)[static_cast<size_t>(domain)]);
}

size_t JEArenaThreadLocalTracker::getEstimatedAllocated(
        const ArenaMallocClient& client, MemoryDomain domain) {
    return clampToZero(clientStates[client.index]
                               .estimates[static_cast<size_t>(domain)]
                               .load(relaxed));
}

void JEArenaThreadLocalTracker::setAllocatedThreshold(
        const ArenaMallocClient& client) {
    auto& state = clientStates[client.index];
    std::lock_guard<std::mutex> guard(thresholdMutex);
    state.clientThreshold.store(client.estimateUpdateThreshold.load(), relaxed);
    updateThreadThreshold(state);
}

bool JEArenaThreadLocalTracker::setThresholdScalingEnabled(bool enabled) {
    const auto cpus = get_cpu_count();
    bool previous = false;
    {
        std::lock_guard<std::mutex> guard(thresholdMutex);
        previous = thresholdScalingEnabled;
        thresholdScalingEnabled = enabled;
    }
    // Recalculate every client's thread threshold for the new setting
    activeBlocksChanged(0, cpus);
    return previous;
}

void JEArenaThreadLocalTracker::memAllocated(uint8_t index,
                                             MemoryDomain domain,
                                             size_t size,
                                             std::align_val_t alignment) {
    if (index != NoClientIndex) [[likely]] {
        if (alignment > std::align_val_t{0}) [[unlikely]] {
            size = je_nallocx(size, MALLOCX_ALIGN(alignment));
        } else {
            size = allocationSize(size);
        }
        account(index, domain, static_cast<int64_t>(size));
    }
}

size_t JEArenaThreadLocalTracker::memDeallocated(uint8_t index,
                                                 MemoryDomain domain,
                                                 void* ptr) {
    if (index != NoClientIndex) [[likely]] {
        auto size = je_sallocx(ptr, 0 /* flags aren't read in this call*/);
        account(index, domain, -static_cast<int64_t>(size));
        return size;
    }
    return 0;
}

void JEArenaThreadLocalTracker::memDeallocated(uint8_t index,
                                               MemoryDomain domain,
                                               size_t size) {
    if (index != NoClientIndex) [[likely]] {
        account(index, domain, -static_cast<int64_t>(allocationSize(size)));
    }
}

} // end namespace cb
