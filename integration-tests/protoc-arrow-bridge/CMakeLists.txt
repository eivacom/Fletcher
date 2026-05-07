cmake_minimum_required(VERSION 3.15)
project(protoc-arrow-bridge-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-protoc CONFIG REQUIRED)
find_package(eiva-fletcher-arrow-bridge CONFIG REQUIRED)
find_package(eiva-fletcher-pubsub CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Each .proto here is run through protoc-gen-fletcher to produce a
# <stem>.fletcher.pb.h header that the test sources include.
set(PROTO_STEMS
    telemetry
    evolution_v1
    evolution_v2)

set(GENERATED_HEADERS)
foreach(_stem IN LISTS PROTO_STEMS)
    set(_src "${PROTO_DIR}/${_stem}.proto")
    set(_hdr "${GENERATED_DIR}/${_stem}.fletcher.pb.h")
    add_custom_command(
        OUTPUT  "${_hdr}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:eiva-fletcher-protoc::plugin>"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "${_src}"
        DEPENDS "${_src}" eiva-fletcher-protoc::plugin
        COMMENT "protoc-gen-fletcher: ${_stem}.proto"
        VERBATIM
    )
    list(APPEND GENERATED_HEADERS "${_hdr}")
endforeach()

add_custom_target(generate_headers ALL DEPENDS ${GENERATED_HEADERS})

enable_testing()

add_executable(integration_tests
    tests/test_byte_compat.cpp
    tests/test_schema_evolution.cpp)
add_dependencies(integration_tests generate_headers)

target_include_directories(integration_tests PRIVATE
    "${GENERATED_DIR}")

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    eiva-fletcher-arrow-bridge::eiva-fletcher-arrow-bridge
    eiva-fletcher-pubsub::eiva-fletcher-pubsub)

include(GoogleTest)
gtest_discover_tests(integration_tests)
