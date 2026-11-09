/*
 *     Copyright 2026-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once

#include <folly/lang/Aligned.h>

#include <cstddef>

namespace cb {

/**
 * The alignment (and so stride, when used for array elements) which keeps
 * adjacent objects free of destructive interference (false sharing) on every
 * platform we build for.
 *
 * 128 bytes rather than 64: x86 processors prefetch the adjacent cache line
 * (so a 128 byte "sector" behaves like one line for contention purposes) and
 * Apple Silicon has 128 byte lines. On 64 byte line arm64 servers this costs
 * some padding but is never wrong.
 *
 * Note that folly::cacheline_aligned must NOT be used for this purpose: it
 * degrades to alignof(max_align_t) (8 or 16 bytes) on every platform other
 * than 64-bit Linux, and on Linux uses the 64 byte "constructive" size.
 */
inline constexpr std::size_t CachelineAlign = 128;

/**
 * Wrap T so that it is aligned to (and occupies a whole multiple of)
 * CachelineAlign bytes. Use for data which is written by one core and must
 * not share a line with data written by another, e.g. the elements of a
 * CoreStore. Access the wrapped value via operator*, operator-> or get().
 */
template <typename T>
using CachelineAligned = folly::
        aligned<T, (alignof(T) > CachelineAlign ? alignof(T) : CachelineAlign)>;

} // namespace cb
