/*
 *     Copyright 2024-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "platform/sysinfo.h"
#include <folly/portability/GTest.h>

#include <platform/cacheline_aligned.h>
#include <platform/unshared.h>

#include <cstdint>
#include <iterator>
#include <thread>
#include <vector>

TEST(Unshared, Init) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);

    EXPECT_EQ(0, counter.getEstimate());
    EXPECT_EQ(0, counter.getEstimateSum());
    EXPECT_EQ(0, counter.getPrecise());
    EXPECT_EQ(0, counter.getPreciseSum());
    EXPECT_EQ(1, counter.getCoreThreshold());
    EXPECT_EQ(1 * cb::get_cpu_count(), counter.getMaximumDrift());
}

TEST(Unshared, Reset) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);
    counter.add(10);
    counter.reset();

    EXPECT_EQ(0, counter.getEstimate());
    EXPECT_EQ(0, counter.getEstimateSum());
    EXPECT_EQ(0, counter.getPrecise());
    EXPECT_EQ(0, counter.getPreciseSum());
    EXPECT_EQ(1, counter.getCoreThreshold());
}

// Althoguh 0 threshold doesn't make sense, Unshared supports this as it can
// happen it test.
TEST(Unshared, ZeroThreshold) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(0);
    counter.add(10);
    counter.reset();
}

TEST(Unshared, Add) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);
    counter.add(10);

    EXPECT_EQ(10, counter.getEstimate());
    EXPECT_EQ(10, counter.getPrecise());
    EXPECT_EQ(10, counter.getEstimateSum());
    EXPECT_EQ(10, counter.getPreciseSum());
}

TEST(Unshared, AddNegative) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);
    counter.add(-10);

    EXPECT_EQ(0, counter.getEstimate());
    EXPECT_EQ(0, counter.getPrecise());
    EXPECT_EQ(0, counter.getEstimateSum());
    EXPECT_EQ(0, counter.getPreciseSum());
}

// Pushing the counters into the negative has no observable effect, as the
// values are capped to 0.
TEST(Unshared, Sub) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);
    counter.sub(10);

    EXPECT_EQ(0, counter.getEstimate());
    EXPECT_EQ(0, counter.getPrecise());
    EXPECT_EQ(0, counter.getEstimateSum());
    EXPECT_EQ(0, counter.getPreciseSum());
}

TEST(Unshared, SubNegative) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);
    // Subtracting negative increases the total.
    counter.sub(-10);

    EXPECT_EQ(10, counter.getEstimate());
    EXPECT_EQ(10, counter.getPrecise());
    EXPECT_EQ(10, counter.getEstimateSum());
    EXPECT_EQ(10, counter.getPreciseSum());
}

// Check that the thresholds are respected.
TEST(Unshared, Thresholds) {
    cb::MonoUnshared<> counter;

    counter.setCoreThreshold(1);
    counter.add(5);
    // Set threshold to 5 and add 5. We shouldn't update the counter until we've
    // drifted by 6 or more.
    counter.setCoreThreshold(5);
    counter.add(5);

    EXPECT_EQ(5, counter.getEstimateSum());
    EXPECT_EQ(10, counter.getPreciseSum());
}

enum class TestIndex {
    First = 0,
    Second = 1,
    Count = 2,
};

// Check that we can use multiple counters and their sum is correct.
TEST(Unshared, Multiple) {
    cb::Unshared<TestIndex> counter;
    counter.setCoreThreshold(10);
    counter.add(5, TestIndex::First);
    counter.add(15, TestIndex::Second);

    EXPECT_EQ(0, counter.getEstimate(TestIndex::First));
    EXPECT_EQ(15, counter.getEstimate(TestIndex::Second));

    EXPECT_EQ(15, counter.getEstimateSum());
    EXPECT_EQ(20, counter.getPreciseSum());

    EXPECT_EQ(5, counter.getEstimate(TestIndex::First));
    EXPECT_EQ(15, counter.getEstimate(TestIndex::Second));

    EXPECT_EQ(5, counter.getPrecise(TestIndex::First));
    EXPECT_EQ(15, counter.getPrecise(TestIndex::Second));
}

// The core-local deltas must each occupy their own cache line, and the
// estimates must not share a line with the data read on every add (the
// object is cache line aligned and at least two lines long).
TEST(Unshared, Layout) {
    using Counter = cb::Unshared<TestIndex>;
    static_assert(alignof(Counter) == cb::CachelineAlign);
    static_assert(sizeof(Counter) >= 2 * cb::CachelineAlign);
    static_assert(alignof(cb::CachelineAligned<int>) == cb::CachelineAlign);
    static_assert(sizeof(cb::CachelineAligned<int>) == cb::CachelineAlign);

    // Elements of a CoreStore of CachelineAligned values are a whole cache
    // line apart, including when allocated on the heap by std::vector.
    CoreStore<cb::CachelineAligned<int>> store;
    ASSERT_GE(store.size(), 1u);
    const auto first = reinterpret_cast<uintptr_t>(&*store.begin());
    EXPECT_EQ(0u, first % cb::CachelineAlign);
    if (store.size() > 1) {
        const auto second =
                reinterpret_cast<uintptr_t>(&*std::next(store.begin()));
        EXPECT_EQ(cb::CachelineAlign, second - first);
    }
}

// Negative counters are clamped individually when summing the precise values
// (as they are when read individually); the estimate sum clamps the total.
TEST(Unshared, PreciseSumClampsEachCounter) {
    cb::Unshared<TestIndex> counter;
    counter.setCoreThreshold(1);
    counter.sub(5, TestIndex::First);
    counter.add(15, TestIndex::Second);

    EXPECT_EQ(0, counter.getEstimate(TestIndex::First));
    EXPECT_EQ(15, counter.getEstimate(TestIndex::Second));
    EXPECT_EQ(10, counter.getEstimateSum());

    EXPECT_EQ(0, counter.getPrecise(TestIndex::First));
    EXPECT_EQ(15, counter.getPrecise(TestIndex::Second));
    EXPECT_EQ(15, counter.getPreciseSum());
}

// Many threads adding and subtracting concurrently (with precise reads mixed
// in); every update must be accounted for once the threads have finished.
TEST(Unshared, Concurrent) {
    cb::Unshared<TestIndex> counter;
    counter.setCoreThreshold(100);
    constexpr int64_t threads = 8;
    constexpr int64_t iterations = 20000;

    std::vector<std::thread> workers;
    for (int64_t tt = 0; tt < threads; tt++) {
        workers.emplace_back([&counter]() {
            for (int64_t ii = 0; ii < iterations; ii++) {
                counter.add(3, TestIndex::First);
                counter.add(7, TestIndex::Second);
                counter.sub(1, TestIndex::First);
                if (ii % 1000 == 0) {
                    counter.getPreciseSum();
                    counter.getPrecise(TestIndex::Second);
                    counter.getEstimateSum();
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(threads * iterations * 2, counter.getPrecise(TestIndex::First));
    EXPECT_EQ(threads * iterations * 7, counter.getPrecise(TestIndex::Second));
    EXPECT_EQ(threads * iterations * 9, counter.getPreciseSum());
    // After a precise read the estimate is exact.
    EXPECT_EQ(threads * iterations * 9, counter.getEstimateSum());
}
