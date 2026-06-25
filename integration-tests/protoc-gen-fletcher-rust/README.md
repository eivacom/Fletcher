# Integration test — `protoc-gen-fletcher` Rust accessor consumer flow

End-to-end integration test that exercises Fletcher's protoc plugin Rust output
(`--fletcher_opt=rust`) from a Rust consumer's perspective, against the official
`arrow` crate (arrow-rs). Like the npm consumer test, it never depends on a
published artifact: it consumes the **locally built** `protoc-gen-fletcher`
plugin, generates from fixture `.proto`s at build time, assembles the generated
files into a `fletcher_gen` module tree, and `cargo test`s the result.

## What it covers

1. **Local plugin build**: `conan create protoc/.` produces the
   `fletcher-protoc` binary.
2. **Generation at build time**: `build.rs` runs the plugin with
   `--fletcher_opt=rust` on each fixture `.proto`, emitting `<stem>.fletcher.rs`
   into `OUT_DIR`. No network, no download.
3. **Module assembly (D-RBA-10)**: `build.rs` groups the generated bare-item
   files by proto **package** and emits a `fletcher_gen.rs` that declares each
   package `pub mod` once and `include!`s every file of a package into that
   package's single module. `src/lib.rs` mounts it with one `include!`.
4. **`cargo test`**: the test suite builds arrow `RecordBatch` / `StructArray`
   fixtures and asserts the generated `<Class>Accessor` reads/validates them
   (positional scalar reads, nullable `Option`, type-mismatch → `Err`,
   name-tolerance, `from_struct` child-slicing at a non-zero offset, wrong column
   count → `Err`, runtime null in a non-nullable column → `Err`, and same-package
   multi-file mounting).

This crate is also the **authoritative Rust well-formedness check** for the
generated `.fletcher.rs`: it compiles all generated Rust against the pinned
`arrow`. (The plugin's C++ no-drift test asserts the `.fletcher.rs` is emitted +
non-empty and preserves byte-identity of existing outputs, but does not itself
compile arrow-dependent Rust.)

## Plugin discovery (env vars, no network)

`build.rs` finds the tools through two environment variables. Each var is
**authoritative**: if set it is used (and a missing path is a hard, clear error).
If a var is **unset**, a **loud local-dev convenience fallback** is attempted (it
prints a `cargo:warning` naming the resolved path and that it is a fallback);
if the fallback finds nothing, the build fails with a clear, actionable message.
There is no silent fallback and no network download — CI always sets both vars,
so CI never takes the fallback (a stale binary can never mask a regression).

| Env var                  | Meaning                                                        |
| ------------------------ | ------------------------------------------------------------- |
| `PROTOC`                 | Absolute path to `protoc`. Unset → loud fallback: Conan cache, then PATH. |
| `FLETCHER_PROTOC_PLUGIN` | Absolute path to the built `fletcher-protoc` (`.exe` on Windows). Unset → loud fallback: Conan cache. |

The well-known-types include root (`telemetry.proto` imports
`google/protobuf/{timestamp,duration}.proto`) is taken from
`PROTOBUF_WKT_INCLUDE_DIR` if set, else auto-derived from the `protoc` binary
(`<protoc_dir>/../include`), so the fixture stays hermetic. `FLETCHER_PROTO_INCLUDE_DIR`
is honoured when set (the RBA-5 fixtures do not import the fletcher options).

## Running locally

From the repo root:

### 1. Build the plugin into the local Conan cache

```bash
conan create protoc/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

(Windows: `-pr:a=.conan-profiles/Windows-msvc194-x86_64-Release`.)

### 2. Export discovery env vars (or rely on the Conan-cache fallback)

```bash
BIN=$(find ~/.conan2/p -path '*/p/bin/fletcher-protoc' -type f -print -quit)   # .exe on Windows
export FLETCHER_PROTOC_PLUGIN="$BIN"
export PROTOC=$(command -v protoc)   # or an explicit path
```

### 3. Build + test

```bash
cd integration-tests/protoc-gen-fletcher-rust
cargo test
```

The pinned toolchain (`rust-toolchain.toml`, 1.96) and the pinned
`arrow = "=59.0.0"` (`Cargo.toml`) keep the build reproducible. The only network
is `cargo`'s one-time crate fetch of the pinned `arrow` (CI caches it).

## Fixtures

- `proto/telemetry.proto` — `package fletcher.rba.telem;` — flat scalar message
  covering non-null scalar, nullable `Option<&str>`, non-null `&[u8]`, nullable
  `Option<&[u8]>`, plus a `google.protobuf.Timestamp` and `google.protobuf.Duration`
  (→ `Timestamp*Array` / `Duration*Array`, getter `-> i64`) that exercise the
  generic temporal unit-parsing path.
- `proto/sensors_a.proto` + `proto/sensors_b.proto` — both
  `package fletcher.rba.shared;` (different messages) — exercise the same-package
  multi-file mounting (one `pub mod shared` for both files, D-RBA-10).
