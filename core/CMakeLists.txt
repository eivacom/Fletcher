cmake_minimum_required(VERSION 3.15)
project(fletcher-core VERSION 0.1.0)

add_library(fletcher-core INTERFACE)
target_include_directories(fletcher-core INTERFACE include)

if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
