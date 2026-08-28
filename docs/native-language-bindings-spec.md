# Native Language Bindings Specification

Authoritative design for full Rust and C# support through bindings to the
Fletcher C++ implementation.

This spec intentionally treats C++ as the source of truth. Rust and C# bindings
must not reimplement the wire format, schema handling, pub/sub behavior, or Arrow
bridging logic independently. They bind to a stable C ABI facade over the C++
components, then expose idiomatic generated APIs in each language.

---

## 1. Goal

Provide production-grade Rust and C# support for Fletcher:

- generated typed row APIs from `.proto`;
- encode/decode parity with generated C++;
- publish/subscribe through existing C++ providers;
- Arrow server-tier access where practical, without copying row data unless the
  target runtime requires it;
- installable packages for normal Rust and .NET consumers;
- CI coverage on Linux and Windows.

The binding stack has three layers:

| Layer | Owner | Purpose |
|---|---|---|
| C++ implementation | existing Fletcher components | Source of truth for row encoding, schema, pub/sub, Arrow bridge |
| C ABI facade | new binding runtime | Stable, compiler-neutral ABI boundary over C++ objects |
| Rust / C# packages | new language packages + generated code | Idiomatic safe APIs, lifetime management, package distribution |

The C ABI is the only binary boundary. Rust and C# do not bind to C++ symbols,
templates, exceptions, STL types, `std::shared_ptr`, or Arrow C++ classes
directly.

## 2. Scope

### In scope

- A new C ABI runtime library, e.g. `fletcher-c-api`, wrapping:
  - core row buffers and positional encode/decode;
  - generated message schema creation and topic metadata;
  - publisher/subscriber creation on top of existing C++ provider instances;
  - callback delivery of encoded rows and attachments;
  - error/status handling;
  - optional Arrow integration through Arrow IPC and/or Arrow C Data Interface.
- `protoc-gen-fletcher` output for:
  - Rust typed row bindings;
  - C# typed row bindings;
  - generated topic/publisher/subscriber helpers per service method.
- Rust package(s):
  - low-level `fletcher-sys` over the C ABI;
  - safe `fletcher` crate;
  - generated code that depends on the safe crate.
- C# package(s):
  - low-level internal P/Invoke layer;
  - public `Fletcher` .NET library;
  - generated `.cs` message/service bindings.
- Packaging:
  - Conan package for the C ABI runtime;
  - crates.io-ready Rust package layout;
  - NuGet-ready C# package layout;
  - local integration tests that consume built artifacts, not source internals.

### Out of scope for the first full-support round

- A pure Rust or pure C# reimplementation of Fletcher.
- Direct binding to provider-specific C++ classes from Rust/C# without the C ABI.
- Exposing arbitrary Arrow C++ objects across language boundaries.
- Browser support; TypeScript already covers that path.
- A stable ABI for every internal C++ type. The ABI is intentionally narrow and
  use-case driven.

## 3. ABI rules

The C ABI must be boring and strict:

- every exported symbol is `extern "C"` and versioned with a `fletcher_c_` prefix;
- no exceptions cross the boundary;
- every function returns a status code or a nullable handle plus an error object;
- every heap object crossing the boundary is an opaque handle with an explicit
  destroy function;
- byte buffers are `(ptr, len)` pairs with documented ownership;
- callbacks receive user data and must never throw through the ABI;
- all strings are UTF-8 `(ptr, len)` or owned C strings with matching free;
- the ABI reports its version at runtime and exposes feature flags.

Representative shape:

```c
typedef struct fletcher_error fletcher_error_t;
typedef struct fletcher_buffer fletcher_buffer_t;
typedef struct fletcher_provider fletcher_provider_t;
typedef struct fletcher_publisher fletcher_publisher_t;
typedef struct fletcher_subscriber fletcher_subscriber_t;

typedef enum fletcher_status {
  FLETCHER_OK = 0,
  FLETCHER_INVALID_ARGUMENT = 1,
  FLETCHER_SCHEMA_ERROR = 2,
  FLETCHER_TRANSPORT_ERROR = 3,
  FLETCHER_INTERNAL_ERROR = 4
} fletcher_status_t;

typedef void (*fletcher_row_callback_t)(
    const uint8_t* row,
    size_t row_len,
    const uint8_t* attachments,
    size_t attachments_len,
    void* user_data);
```

The exact ABI is designed in BIND-1 and locked before Rust/C# generated APIs
depend on it.

## 4. Generated API shape

Generated Rust and C# should feel like native row APIs, while delegating the
actual encoding/decoding to the C ABI.

Rust:

```rust
let row = SensorReading::new()
    .with_sensor_id(42)
    .with_temperature(23.5)
    .with_location("Room 101");

let bytes = row.encode()?;
let decoded = SensorReading::decode(&bytes)?;
publisher.publish(&row)?;
```

C#:

```csharp
var row = new SensorReading {
    SensorId = 42,
    Temperature = 23.5,
    Location = "Room 101"
};

using var encoded = row.Encode();
var decoded = SensorReading.Decode(encoded.Span);
publisher.Publish(row);
```

The generated code owns the field-level ergonomics: nullable fields map to
`Option<T>` / nullable C# types where possible, repeated/list/map/composite shapes
follow the existing Fletcher type mapping, and generated service helpers hide the
low-level topic/schema calls.

## 5. Arrow integration

Arrow support must be explicit because Arrow's C++, Rust, and .NET object models
do not share one native object representation.

Minimum full-support target:

- row-oriented encode/decode and pub/sub are fully supported;
- server-tier batch handoff is supported through Arrow IPC schema/record batch
  bytes, so both Rust `arrow` and Apache.Arrow C# can consume the data without
  binding to Arrow C++ object lifetimes.

Preferred stretch target:

- expose Arrow C Data Interface handles for zero-copy `RecordBatch` exchange
  where the Rust/.NET Arrow libraries can safely import them on the target
  platform.

The round must decide this early. Arrow IPC is simpler and more portable; Arrow C
Data is lower-copy but materially raises lifetime and platform risk.

## 6. Safety and lifetime contracts

Rust and C# wrappers are responsible for making invalid use hard:

- Rust handles implement `Drop`, are `Send`/`Sync` only when the underlying C++
  object is proven safe, and expose callbacks through panic-catching trampolines.
- C# handles derive from `SafeHandle` or equivalent, avoid finalizer-only cleanup
  for hot objects, and marshal callbacks through rooted delegates.
- Both wrappers copy data only when the ABI ownership contract requires it.
- Subscriber callbacks must have a clear threading model: either invoked on the
  provider thread with documented constraints or delivered through a language-owned
  queue.

## 7. CI and release contract

CI must prove all three layers together:

- C ABI unit tests in C/C++;
- Rust crate tests that consume the built C ABI library;
- C# tests that consume the built C ABI library;
- generated-code integration tests from the same `.proto` fixtures in C++, Rust,
  and C#;
- Linux and Windows coverage, matching the existing C++ support matrix.

Release packaging should follow existing component conventions:

- C ABI runtime is versioned with Fletcher components and distributed as a Conan
  package;
- Rust crates and NuGet packages pin/declare the matching native runtime version;
- official releases align `MAJOR.MINOR` with the native components, matching the
  repository's existing release scheme.

## 8. Design risks

Primary risks:

- ABI scope creep: binding too much of C++ makes the ABI unstable.
- Callback lifetime bugs: pub/sub callbacks are where Rust/C# safety will fail if
  ownership is vague.
- Arrow batch transfer: zero-copy across three Arrow implementations may be more
  expensive than the core row binding work.
- Packaging: making native libraries easy to consume from Cargo and NuGet is a
  separate deliverable, not an afterthought.
- Generator surface: full support means generated service helpers, not only a
  low-level runtime crate/library.

The plan therefore front-loads the ABI spike and keeps row encode/decode green
before adding pub/sub, Arrow, and packaging.

