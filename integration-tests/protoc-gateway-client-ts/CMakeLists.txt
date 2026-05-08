cmake_minimum_required(VERSION 3.15)
project(protoc-gateway-client-ts-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-protoc CONFIG REQUIRED)
find_package(eiva-fletcher-pubsub CONFIG REQUIRED)
find_package(eiva-fletcher-core CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

if(NOT TARGET eiva-fletcher-protoc::plugin)
    message(FATAL_ERROR
        "eiva-fletcher-protoc::plugin target was not created — the protoc "
        "plugin executable is required for code generation.")
endif()

set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_TS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated-ts")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
file(MAKE_DIRECTORY "${GENERATED_TS_DIR}")

# Run protoc-gen-fletcher twice per proto: once for the C++ row class
# (.fletcher.pb.h, used by the vector emitter), once for the TypeScript
# bindings (.fletcher.ts, used by the vitest test). The TS output goes
# next to the test so vitest can import it without a build artifact path.
set(PROTO_STEMS telemetry)

set(GENERATED_HEADERS)
foreach(_stem IN LISTS PROTO_STEMS)
    set(_src     "${PROTO_DIR}/${_stem}.proto")
    set(_cpp_hdr "${GENERATED_DIR}/${_stem}.fletcher.pb.h")
    set(_ts_out  "${GENERATED_TS_DIR}/${_stem}.fletcher.ts")

    add_custom_command(
        OUTPUT  "${_cpp_hdr}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:eiva-fletcher-protoc::plugin>"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "${_src}"
        DEPENDS "${_src}" eiva-fletcher-protoc::plugin
        COMMENT "protoc-gen-fletcher (C++): ${_stem}.proto"
        VERBATIM
    )

    add_custom_command(
        OUTPUT  "${_ts_out}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:eiva-fletcher-protoc::plugin>"
                "--fletcher_out=ts:${GENERATED_TS_DIR}"
                "-I" "${PROTO_DIR}"
                "${_src}"
        DEPENDS "${_src}" eiva-fletcher-protoc::plugin
        COMMENT "protoc-gen-fletcher (TS): ${_stem}.proto"
        VERBATIM
    )

    list(APPEND GENERATED_HEADERS "${_cpp_hdr}" "${_ts_out}")
endforeach()

add_custom_target(generate_headers ALL DEPENDS ${GENERATED_HEADERS})

add_executable(emit_vectors src/emit_vectors.cpp)
add_dependencies(emit_vectors generate_headers)

target_include_directories(emit_vectors PRIVATE "${GENERATED_DIR}")
target_link_libraries(emit_vectors PRIVATE
    eiva-fletcher-pubsub::eiva-fletcher-pubsub
    eiva-fletcher-core::eiva-fletcher-core)
