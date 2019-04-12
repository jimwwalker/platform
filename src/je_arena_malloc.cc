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

#include <folly/CachelinePadded.h>
#include <jemalloc/jemalloc.h>

#include <stdexcept>
#include <string>
#include <system_error>

// We are storing the arena in a uint16_t, assert that this constant is as
// expected, MALLCTL_ARENAS_ALL -1 is the largest possible arena ID
static_assert(MALLCTL_ARENAS_ALL == 4096,
              "je_malloc MALLCTL_ARENAS_ALL is not the expected 4096");

namespace cb {

struct ThreadData {
    /// The tcache ID to use for the thread/client
    uint16_t tcache;
};

using ClientDataArray = std::array<ThreadData, ArenaMallocMaxClients>;

/**
 * ThreadLocalData (TLD) defines the per thread information.
 * When a client switches to their ArenaMallocClient, the current client is
 * recorded in the TLD and that client information can then be used in
 * subsequent allocation activity that is redirected to the ArenaMalloc API.
 *
 * At the point of switching the je_malloc flags are also computed and stored
 * in TSD, these flags are then used by all allocation calls and encode the
 * arena and thread-cache to use (if thread-cached is enabled).
 *
 * A pointer to the thread's TLD is what is actually stored in thread_local
 * storage.
 */
struct ThreadLocalData {
    //// switchToClient will write to this struct the current client
    // ArenaMallocClient client;
    /// The flags to use for je_malloc 'x' calls
    // int jeMallocFlags;
    /// Per client data for the thread. Stores the tcache IDs
    ClientDataArray clientDataArray;
};

struct ThreadLocalDataDestroy {
    void operator()(ThreadLocalData* tld) {
        if (tld) {
            for (auto& td : tld->clientDataArray) {
                if (td.tcache) {
                    unsigned tcache = td.tcache;
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
        // de-allocate the ThreadLocalData from the default arena
        je_dallocx((void*)tld, 0);
    }
};

/// Thread local unique_ptr to the ThreadLocalData
static thread_local std::unique_ptr<ThreadLocalData, ThreadLocalDataDestroy>
        threadLocalData;

struct CurrentClient {
    int mallocFlags{0};
    uint8_t index{NoClientIndex};
};

static thread_local CurrentClient currentClient;

ThreadLocalData* makeThreadLocalData() {
    // Always create the thread local data in the default arena/cache
    auto* vptr =
            (ThreadLocalData*)je_mallocx(sizeof(ThreadLocalData), MALLOCX_ZERO);
    return new (vptr) ThreadLocalData();
}

ThreadLocalData& getThreadLocalData() {
    auto* arrayPtr = threadLocalData.get();
    if (!arrayPtr) {
        arrayPtr = makeThreadLocalData();
        threadLocalData.reset(arrayPtr);
    }
    return *arrayPtr;
}

/// Overrides any client tcache wishes
static bool tcacheEnabled{true};

struct Client {
    void reset(int arena) {
        used = false;
        this->arena = arena;
    }
    int arena = 0;
    bool used = false;
};

static folly::Synchronized<std::array<Client, ArenaMallocMaxClients>> clients;

template <>
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

template <>
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

template <>
PLATFORM_PUBLIC_API ArenaMallocClient
JEArenaMalloc::registerClient(bool threadCache) {
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
                        "JEArenaMalloc::registerClient did not expect to have "
                        "arena 0");
            }
            client.used = true;
            ArenaMallocClient newClient{
                    client.arena, index, threadCache && tcacheEnabled};
            clientRegistered(newClient);
            return newClient;
        }
    }
    throw std::runtime_error(
            "JEArenaMalloc::registerClient no available indices");
}

template <>
PLATFORM_PUBLIC_API void JEArenaMalloc::unregisterClient(
        const ArenaMallocClient& client) {
    auto lockedClients = clients.wlock();
    auto& c = lockedClients->at(client.index);
    if (!c.used) {
        throw std::invalid_argument(
                "JEArenaMalloc::unregisterClient client is not in-use "
                "client.index:" +
                std::to_string(client.index));
    }
    // Reset the state, but we re-use the arena
    c.reset(c.arena);
}

template <>
PLATFORM_PUBLIC_API void JEArenaMalloc::unregisterCurrentClient() {
    auto c = currentClient;
    if (c.index == NoClientIndex) {
        throw std::runtime_error(
                "JEArenaMalloc::unregisterCurrentClient index is not set");
    }

    auto lockedClients = clients.wlock();
    auto& clientSlot = lockedClients->at(c.index);
    if (!clientSlot.used) {
        throw std::runtime_error(
                "JEArenaMalloc::unregisterCurrentClient client is not in use "
                "index:" +
                std::to_string(c.index));
    }
    clientSlot.reset(clientSlot.arena);
}

template <>
PLATFORM_PUBLIC_API void JEArenaMalloc::switchToClient(
        const ArenaMallocClient& client) {
    if (client.index == NoClientIndex) {
        currentClient.mallocFlags =
                client.threadCache && tcacheEnabled ? 0 : MALLOCX_TCACHE_NONE;
        currentClient.index = NoClientIndex;
        return;
    }

    int tcacheFlags = MALLOCX_TCACHE_NONE;
    // client can change tcache setting, but tcacheEnabled overrides
    if (client.threadCache && tcacheEnabled) {
        auto& tld = getThreadLocalData();
        int tcache = tld.clientDataArray[client.index].tcache;
        // Does this index have a tcache?
        if (tcache == 0) {
            tcache = makeTCache();
            tld.clientDataArray[client.index].tcache = tcache;
        }
        tcacheFlags = MALLOCX_TCACHE(tcache);

        // This is the 'production' version where tcache is on, the first alloc
        // by the thread will do this block once. Since this is the first and
        // only call for the thread we can just reset (which is what threadUp
        // does)
        currentClient.mallocFlags = 0;
        currentClient.index = NoClientIndex;
        threadUp(client.index);
    } else {
        // This is the test code version where tcache is off, every switchTo
        // ends up here and will reset the ThreadCachedInt but ensure no+
        // recursive death when the thread allocates. This is always called
        // so we just call getPrecise to maintain the current mem_used
        currentClient.mallocFlags = 0;
        currentClient.index = NoClientIndex;
        threadUp(client.index);
    }

    currentClient.mallocFlags = MALLOCX_ARENA(client.arena) | tcacheFlags;
    currentClient.index = client.index;
}

template <>
PLATFORM_PUBLIC_API void JEArenaMalloc::switchFromClient() {
    // Set to 0, no client, all tracking/allocations go to default arena/tcache
    switchToClient({0, NoClientIndex, tcacheEnabled});
}

template <>
void* JEArenaMalloc::malloc(size_t size) {
    if (size == 0) {
        size = 8;
    }
    auto c = currentClient;
    memAllocated(c.index, size);
    return je_mallocx(size, c.mallocFlags);
}

template <>
void* JEArenaMalloc::calloc(size_t nmemb, size_t size) {
    auto c = currentClient;
    memAllocated(c.index, size);
    return je_mallocx(nmemb * size, c.mallocFlags | MALLOCX_ZERO);
}

template <>
void* JEArenaMalloc::realloc(void* ptr, size_t size) {
    if (size == 0) {
        size = 8;
    }

    auto c = currentClient;

    if (!ptr) {
        memAllocated(c.index, size);
        return je_mallocx(size, c.mallocFlags);
    }

    memDeallocated(c.index, ptr);
    memAllocated(c.index, size);
    return je_rallocx(ptr, size, c.mallocFlags);
}

template <>
void JEArenaMalloc::free(void* ptr) {
    if (ptr) {
        auto c = currentClient;
        memDeallocated(c.index, ptr);
        je_dallocx(ptr, c.mallocFlags);
    }
}

template <>
void JEArenaMalloc::sized_free(void* ptr, size_t size) {
    if (ptr) {
        auto c = currentClient;
        memDeallocated(c.index, ptr);
        je_sdallocx(ptr, size, c.mallocFlags);
    }
}

template <>
size_t JEArenaMalloc::malloc_usable_size(void* ptr) {
    return je_sallocx(ptr, 0);
}

template <>
void JEArenaMalloc::setTCacheEnabled(bool value) {
    tcacheEnabled = value;
}

static size_t mib_small[5]{};
static size_t miblen_small = []() {
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

static size_t mib_large[5]{};
static size_t miblen_large = []() {
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

template <>
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

} // namespace cb
