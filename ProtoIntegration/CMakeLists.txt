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

set(PROTO_STEMS simple temporal nested collections maps complex pubsub evolution_v1 evolution_v2 geoarrow_test)

set(GEOARROW_PROTO_DIR "${CMAKE_SOURCE_DIR}/proto")

set(GENERATED_HEADERS)
foreach(_stem ${PROTO_STEMS})
    set(_src "${PROTO_DIR}/${_stem}.proto")
    set(_hdr "${GENERATED_DIR}/${_stem}.fletcher.pb.h")
    set(_vhdr "${GENERATED_DIR}/${_stem}.fletcher.view.pb.h")

    add_custom_command(
        OUTPUT  "${_hdr}" "${_vhdr}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:protoc-gen-fletcher>"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
                "-I" "${GEOARROW_PROTO_DIR}"
                "${_src}"
        DEPENDS "${_src}" protoc-gen-fletcher "${GEOARROW_PROTO_DIR}/fletcher/options.proto"
        COMMENT "protoc-gen-fletcher: ${_stem}.proto"
        VERBATIM
    )
    list(APPEND GENERATED_HEADERS "${_hdr}" "${_vhdr}")
endforeach()

# GeoArrow proto lives under the repo-root proto/ directory.
# Generate its header into a geoarrow/ subdirectory to match the import path.
file(MAKE_DIRECTORY "${GENERATED_DIR}/geoarrow")
set(_geoarrow_src "${GEOARROW_PROTO_DIR}/geoarrow/geoarrow.proto")
set(_geoarrow_hdr "${GENERATED_DIR}/geoarrow/geoarrow.fletcher.pb.h")
set(_geoarrow_vhdr "${GENERATED_DIR}/geoarrow/geoarrow.fletcher.view.pb.h")
add_custom_command(
    OUTPUT  "${_geoarrow_hdr}" "${_geoarrow_vhdr}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:protoc-gen-fletcher>"
            "--fletcher_out=${GENERATED_DIR}/geoarrow"
            "-I" "${GEOARROW_PROTO_DIR}/geoarrow"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${_geoarrow_src}"
    DEPENDS "${_geoarrow_src}" protoc-gen-fletcher
    COMMENT "protoc-gen-fletcher: geoarrow/geoarrow.proto"
    VERBATIM
)
list(APPEND GENERATED_HEADERS "${_geoarrow_hdr}" "${_geoarrow_vhdr}")

add_custom_target(fletcher_proto_codegen ALL
    DEPENDS ${GENERATED_HEADERS}
)

# ---- Test executable ----
add_executable(fletcher_proto_integration_tests
    tests/test_generated.cpp
    tests/test_schema_evolution.cpp
    tests/test_geoarrow.cpp
)
add_dependencies(fletcher_proto_integration_tests fletcher_proto_codegen)

target_include_directories(fletcher_proto_integration_tests PRIVATE
    "${GENERATED_DIR}"
)
target_link_libraries(fletcher_proto_integration_tests PRIVATE
    Catch2::Catch2WithMain
    row_codec
    pubsub
    arrow::arrow
)

include(Catch)
catch_discover_tests(fletcher_proto_integration_tests)
