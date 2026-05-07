find_package(GTest REQUIRED)

add_executable(xrce_provider_tests
    test_xrce_provider.cpp)

target_link_libraries(xrce_provider_tests PRIVATE
    GTest::gtest_main
    xrce_dds_pubsub_provider
    microxrcedds_client)

include(GoogleTest)
gtest_add_tests(TARGET xrce_provider_tests)
