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

#include <platform/cb_malloc.h>

#include <cstring>

// Which underlying memory allocator should we use?
#if defined(HAVE_JEMALLOC)
#  define MALLOC_PREFIX je_
/* Irrespective of how jemalloc was configured on this platform,
 * don't rename je_FOO to FOO.
 */
#  define JEMALLOC_NO_RENAME
#  include <jemalloc/jemalloc.h>

#else /* system allocator */
#  define MALLOC_PREFIX
#if defined(HAVE_MALLOC_USABLE_SIZE)
#include <malloc.h>
#endif
#endif

#include <pthread.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

static __thread uint64_t tls_index_arena;
static __thread std::vector<uint16_t>* tls_tcache_vector;;

// User-registered new and delete hooks
cb_malloc_new_hook_t cb_new_hook = nullptr;
cb_malloc_delete_hook_t cb_delete_hook = nullptr;

PLATFORM_PUBLIC_API void cb_malloc_set_engine_index_and_arena(int index,
                                                              int arena) {
    if (arena == 0) {
        tls_index_arena = 0;
    }

    auto* vptr = tls_tcache_vector;

    if (!vptr) {
        tls_index_arena = 0;
        auto size = sizeof(std::vector<uint16_t>);
        vptr = (std::vector<uint16_t>*)je_malloc(size);
        new (vptr) std::vector<uint16_t>(100); // 100 engines
        tls_tcache_vector = vptr;
    }

    int tcache = (*vptr)[index];

    // First call, no tcache
    if (tcache == 0) {
        unsigned tc = 0;
        size_t sz = sizeof(unsigned);
        if (je_mallctl("tcache.create", (void*)&tc, &sz, nullptr, 0) != 0) {
            tls_index_arena = MALLOCX_ARENA(arena);
        }
        (*vptr)[index] = tc;
        tcache = tc;
    }

    tls_index_arena = MALLOCX_ARENA(arena) | MALLOCX_TCACHE(tcache);
}

// Macros to form the name of the memory allocation function to use based on
// the compile-time selected malloc library's prefix ('je_', 'tc_', '')
#define CONCAT(A, B) A##B
#define CONCAT2(A, B) CONCAT(A, B)
#define MEM_ALLOC(name) CONCAT2(MALLOC_PREFIX, name)

static int get_je_flags() {
    return tls_index_arena;
}

PLATFORM_PUBLIC_API void* cb_malloc(size_t size) throw() {
    if (size == 0) {
        return je_malloc(0);
    }
    void* ptr = je_mallocx(size, get_je_flags());
    return ptr;
}

PLATFORM_PUBLIC_API void* cb_calloc(size_t nmemb, size_t size) throw() {
    void* ptr = je_mallocx(nmemb * size, get_je_flags() | MALLOCX_ZERO);
    return ptr;
}

PLATFORM_PUBLIC_API void* cb_realloc(void* ptr, size_t size) throw() {
    if (size == 0) {
        return je_realloc(ptr, size);
    }
    if (ptr == nullptr) {
        return je_mallocx(size, get_je_flags());
    }
    void* result = je_rallocx(ptr, size, get_je_flags());
    return result;
}

PLATFORM_PUBLIC_API void cb_free(void* ptr) throw() {
    if (ptr) {
        je_dallocx(ptr, get_je_flags());
    } else {
        je_free(ptr);
    }

}

PLATFORM_PUBLIC_API void cb_sized_free(void* ptr, size_t size) throw() {
#if defined(HAVE_JEMALLOC_SDALLOCX)
    if (ptr != nullptr) {
        MEM_ALLOC(sdallocx)(ptr, size, get_je_flags());
    }
#else
    MEM_ALLOC(free)(ptr);
#endif
}

PLATFORM_PUBLIC_API char* cb_strdup(const char* s1) {
    size_t len = std::strlen(s1);
    char* result = static_cast<char*>(cb_malloc(len + 1));
    if (result != nullptr) {
        std::strncpy(result, s1, len + 1);
    }
    return result;
}

#if defined(HAVE_MALLOC_USABLE_SIZE)
PLATFORM_PUBLIC_API size_t cb_malloc_usable_size(void* ptr) throw() {
    return MEM_ALLOC(malloc_usable_size)(ptr);
}
#endif

/*
 * Allocation / deallocation hook functions.
 */
bool cb_add_new_hook(cb_malloc_new_hook_t f) {
    if (cb_new_hook == nullptr) {
        cb_new_hook = f;
        return true;
    } else {
        return false;
    }
}

bool cb_remove_new_hook(cb_malloc_new_hook_t f) {
    if (cb_new_hook == f) {
        cb_new_hook = nullptr;
        return true;
    } else {
        return false;
    }
}

bool cb_add_delete_hook(cb_malloc_delete_hook_t f) {
    if (cb_delete_hook == nullptr) {
        cb_delete_hook = f;
        return true;
    } else {
        return false;
    }
}

bool cb_remove_delete_hook(cb_malloc_delete_hook_t f) {
    if (cb_delete_hook == f) {
        cb_delete_hook = nullptr;
        return true;
    } else {
        return false;
    }
}

void cb_invoke_new_hook(const void* ptr, size_t size) {
    if (cb_new_hook != nullptr) {
        cb_new_hook(ptr, size);
    }
}

void cb_invoke_delete_hook(const void* ptr) {
    if (cb_delete_hook != nullptr) {
        cb_delete_hook(ptr);
    }
}
