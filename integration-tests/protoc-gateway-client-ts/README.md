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

Open the devcontainer at `integration-tests/protoc-gateway-client-ts/.devcontainer`. The `postCreateCommand` runs `conan config install` automatically, but the conan-eiva remote needs credentials before you can pull the released Fletcher packages.

### Log in to conan-eiva (one-time per container)

Inside the devcontainer terminal, log in to the EIVA Artifactory. Replace `<user>` with your Artifactory username; conan will prompt for the password:

```bash
conan remote login conan-eiva <user>
```

Verify the login resolved:

```bash
conan search 'eiva-fletcher-*' -r conan-eiva
```

You should see the published versions of `eiva-fletcher-core`, `eiva-fletcher-pubsub`, `eiva-fletcher-protoc` etc. listed.

### Build the C++ side

The Conan recipe pulls the released `eiva-fletcher-*` packages from `conan-eiva` — no `conan create` of components needed. From this directory:

```bash
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
cmake --preset conan-release
```

```bash
cmake --build --preset conan-release
```

The last step produces `build/Release/emit_vectors` plus the protoc-generated headers under `build/Release/generated/<stem>.fletcher.pb.h` (consumed by the C++ binary) and `generated-ts/<stem>.fletcher.ts` (consumed by vitest).

### Run the TS test

```bash
npm ci
```

```bash
npm test
```

vitest spawns `emit_vectors`, parses the JSON-line output, and asserts both decode-correctness and byte-equality against the hardcoded scenario inputs in `test/byte-compat.test.ts`.
