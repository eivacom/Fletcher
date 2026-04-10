include(CTest)

add_executable(xrce_provider_tests
    test_xrce_provider.cpp)

target_link_libraries(xrce_provider_tests PRIVATE
    Catch2::Catch2WithMain
    xrce_dds_pubsub_provider
    microxrcedds_client)

include(Catch)
catch_discover_tests(xrce_provider_tests)
