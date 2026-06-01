# Integration test — `@eiva/protoc-gen-fletcher` consumer flow

End-to-end integration test that exercises Fletcher's protoc plugin from a npm consumer's perspective. The test never depends on a published version of `@eiva/protoc-gen-fletcher` — it installs the shim from this branch's `protoc/npm/` source so the test reflects the current state of the npm package, not whatever happens to be on the registry.

## What it covers

For the smallest end-to-end loop a downstream user would actually run:

1. **Local plugin build**: `conan create protoc/. -o "*:shared=False"` produces the statically-linked `fletcher-protoc` binary (matching the npm release).
2. **Cache priming**: the binary is copied into the shim's lookup path (`~/.cache/protoc-gen-fletcher/<version>/protoc-gen-fletcher-linux-x64`) so the shim's runtime download path becomes a cache hit. No network call — the test is hermetic.
3. **`npm install`** resolves the deps declared in `package.json`: `@eiva/protoc-gen-fletcher` from the local working copy (via a `file:../../protoc/npm` path spec), `@protobuf-ts/protoc` from the public registry, and the published `@eiva/fletcher-gateway-client` installed under the bare alias `fletcher-gateway-client` so the generated code's bare `'fletcher-gateway-client'` import resolves at runtime.
4. **`npm run build`** triggers the `prebuild` hook (`npm run proto:gen`) which invokes `protoc` via `@protobuf-ts/protoc` with our shim wired as the `protoc-gen-fletcher=` plugin; the shim hits the primed cache and exec's the native binary. `tsc` then compiles the generated `.fletcher.ts` against `src/main.ts`.
5. **`npm test`** runs the compiled program. It asserts that the generated `TypedSchema<IGreeting>` const carries the expected `protoPackage` / `protoMessage` / `fields.length`, and that the emitted `IGreeting` interface compiles for a real value.

A failing step blocks PR merge — the same coverage a downstream npm consumer would get on `npm install` + `npm run build`.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer. From the repo root:

### 1. Build the protoc plugin into the local Conan cache (statically linked)

```bash
conan create protoc/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release -o "*:shared=False"
```

### 2. Prime the npm shim's cache with that binary

```bash
VERSION=$(node -p "require('./protoc/npm/package.json').version")
BIN=$(find ~/.conan2/p -path '*/p/bin/fletcher-protoc' -type f -print -quit)
mkdir -p ~/.cache/protoc-gen-fletcher/"$VERSION"
cp "$BIN" ~/.cache/protoc-gen-fletcher/"$VERSION"/protoc-gen-fletcher-linux-x64
chmod +x ~/.cache/protoc-gen-fletcher/"$VERSION"/protoc-gen-fletcher-linux-x64
```

### 3. Install + build + test

```bash
cd integration-tests/protoc-gen-fletcher-npm
```

```bash
npm install
```

```bash
npm run build
```

```bash
npm test
```

A successful run prints `OK: TypedSchema<IGreeting> generated and instantiated correctly`.

## Why prime the cache instead of testing the real download path?

The shim's GitHub-Release download path is exercised on every real `npm install` after a release ships — the test wouldn't gain much by repeating it under fake-URL conditions. By priming the cache with the just-built binary, the test stays hermetic (no outbound network), reflects what a cached consumer's invocation actually looks like, and catches the parts of the pipeline that genuinely depend on this PR: the package layout, the bin shim, the prebuild hook, the protoc invocation, and the TypeScript type contract.
