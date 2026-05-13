cmake_minimum_required(VERSION 3.15)
project(schema-transport-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-pubsub-arrow CONFIG REQUIRED)
find_package(eiva-fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

add_executable(integration_tests
    tests/test_schema_transport_fastdds.cpp)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow
    eiva-fletcher-fastdds-pubsub-provider::eiva-fletcher-fastdds-pubsub-provider)

include(GoogleTest)
gtest_discover_tests(integration_tests)
