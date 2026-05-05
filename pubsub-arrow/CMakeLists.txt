cmake_minimum_required(VERSION 3.15)
project(fletcher-pubsub-arrow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-pubsub CONFIG REQUIRED)
find_package(eiva-fletcher-arrow-bridge CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)

add_library(fletcher-pubsub-arrow STATIC
    src/pubsub_arrow.cpp)

target_include_directories(fletcher-pubsub-arrow
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(fletcher-pubsub-arrow PUBLIC
    eiva-fletcher-pubsub::eiva-fletcher-pubsub
    eiva-fletcher-arrow-bridge::eiva-fletcher-arrow-bridge
    arrow::arrow)

option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
