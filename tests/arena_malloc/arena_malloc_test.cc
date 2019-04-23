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

#include "relaxed_atomic.h"

#include <platform/cb_arena_malloc.h>
#include <platform/cb_malloc.h>
#include <platform/dirutils.h>

#include <folly/portability/GTest.h>

#include <thread>
#include <utility>
#include <vector>

TEST(ArenaMalloc, basicUsage) {
    // JEMalloc will 'lazy' deallocate, so thread cache should be off, with
    // thread caching on we would observe larger allocations than we requested
    // as the tcache fills with big chunks and we may not see the affect of
    // deallocation as the cache will dealloc but not return memory to the arena
    auto client = cb::ArenaMalloc::registerClient(false);

    auto sz1 = cb::ArenaMalloc::getAllocated(client);

    // 1) Track an allocation
    cb::ArenaMalloc::switchToClient(client);
    auto p = cb_malloc(4096);
    cb::ArenaMalloc::switchFromClient();

    auto sz2 = cb::ArenaMalloc::getAllocated(client);
    EXPECT_LT(sz1, sz2);

    // 2) Allocation outside of switchTo/From not accounted
    auto p2 = cb_malloc(4096);
    EXPECT_EQ(sz2, cb::ArenaMalloc::getAllocated(client));

    // 3) Track deallocation
    cb::ArenaMalloc::switchToClient(client);
    cb_free(p);
    cb::ArenaMalloc::switchFromClient();
    EXPECT_LT(cb::ArenaMalloc::getAllocated(client), sz2);
    cb_free(p2);

    cb::ArenaMalloc::unregisterClient(client);
}

struct ArenaMallocFree {
    void operator()(void* ptr) {
        cb_free(ptr);
    }
};

TEST(ArenaMalloc, totalCounters) {
    std::vector<cb::ArenaMallocClient> clients(10);
    std::vector<cb::RelaxedAtomic<int64_t>> totals(10);
    std::vector<std::unique_ptr<void, ArenaMallocFree>> data;
    std::generate(clients.begin(), clients.end(), []() {
        return cb::ArenaMalloc::registerClient(false);
    });

    for (size_t i = 0; i < clients.size(); i++) {
        cb::ArenaMalloc::registerTotalCounter(clients[i], &totals[i]);
    }

    for (const auto& c : clients) {
        cb::ArenaMalloc::switchToClient(c);
        void* p = cb_malloc(128);
        data.emplace_back(p);
        cb::ArenaMalloc::switchFromClient();
    }

    for (const auto& t : totals) {
        EXPECT_EQ(0, t.load());
    }

    cb::ArenaMalloc::updateTotalCounters();

    // Now we have some values
    for (const auto& t : totals) {
        EXPECT_NE(0, t.load());
    }

    for (const auto& c : clients) {
        cb::ArenaMalloc::unregisterClient(c);
    }
}