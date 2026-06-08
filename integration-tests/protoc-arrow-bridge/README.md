# Integration test — protoc + arrow-bridge

End-to-end test that verifies **byte-format compatibility** between:

- `fletcher-protoc`-generated row classes (their `Encode()` and `EncodedRow`-taking constructor)
- `fletcher-arrow-bridge`'s `Codec` (its `EncodeRow()` / `DecodeRow()` over `arrow::Schema` + `ArrowRow`)

Both implement the same positional wire format documented in `arrow-bridge/include/arrow_bridge/codec.hpp`. This test is the proof that they actually agree on every byte.

## What it covers

- Generated `Encode()` produces byte-identical output to `Codec.EncodeRow()` for the same row content + schema.
- `Codec.DecodeRow()` correctly round-trips bytes produced by generated `Encode()`.
- The generated row class's `EncodedRow` constructor correctly round-trips bytes produced by `Codec.EncodeRow()`.

Why this matters: an edge device using the typed proto-row classes (no Apache Arrow C++) sends bytes that a server-side codec (with full Apache Arrow C++) must decode without any translation layer. Each component has its own unit tests, but only this integration test verifies their outputs agree.

## How it runs in CI

The workflow `.github/workflows/ci.integration-test.protoc-arrow-bridge.yml` triggers when any of `core/**`, `protoc/**`, `arrow-bridge/**`, `pubsub/**` or this directory changes on a PR. It:

1. Builds each component locally via `conan create <component>/.`, putting the branch's in-flight versions in the Conan cache.
2. `conan build .` in this directory — version ranges (`[*]`) resolve to whatever just landed in the cache, never the versions published on GitHub Releases.
3. Configures + builds + runs `byte_compat_tests` via gtest.

That way a PR can change protoc and arrow-bridge in the same commit, and the integration test verifies they still agree on bytes — no need to publish first.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Conan profiles live in [`.conan-profiles/`](../../.conan-profiles) and are referenced by relative path — no profile-install step is needed. The integration test builds every component from this branch's source into the local Conan cache, and the `conan build .` step resolves against that cache.

### Build the components from this branch into the local cache

The integration test should run against this branch's component code, not whatever has been published on GitHub Releases. From the repo root:

```bash
cd /workspaces/Fletcher
```

```bash
conan create core/.         --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create arrow-bridge/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create pubsub/.       --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create protoc/.       --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

Each `conan create` builds the component and registers the branch's version in `~/.conan2/p/`. Subsequent `conan build .` calls find them there.

### Build and run the integration test

```bash
cd /workspaces/Fletcher/integration-tests/protoc-arrow-bridge
```

```bash
conan build . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
```

`conan build .` configures, builds the `integration_tests` gtest binary (plus the protoc-generated headers `build/.../generated/<stem>.fletcher.pb.h` and `<stem>.fletcher.arrow.pb.h`), and runs the suite via ctest. Conan drives the configure/build/test preset that matches the active generator, so the same command works on the Linux single-config and Windows multi-config (MSVC) generators.

### Iterating after a component change

Re-run only the `conan create` for the component you touched, then redo `conan build .` for the integration test. Conan picks up the latest version in the local cache automatically.
