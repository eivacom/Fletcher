# BIND — Rust + C# Native Bindings — Execution Plan

Round plan + tracker for full Rust and C# support through bindings to the
Fletcher C++ implementation.
Spec: [docs/native-language-bindings-spec.md](../docs/native-language-bindings-spec.md).
This file is both the `plan_path` (the tracker) and the `user_stories_path`.

> **Planned restructuring (recorded 2026-07-10, not yet applied).** This round is
> to be **split into two**: **BIND-C# first**, then **BIND-Rust**. BIND-C# is the
> first non-C++ backend over the generator IR (see the GIR round /
> `docs/robustness-plan.md`), so it proves the IR is language-neutral; BIND-Rust
> follows and establishes the Rust logical-type table. The generator roadmap is
> **GIR → BIND-C# → BIND-Rust → RIR**, where **RIR**
> ([RIR-rba-onto-ir.md](RIR-rba-onto-ir.md)) reconciles the RBA accessor onto the
> IR **after BIND-Rust** (it reuses that Rust table) and retires `FieldKind`.
> Also note: **BIND-1..4 (ABI + runtime crates) and the generated-emitter items
> (5/6) are gated after GIR's IR** — do not start the Rust/C# *row* emitters on the
> flat generator. The 10-item breakdown below predates this split and the IR;
> re-decompose per language at kickoff.

## Goal

Make Rust and C# first-class Fletcher consumer languages without reimplementing
Fletcher in either language. The C++ implementation remains the source of truth;
a new stable C ABI facade wraps the existing C++ components; Rust and C# expose
safe, idiomatic packages and generated `.proto` bindings on top of that ABI.

Definition of "full support" for this round:

- generated typed row APIs in Rust and C#;
- encode/decode parity with generated C++;
- generated topic/publisher/subscriber helpers;
- pub/sub through existing C++ provider implementations;
- server-tier Arrow batch exchange through an explicitly chosen portable boundary
  (minimum: Arrow IPC; stretch: Arrow C Data Interface);
- installable Rust and .NET packages;
- Linux + Windows CI.

Existing `--fletcher_opt=rust` RecordBatch accessors are not removed. They become
one Rust-side read path, but they are not sufficient for full Rust support because
they do not cover generated row classes, services, publisher/subscriber helpers,
or native runtime packaging.

## Branch strategy

- Branch: **`feature/native-language-bindings`**, based on a branch that already
  contains RBA if the Rust accessor path is kept in the generated Rust package.
- DICT is not a hard prerequisite unless dictionary columns are required in the
  first Arrow batch milestone. If DICT lands first, BIND must include dictionary
  parity fixtures in the cross-language tests.
- No PR until the round is green and reviewed; the PR/merge is the user's step.

## Sequencing

Strictly linear through the ABI and row layers; pub/sub and packaging must not
start until row encode/decode is green in both languages:

```
BIND-1 ABI slice + ownership rules  ->  BIND-2 C ABI row runtime  ->
BIND-3 Rust sys/safe crates          ->  BIND-4 C# P/Invoke runtime ->
BIND-5 generator: Rust rows/services ->  BIND-6 generator: C# rows/services ->
BIND-7 pub/sub callbacks/providers   ->  BIND-8 Arrow batch exchange ->
BIND-9 packaging + CI matrix         ->  BIND-10 docs + capstone parity
```

---

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)

| Item | Title | Forcing test | Status |
|------|-------|--------------|--------|
| BIND-1 | ABI design spike + locked ownership rules | `c_api_abi_smoke` loads DLL/SO and round-trips version/error APIs | ⚪ |
| BIND-2 | C ABI row runtime | `CApiRowTest.EncodeDecodeMatchesCppGeneratedRow` | ⚪ |
| BIND-3 | Rust runtime crates | `cargo test -p fletcher` row encode/decode parity | ⚪ |
| BIND-4 | C# runtime library | `dotnet test Fletcher.Tests` row encode/decode parity | ⚪ |
| BIND-5 | Rust generator output | `protoc-gen-fletcher-rust-full` generated fixture test | ⚪ |
| BIND-6 | C# generator output | `protoc-gen-fletcher-csharp` generated fixture test | ⚪ |
| BIND-7 | Pub/sub bindings | `RustCSharpPubSubTest.CrossLanguageRowsOverCppProvider` | ⚪ |
| BIND-8 | Arrow batch exchange | `BindingsArrowTest.RecordBatchVisibleInRustAndCSharp` | ⚪ |
| BIND-9 | Packaging + CI | Linux/Windows CI consumes Conan + Cargo + NuGet artifacts | ⚪ |
| BIND-10 | Docs + capstone parity | C++/Rust/C# capstone reads same fixture and bytes | ⚪ |

Expected suite shape:

- new native component `c-api/` or `bindings/c-api/` with C/C++ tests;
- new Rust workspace under `bindings/rust/`;
- new .NET solution under `bindings/csharp/`;
- new generator integration fixtures shared across C++, Rust, and C#;
- new CI workflows for C ABI, Rust bindings, C# bindings, and cross-language
  integration.

---

## Items

### BIND-1 — ABI design spike + locked ownership rules

**Story.** As a binding author, I have a narrow, stable C ABI contract that can
be safely wrapped by Rust and C# without depending on C++ compiler ABI details.

**Scope.**

- Add the `docs/native-language-bindings-spec.md` ABI section to a locked
  decision file before implementation starts.
- Decide component layout: `c-api/` top-level component vs `bindings/c-api/`.
- Decide symbol visibility/export macros and ABI versioning.
- Define opaque handle conventions, error/status API, buffer ownership,
  string ownership, callback threading, and shutdown behavior.
- Decide the Arrow exchange target for BIND-8: Arrow IPC minimum vs Arrow C Data
  stretch.

**Forcing test.** A tiny C executable dynamically loads the built library, calls
`fletcher_c_version()`, creates/frees an error object, and verifies no C++ symbols
or exceptions are required by the consumer.

**Acceptance.** The ABI rules are documented and reviewed; every later item has
a clear ownership contract to implement against.

### BIND-2 — C ABI row runtime

**Story.** A C consumer can encode and decode generated Fletcher rows through the
C++ implementation using only opaque handles and byte buffers.

**Scope.**

- Implement the first production C ABI library.
- Wrap generated C++ row encode/decode through a schema/message descriptor
  handle model that generated Rust/C# code can call.
- Expose row buffer allocation/free and error APIs.
- Add C/C++ tests proving byte-for-byte parity with generated C++ fixtures.
- Keep this layer independent of Rust and C# build systems.

**Forcing test.** `CApiRowTest.EncodeDecodeMatchesCppGeneratedRow` builds a row
with representative scalar, nullable, bytes, list/map/struct, and WKT fields,
encodes via C++, decodes/encodes via the C ABI path, and compares bytes and field
values.

**Acceptance.** Row parity is green without Rust or C# in the loop; every failure
path returns a status/error, never an exception across the ABI.

### BIND-3 — Rust runtime crates

**Story.** Rust consumers can use a safe runtime crate that owns C ABI handles
correctly and exposes Fletcher errors as Rust results.

**Scope.**

- Add `bindings/rust/fletcher-sys` for raw FFI declarations.
- Add `bindings/rust/fletcher` for safe wrappers.
- Implement `Drop`, error conversion, buffer views, owned buffers, and callback
  panic guards.
- Pin native-runtime discovery for local tests: explicit env var first, loud
  Conan/local fallback second, matching the existing Rust integration style.

**Forcing test.** `cargo test -p fletcher` encodes/decodes the shared fixture
through the safe Rust API and asserts byte parity with committed C++ fixture
bytes.

**Acceptance.** No generated Rust code yet; the runtime crate is independently
usable and has clear unsafe boundaries isolated to `fletcher-sys`.

### BIND-4 — C# runtime library

**Story.** .NET consumers can use a safe runtime library that owns native handles
with `SafeHandle`-style lifetime management and exposes Fletcher errors as .NET
exceptions or result objects.

**Scope.**

- Add `bindings/csharp/Fletcher.Native` for internal P/Invoke declarations.
- Add `bindings/csharp/Fletcher` for public runtime APIs.
- Implement native library loading, `SafeHandle` wrappers, buffer spans, string
  marshalling, and callback delegate rooting.
- Add test matrix for `net8.0` or the chosen LTS target.

**Forcing test.** `dotnet test Fletcher.Tests` encodes/decodes the shared fixture
through the public C# runtime and asserts byte parity with committed C++ fixture
bytes.

**Acceptance.** No generated C# code yet; the runtime library is independently
usable and native handles are not leaked in normal success/error paths.

### BIND-5 — Generator: Rust rows/services

**Story.** As a Rust user, I can run `protoc-gen-fletcher` and get typed Rust
message and service helpers backed by the safe Rust runtime.

**Scope.**

- Add a new generator option token, e.g. `--fletcher_opt=rust_full`, or define
  how it coexists with the current `rust` RecordBatch accessor token.
- Emit Rust structs/builders, encode/decode implementations, topic constants,
  and publisher/subscriber helper types.
- Map proto optionality and Fletcher type mapping into idiomatic Rust types.
- Reuse existing Rust accessor output where appropriate, but keep row/service
  support as a distinct generated surface.

**Forcing test.** A Rust integration crate generates from the shared fixtures at
build time, builds typed rows, publishes through a fake/native provider where
available, decodes bytes, and asserts parity with C++ generated code.

**Acceptance.** Generated Rust covers the same representative message shapes as
the C++ generator for the row/service layer.

### BIND-6 — Generator: C# rows/services

**Story.** As a C# user, I can run `protoc-gen-fletcher` and get typed C#
message and service helpers backed by the C# runtime package.

**Scope.**

- Add a generator option token, e.g. `--fletcher_opt=csharp`.
- Emit C# classes/records, encode/decode methods, topic constants, and
  publisher/subscriber helper types.
- Decide generated namespace mapping from proto package names.
- Map nullable fields to idiomatic nullable reference/value types and keep
  `#nullable enable` output clean.

**Forcing test.** A .NET integration project generates from the shared fixtures,
builds typed rows, decodes bytes produced by generated C++, and asserts parity.

**Acceptance.** Generated C# covers the same representative message shapes as the
C++ generator for the row/service layer.

### BIND-7 — Pub/sub callbacks/providers

**Story.** Rust and C# generated service helpers can publish and subscribe using
the existing C++ provider implementations.

**Scope.**

- Extend the C ABI with provider/publisher/subscriber handles.
- Bind callback registration with explicit threading and lifetime rules.
- Add a deterministic in-memory provider test path first; then validate against
  at least one real provider available in CI.
- Ensure attachments are represented consistently in both languages.

**Forcing test.** `RustCSharpPubSubTest.CrossLanguageRowsOverCppProvider` sends
rows C++ -> Rust, Rust -> C#, and C# -> C++ through the same provider abstraction,
asserting schema/topic agreement and row byte parity.

**Acceptance.** Cross-language pub/sub works without each language implementing a
transport provider.

### BIND-8 — Arrow batch exchange

**Story.** Rust and C# consumers can access server-tier batches produced by the
C++ implementation through a portable Arrow boundary.

**Scope.**

- Implement the BIND-1 Arrow decision:
  - minimum: Arrow IPC schema/record-batch bytes;
  - stretch: Arrow C Data Interface import/export.
- Add Rust integration using the pinned/declared `arrow` crate.
- Add C# integration using Apache.Arrow.
- Define behavior for dictionary columns, metadata, and nested fields.

**Forcing test.** `BindingsArrowTest.RecordBatchVisibleInRustAndCSharp` creates
or subscribes to the same fixture batch in C++, imports it in Rust and C#, and
asserts schema, metadata, nulls, nested values, and representative scalar values.

**Acceptance.** Arrow transfer is documented as either copy-via-IPC or zero-copy
where supported; both languages have a green batch test.

### BIND-9 — Packaging + CI matrix

**Story.** Consumers can install the native runtime and language packages through
normal package workflows, and CI proves the packages work together.

**Scope.**

- Add Conan packaging for the C ABI runtime.
- Add Cargo package metadata and native library discovery.
- Add NuGet package metadata and native asset layout.
- Add Linux and Windows workflows:
  - build C ABI;
  - test Rust runtime + generated fixtures;
  - test C# runtime + generated fixtures;
  - run cross-language capstone.

**Forcing test.** CI builds packages from source, then runs consumer tests from a
clean integration directory that depends on packages rather than source-relative
headers or project files.

**Acceptance.** Local dev still works, but official CI exercises the consumer
path that users will rely on.

### BIND-10 — Docs + capstone parity

**Story.** Users and maintainers can understand the binding architecture, support
matrix, package installation flow, and exact parity guarantees.

**Scope.**

- Update README generated-file table and repository layout.
- Add Rust and C# quick starts.
- Document ABI stability policy and native-runtime version matching.
- Add a capstone fixture shared by C++, Rust, and C#.
- Record known limitations and follow-ups.

**Forcing test.** A capstone test runs the same fixture through generated C++,
generated Rust, and generated C#, comparing schema, encoded bytes, decoded
values, pub/sub delivery, and Arrow batch visibility.

**Acceptance.** Docs and capstone define the supported surface. Anything not in
the capstone is explicitly called out as a limitation or follow-up.

---

## Size estimate relative to DICT and RBA

This is materially larger than both DICT and RBA.

| Round | Shape | Relative size |
|---|---|---|
| DICT | 6 items; one generator feature plus focused runtime/schema tests | 1.0x baseline |
| RBA | 7 items; substantial generator work plus Rust accessor crate and parity tests | about 1.7-2.0x DICT |
| BIND | 10 items; new ABI component, two language runtimes, two generated surfaces, pub/sub, Arrow, packaging, CI | about 3.0-4.0x DICT; about 1.8-2.3x RBA |

Why:

- DICT mostly changes existing generator/type-mapping paths and adds tests.
- RBA adds a significant generated API surface, but it is still a generator-side
  feature with one Rust integration crate.
- BIND adds new binary compatibility surface area, new component packaging, two
  separate language ecosystems, native library loading, callback lifetimes,
  cross-language pub/sub, and Arrow exchange.

Pragmatic split recommendation:

- **BIND-A (ABI + row support):** BIND-1..6. Roughly RBA-sized.
- **BIND-B (pub/sub + Arrow + packaging):** BIND-7..10. Roughly DICT-to-RBA sized
  depending on the Arrow boundary choice.

Doing it as one PR-sized round is possible but high risk. Treating BIND-A as the
first mergeable milestone gives users useful Rust/C# generated row support while
de-risking the ABI before callbacks and Arrow lifetimes are added.

## Definition of done

All ten forcing tests are green; Rust and C# generated code round-trip the same
fixtures as C++; cross-language pub/sub works through the C++ provider
abstraction; Arrow batch exchange is green in both languages; packages are
consumed from clean integration tests on Linux and Windows; docs state the ABI,
package, and versioning contracts.

