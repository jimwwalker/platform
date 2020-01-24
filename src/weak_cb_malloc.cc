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

#include <platform/visibility.h>

#include <cstdlib>
#include <cstring>

extern "C" {
#if !(defined MSVC)
PLATFORM_PUBLIC_API
void* __attribute__((weak)) cb_malloc(size_t);
PLATFORM_PUBLIC_API
void* __attribute__((weak)) cb_calloc(size_t, size_t);
PLATFORM_PUBLIC_API
void* __attribute__((weak)) cb_realloc(void*, size_t);
PLATFORM_PUBLIC_API
void __attribute__((weak)) cb_free(void*);
PLATFORM_PUBLIC_API
void __attribute__((weak)) cb_sized_free(void*, size_t);
#if defined(HAVE_MALLOC_USABLE_SIZE)
PLATFORM_PUBLIC_API
size_t __attribute__((weak)) cb_malloc_usable_size(void*);
#endif
PLATFORM_PUBLIC_API
char* __attribute__((weak)) cb_strdup(const char*);

#define CB_MALLOC cb_malloc
#define CB_CALLOC cb_calloc
#define CB_REALLOC cb_realloc
#define CB_FREE cb_free
#define CB_SIZED_FREE cb_sized_free
#define CB_MALLOC_USABLE_SIZE cb_malloc_usable_size
#define CB_STRDUP cb_strdup

#else

// Windows 'weak' symbol using /altername
/*
PLATFORM_PUBLIC_API
void* cb_malloc(size_t);
PLATFORM_PUBLIC_API
void* cb_calloc(size_t, size_t);
PLATFORM_PUBLIC_API
void* cb_realloc(void*, size_t);
PLATFORM_PUBLIC_API
void cb_free(void*);
PLATFORM_PUBLIC_API
void cb_sized_free(void*, size_t);
#if defined(HAVE_MALLOC_USABLE_SIZE)
PLATFORM_PUBLIC_API
size_t cb_malloc_usable_size(void*);
#endif
PLATFORM_PUBLIC_API
char* cb_strdup(const char*);
*/

PLATFORM_PUBLIC_API
void* cb_weak_malloc(size_t);
PLATFORM_PUBLIC_API
void* cb_weak_calloc(size_t, size_t);
PLATFORM_PUBLIC_API
void* cb_weak_realloc(void*, size_t);
PLATFORM_PUBLIC_API
void cb_weak_free(void*);
PLATFORM_PUBLIC_API
void cb_weak_sized_free(void*, size_t);
#if defined(HAVE_MALLOC_USABLE_SIZE)
PLATFORM_PUBLIC_API
size_t cb_weak_malloc_usable_size(void*);
#endif
PLATFORM_PUBLIC_API
char* cb_weak_strdup(const char*);
#pragma comment(linker, "/alternatename:cb_malloc=cb_weak_malloc")
#pragma comment(linker, "/alternatename:cb_calloc=cb_weak_calloc")
#pragma comment(linker, "/alternatename:cb_realloc=cb_weak_realloc")
#pragma comment(linker, "/alternatename:cb_free=cb_weak_free")
#pragma comment(linker, "/alternatename:cb_sized_free=cb_weak_sized_free")
#pragma comment(linker, "/alternatename:cb_malloc_usable_size=cb_weak_malloc_usable_size")


#define CB_MALLOC cb_weak_malloc
#define CB_CALLOC cb_weak_calloc
#define CB_REALLOC cb_weak_realloc
#define CB_FREE cb_weak_free
#define CB_SIZED_FREE cb_weak_sized_free
#define CB_MALLOC_USABLE_SIZE cb_weak_malloc_usable_size
#define CB_STRDUP cb_weak_strdup
#endif

void* CB_MALLOC(size_t size) {
    return malloc(size);
}

void* CB_CALLOC(size_t count, size_t size) {
    return calloc(count, size);
}

void* CB_REALLOC(void* p, size_t size) {
    return realloc(p, size);
}

void CB_FREE(void* p) {
    free(p);
}

void CB_SIZED_FREE(void* p, size_t) {
    free(p);
}

char* CB_STRDUP(const char* c) {
    return strdup(c);
}

#if defined(HAVE_MALLOC_USABLE_SIZE)
size_t CB_MALLOC_USABLE_SIZE(void* ptr) {
    return malloc_usable_size(ptr);
}
#endif

}