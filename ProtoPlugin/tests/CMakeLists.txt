add_executable(fletcher_proto_plugin_tests
    test_type_mapper.cpp)

target_link_libraries(fletcher_proto_plugin_tests PRIVATE
    GTest::gtest_main
    fletcher_plugin_core)

include(GoogleTest)
gtest_discover_tests(fletcher_proto_plugin_tests)
