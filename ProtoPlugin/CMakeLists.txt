# fletcher_plugin_core: type mapper + generator logic, usable by tests without
# needing the full plugin executable.
add_library(fletcher_plugin_core STATIC
    src/type_mapper.cpp
    src/generator.cpp)
target_include_directories(fletcher_plugin_core PUBLIC src)
target_link_libraries(fletcher_plugin_core PUBLIC
    protobuf::libprotoc
    protobuf::libprotobuf)

# The plugin executable: reads CodeGeneratorRequest from stdin, writes
# CodeGeneratorResponse to stdout. protoc invokes it as a subprocess.
add_executable(protoc-gen-fletcher src/main.cpp)
target_link_libraries(protoc-gen-fletcher PRIVATE fletcher_plugin_core)

add_subdirectory(tests)
