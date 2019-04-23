ADD_EXECUTABLE(platform-arena_malloc-test arena_malloc_test.cc)
TARGET_LINK_LIBRARIES(platform-arena_malloc-test PRIVATE
        gtest
        gtest_main
        platform)

ADD_EXECUTABLE(platform-arena_malloc-tracker-test arena_malloc_tracker_test.cc)
TARGET_LINK_LIBRARIES(platform-arena_malloc-tracker-test PRIVATE
        dirutils
        gtest
        gtest_main
        platform)

ADD_LIBRARY(platform_memory_tracking_plugin MODULE
            memory_tracking_plugin.cc)
TARGET_LINK_LIBRARIES(platform_memory_tracking_plugin platform)
SET_TARGET_PROPERTIES(platform_memory_tracking_plugin PROPERTIES PREFIX "")

# Need to ensure that the directory containing
# platform_memory_tracking_test is added to the RPATH so we can
# successfully dlopen() the plugin.
LINK_DIRECTORIES(${Platform_BINARY_DIR})

# Only run the tests if de-allocations are tracked
IF (NOT MEMORY_ALLOCATOR STREQUAL "system")
ADD_TEST(platform-arena_malloc-test platform-arena_malloc-test)
ADD_TEST(platform-arena_malloc-tracker-test platform-arena_malloc-tracker-test)
ENDIF (NOT MEMORY_ALLOCATOR STREQUAL "system")