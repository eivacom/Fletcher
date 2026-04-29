find_package(GTest REQUIRED)

add_executable(core_tests
    test_envelope.cpp)

target_link_libraries(core_tests PRIVATE
    GTest::gtest_main
    fletcher-core)

include(GoogleTest)
gtest_discover_tests(core_tests)