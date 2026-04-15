add_executable(arrow_row_codec_tests
    test_fingerprint.cpp
    test_scalars.cpp
    test_recursive.cpp
    test_nulls.cpp
    test_errors.cpp
    test_schema_evolution.cpp
    test_positional.cpp)

# GTest::gtest_main must come BEFORE row_codec on the link line.
# row_codec -> arrow::arrow -> boost::boost pulls in libboost_test_exec_monitor,
# which also provides main() and expects test_main().  MSVC resolves 'main'
# from the first library that defines it, so gtest_main must win.
target_link_libraries(arrow_row_codec_tests PRIVATE
    GTest::gtest_main
    row_codec)

include(GoogleTest)
gtest_discover_tests(arrow_row_codec_tests)
