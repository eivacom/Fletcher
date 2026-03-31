include(CTest)

add_executable(arrow_row_proto_plugin_tests
    test_type_mapper.cpp)

target_link_libraries(arrow_row_proto_plugin_tests PRIVATE
    Catch2::Catch2WithMain
    arrow_row_plugin_core)

include(Catch)
catch_discover_tests(arrow_row_proto_plugin_tests)
