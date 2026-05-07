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

The workflow `.github/workflows/integration-tests.yml` triggers when any of `core/**`, `protoc/**`, `arrow-bridge/**`, `pubsub/**` or this directory changes on a PR. It:

1. Builds each component locally via `conan create <component>/.`, putting the branch's in-flight versions in the Conan cache.
2. `conan install`s this directory — version ranges (`[*]`) resolve to whatever just landed in the cache, never the published versions on conan-eiva.
3. Configures + builds + runs `byte_compat_tests` via gtest.

That way a PR can change protoc and arrow-bridge in the same commit, and the integration test verifies they still agree on bytes — no need to publish first.

## Running locally

Open in the devcontainer (`integration-tests/protoc-arrow-bridge/.devcontainer`), then:

```bash
# From the repo root:
conan create core/.        --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create protoc/.      --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create arrow-bridge/. --build=missing -pr:a=Ubuntu22-gcc-12-Release

# In this directory:
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Release
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```
