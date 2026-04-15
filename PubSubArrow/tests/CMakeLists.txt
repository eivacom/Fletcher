add_executable(pubsub_arrow_tests
    test_pubsub_arrow.cpp)

target_link_libraries(pubsub_arrow_tests PRIVATE
    GTest::gtest_main
    pubsub_arrow)

include(GoogleTest)
gtest_discover_tests(pubsub_arrow_tests)
