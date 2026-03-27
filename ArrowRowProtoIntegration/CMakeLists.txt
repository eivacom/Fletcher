cmake_minimum_required(VERSION 3.15)
project(ArrowRowProtoIntegration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 REQUIRED)
include(CTest)

# ---- Locate protobuf WKT include directory at configure time ----
# Query each imported config variant; take the first hit.
foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir  "${_protoc_loc}" DIRECTORY)
        get_filename_component(_protoc_root "${_protoc_dir}/.." ABSOLUTE)
        set(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_root}/include")
        break()
    endif()
endforeach()

if(NOT PROTOBUF_WKT_INCLUDE_DIR)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir  "${_protoc_loc}" DIRECTORY)
        get_filename_component(_protoc_root "${_protoc_dir}/.." ABSOLUTE)
        set(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_root}/include")
    else()
        message(FATAL_ERROR
            "Cannot determine protobuf WKT include dir.\n"
            "Expected google/protobuf/*.proto to live under <protobuf_root>/include/")
    endif()
endif()

message(STATUS "Protobuf WKT include dir: ${PROTOBUF_WKT_INCLUDE_DIR}")

# ---- Proto codegen ----
set(PROTO_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(PROTO_STEMS simple temporal nested collections maps complex pubsub)

set(GENERATED_HEADERS)
foreach(_stem ${PROTO_STEMS})
    set(_src  "${PROTO_DIR}/${_stem}.proto")
    set(_hdr  "${GENERATED_DIR}/${_stem}.arrow_row.pb.h")

    add_custom_command(
        OUTPUT  "${_hdr}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-arrow-row=$<TARGET_FILE:protoc-gen-arrow-row>"
                "--arrow-row_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
                "${_src}"
        DEPENDS "${_src}" protoc-gen-arrow-row
        COMMENT "protoc-gen-arrow-row: ${_stem}.proto"
        VERBATIM
    )
    list(APPEND GENERATED_HEADERS "${_hdr}")
endforeach()

add_custom_target(arrow_row_proto_codegen ALL
    DEPENDS ${GENERATED_HEADERS}
)

# ---- Test executable ----
add_executable(arrow_row_proto_integration_tests
    tests/test_generated.cpp
)
add_dependencies(arrow_row_proto_integration_tests arrow_row_proto_codegen)

target_include_directories(arrow_row_proto_integration_tests PRIVATE
    "${GENERATED_DIR}"
)
target_link_libraries(arrow_row_proto_integration_tests PRIVATE
    Catch2::Catch2WithMain
    row_codec
)

include(Catch)
catch_discover_tests(arrow_row_proto_integration_tests)
