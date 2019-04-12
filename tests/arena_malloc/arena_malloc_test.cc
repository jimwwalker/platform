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

#include <folly/portability/GTest.h>

#include <vector>

class ArenaMalloc : public ::testing::Test {
public:
    void SetUp() override {
        client = cb::ArenaMalloc::registerClient();
    }

    void TearDown() override {
        cb::ArenaMalloc::unregisterClient(client);
    }

    cb::ArenaMallocClient client;
};

TEST_F(ArenaMalloc, basicUsage) {
    auto sz1 = cb::ArenaMalloc::getPreciseAllocated(client);

    // 1) Track an allocation
    cb::ArenaMalloc::switchToClient(client);
    auto p = cb_malloc(4096);
    cb::ArenaMalloc::switchFromClient();

    auto sz2 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_LT(sz1, sz2);

    // 2) Allocation outside of switchTo/From not accounted
    auto p2 = cb_malloc(4096);
    EXPECT_EQ(sz2, cb::ArenaMalloc::getPreciseAllocated(client));

    // 3) Track deallocation
    cb::ArenaMalloc::switchToClient(client);
    cb_free(p);
    cb::ArenaMalloc::switchFromClient();
    EXPECT_LT(cb::ArenaMalloc::getPreciseAllocated(client), sz2);
    cb_free(p2);
}

TEST_F(ArenaMalloc, checkAllAllocMethods) {
    auto sz1 = cb::ArenaMalloc::getPreciseAllocated(client);

    // 1) Track an allocation
    cb::ArenaMalloc::switchToClient(client);

    auto* p1 = cb_malloc(2048);
    auto sz2 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_LT(sz1, sz2);

    auto* p2 = cb_calloc(10, 100);
    auto sz3 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_LT(sz2, sz3);

    auto* p3 = cb_realloc(p1, 3121);
    auto sz4 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_LT(sz3, sz4);

    cb_free(p3);
    sz3 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_LT(sz3, sz4);

    cb_free(p2);
    sz2 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_LT(sz2, sz3);
    cb_sized_free(p1, 2048);
    sz1 = cb::ArenaMalloc::getPreciseAllocated(client);
    EXPECT_EQ(0, sz1);
    cb::ArenaMalloc::switchFromClient();
}

void thread(cb::ArenaMallocClient client) {
    auto sz1 = cb::ArenaMalloc::getPreciseAllocated(client);
    cb::ArenaMalloc::switchToClient(client);
    auto p = cb_malloc(4096);
    EXPECT_LT(sz1, cb::ArenaMalloc::getPreciseAllocated(client));
    cb_free(p);
    EXPECT_EQ(sz1, cb::ArenaMalloc::getPreciseAllocated(client));
}

TEST_F(ArenaMalloc, threads) {
    // Create threads and alloc on them
    const int nThreads = 4;
    std::array<std::pair<cb::ArenaMallocClient, std::thread>, nThreads> threads;
    for (int ii = 0; ii < nThreads; ii++) {
        auto c = cb::ArenaMalloc::registerClient();
        threads[ii] = {c, std::thread(thread, c)};
    }

    for (int ii = 0; ii < nThreads; ii++) {
        threads[ii].second.join();
        cb::ArenaMalloc::unregisterClient(threads[ii].first);
    }
}