# Creates an imported executable target: fletcher-protoc::fletcher-protoc
#
# Consumers can reference this target in add_custom_command() calls:
#
#   add_custom_command(
#       OUTPUT  "${out}.fletcher.pb.h"
#       COMMAND "$<TARGET_FILE:protobuf::protoc>"
#               "--plugin=fletcher-protoc=$<TARGET_FILE:fletcher-protoc::fletcher-protoc>"
#               "--fletcher_out=${GENERATED_DIR}"
#               "-I" "${PROTO_DIR}"
#               "${PROTO_DIR}/${stem}.proto"
#       DEPENDS "${PROTO_DIR}/${stem}.proto" fletcher-protoc::fletcher-protoc
#   )

if(TARGET fletcher-protoc::fletcher-protoc)
    return()
endif()

# Resolve the package root (one level up from cmake/).
get_filename_component(_fletcher_protoc_pkg_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)

# First look inside the Conan package itself.
find_program(_fletcher_protoc_plugin
    NAMES fletcher-protoc
    PATHS "${_fletcher_protoc_pkg_root}/bin"
    NO_DEFAULT_PATH
)

# Fall back to PATH so locally-installed builds also work.
if(NOT _fletcher_protoc_plugin)
    find_program(_fletcher_protoc_plugin NAMES fletcher-protoc)
endif()

if(_fletcher_protoc_plugin)
    add_executable(fletcher-protoc::fletcher-protoc IMPORTED GLOBAL)
    set_target_properties(fletcher-protoc::fletcher-protoc PROPERTIES
        IMPORTED_LOCATION "${_fletcher_protoc_plugin}")
    message(STATUS "Found fletcher-protoc: ${_fletcher_protoc_plugin}")
else()
    message(WARNING "fletcher-protoc not found — fletcher-protoc::fletcher-protoc target not created")
endif()

unset(_fletcher_protoc_pkg_root)
unset(_fletcher_protoc_plugin)
