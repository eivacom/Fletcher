# fletcher-protoc

A `protoc` compiler plugin that reads `.proto` files and generates C++ header files containing row wrapper classes in the `fletcher_gen` namespace, along with TypeScript schema descriptors. Each supported proto message gets a class with a typed setter API that produces an `EncodedRow` via `Encode()`, including the Arrow schema it was generated from. Service definitions with eligible RPC methods additionally generate `Publisher` and `Subscriber` classes for pub/sub.

## Building locally

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

### Windows

Build locally:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Build locally and run unit tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

Create the Conan package and run test_package:

```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Create the Conan package with unit tests:

```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory:

Build locally:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

Build locally and run unit tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Create the Conan package and run test_package:

```bash
conan create . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

Create the Conan package with unit tests:

```bash
conan create . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

## Using the plugin

```bash
protoc \
    --plugin=protoc-gen-fletcher=/path/to/fletcher-protoc \
    --fletcher_out=generated/ \
    -I proto/ \
    proto/my_service.proto
```

> **Note:** protoc requires the `--plugin` name to follow the `protoc-gen-<NAME>` convention to map to `--<NAME>_out`. This is a protoc constraint.

### Options

- `--fletcher_opt=ts` — generate TypeScript schema descriptors (`.fletcher.ts`) instead of C++ headers.
- `--fletcher_opt=ipc` — additionally write one serialized Arrow IPC schema file per message, named `<stem>.<Message>.ipc`, next to the generated headers. Each file is a schema-only [Arrow IPC stream](https://arrow.apache.org/docs/format/Columnar.html#ipc-streaming-format), byte-identical to the schema bytes Fletcher providers announce at runtime, and readable by any Arrow implementation (e.g. `pyarrow.ipc.open_stream`).

Options combine comma-separated: `--fletcher_opt=ts,ipc`.

## Conan package (C++ consumers)

The `fletcher-protoc` package ships only the plugin executable (self-contained — protobuf is statically linked into it) and a CMake module. The `protoc` compiler itself is **not** included: it comes from the `protobuf` Conan package, and Conan does not propagate the plugin's protobuf requirement as a CMake target to downstream consumers. So your conanfile needs both:

```python
def requirements(self):
    self.requires("fletcher-protoc/<version>")
    self.requires("protobuf/3.21.12")  # provides protoc; match the plugin's pinned version
```

When consumed as a Conan package, `fletcher-protoc` provides an imported CMake target:

```cmake
find_package(fletcher-protoc REQUIRED)
find_package(protobuf REQUIRED)

# protoc requires the output directory to exist — it does not create it.
file(MAKE_DIRECTORY "${GENERATED_DIR}")

add_custom_command(
    OUTPUT  "${GENERATED_DIR}/${stem}.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/${stem}.proto"
    DEPENDS "${PROTO_DIR}/${stem}.proto" fletcher-protoc::plugin
)
```

See [`test_package/`](test_package/) for a complete, working consumer example — its [`conanfile.py`](test_package/conanfile.py) and [`CMakeLists.txt`](test_package/CMakeLists.txt) exercise exactly this flow (C++ and TypeScript generation) and run on every `conan create`.

## npm package (TypeScript / JS consumers)

For TypeScript / JavaScript projects that consume Fletcher topics via `@eiva/fletcher-gateway-client`, the plugin ships as the [**`@eiva/protoc-gen-fletcher`**](https://www.npmjs.com/package/@eiva/protoc-gen-fletcher) npm package — a Node shim that downloads the platform-matching native binary from this repo's GitHub Releases on first invocation and exposes it as `protoc-gen-fletcher` in `node_modules/.bin/`.

```bash
npm install --save-dev @eiva/protoc-gen-fletcher
```

The full install + `proto:gen` + `prebuild` recipe, plus the `tsconfig` paths alias and platform-support matrix, lives in the [npm package's README](npm/README.md) (which is also what npm.js shows on the registry page). The integration test at [`integration-tests/protoc-gen-fletcher-npm/`](../integration-tests/protoc-gen-fletcher-npm/) exercises the consumer flow end-to-end on every PR.

> **Not** [`@grpc/proto-loader`](https://www.npmjs.com/package/@grpc/proto-loader): that package loads `.proto` files at **runtime** into JavaScript objects via `protobufjs` reflection — a different problem from code generation. It does not produce the typed `TypedSchema<T>` objects that `FletcherClient.publish`/`subscribe` expect.

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
