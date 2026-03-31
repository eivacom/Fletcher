# arrow_row_plugin_core: type mapper + generator logic, usable by tests without
# needing the full plugin executable.
add_library(arrow_row_plugin_core STATIC
    src/type_mapper.cpp
    src/generator.cpp)
target_include_directories(arrow_row_plugin_core PUBLIC src)
target_link_libraries(arrow_row_plugin_core PUBLIC
    protobuf::libprotoc
    protobuf::libprotobuf)

# The plugin executable: reads CodeGeneratorRequest from stdin, writes
# CodeGeneratorResponse to stdout. protoc invokes it as a subprocess.
add_executable(protoc-gen-arrow-row src/main.cpp)
target_link_libraries(protoc-gen-arrow-row PRIVATE arrow_row_plugin_core)

add_subdirectory(tests)
