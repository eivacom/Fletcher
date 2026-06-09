# Integration test — fletcher-protoc command line (no npm packages)

End-to-end test that verifies the locally-built `fletcher-protoc` plugin works **directly with Google's official `protoc.exe`** through the standard `--plugin=` command-line interface — the scenario every non-npm consumer of the plugin hits.

## Why this exists

The sibling test `integration-tests/protoc-gen-fletcher-npm/` exercises the npm-consumer path: install `@eiva/protoc-gen-fletcher`, let the shim download the platform binary, run `protoc` resolved by `npm`'s `node_modules/.bin/` PATH. That path is opinionated — it relies on npm peer-deps and the shim's lazy-download logic.

This test exercises the **raw plugin path**:

- `protoc.exe` is the unmodified Google release (downloaded from [`protocolbuffers/protobuf` releases](https://github.com/protocolbuffers/protobuf/releases)).
- `fletcher-protoc[.exe]` is the binary out of the local Conan cache, built **statically** (no transitive runtime DLLs).
- The test invokes `protoc --plugin=protoc-gen-fletcher=<path-to-fletcher-protoc> ...` — exactly the command shape used by a developer who downloaded the binary and dropped it somewhere.

The third test case ("runs standalone when copied to a clean directory with no co-located DLLs") is the regression guard against the Windows bug we hit in PR #82: a `shared`-linked plugin failed with `STATUS_ENTRYPOINT_NOT_FOUND` because `libprotobuf.dll` / `libprotoc.dll` / `zlib1.dll` weren't next to the `.exe`. The fix is to link the binary statically (static is the build default, and the `protoc` recipe pins protobuf static) so all dependencies are linked in — exactly how Google's own `protoc.exe` is shipped.

## What it covers

For one canonical `.proto` (`proto/simple.proto`):

| # | Scenario | Asserts |
|---|---|---|
| 1 | `protoc --plugin=protoc-gen-fletcher=<plugin> --fletcher_out=<dir> simple.proto` | Default C++ output: `simple.fletcher.pb.h` exists |
| 2 | … + `--fletcher_opt=ts` | TypeScript output: `simple.fletcher.ts` exists and contains `export const Simple`, `protoPackage`, `protoMessage` |
| 3 | Plugin copied to a clean tmp dir with **no nearby DLLs**, then invoked | Plugin returns exit 0 — proves the binary is standalone |

## How it runs in CI

The workflow [`.github/workflows/ci.integration-test.protoc-plugin-cli.yml`](../../.github/workflows/ci.integration-test.protoc-plugin-cli.yml) runs on every PR that touches `protoc/**` or this directory. Each job (Linux + Windows):

1. Builds the plugin **statically** via `conan create protoc/. --build=missing -pr:a=<profile>`. Static is the default (the `protoc` recipe pins protobuf static), so protobuf, zlib, and any other runtime-DLL-producing transitive deps are statically linked into `fletcher-protoc[.exe]`.
2. `cd integration-tests/protoc-plugin-cli`.
3. `npm ci`.
4. `npm test` — vitest discovers the binary in `~/.conan2/`, downloads Google's `protoc` once into a project-local `.cache/`, then runs the three assertions above.

No environment variables are injected — the test discovers both binaries itself. Step (1) is the only build orchestration the workflow performs.

## Running locally

The same flow works in the devcontainer (Linux) and on a native Windows machine. Conan profiles live in [`.conan-profiles/`](../../.conan-profiles) and are referenced by relative path — no profile-install step needed.

### Build the plugin (statically linked)

From the repo root:

**Linux (devcontainer):**

```bash
conan create protoc/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

**Windows (native):**

```powershell
conan create protoc/. --build=missing -pr:a=.conan-profiles/Windows-msvc194-x86_64-Release
```

Static is the default (the `protoc` recipe pins protobuf static), so every dependency (protobuf, zlib, abseil if present) is built static. The first time you run this Conan builds protobuf from source, which takes a few minutes; subsequent invocations hit the cache.

### Run the test

```bash
cd integration-tests/protoc-plugin-cli
```

```bash
npm ci
```

```bash
npm test
```

First-run `npm test` downloads Google's `protoc-21.12` into `integration-tests/protoc-plugin-cli/.cache/protoc/21.12/`. Subsequent runs find the cached binary and skip the download. The directory is git-ignored. (Google switched to date-based version tags at major bump 21; the matching C++ runtime is `protobuf/3.21.12` — same wire format, different naming convention.)

### Iterating after a plugin change

Re-run the static `conan create` above, then `npm test`. The test's `findFletcherProtoc()` always picks the most recently modified `fletcher-protoc[.exe]` in the Conan cache, so iterations are transparent — no need to clean caches or pass paths.

## Files

```
integration-tests/protoc-plugin-cli/
├── package.json            # vitest + @types/node + typescript — nothing else
├── tsconfig.json
├── vitest.config.ts
├── proto/
│   └── simple.proto        # scalar + repeated scalar; exercises typed-schema field plumbing
├── test/
│   ├── basic.test.ts       # the three CLI scenarios above
│   └── util/
│       ├── ensure-protoc.ts        # downloads Google's protoc into .cache/
│       └── find-fletcher-protoc.ts # discovers the locally-built plugin in ~/.conan2/
└── README.md
```

## License

LGPL-3.0-or-later © The Fletcher Authors.
