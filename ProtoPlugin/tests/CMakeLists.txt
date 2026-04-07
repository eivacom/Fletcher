include(CTest)

add_executable(fletcher_proto_plugin_tests
    test_type_mapper.cpp)

target_link_libraries(fletcher_proto_plugin_tests PRIVATE
    Catch2::Catch2WithMain
    fletcher_plugin_core)

include(Catch)
catch_discover_tests(fletcher_proto_plugin_tests)
