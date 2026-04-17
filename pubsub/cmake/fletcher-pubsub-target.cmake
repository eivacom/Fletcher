# Creates an imported executable target: fletcher-pubsub::plugin
#
# Usage example:
#
#   add_custom_command(
#       OUTPUT  "${out}.fletcher.pb.h"
#       COMMAND "$<TARGET_FILE:protobuf::pubsub>"
#               "--plugin=pubsub-gen-fletcher=$<TARGET_FILE:fletcher-pubsub::plugin>"
#               "--fletcher_out=${GENERATED_DIR}"
#               "-I" "${PROTO_DIR}"
#               "${PROTO_DIR}/${stem}.proto"
#       DEPENDS "${PROTO_DIR}/${stem}.proto" fletcher-pubsub::plugin
#   )

if(TARGET fletcher-pubsub::plugin)
    return()
endif()

# Resolve the package root (one level up from cmake/).
get_filename_component(_fletcher_pubsub_pkg_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)

# First look inside the Conan package itself.
find_program(_fletcher_pubsub_plugin
    NAMES fletcher-pubsub
    PATHS "${_fletcher_pubsub_pkg_root}/bin"
    NO_DEFAULT_PATH
)

# Fall back to PATH so locally-installed builds also work.
if(NOT _fletcher_pubsub_plugin)
    find_program(_fletcher_pubsub_plugin NAMES fletcher-pubsub)
endif()

if(_fletcher_pubsub_plugin)
    add_executable(fletcher-pubsub::plugin IMPORTED GLOBAL)
    set_target_properties(fletcher-pubsub::plugin PROPERTIES
        IMPORTED_LOCATION "${_fletcher_pubsub_plugin}")
    message(STATUS "Found fletcher-pubsub: ${_fletcher_pubsub_plugin}")
else()
    message(WARNING "fletcher-pubsub not found — fletcher-pubsub::plugin target not created")
endif()

unset(_fletcher_pubsub_pkg_root)
unset(_fletcher_pubsub_plugin)
