/*
 *     Copyright 2024-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once

#include <platform/cacheline_aligned.h>
#include <platform/corestore.h>
#include <relaxed_atomic.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace cb {

/**
 * Enumeration used as the default Index template parameter of Unshared<Index,
 * Count, Integer>. Allows only a single value to be stored.
 */
enum class MonoIndex : std::size_t {
    Default = 0,
    Count = 1,
};

/**
 * A high-performance counter (or set of counters) optimised to reduce
 * cross-cache communication.
 *
 * The counters can be read in two modes:
 * - estimate (off by up to NUM_CPUS x threshold)
 * - precise (which updates the estimate as a side effect)
 *
 * Every update to these counters is stored as a delta in a core-local counter.
 * Whenever the core-local delta goes above the configured threshold, the delta
 * is cleared and the estimate is updated with it.
 *
 * The precise read works by clearing all of the deltas, and updating the
 * estimate. Since clearing the deltas is not a single atomic operation, the
 * precise value is still not a point-in-time snapshot. The cleared deltas are
 * applied to the estimate once at the end of the read (rather than once per
 * core) so a concurrent estimate read may briefly observe a value lower than
 * it would otherwise; the difference is within the documented drift of
 * NUM_CPUS x threshold.
 *
 * Additionally, an interleaved sequence of Decrement(Core-1), GetPrecise,
 * Increment(Core-2) could leave the internal estimate negative.
 *
 * To fix this, any negative values are capped to 0 when they are returned to
 * the caller and the caller will never observe the negative values.
 *
 * Memory layout: the data read on every add (the core-local delta storage and
 * the threshold) is kept on a different cache line from the estimates, which
 * are written whenever a core's delta is flushed. Each core's deltas occupy
 * their own cache line (see CachelineAligned). Objects of this type are
 * therefore CachelineAlign aligned and at least two cache lines in size.
 *
 * @tparam Index an enumeration type which can be used to read the stored value.
 * If this type has a Default member, that will be used as a default parameter
 * to some methods.
 * @tparam Count the number of values that should be stored. If the enumeration
 * has a Count enumerator, it's integer value is used as the count.
 * @tparam Integer the signed integer type to use for the counters.
 * @tparam CountFn passed down to CoreStore.
 * @tparam IndexFn passed down to CoreStore.
 */
template <typename Index,
          std::size_t Count = std::size_t(Index::Count),
          typename Integer = std::int64_t,
          CountFnType CountFn = cb::get_cpu_count,
          IndexFnType IndexFn = cb::stripe_for_current_cpu_inline>
struct Unshared {
public:
    static_assert(std::is_enum_v<Index>, "Index must be an enumeration");
    static_assert(Count, "Count must be greater than 0");
    static_assert(std::is_signed_v<Integer>,
                  "Integer must be a signed integer type");

    /**
     * Set the threshold for the maximum core-local delta that is allowed,
     * before we have to update the estimate.
     */
    void setCoreThreshold(Integer value) {
        coreThreshold = value;
    }

    /**
     * Get the threshold for the maximum core-local delta.
     */
    Integer getCoreThreshold() const {
        return coreThreshold;
    }

    /**
     * Returns the maximum difference that can be observed between an estimate
     * read and a precise read.
     */
    Integer getMaximumDrift() const {
        return coreThreshold * static_cast<Integer>(CountFn());
    }

    /**
     * Performs arithmetic addition on the counter.
     * This may update the estimate, if it causes the core-local delta to go
     * above the allowed threshold.
     */
    void add(Integer value, Index index = Index::Default) {
        const auto i = static_cast<std::size_t>(index);
        auto& delta = (*coreDeltas.get())[i];
        const auto newDelta = delta.fetch_add(value) + value;

        // Check if we need to update the estimates.
        if (std::abs(newDelta) > coreThreshold.load()) [[unlikely]] {
            const auto clearedDelta = delta.exchange(0);
            (*estimates)[i].fetch_add(clearedDelta);
        }
    }

    /**
     * Performs arithmetic subtraction on the counter.
     * This may update the estimate, if it causes the core-local delta to go
     * above the allowed threshold.
     */
    void sub(Integer value, Index index = Index::Default) {
        return add(-value, index);
    }

    /**
     * Reads the current estimate for the element at the given index.
     * NOTE: If the Index type is MonoIndex, this function can be called without
     * parameters.
     * @return the current estimate
     */
    Integer getEstimate(Index index = Index::Default) const {
        return std::max(Integer{},
                        (*estimates)[static_cast<std::size_t>(index)].load());
    }

    /**
     * Updates the estimate for the element at the given index, by clearing all
     * CoreLocal deltas, and returns the new estimate.
     * NOTE: If the Index type is MonoIndex, this function can be called without
     * parameters.
     * @return the current estimate
     */
    Integer getPrecise(Index index = Index::Default) const {
        const auto i = static_cast<std::size_t>(index);
        Integer cleared{};
        for (auto& core : coreDeltas) {
            cleared += clearDelta((*core)[i]);
        }
        return std::max(Integer{}, applyToEstimate(i, cleared));
    }

    /**
     * Returns the sum of the current estimates. The estimates are loaded
     * individually (all from one cache line) so the sum is not a snapshot;
     * the inconsistency is bounded by a single core-local threshold.
     * @return the current estimate
     */
    Integer getEstimateSum() const {
        Integer sum{};
        for (const auto& estimate : *estimates) {
            sum += estimate.load();
        }
        return std::max(Integer{}, sum);
    }

    /**
     * Updates the estimates of all elements, by clearing the core-local deltas,
     * and returns the sum of all estimates after the update. Each core's
     * cache line is visited once for all elements.
     * @return the sum of the updated estimates
     */
    Integer getPreciseSum() const {
        std::array<Integer, Count> cleared{};
        for (auto& core : coreDeltas) {
            auto& deltas = *core;
            for (std::size_t i = 0; i < Count; i++) {
                cleared[i] += clearDelta(deltas[i]);
            }
        }
        Integer sum{};
        for (std::size_t i = 0; i < Count; i++) {
            sum += std::max(Integer{}, applyToEstimate(i, cleared[i]));
        }
        return std::max(Integer{}, sum);
    }

    /**
     * Zeroes everything, except the threshold.
     */
    void reset() {
        for (auto& core : coreDeltas) {
            auto& deltas = *core;
            std::fill(deltas.begin(), deltas.end(), Integer{});
        }
        std::fill(estimates->begin(), estimates->end(), Integer{});
    }

private:
    /**
     * Storage for the required number of counters.
     */
    using Array = std::array<cb::RelaxedAtomic<Integer>, Count>;

    /**
     * Clear a single core-local delta, returning the value cleared. Idle
     * cores (delta of zero) are only read, not written, so their cache line
     * is not taken from them.
     */
    static Integer clearDelta(cb::RelaxedAtomic<Integer>& delta) {
        if (delta.load() == 0) {
            return Integer{};
        }
        return delta.exchange(0);
    }

    /**
     * Add the given cleared delta to the estimate for element i.
     * @return the updated estimate
     */
    Integer applyToEstimate(std::size_t i, Integer cleared) const {
        auto& estimate = (*estimates)[i];
        if (cleared == 0) {
            return estimate.load();
        }
        return estimate.fetch_add(cleared) + cleared;
    }

    /**
     * The core-local deltas of all counters, one cache line per core.
     * Marked mutable so we can getPrecise on a const object while also updating
     * the estimates.
     * Read on every add (the vector header), so kept on a different line from
     * the estimates below.
     */
    mutable CoreStore<CachelineAligned<Array>, CountFn, IndexFn> coreDeltas{};

    /**
     * The threshold for the core-local deltas for all counters.
     * Read on every add, so kept with coreDeltas.
     */
    cb::RelaxedAtomic<Integer> coreThreshold{};

    /**
     * The current estimates of all counters.
     * Mutable for the reason given above.
     * Written when a core flushes its delta and by precise reads; on its own
     * cache line so those writes don't invalidate coreDeltas/coreThreshold in
     * every other core's cache.
     */
    mutable CachelineAligned<Array> estimates{};
};

/**
 * A variant of Unshared with only a single value.
 */
template <typename Integer = std::int64_t>
using MonoUnshared = Unshared<MonoIndex, 1, Integer>;

} // namespace cb
