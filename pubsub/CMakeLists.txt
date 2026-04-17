cmake_minimum_required(VERSION 3.15)
project(fletcher-pubsub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(protobuf REQUIRED)
find_package(nanoarrow REQUIRED)

# Core logic as a static library (reusable by tests).
add_library(pubsub
    src/driver.cpp
    src/schema_ipc.cpp)
target_include_directories(pubsub PUBLIC include)
target_link_libraries(pubsub PUBLIC nanoarrow::nanoarrow nanoarrow::nanoarrow_ipc)



# Tests (opt-in via FLETCHER_BUILD_TESTS, default OFF).
option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS pubsub DESTINATION bin)
