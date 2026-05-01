cmake_minimum_required(VERSION 3.15)
project(fletcher-pubsub LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-core CONFIG REQUIRED)

add_subdirectory(third_party/nanoarrow)

add_library(fletcher-pubsub STATIC
    src/driver.cpp
    src/schema_ipc.cpp)

target_include_directories(fletcher-pubsub PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(fletcher-pubsub PUBLIC
    eiva-fletcher-core::eiva-fletcher-core
    nanoarrow)

option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
