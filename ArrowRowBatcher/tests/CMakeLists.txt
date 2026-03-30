include(CTest)

add_executable(arrow_row_batcher_tests
    test_sqlite_wal.cpp
    test_batcher.cpp)

# Catch2::Catch2WithMain must come BEFORE row_batcher on the link line.
# row_batcher -> arrow::arrow -> boost::boost pulls in libboost_test_exec_monitor,
# which also provides main() and expects test_main().  MSVC resolves 'main'
# from the first library that defines it, so Catch2WithMain must win.
target_link_libraries(arrow_row_batcher_tests PRIVATE
    Catch2::Catch2WithMain
    row_batcher
    sqlite_dbs
    row_codec)

include(Catch)
catch_discover_tests(arrow_row_batcher_tests)
