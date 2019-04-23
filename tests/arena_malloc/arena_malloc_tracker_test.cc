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

/**
 * Test migrated from
 *  - kv_engine/engines/ep/tests/module_tests/memory_tracker_test.cc
 * Modified to use ArenaMalloc and runs standalone so that no arena recycling
 * affects the expects.
 */

#include <platform/cb_arena_malloc.h>
#include <platform/cb_malloc.h>
#include <platform/dirutils.h>

#include <folly/portability/GTest.h>

#include <thread>
#include <utility>

// Test pointer in global scope to prevent compiler optimizing malloc/free away
// via DCE.
char* p;

class MemoryTrackerTest : public ::testing::Test {
public:
    // function executed by our accounting test thread.
    static void AccountingTestThread(const cb::ArenaMallocClient& client);
};

void MemoryTrackerTest::AccountingTestThread(
        const cb::ArenaMallocClient& client) {
    cb::ArenaMalloc::switchToClient(client);
    ASSERT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test new & delete //////////////////////////////////////////////////
    p = new char();
    EXPECT_GT(cb::ArenaMalloc::getAllocated(client), 0);
    delete p;
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test sized delete //////////////////////////////////////////////////
    p = new char();
    EXPECT_GT(cb::ArenaMalloc::getAllocated(client), 0);
    operator delete(p, sizeof(char));
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test new[] & delete[] //////////////////////////////////////////////
    p = new char[100];
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 100);
    delete[] p;
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test sized delete[] ////////////////////////////////////////////////
    p = new char[100];
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 100);
    operator delete[](p, sizeof(char) * 100);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test cb_malloc() / cb_free() ///////////////////////////////////////////
    p = static_cast<char*>(cb_malloc(sizeof(char) * 10));
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 10);
    cb_free(p);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test cb_realloc() /////////////////////////////////////////////////////
    p = static_cast<char*>(cb_malloc(1));
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 1);

    // Allocator may round up allocation sizes; so it's hard to
    // accurately predict how much cb::ArenaMalloc::getAllocated(client) will
    // increase. Hence we just increase by a "large" amount and check at least
    // half that increment.
    size_t prev_size = cb::ArenaMalloc::getAllocated(client);
    p = static_cast<char*>(cb_realloc(p, sizeof(char) * 100));
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), (prev_size + 50));

    prev_size = cb::ArenaMalloc::getAllocated(client);
    p = static_cast<char*>(cb_realloc(p, 1));
    EXPECT_LT(cb::ArenaMalloc::getAllocated(client), prev_size);

    prev_size = cb::ArenaMalloc::getAllocated(client);
    char* q = static_cast<char*>(cb_realloc(NULL, 10));
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), prev_size + 10);

    cb_free(p);
    cb_free(q);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test cb_calloc() //////////////////////////////////////////////////////
    p = static_cast<char*>(cb_calloc(sizeof(char), 20));
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 20);
    cb_free(p);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test indirect use of malloc() via cb_strdup() /////////////////////////
    p = cb_strdup("random string");
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), sizeof("random string"));
    cb_free(p);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    // Test memory allocations performed from another shared library loaded
    // at runtime.
    cb::ArenaMalloc::switchFromClient();
    auto plugin = cb::io::loadLibrary("platform_memory_tracking_plugin");
    cb::ArenaMalloc::switchToClient(client);
    // dlopen()ing a plugin can allocate memory.
    typedef void* (*plugin_malloc_t)(size_t);
    auto plugin_malloc = plugin->find<plugin_malloc_t>("plugin_malloc");
    p = static_cast<char*>(plugin_malloc(100));
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 100);

    typedef void (*plugin_free_t)(void*);
    auto plugin_free = plugin->find<plugin_free_t>("plugin_free");
    plugin_free(p);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    typedef char* (*plugin_new_char_t)(size_t);
    auto plugin_new_char =
            plugin->find<plugin_new_char_t>("plugin_new_char_array");
    p = plugin_new_char(200);
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 200);

    typedef void (*plugin_delete_array_t)(char*);
    auto plugin_delete_char =
            plugin->find<plugin_delete_array_t>("plugin_delete_array");
    plugin_delete_char(p);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));

    typedef std::string* (*plugin_new_string_t)(const char*);
    auto plugin_new_string =
            plugin->find<plugin_new_string_t>("plugin_new_string");
    auto* string = plugin_new_string("duplicate_string");
    EXPECT_GE(cb::ArenaMalloc::getAllocated(client), 16);

    typedef void (*plugin_delete_string_t)(std::string * ptr);
    auto plugin_delete_string =
            plugin->find<plugin_delete_string_t>("plugin_delete_string");
    plugin_delete_string(string);
    EXPECT_EQ(0, cb::ArenaMalloc::getAllocated(client));
}

// Test that the various memory allocation / deletion functions are correctly
// accounted for, when run in a parallel thread.
TEST_F(MemoryTrackerTest, Accounting) {
    // Register and disable thread-cache
    auto client = cb::ArenaMalloc::registerClient(false);
    std::thread accounting(AccountingTestThread, std::cref(client));
    accounting.join();
}