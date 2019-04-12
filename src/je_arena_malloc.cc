/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc
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

#include <platform/je_arena_malloc.h>

#include "relaxed_atomic.h"

#include <jemalloc/jemalloc.h>

#include <stdexcept>
#include <string>
#include <system_error>

namespace cb {

/**
 * Two thread locals are used so that we can allocate into a specific arena
 * and a client associated thread cache.
 *
 * currentJeMallocFlags stores the current flags to use in all subsequent
 * allocx calls, and is computed during switchToClient
 *
 * tcacheArray points to a vector of thread caches, where the current index
 * is used to find the thread cache to use in the allocation/deallocation.
 *
 * Note these are not in the JEArenaMalloc 'class' as MSVC doesn't allow
 * thread_local in the 'dll' api
 *
 */
static thread_local int currentJeMallocFlags = 0;

struct TCacheDestroy {
    void operator()(TCacheArray* ptr) {
        if (ptr) {
            for (auto tc : *ptr) {
                if (tc) {
                    unsigned tcache = tc;
                    size_t sz = sizeof(unsigned);
                    if (je_mallctl("tcache.destroy",
                                   nullptr,
                                   0,
                                   (void*)&tcache,
                                   sz) != 0) {
                        throw std::logic_error(
                                "JEArenaMalloc::TCacheDestroy: Could not "
                                "destroy "
                                "tcache");
                    }
                }
            }
        }
        currentJeMallocFlags = 0;
        // de-allocate the array from the default arena/t-cache
        je_dallocx((void*)ptr, 0);
    }
};

static thread_local std::unique_ptr<TCacheArray, TCacheDestroy> tcacheArray =
        nullptr;

ArenaMallocClient JEArenaMalloc::registerClient(bool threadCache) {
    auto lockedClients = clients.wlock();
    for (uint8_t index = 0; index < lockedClients->size(); index++) {
        auto& client = lockedClients->at(index);
        if (!client.used) {
            if (client.arena == 0) {
                client.arena = makeArena();
            }

            // We use arena 0 as no arena and don't expect it to be created
            if (client.arena == 0) {
                throw std::runtime_error(
                        "JEArenaMalloc::registerClient we cannot register "
                        "arena 0");
            }
            client.used = true;
            return {index, threadCache && tcacheEnabled, client.arena};
        }
    }
    throw std::runtime_error(
            "JEArenaMalloc::registerClient no available indices");
}

void JEArenaMalloc::registerTotalCounter(
        const ArenaMallocClient& client,
        cb::RelaxedAtomic<int64_t>* counterAddress) {
    clients.wlock()->at(client.index).total = counterAddress;
}

void JEArenaMalloc::unregisterClient(const ArenaMallocClient& client) {
    auto lockedClients = clients.wlock();
    auto& c = lockedClients->at(client.index);
    if (!c.used) {
        throw std::invalid_argument(
                "JEArenaMalloc::unregisterClient client is not in-use "
                "client.index:" +
                std::to_string(client.index));
    }
    // Reset the state, but we re-use the arena
    c = {false, c.arena, nullptr};
}

void JEArenaMalloc::unregisterCurrentClient() {
    auto flags = getFlags();
    // Extract the arena and lookup the client using that arena
    int arena = (flags >> 20) - 1;
    auto lockedClients = clients.wlock();
    for (uint8_t index = 0; index < lockedClients->size(); index++) {
        auto& client = lockedClients->at(index);
        if (client.arena == arena) {
            client = {false, client.arena, nullptr};
            return;
        }
    }
    throw std::runtime_error(
            "JEArenaMalloc::unregisterCurrentClient failed to find arena:" +
            std::to_string(arena));
}

void JEArenaMalloc::switchToClient(const ArenaMallocClient& client) {
    // Compute flags and store in the TLS
    if (client.arena == 0) {
        // client can change tcache setting, but tcacheEnabled overrides
        currentJeMallocFlags =
                client.threadCache && tcacheEnabled ? 0 : MALLOCX_TCACHE_NONE;
        return;
    }

    int tcacheFlags = MALLOCX_TCACHE_NONE;
    // client can change tcache setting, but tcacheEnabled overrides
    if (client.threadCache && tcacheEnabled) {
        // Lookup this threads table of tcache indexes
        auto* arrayPtr = tcacheArray.get();
        if (!arrayPtr) {
            arrayPtr = makeTCacheArray();
            tcacheArray.reset(arrayPtr);
        }

        int tcache = (*arrayPtr)[client.index];

        // Does this index have a tcache?
        if (tcache == 0) {
            tcache = makeTCache();
            (*arrayPtr)[client.index] = tcache;
        }
        tcacheFlags = MALLOCX_TCACHE(tcache);
    }
    currentJeMallocFlags = MALLOCX_ARENA(client.arena) | tcacheFlags;
}

void JEArenaMalloc::switchFromClient() {
    // Set to 0, no client, all tracking/allocations go to default arena/tcache
    switchToClient({0, tcacheEnabled, 0});
}

size_t JEArenaMalloc::getAllocated(const ArenaMallocClient& client) {
    return getAllocated(client.arena);
}

void JEArenaMalloc::updateTotalCounters() {
    auto lockedClients = clients.wlock();
    for (auto& client : *lockedClients) {
        if (client.used && client.arena && client.total) {
            client.total->store(getAllocated(client.arena));
        }
    }
}

void* JEArenaMalloc::malloc(size_t size) {
    if (size == 0) {
        size = 8;
    }
    return je_mallocx(size, getFlags());
}

void* JEArenaMalloc::calloc(size_t nmemb, size_t size) {
    return je_mallocx(nmemb * size, getFlags() | MALLOCX_ZERO);
}
void* JEArenaMalloc::realloc(void* ptr, size_t size) {
    if (size == 0) {
        size = 8;
    }
    if (!ptr) {
        return je_mallocx(size, getFlags());
    }
    return je_rallocx(ptr, size, getFlags());
}

void JEArenaMalloc::free(void* ptr) {
    if (ptr) {
        je_dallocx(ptr, getFlags());
    }
}

void JEArenaMalloc::sized_free(void* ptr, size_t size) {
    if (ptr) {
        je_sdallocx(ptr, size, getFlags());
    }
}

size_t JEArenaMalloc::malloc_usable_size(void* ptr) {
    return je_malloc_usable_size(ptr);
}

void JEArenaMalloc::setTCacheEnabled(bool value) {
    tcacheEnabled = value;
}

int JEArenaMalloc::getFlags() {
    return currentJeMallocFlags;
}

int JEArenaMalloc::makeArena() {
    unsigned arena = 0;
    size_t sz = sizeof(unsigned);
    int rv = je_mallctl("arenas.create", (void*)&arena, &sz, nullptr, 0);

    if (rv != 0) {
        throw std::runtime_error(
                "JEArenaMalloc::makeArena not create arena. rv:" +
                std::to_string(rv));
    }
    return arena;
}

int JEArenaMalloc::makeTCache() {
    unsigned tcache = 0;
    size_t sz = sizeof(unsigned);

    int rv = je_mallctl("tcache.create", (void*)&tcache, &sz, nullptr, 0);
    if (rv != 0) {
        throw std::runtime_error(
                "JEArenaMalloc::makeTCache: Could not create tcache. rv:" +
                std::to_string(rv));
    }
    return tcache;
}

TCacheArray* JEArenaMalloc::makeTCacheArray() {
    auto size = sizeof(TCacheArray);
    // Always create the tcache vector in the default arena/cache
    auto* vptr = (TCacheArray*)je_mallocx(size, MALLOCX_ZERO);
    // in-place new (n)
    new (vptr) TCacheArray();
    return vptr;
}

size_t JEArenaMalloc::getAllocated(int arena) {
    size_t epoch = 1;
    size_t sz = sizeof(epoch);
    if (auto rv = je_mallctl("epoch", &epoch, &sz, &epoch, sz)) {
        throw std::runtime_error(
                "JEArenaMalloc::getAllocated: je_mallctl epoch error rv:" +
                std::to_string(rv));
    }

    size_t allocSmall = 0;
    size_t allocLarge = 0;

    mib_small[2] = arena;
    mib_large[2] = arena;

    auto size = sizeof(size_t);
    auto rv1 = je_mallctlbymib(
            mib_small, miblen_small, &allocSmall, &size, nullptr, 0);
    auto rv2 = je_mallctlbymib(
            mib_large, miblen_large, &allocLarge, &size, nullptr, 0);

    if (rv1 || rv2) {
        throw std::runtime_error(
                "JEArenaMalloc::getAllocated: je_mallctlbymib error rv1:" +
                std::to_string(rv1) + ", rv2:" + std::to_string(rv2));
    }
    return allocSmall + allocLarge;
}

bool JEArenaMalloc::tcacheEnabled{true};
folly::Synchronized<std::array<JEArenaMalloc::Client, ArenaMallocMaxClients>>
        JEArenaMalloc::clients;

size_t JEArenaMalloc::mib_small[5]{};
size_t JEArenaMalloc::miblen_small = []() {
    size_t mibLen = 5;
    auto rv = je_mallctlnametomib(
            "stats.arenas.0.small.allocated", mib_small, &mibLen);
    if (rv != 0) {
        throw std::runtime_error(
                "JEArenaMalloc. Cannot make mib "
                "stats.arenas.0.small.allocated. rv:" +
                std::to_string(rv));
    }
    return mibLen;
}();

size_t JEArenaMalloc::mib_large[5]{};
size_t JEArenaMalloc::miblen_large = []() {
    size_t mibLen = 5;
    auto rv = je_mallctlnametomib(
            "stats.arenas.0.large.allocated", mib_large, &mibLen);
    if (rv != 0) {
        throw std::runtime_error(
                "JEArenaMalloc. Cannot make mib "
                "stats.arenas.0.large.allocated. rv:" +
                std::to_string(rv));
    }
    return mibLen;
}();

} // namespace cb
