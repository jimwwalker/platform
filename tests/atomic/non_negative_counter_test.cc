/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
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
#include <platform/non_negative_counter.h>
#include <platform/backtrace.h>
#include <folly/portability/GTest.h>

template <typename T>
class NonNegativeCounterTest : public testing::Test {
public:
};

using MyTypes = ::testing::Types<uint8_t, uint16_t, uint32_t, uint64_t>;
TYPED_TEST_SUITE(NonNegativeCounterTest, MyTypes);

TYPED_TEST(NonNegativeCounterTest, Increment) {
    cb::NonNegativeCounter<TypeParam> nnAtomic(1);
    ASSERT_EQ(1u, nnAtomic);
    EXPECT_EQ(2u, ++nnAtomic);
    EXPECT_EQ(2u, nnAtomic++);
    EXPECT_EQ(3u, nnAtomic);
}

TYPED_TEST(NonNegativeCounterTest, Add) {
    cb::NonNegativeCounter<TypeParam> nnAtomic(1);
    ASSERT_EQ(1u, nnAtomic);

    EXPECT_EQ(3u, nnAtomic += 2);
    EXPECT_EQ(3u, nnAtomic.fetch_add(2));
    EXPECT_EQ(5u, nnAtomic);

    // Adding a negative should subtract from the value
    EXPECT_EQ(5u, nnAtomic.fetch_add(-2));
    EXPECT_EQ(3u, nnAtomic);

    EXPECT_EQ(3u, nnAtomic.fetch_add(-3));
    EXPECT_EQ(0u, nnAtomic);
}

TYPED_TEST(NonNegativeCounterTest, Decrement) {
    cb::NonNegativeCounter<TypeParam> nnAtomic(2);
    ASSERT_EQ(2u, nnAtomic);

    EXPECT_EQ(1u, --nnAtomic);
    EXPECT_EQ(1u, nnAtomic--);
    EXPECT_EQ(0u, nnAtomic);
}

TYPED_TEST(NonNegativeCounterTest, Subtract) {
    cb::NonNegativeCounter<TypeParam> nnAtomic(4);
    ASSERT_EQ(4u, nnAtomic);

    EXPECT_EQ(2u, nnAtomic -= 2);
    EXPECT_EQ(2u, nnAtomic.fetch_sub(2));
    EXPECT_EQ(0u, nnAtomic);

    EXPECT_EQ(2u, nnAtomic -= -2);
    EXPECT_EQ(2u, nnAtomic.fetch_sub(-2));
    EXPECT_EQ(4u, nnAtomic);
}

// Test that a NonNegativeCounter will clamp to zero/max
TYPED_TEST(NonNegativeCounterTest, Saturate) {
    cb::NonNegativeCounter<TypeParam, cb::SaturateOverflowPolicy> nnAtomic(0);

    EXPECT_EQ(0u, --nnAtomic);
    EXPECT_EQ(0u, nnAtomic--);
    EXPECT_EQ(0u, nnAtomic);

    nnAtomic = 5;
    EXPECT_EQ(5u, nnAtomic.fetch_sub(10)); // returns previous value
    EXPECT_EQ(0u, nnAtomic); // has been clamped to zero

    nnAtomic = 5;
    EXPECT_EQ(5u, nnAtomic.fetch_add(-10)); // return previous value
    EXPECT_EQ(0u, nnAtomic); // has been clamped to zero

    // Now exceed max T
    nnAtomic = std::numeric_limits<TypeParam>::max();
    nnAtomic.fetch_add(1);
    EXPECT_EQ(std::numeric_limits<TypeParam>::max(), nnAtomic);
    nnAtomic++;
    EXPECT_EQ(std::numeric_limits<TypeParam>::max(), nnAtomic);
}

// Test the ThrowException policy.
TYPED_TEST(NonNegativeCounterTest, ThrowExceptionPolicy) {
    cb::NonNegativeCounter<TypeParam, cb::ThrowExceptionOverflowPolicy>
            nnAtomic(0);

    EXPECT_THROW(--nnAtomic, std::underflow_error);
    EXPECT_EQ(0u, nnAtomic);
    EXPECT_THROW(nnAtomic--, std::underflow_error);
    EXPECT_EQ(0u, nnAtomic);

    EXPECT_THROW(nnAtomic.fetch_add(-1), std::underflow_error);
    EXPECT_EQ(0u, nnAtomic);

    EXPECT_THROW(nnAtomic += -1, std::underflow_error);
    EXPECT_EQ(0u, nnAtomic);

    EXPECT_THROW(nnAtomic -= 2, std::underflow_error);
    EXPECT_EQ(0u, nnAtomic);

    // Now exceed max T
    nnAtomic = std::numeric_limits<TypeParam>::max();
    EXPECT_THROW(nnAtomic.fetch_add(1), std::overflow_error)
            << size_t(nnAtomic);
    EXPECT_THROW(nnAtomic.fetch_sub(-1), std::overflow_error)
            << size_t(nnAtomic);
    EXPECT_THROW(nnAtomic++, std::overflow_error) << size_t(nnAtomic);
}

// Test that ThrowException policy throws an exception which records where
// the exception was thrown from.
TYPED_TEST(NonNegativeCounterTest, ThrowExceptionPolicyBacktrace) {
    cb::NonNegativeCounter<TypeParam, cb::ThrowExceptionOverflowPolicy>
            nnAtomic(0);
    try {
        cb::backtrace::initialize();
    } catch (const std::exception& exception) {
        FAIL() << "Failed to initialize backtrace: " << exception.what();
    }

    try {
        --nnAtomic;
    } catch (const std::underflow_error& e) {
        const auto* st = cb::getBacktrace(e);
        ASSERT_TRUE(st);
        // MB-44173: print_backtrace doesn't symbolify for Windows.

        // Hard to accurately predict what we'll see in the backtrace;
        // just check it contains the executable name somewhere
        std::string backtrace;
        print_backtrace_frames(*st, [&backtrace](const char* frame) {
                backtrace += frame;
                backtrace += '\n';
            });
        EXPECT_TRUE(backtrace.find("platform-non_negative_counter-test")
                    != std::string::npos)
            << "when verifying exception backtrace: " << backtrace;
    }
}
