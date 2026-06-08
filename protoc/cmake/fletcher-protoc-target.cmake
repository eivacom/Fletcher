# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Creates an imported executable target: fletcher-protoc::plugin
#
# Usage example:
#
#   add_custom_command(
#       OUTPUT  "${out}.fletcher.pb.h"
#       COMMAND "$<TARGET_FILE:protobuf::protoc>"
#               "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
#               "--fletcher_out=${GENERATED_DIR}"
#               "-I" "${PROTO_DIR}"
#               "${PROTO_DIR}/${stem}.proto"
#       DEPENDS "${PROTO_DIR}/${stem}.proto" fletcher-protoc::plugin
#   )

# Resolve the package root (one level up from cmake/).
get_filename_component(_fletcher_protoc_pkg_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(FLETCHER_PROTO_INCLUDE_DIR "${_fletcher_protoc_pkg_root}/include")

if(TARGET fletcher-protoc::plugin)
    unset(_fletcher_protoc_pkg_root)
    return()
endif()

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
    add_executable(fletcher-protoc::plugin IMPORTED GLOBAL)
    set_target_properties(fletcher-protoc::plugin PROPERTIES
        IMPORTED_LOCATION "${_fletcher_protoc_plugin}")
    message(STATUS "Found fletcher-protoc: ${_fletcher_protoc_plugin}")
else()
    message(WARNING "fletcher-protoc not found — fletcher-protoc::plugin target not created")
endif()

unset(_fletcher_protoc_pkg_root)
unset(_fletcher_protoc_plugin)
