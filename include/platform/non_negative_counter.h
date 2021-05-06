/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
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
#pragma once

#include <atomic>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <platform/exceptions.h>

namespace cb {

/// Policy class for handling underflow by saturating at max or 0.
template <class T>
struct SaturateOverflowPolicy {
    using SignedT = typename std::make_signed<T>::type;

    void exceed(T& desired, T current, SignedT arg) {
        desired = std::numeric_limits<T>::max();
    }

    void subceed(T& desired, T current, SignedT arg) {
        desired = 0;
    }
};

template <class T>
using ClampAtZeroUnderflowPolicy = SaturateOverflowPolicy<T>;

/// Policy class for handling underflow by throwing an exception. Prints the
/// previous value stored in the counter and the argument (the value that we
// were attempting to subtract)
template <class T>
struct ThrowExceptionOverflowPolicy {
    using SignedT = typename std::make_signed<T>::type;

    void exceed(T& desired, T current, SignedT arg) {
        using std::to_string;
        cb::throwWithTrace(std::overflow_error(
                "ThrowExceptionOverflowPolicy operation cannot exceed " +
                to_string(std::numeric_limits<T>::max()) +
                " current:" + to_string(current) + " arg:" + to_string(arg)));
    }

    void subceed(T& desired, T current, SignedT arg) {
        using std::to_string;
        cb::throwWithTrace(std::underflow_error(
                "ThrowExceptionOverflowPolicy operation cannot subceed 0 "
                "current:" +
                to_string(current) + " arg:" + to_string(arg)));
    }
};

template <class T>
using ThrowExceptionUnderflowPolicy = ThrowExceptionOverflowPolicy<T>;

// Default NonNegativeCounter policy (if user doesn't explicitly
// specify otherwise) - use SaturateOverflowPolicy for Release builds, and
// ThrowExceptionPolicy for Pre-Release builds.
template <class T>
#if CB_DEVELOPMENT_ASSERTS
using DefaultOverflowPolicy = ThrowExceptionOverflowPolicy<T>;
#else
using DefaultOverflowPolicy = SaturateOverflowPolicy<T>;
#endif

/**
 * The NonNegativeCounter class wraps std::atomic<T> and detects
 * when a modification's result cannot be represented by T. The class requires
 * that T is an unsigned type and thus "subtraction" cannot take the value below
 * 0 and "addition" above std::numeric_limits<T>::max(). On detection of a
 * modification where the result cannot be represented, the behaviour compile
 * time configurable. When CB_DEVELOPMENT_ASSERTS is defined, violation of the
 * valid range of the type T will result in an exception. An alternative
 * behaviour is available, and is the default when CB_DEVELOPMENT_ASSERTS is not
 * defined and that behaviour is to 'saturate' the result at either
 * std::numeric_limits<T>::max() or 0 depending on the operation being applied.
 *
 * The definition of overflow/underflow varies, but for the purpose of this
 * class addition that exceeds std::numeric_limits<T>::max() is overflow and
 * subtraction that subceeds 0 is underflow. The throwing policy makes use of
 * std::overflow_eror and std::underflow_error respectively.
 *
 * The type T is unsigned, but the class allows modification with signed types.
 * E.g. Consider the following T=uint8_t operations
 *   0 + -1    -> Subtraction fails, cannot subceed 0
 *   0xff + 1  -> Addition fails, cannot exceed max
 *   0xff + -1 -> Subtraction succeeds and results in 0xfe
 *   0 - 1     -> Subtraction fails, cannot subceed 0
 *   0 - -1    -> Addition succeeds and results in 1
 *   0xff - 1  -> Subtraction succeeds and results in 0xfe
 *   0xff - -1 -> Addition fails, cannot exceed max
 *
 * For the template class configuration 2 parameters are available.
 * @tparam T
 * @tparam OverflowPolicy a class that defines what happens when an operation
 *         attempts to exceed or subceed the limits
 */
template <typename T,
          template <class> class OverflowPolicy = DefaultOverflowPolicy>
class NonNegativeCounter : public OverflowPolicy<T> {
    static_assert(
            std::is_unsigned<T>::value,
            "NonNegativeCounter should only be templated over unsigned types");

    using SignedT = typename std::make_signed<T>::type;

public:
    using value_type = T;

    NonNegativeCounter() = default;

    NonNegativeCounter(T initial) {
        store(initial);
    }

    NonNegativeCounter(const NonNegativeCounter& other) noexcept {
        store(other.load());
    }

    operator T() const noexcept {
        return load();
    }

    [[nodiscard]] T load() const noexcept {
        return value.load(std::memory_order_relaxed);
    }

    void store(T desired) {
        value.store(desired, std::memory_order_relaxed);
    }

    /**
     * Add 'arg' to the current value. If the new value would underflow (i.e. if
     * arg was negative and current less than arg) then calls underflow() on the
     * selected OverflowPolicy.
     *
     * Note: Not marked 'noexcept' as underflow() could throw.
     */
    T fetch_add(SignedT arg) {
        T current = load();
        T desired;
        do {
            if (arg < 0) {
                desired = current - T(std::abs(arg));
                if (SignedT(current) + arg < 0) {
                    OverflowPolicy<T>::subceed(desired, current, arg);
                }
            } else {
                desired = current + T(arg);
                if (desired < T(arg)) {
                    OverflowPolicy<T>::exceed(desired, current, arg);
                }
            }
            // Attempt to set the atomic value to desired. If the atomic value
            // is not the same as current then it has changed during
            // operation. compare_exchange_weak will reload the new value
            // into current if it fails, and we will retry.
        } while (!value.compare_exchange_weak(
                current, desired, std::memory_order_relaxed));

        return current;
    }

    /**
     * Subtract 'arg' from the current value. If the new value would underflow
     * then calls underflow() on the selected OverflowPolicy.
     *
     * Note: Not marked 'noexcept' as underflow() could throw.
     */
    T fetch_sub(SignedT arg) {
        T current = load();
        T desired;
        do {
            if (arg < 0) {
                desired = current + T(std::abs(arg));
                if (desired < T(arg)) {
                    OverflowPolicy<T>::exceed(desired, current, arg);
                }
            } else {
                desired = current - T(arg);
                if (desired < T(arg)) {
                    OverflowPolicy<T>::subceed(desired, current, arg);
                }
            }

            // Attempt to set the atomic value to desired. If the atomic value
            // is not the same as current then it has changed during
            // operation. compare_exchange_weak will reload the new value
            // into current if it fails, and we will retry.
        } while (!value.compare_exchange_weak(
                current, desired, std::memory_order_relaxed));

        return current;
    }

    T exchange(T arg) noexcept {
        return value.exchange(arg, std::memory_order_relaxed);
    }

    NonNegativeCounter& operator=(const NonNegativeCounter& rhs) noexcept {
        value.store(rhs.load(), std::memory_order_relaxed);
        return *this;
    }

    NonNegativeCounter& operator+=(const T rhs) {
        fetch_add(rhs);
        return *this;
    }

    NonNegativeCounter& operator+=(const NonNegativeCounter& rhs) {
        fetch_add(rhs.load());
        return *this;
    }

    NonNegativeCounter& operator-=(const T rhs) {
        fetch_sub(rhs);
        return *this;
    }

    NonNegativeCounter& operator-=(const NonNegativeCounter& rhs) {
        fetch_sub(rhs.load());
        return *this;
    }

    T operator++() {
        return fetch_add(1) + 1;
    }

    T operator++(int) {
        return fetch_add(1);
    }

    T operator--() {
        T previous = fetch_sub(1);
        if (previous == 0) {
            // If we are doing a clamp underflow we can pass in previous,
            // it's already 0 and we are returning 0. If we are going to
            // throw, we want to print previous.
            OverflowPolicy<T>::subceed(previous, previous, 1);
            return 0;
        }
        return previous - 1;
    }

    T operator--(int) {
        return fetch_sub(1);
    }

    NonNegativeCounter& operator=(T val) {
        store(val);
        return *this;
    }

private:
    std::atomic<T> value{0};
};

} // namespace cb
