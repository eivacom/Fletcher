include(CTest)

add_executable(arrow_row_codec_tests
    test_fingerprint.cpp
    test_scalars.cpp
    test_recursive.cpp
    test_nulls.cpp
    test_errors.cpp
    test_schema_evolution.cpp
    test_envelope.cpp)

# Catch2::Catch2WithMain must come BEFORE row_codec on the link line.
# row_codec -> arrow::arrow -> boost::boost pulls in libboost_test_exec_monitor,
# which also provides main() and expects test_main().  MSVC resolves 'main'
# from the first library that defines it, so Catch2WithMain must win.
target_link_libraries(arrow_row_codec_tests PRIVATE
    Catch2::Catch2WithMain
    row_codec)

# CTest discovers the Catch2 test cases via the standard --list-tests protocol.
include(Catch)
catch_discover_tests(arrow_row_codec_tests)
