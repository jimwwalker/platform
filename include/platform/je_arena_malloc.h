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

#pragma once

#include <folly/Synchronized.h>
#include <platform/cb_arena_malloc_client.h>
#include <platform/visibility.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace cb {
template <class t>
class RelaxedAtomic;

using TCacheArray = std::array<uint16_t, ArenaMallocMaxClients>;

/**
 * JEArenaMalloc implements the ArenaMalloc class providing memory allocation
 * via je_malloc - https://github.com/jemalloc/jemalloc.
 *
 * registering for an arena gives the client a je_malloc arena to encapsulate
 * their allocation activity and providing mem_used functionality.
 *
 */
class PLATFORM_PUBLIC_API JEArenaMalloc {
public:
    static ArenaMallocClient registerClient(bool threadCache);
    static void registerTotalCounter(
            const ArenaMallocClient& client,
            cb::RelaxedAtomic<int64_t>* counterAddress);
    static void unregisterClient(const ArenaMallocClient& client);
    static void unregisterCurrentClient();
    static void switchToClient(const ArenaMallocClient& client);
    static void switchFromClient();
    static size_t getAllocated(const ArenaMallocClient& client);
    static void updateTotalCounters();
    static void* malloc(size_t size);
    static void* calloc(size_t nmemb, size_t size);
    static void* realloc(void* ptr, size_t size);
    static void free(void* ptr);
    static void sized_free(void* ptr, size_t size);
    static size_t malloc_usable_size(void* ptr);
    static constexpr bool canTrackAllocations() {
        return true;
    }
    static void setTCacheEnabled(bool value);
    static bool releaseFreeMemory();

private:
    static int getFlags();
    static int makeArena();
    static int makeTCache();
    static TCacheArray* makeTCacheArray();
    static size_t getAllocated(int arena);

    /// Determines if we can ever use a tcache (overrides client setting)
    static bool tcacheEnabled;

    struct Client {
        bool used = false;
        int arena = 0;
        cb::RelaxedAtomic<int64_t>* total = nullptr;
    };

    static size_t mib_small[5];
    static size_t miblen_small;
    static size_t mib_large[5];
    static size_t miblen_large;
    static folly::Synchronized<std::array<Client, ArenaMallocMaxClients>>
            clients;
};
} // namespace cb