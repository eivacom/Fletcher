cmake_minimum_required(VERSION 3.15)
project(ArrowRow VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Arrow REQUIRED)
find_package(SQLite3 REQUIRED)

add_subdirectory(ArrowRowCodec)
add_subdirectory(ArrowRowBatcher)
