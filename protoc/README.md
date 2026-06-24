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
- `--fletcher_opt=accessor` — additionally generate the read-only C++ **RecordBatch accessor** header `<stem>.fletcher.accessor.pb.h` (one `<Class>Accessor` per message). See [RecordBatch accessors](#recordbatch-accessors) below.
- `--fletcher_opt=rust` — additionally generate the read-only **Rust** RecordBatch accessor module `<stem>.fletcher.rs` (the Rust counterpart of `accessor`, targeting the official [`arrow`](https://crates.io/crates/arrow) crate). See [RecordBatch accessors](#recordbatch-accessors) below.

The `accessor` and `rust` tokens are purely additive: they emit only their own new files and never change the bytes of the C++ / TypeScript / IPC outputs, with or without the other options.

Options combine comma-separated: `--fletcher_opt=ts,ipc` or `--fletcher_opt=accessor,rust,ipc`.

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

set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

# protoc requires the output directory to exist — it does not create it.
file(MAKE_DIRECTORY "${GENERATED_DIR}")

add_custom_command(
    OUTPUT  "${GENERATED_DIR}/my_service.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/my_service.proto"
    DEPENDS "${PROTO_DIR}/my_service.proto" fletcher-protoc::plugin
)
```

### Resolving `fletcher/*` and well-known-type imports

The single `-I "${PROTO_DIR}"` above is enough only for self-contained `.proto` files. As soon as a proto imports something that lives **outside** your proto tree, protoc needs an extra import root for it — these imports are resolved against the `-I` roots, not relative to the importing file:

- **The `fletcher/` proto package.** The package's CMake module sets **`FLETCHER_PROTO_INCLUDE_DIR`** to the root under which `fletcher-protoc` ships its public `.proto` files, so adding it as a root resolves any `import "fletcher/<name>.proto"`:

  ```cmake
  "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
  ```

  Today the package ships only `fletcher/options.proto` (the `(fletcher.flatten)` / `(fletcher.flatten_field)` options), but the same root covers any additional `fletcher/*.proto` the package adds later — consumers need no change.

- **`import "google/protobuf/*.proto"`** (Timestamp, Duration, wrappers — and `descriptor.proto`, which `fletcher/options.proto` imports itself, so this root is needed whenever you use Fletcher options). These ship next to the `protoc` binary; locate that include dir from the `protobuf::protoc` target:

  ```cmake
  foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO "")
      if(_cfg)
          get_target_property(_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
      else()
          get_target_property(_loc protobuf::protoc IMPORTED_LOCATION)
      endif()
      if(_loc AND NOT _loc MATCHES "NOTFOUND")
          get_filename_component(_bindir "${_loc}" DIRECTORY)
          get_filename_component(PROTOBUF_WKT_INCLUDE_DIR "${_bindir}/../include" ABSOLUTE)
          break()
      endif()
  endforeach()
  ```

Putting all three roots together:

```cmake
add_custom_command(
    OUTPUT  "${GENERATED_DIR}/my_service.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"   # fletcher/*.proto (options, …)
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"     # google/protobuf/*.proto
            "${PROTO_DIR}/my_service.proto"
    DEPENDS "${PROTO_DIR}/my_service.proto" fletcher-protoc::plugin
)
```

See [`test_package/`](test_package/) for a complete, working consumer example — its [`conanfile.py`](test_package/conanfile.py) and [`CMakeLists.txt`](test_package/CMakeLists.txt) wire up exactly these import roots and run the plugin (C++, TypeScript, and `.ipc` output) on every `conan create`.

## RecordBatch accessors

With `--fletcher_opt=accessor` (C++) and/or `--fletcher_opt=rust` (Rust), the plugin additionally generates a **read-only, column-oriented accessor** per message — `<Class>Accessor` — for reading whole Arrow `RecordBatch`es without the usual cast-and-index boilerplate. Each accessor:

- is **read-only** (no setters, builders, or writer API);
- is **opt-gated** — emitted only when its token is passed, into its own new file (`<stem>.fletcher.accessor.pb.h` / `<stem>.fletcher.rs`), never altering existing outputs;
- **constructs from a `RecordBatch` or an `arrow::StructArray`** (the struct factory is what nested fields use, and is also a public entry point);
- **validates positionally** at construction (column count + each column's Arrow type) and returns a `Result` on mismatch — it never throws/panics. Field names and the schema's nullable *flag* are tolerated; columns the proto marks non-nullable are additionally checked to hold no actual nulls;
- caches the type-checked down-casts **once**, so per-row getters index the concrete arrays directly (no per-cell allocation);
- exposes **generic** schema- and per-field metadata (`schema_metadata()` / `field_metadata(i)`) verbatim, with no built-in knowledge of any key;
- nullable scalars return `std::optional` / `Option`, and struct values that can be null (nullable 1:1 fields, list/map elements, nested-list levels) are returned as optionals — you can never read through a null.

It does **not** add `Table` / `ChunkedArray` input, a mutable accessor, dictionary columns, or any third language. The full contract is the oracle spec [`docs/recordbatch-accessor-spec.md`](../docs/recordbatch-accessor-spec.md).

A minimal C++ read (generate with `--fletcher_opt=accessor`, include `<stem>.fletcher.accessor.pb.h`):

```cpp
#include "my_service.fletcher.accessor.pb.h"

using fletcher_gen::my_pkg::MyRowAccessor;

void Read(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto result = MyRowAccessor::Make(batch);  // validated once; returns arrow::Result
    if (!result.ok()) { /* handle result.status() */ return; }
    const MyRowAccessor& accessor = *result;

    int32_t first_id = accessor.id(0);                       // non-nullable scalar
    std::optional<int32_t> maybe = accessor.opt_count(1);    // nullable -> std::nullopt if null
}
```

> The exact getter names mirror your proto field names; replace `id` / `opt_count` with your own. This snippet is mirrored by a compile-checked integration test (`integration-tests/protoc-arrow-bridge/tests/test_accessor_readme_example.cpp`), and the cross-language C++/Rust parity is proven by the capstone in [`integration-tests/accessor-capstone/`](../integration-tests/accessor-capstone/).

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
