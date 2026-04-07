# Emscripten build for the Fletcher WASM row decoder.
#
# Build:
#   emcmake cmake -S . -B build
#   cmake --build build
#
# Output:
#   build/fletcher_decoder.wasm
#   build/fletcher_decoder.js       (ES module glue)

cmake_minimum_required(VERSION 3.20)
project(fletcher_wasm_decoder C)

add_executable(fletcher_decoder src/decoder.c)

# Emscripten link flags:
#   MODULARIZE        — wraps output in a factory function
#   EXPORT_ES6        — emit an ES module instead of CJS
#   EXPORTED_FUNCTIONS — keep our C API alive (not dead-stripped)
#   EXPORTED_RUNTIME_METHODS — expose cwrap/ccall for the TS wrapper
#   ALLOW_MEMORY_GROWTH — dynamic heap (rows can vary in size)
#   STANDALONE_WASM=0 — need the JS glue for memory management
set_target_properties(fletcher_decoder PROPERTIES
    SUFFIX ".js"
    LINK_FLAGS "\
        -s MODULARIZE=1 \
        -s EXPORT_ES6=1 \
        -s EXPORT_NAME=createFletcherDecoder \
        -s EXPORTED_FUNCTIONS=['_fletcher_decode_row','_fletcher_decode_struct','_fletcher_get_schema_hash','_fletcher_list_count','_fletcher_map_count','_malloc','_free'] \
        -s EXPORTED_RUNTIME_METHODS=['cwrap','ccall','HEAPU8'] \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s INITIAL_MEMORY=65536 \
        -s STACK_SIZE=16384 \
        -s ENVIRONMENT='web,worker' \
        -O2"
)
