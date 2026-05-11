# Integration test — protoc + arrow-bridge

End-to-end test that verifies **byte-format compatibility** between:

- `eiva-fletcher-protoc`-generated row classes (their `Encode()` and `EncodedRow`-taking constructor)
- `eiva-fletcher-arrow-bridge`'s `Codec` (its `EncodeRow()` / `DecodeRow()` over `arrow::Schema` + `ArrowRow`)

Both implement the same positional wire format documented in `arrow-bridge/include/arrow_bridge/codec.hpp`. This test is the proof that they actually agree on every byte.

## What it covers

- Generated `Encode()` produces byte-identical output to `Codec.EncodeRow()` for the same row content + schema.
- `Codec.DecodeRow()` correctly round-trips bytes produced by generated `Encode()`.
- The generated row class's `EncodedRow` constructor correctly round-trips bytes produced by `Codec.EncodeRow()`.

Why this matters: an edge device using the typed proto-row classes (no Apache Arrow C++) sends bytes that a server-side codec (with full Apache Arrow C++) must decode without any translation layer. Each component has its own unit tests, but only this integration test verifies their outputs agree.

## How it runs in CI

The workflow `.github/workflows/integration-test.protoc-arrow-bridge.yml` triggers when any of `core/**`, `protoc/**`, `arrow-bridge/**`, `pubsub/**` or this directory changes on a PR. It:

1. Builds each component locally via `conan create <component>/.`, putting the branch's in-flight versions in the Conan cache.
2. `conan install`s this directory — version ranges (`[*]`) resolve to whatever just landed in the cache, never the published versions on conan-eiva.
3. Configures + builds + runs `byte_compat_tests` via gtest.

That way a PR can change protoc and arrow-bridge in the same commit, and the integration test verifies they still agree on bytes — no need to publish first.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). The `postCreateCommand` runs `conan config install` automatically — no `conan-eiva` login is needed because the integration test builds every component from this branch's source into the local Conan cache, and `conan install` resolves against that cache.

### Build the components from this branch into the local cache

The integration test should run against this branch's component code, not whatever happens to be on conan-eiva. From the repo root:

```bash
cd /workspaces/Fletcher
```

```bash
conan create core/.         --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create arrow-bridge/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create pubsub/.       --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create protoc/.       --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

Each `conan create` builds the component and registers the branch's version in `~/.conan2/p/`. Subsequent `conan install` calls find them there.

### Build the integration test

```bash
cd /workspaces/Fletcher/integration-tests/protoc-arrow-bridge
```

```bash
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
cmake --preset conan-release
```

```bash
cmake --build --preset conan-release
```

The last step produces the `integration_tests` gtest binary plus the protoc-generated headers under `build/Release/generated/<stem>.fletcher.pb.h` and `<stem>.fletcher.arrow.pb.h`.

### Run the tests

```bash
ctest --preset conan-release --output-on-failure
```

### Iterating after a component change

Re-run only the `conan create` for the component you touched, then redo `conan install` + `cmake --build` for the integration test. Conan picks up the latest version in the local cache automatically.
