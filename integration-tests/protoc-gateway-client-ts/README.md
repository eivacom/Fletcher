# Integration test — protoc + gateway-client-ts (cross-language)

End-to-end test that verifies **byte-format compatibility** between the C++ and TypeScript sides of Fletcher:

- C++ side: `protoc-gen-fletcher`-generated row classes (`<stem>.fletcher.pb.h`) + the `Encode()` method.
- TypeScript side: `protoc-gen-fletcher`-generated bindings (`<stem>.fletcher.ts`) + the codec exported from `eiva-fletcher-gateway-client`.

Both implement the same positional wire format. This test is the proof that they actually agree on every byte across the language boundary, in both directions.

## What it covers

For every scenario:

- **Forward decode**: C++ `Encode()` produces bytes; TS `ObjectBackend.decode()` decodes them; values match the original input.
- **Byte equality**: TS `encodePositional()` produces bytes; the C++ bytes are byte-identical.

The companion test in `integration-tests/protoc-arrow-bridge/` proves the same invariant for `protoc + arrow-bridge` within C++. Together they cover all three directions: protoc → arrow-bridge::Codec, protoc → TS codec, TS codec ↔ arrow-bridge::Codec.

## How it runs in CI

The workflow `.github/workflows/integration-tests.yml` triggers when any of `core/**`, `protoc/**`, `pubsub/**`, `gateway-client-ts/**`, or this directory changes on a PR. It:

1. Builds each component locally via `conan create <component>/.`, putting the branch's in-flight versions in the Conan cache.
2. `conan install` + `cmake build` produces the `emit_vectors` C++ binary that emits canonical scenario vectors as JSON lines.
3. `npm ci` + `npm test` runs vitest against the binary — the TS test spawns it, parses the vectors, and asserts both decode-correctness and byte-equality.

## Running locally

Open in the devcontainer (`integration-tests/protoc-gateway-client-ts/.devcontainer`), then:

```bash
# From the repo root, build the C++ deps:
conan create core/.   --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create pubsub/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create protoc/. --build=missing -pr:a=Ubuntu22-gcc-12-Release

# In this directory:
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Release
cmake --preset conan-release
cmake --build --preset conan-release   # produces build/Release/emit_vectors

# Run the TS test:
npm ci
npm test
```
