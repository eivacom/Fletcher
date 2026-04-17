find_package(GTest REQUIRED)

add_executable(pubsub_tests
    test_driver.cpp
    test_envelope.cpp)

target_link_libraries(pubsub_tests PRIVATE
    GTest::gtest_main
    pubsub)

include(GoogleTest)
gtest_discover_tests(pubsub_tests)
