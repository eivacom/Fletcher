cmake_minimum_required(VERSION 3.15)
project(ArrowRowSerializer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Arrow REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(row_codec
    src/row_codec.cpp)
target_include_directories(row_codec PUBLIC include PRIVATE src)
target_link_libraries(row_codec PUBLIC arrow::arrow)

add_library(row_batcher
    src/row_batcher.cpp)
target_include_directories(row_batcher PUBLIC include PRIVATE src)
target_link_libraries(row_batcher PUBLIC arrow::arrow PRIVATE row_codec)

add_library(sqlite_dbs
    src/sqlite_wal.cpp)
target_include_directories(sqlite_dbs PUBLIC include)
target_link_libraries(sqlite_dbs PUBLIC arrow::arrow SQLite::SQLite3 row_codec)

add_executable(${PROJECT_NAME}
    src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE row_codec row_batcher sqlite_dbs)
