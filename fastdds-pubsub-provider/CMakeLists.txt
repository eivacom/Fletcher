cmake_minimum_required(VERSION 3.15)
project(eiva-fletcher-fastdds-pubsub-provider CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(eiva-fletcher-pubsub REQUIRED)
find_package(eiva-fletcher-core REQUIRED)
find_package(fastdds REQUIRED)

add_library(fast_dds_pubsub_provider STATIC
    src/fast_dds_pubsub_provider.cpp)
target_include_directories(fast_dds_pubsub_provider PUBLIC include)
target_link_libraries(fast_dds_pubsub_provider
    PUBLIC  eiva-fletcher-pubsub::eiva-fletcher-pubsub
    PRIVATE eiva-fletcher-core::eiva-fletcher-core
            fast-dds::fast-dds)

if(FLETCHER_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
