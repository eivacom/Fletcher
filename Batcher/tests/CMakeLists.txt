add_executable(arrow_row_batcher_tests
    test_sqlite_wal.cpp
    test_batcher.cpp)

# GTest::gtest_main must come BEFORE row_batcher on the link line.
# row_batcher -> arrow::arrow -> boost::boost pulls in libboost_test_exec_monitor,
# which also provides main() and expects test_main().  MSVC resolves 'main'
# from the first library that defines it, so gtest_main must win.
target_link_libraries(arrow_row_batcher_tests PRIVATE
    GTest::gtest_main
    row_batcher
    sqlite_dbs
    row_codec)

include(GoogleTest)
gtest_discover_tests(arrow_row_batcher_tests)
