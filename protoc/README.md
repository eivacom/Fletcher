# fletcher-protoc

A `protoc` compiler plugin that reads `.proto` files and generates C++ header files containing row wrapper classes in the `fletcher_gen` namespace, along with TypeScript schema descriptors. Each supported proto message gets a class with a typed setter API that produces an `EncodedRow` via `Encode()`, including the Arrow schema it was generated from. Service definitions with eligible RPC methods additionally generate `Publisher` and `Subscriber` classes for pub/sub.

## Building

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

```bash
conan install . --build=missing --output-folder=build
cmake --preset conan-default
cmake --build build --config Release
```

### Running tests

```bash
conan install . --build=missing --output-folder=build -o "&:with_tests=True"
cmake --preset conan-default -DFLETCHER_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --build-config Release
```

## Using the plugin

```bash
protoc \
    --plugin=fletcher-protoc=/path/to/fletcher-protoc \
    --fletcher_out=generated/ \
    -I proto/ \
    proto/my_service.proto
```

### Options

- `--fletcher_opt=ts` — generate TypeScript schema descriptors (`.fletcher.ts`) instead of C++ headers.

## Conan package

When consumed as a Conan package, `fletcher-protoc` provides an imported CMake target:

```cmake
find_package(fletcher-protoc REQUIRED)

add_custom_command(
    OUTPUT  "${GENERATED_DIR}/${stem}.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=fletcher-protoc=$<TARGET_FILE:fletcher-protoc::fletcher-protoc>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/${stem}.proto"
    DEPENDS "${PROTO_DIR}/${stem}.proto" fletcher-protoc::fletcher-protoc
)
```

## Proto to Arrow type mapping

| Proto type | Arrow type | Notes |
|---|---|---|
| `bool` | `boolean()` | |
| `int32`, `sint32`, `sfixed32` | `int32()` | |
| `int64`, `sint64`, `sfixed64` | `int64()` | |
| `uint32`, `fixed32` | `uint32()` | |
| `uint64`, `fixed64` | `uint64()` | |
| `float` | `float32()` | |
| `double` | `float64()` | |
| `string` | `utf8()` | |
| `bytes` | `binary()` | |
| `enum` | `int32()` | enum value cast to int32 |
| `google.protobuf.Timestamp` | `timestamp(NANO)` | seconds * 1e9 + nanos |
| `google.protobuf.Duration` | `duration(NANO)` | seconds * 1e9 + nanos |
| `google.protobuf.*Value` | nullable scalar | wrapper presence → null |
| `repeated T` | `list(T)` | |
| `map<K,V>` | `map(K, V)` | |
| nested `message` | `struct<...>` | |
