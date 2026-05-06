cmake_minimum_required(VERSION 3.15)
project(eiva-fletcher-fastdds-pubsub-provider CXX)

find_package(eiva-fletcher-pubsub REQUIRED)
find_package(eiva-fletcher-core REQUIRED)
find_package(fastdds REQUIRED)

add_library(fast_dds_pubsub_provider
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
