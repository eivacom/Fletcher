add_library(sqlite_dbs
    src/sqlite_wal.cpp)
target_include_directories(sqlite_dbs PUBLIC include)
# sqlite_wal.cpp uses scalar_codec.hpp which lives in ArrowRowCodec/src (internal).
target_include_directories(sqlite_dbs PRIVATE
    ${CMAKE_SOURCE_DIR}/ArrowRowCodec/src)
target_link_libraries(sqlite_dbs PUBLIC arrow::arrow SQLite::SQLite3 row_codec)

add_library(row_batcher
    src/row_batcher.cpp
    src/generic_row_batcher.cpp)
target_include_directories(row_batcher PUBLIC include)
target_link_libraries(row_batcher PUBLIC arrow::arrow PRIVATE row_codec)

add_library(arrow_row_batcher_capi STATIC
    src/arrow_row_batcher_capi.cpp)
# Exposes arrow_row_batcher_capi.h, which includes arrow_row_codec_capi.h.
target_include_directories(arrow_row_batcher_capi PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/ArrowRowCodec/include)
target_link_libraries(arrow_row_batcher_capi PRIVATE
    row_batcher sqlite_dbs row_codec arrow_row_codec_capi arrow::arrow)

add_subdirectory(tests)
