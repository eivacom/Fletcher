# Integration test — `@eiva/protoc-gen-fletcher` consumer flow

End-to-end integration test that exercises Fletcher's protoc plugin from a npm consumer's perspective. The test never depends on published versions — it installs both `@eiva/protoc-gen-fletcher` (from this branch's `protoc/npm/` source) and `@eiva/fletcher-gateway-client` (built locally from `gateway-client-ts/`) via `file:` specs, so it reflects the current state of both packages, not whatever happens to be on the registry.

## What it covers

For the smallest end-to-end loop a downstream user would actually run:

1. **Local plugin build**: `conan create protoc/.` produces the statically-linked `fletcher-protoc` binary (static is the default; matches the npm release).
2. **Cache priming**: the binary is copied into the shim's lookup path (`~/.cache/protoc-gen-fletcher/<version>/protoc-gen-fletcher-linux-x64`) so the shim's runtime download path becomes a cache hit. No network call — the test is hermetic.
3. **`npm install`** resolves the deps declared in `package.json`: `@eiva/protoc-gen-fletcher` and `@eiva/fletcher-gateway-client` from the local working copies (via `file:` specs — `../../protoc/npm` and `../../gateway-client-ts`), plus `@protobuf-ts/protoc` from the public registry. The generated `.fletcher.ts` files import `@eiva/fletcher-gateway-client` directly, so it resolves via standard Node resolution (no alias trickery).
4. **`npm run build`** triggers the `prebuild` hook (`npm run proto:gen`) which invokes `protoc` via `@protobuf-ts/protoc` with our shim wired as the `protoc-gen-fletcher=` plugin; the shim hits the primed cache and exec's the native binary. `tsc` then compiles the generated `.fletcher.ts` against `src/main.ts`.
5. **`npm test`** runs the compiled program. It asserts that the generated `TypedSchema<IGreeting>` const carries the expected `protoPackage` / `protoMessage` / `fields.length`, and that the emitted `IGreeting` interface compiles for a real value.

A failing step blocks PR merge — the same coverage a downstream npm consumer would get on `npm install` + `npm run build`.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer. From the repo root:

### 1. Build the protoc plugin into the local Conan cache (statically linked)

```bash
conan create protoc/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

### 2. Prime the npm shim's cache with that binary

```bash
VERSION=$(node -p "require('./protoc/npm/package.json').version")
BIN=$(find ~/.conan2/p -path '*/p/bin/fletcher-protoc' -type f -print -quit)
mkdir -p ~/.cache/protoc-gen-fletcher/"$VERSION"
cp "$BIN" ~/.cache/protoc-gen-fletcher/"$VERSION"/protoc-gen-fletcher-linux-x64
chmod +x ~/.cache/protoc-gen-fletcher/"$VERSION"/protoc-gen-fletcher-linux-x64
```

### 3. Build gateway-client-ts (the local `@eiva/fletcher-gateway-client` dependency)

The `file:../../gateway-client-ts` dependency is consumed as its built `dist/`, so build it first:

```bash
( cd gateway-client-ts && npm ci && npm run build )
```

### 4. Install + build + test

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
