<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
<!-- Copyright (C) 2026 The Fletcher Authors -->

# accessor-capstone — RBA-7 cross-language parity capstone

This directory is the **single shared source of truth** for the RBA-7 capstone
(`accessor_cpp_and_rust_agree_on_same_batch`, D-RBA-8). Three arms consume the
**same three files** here, C# joining C++ and Rust in BIND-7d (D-BIND-11):

| Arm | Test | Lane |
|---|---|---|
| C++ | [`test_accessor_capstone.cpp`](../protoc-arrow-bridge/tests/test_accessor_capstone.cpp), over the generated `CapstoneBatchAccessor` (`--fletcher_opt=accessor`) | `ci.integration-test.protoc-arrow-bridge` |
| Rust | [`accessor_capstone.rs`](../protoc-gen-fletcher-rust/tests/accessor_capstone.rs) (`--fletcher_opt=rust`) | `ci.integration-test.protoc-gen-fletcher-rust` |
| C# | [`CapstoneTests.cs`](../protoc-dotnet/dotnet/ProtocDotnet/CapstoneTests.cs), over the generated `CapstoneBatchAccessor` and `CapstoneBatchView` (`--fletcher_opt=csharp,csharp_model_only,csharp_accessor`) | `ci.integration-test.protoc-dotnet` |

Each lane's path filter and checkout include this directory, so a change here runs all three.

| File | Role |
|---|---|
| [`proto/accessor_capstone.proto`](proto/accessor_capstone.proto) | the **one** schema — all three accessors are generated from it, so they cannot be hand-aligned. |
| [`fixtures/accessor_capstone_fixture.json`](fixtures/accessor_capstone_fixture.json) | the **one** logical fixture — each arm builds an in-memory Arrow batch from it (its language's Arrow arrays, not IPC), so all carry identical rows, nulls and metadata. |
| [`fixtures/accessor_capstone_expected.json`](fixtures/accessor_capstone_expected.json) | the **one** oracle — each arm normalizes what its accessor reads into this shape and asserts `observed == expected`. |

Because **every** language compares against the **one** committed expected file,
`observed_cpp == expected ∧ observed_rust == expected ∧ observed_cs == expected` makes the
three readouts equal transitively, without hand-transcribing expected values into any test.

## What is covered

`CapstoneBatch` enumerates **every** accessor field kind, each with a
representative **null in its optional path**:

| Field | Kind | Required null |
|---|---|---|
| `id` | non-nullable scalar | — |
| `label` | nullable scalar | null scalar row (row 1) |
| `maybe_child` | nullable 1:1 STRUCT | null struct row (row 1) |
| `samples` | REPEATED_SCALAR | null scalar element via `is_null(j)` (row 0) |
| `children` | REPEATED_STRUCT | null struct element (row 0) |
| `scores` | MAP scalar value | null scalar value via `value_is_null(j)` (row 0) |
| `child_by_key` | MAP message value | null map message value (row 0) |
| `nested_children` | NESTED_LIST (depth 2) | null inner list (row 0) |

Plus schema-level metadata and **positional** per-field metadata, with absent
entries proving the C++-`nullptr` ≡ Rust-empty-map normalization.

## Fixture rows (logical)

- **row 0** — present-and-rich: every collection populated, each carrying its
  one representative null element/value/inner-list.
- **row 1** — sparse: `label` and `maybe_child` are null, every collection empty
  (distinguishing *empty* from *null*).
- **row 2** — present: single-element collections, no nulls.

Maps are encoded as ordered `{"key","value"}` arrays (not JSON objects) so key
order and duplicate-key behaviour stay explicit and Arrow-compatible.
`field_metadata` is an array indexed by **positional** field index (canonical,
spec §5); absent entries are `{}`.
