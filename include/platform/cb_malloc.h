/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
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

/** This file defines the Couchbase memory allocation API.
 *
 * It doesn't implement allocation itself - the actual memory allocation will
 * be performed by an exising 'proper' memory allocator. It exists for
 * two reasons:
 *
 * 1. To give us a single 'interposing' point to use an alternative
 *    allocator (e.g. jemalloc) instead of the system one.
 *    (On most *ix platforms it's relatively easy to interpose malloc and
 *    friends; you can simply define your own symbols in the application binary
 *    and those will be used; however this isn't possible on Windows so we
 *    need our own explicit API for the C memory allocation functions).
 *
 * 2. To allow us to insert hooks for memory tracking - e.g. so we can track
 *    how much memory each bucket (aka engine instance) are using.
 */

#pragma once

#include <platform/dynamic.h>
#include <platform/visibility.h>

#include <stdlib.h>

#if !defined(__cplusplus)
#define throwspec
#else
#define throwspec throw()
#endif

/*
 * Couchbase memory allocation API functions.
 *
 * Equivalent to the libc functions with the same suffix.
 */

#ifdef __cplusplus
extern "C" {
#endif

PLATFORM_PUBLIC_API void* cb_malloc(size_t size) throwspec;
PLATFORM_PUBLIC_API void* cb_calloc(size_t nmemb, size_t size) throwspec;
PLATFORM_PUBLIC_API void* cb_realloc(void* ptr, size_t size) throwspec;
PLATFORM_PUBLIC_API void cb_free(void* ptr) throwspec;
PLATFORM_PUBLIC_API void cb_sized_free(void* ptr, size_t size) throwspec;

#if defined(HAVE_MALLOC_USABLE_SIZE)
PLATFORM_PUBLIC_API size_t cb_malloc_usable_size(void* ptr) throwspec;
#endif

#undef throwspec

/*
 * Replacements for other libc functions which allocate memory via 'malloc'.
 *
 * For our 'cb' versions we use cb_malloc instead.
 */

PLATFORM_PUBLIC_API char* cb_strdup(const char* s1);

#ifdef __cplusplus
} // extern "C"
#endif
