ADD_EXECUTABLE(platform-arena_malloc-test arena_malloc_test.cc)
TARGET_LINK_LIBRARIES(platform-arena_malloc-test PRIVATE
        gtest
        gtest_main
        platform)
ADD_TEST(platform-arena_malloc-test platform-arena_malloc-test)
