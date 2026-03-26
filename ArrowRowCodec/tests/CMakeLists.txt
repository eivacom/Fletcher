find_package(Catch2 REQUIRED)
include(CTest)

add_executable(arrow_row_codec_tests
    test_fingerprint.cpp
    test_scalars.cpp
    test_recursive.cpp
    test_nulls.cpp
    test_errors.cpp)

target_link_libraries(arrow_row_codec_tests PRIVATE
    row_codec
    Catch2::Catch2WithMain)

# CTest discovers the Catch2 test cases via the standard --list-tests protocol.
include(Catch)
catch_discover_tests(arrow_row_codec_tests)
