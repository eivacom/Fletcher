# BIND-3 — code review, all four slices

**Subject:** the code BIND-3 landed — `dotnet/src/Fletcher.Interop/`
(declarations, ABI types, handles, loader), `dotnet/src/Fletcher/` (errors, codec
tier, the write window and its thunk, the decoded-schema rewrite),
`dotnet/tests/Fletcher.Tests/`, the dictionary work in `c-abi/src/nanoarrow_codec.*`,
`integration-tests/binding-abi-conformance/`, and the CI wiring for both.
Slices `88d7ed0` + `63c1a58` + `3a135b9` (3a), `668e68d` (3b), `e12d484` +
`934512c` (3c), `f9ddf37` → `a5fabed` (3d).
**Reviewed at:** `a5fabed`, tree clean. CI green 46/46 on `4d3382d`; `afddc09`
and `a5fabed` not yet CI-verified at the time of writing.

**Outcome: 0 BLOCKERs, 4 DEBT, 3 NITs.** No defect was found in the handle
lifetimes, the containment discipline or the wire-format path — the three places a
defect would have been expensive. The DEBT is concentrated in one place and it is
the same place twice: **two independent derivations of one rule, with the
agreement between them under-tested.**

**THE LIMIT OF THIS REVIEW, STATED FIRST BECAUSE IT CHANGES HOW TO READ IT.** The
same author wrote and reviewed this code, in one session, hours after writing it.
That is not a fresh-eyes pass and it should not be recorded as one. BIND-1's review
carried the same caveat and was defensible because the maintainer ruled its
blocker; here the maintainer ruled D-BIND-39, D-BIND-40 and D-BIND-41, which is the
only outside judgement in the item. #129 is a DRAFT until BIND-10 (D-BIND-30), so
`plans/reviews/` is the only review this work gets — which is an argument for
weighting the mechanical checks below over the prose findings, because a mechanical
check does not care who wrote it.

---

## What I verified mechanically (claim → result)

Each row is a claim the code or the record makes about itself, checked by running
something rather than by re-reading.

| Claim | Result |
|---|---|
| 21 entry points declared, exactly the implemented set | ✅ 21 by name, cross-checked against `FL_ABI_EXPORT` in `binding.h` — every one is exported. **The check nearly produced a false finding:** the first regex excluded digits, so `uint32_t fl_binding_abi_version` and `const char* fl_single_copy_marker` read as undeclared. Verified directly before believing it |
| No method returns encoded bytes (D-BIND-1) | ✅ the public surface is `FletcherCodec`, `BoundRows`, `FletcherException`, `FletcherFormatException`, `FletcherStatus`, `FletcherOrigin`. `Encode` takes an `IBufferWriter<byte>`; nothing returns `byte[]` or `Memory<byte>` |
| Every `[UnmanagedCallersOnly]` method is contained | ✅ `ThunkDisciplineTests` requires the OUTERMOST clause to be a catch-all, over both binding assemblies, with a vacuity guard. Falsified twice — see the history in that file |
| `BoundRows.Dispose` unbinds THEN releases | ✅ structurally, not by convention: `BoundRowsHandle` takes a `DangerousAddRef` on the export, so the release cannot run first even under finalization in an unspecified order |
| The array is borrowed and never consumed across N encodes | ✅ `BindingBorrowsTheExportAndNeverConsumesIt` reads the export's `release` after the bind and after every encode |
| Closing the codec before unbinding is survivable | ✅ asserted from managed code, not inherited from the C++ claim |
| A refill preserves the bytes below the cursor | ✅ and falsified: removing the carry-over turns exactly the five refill-dependent rows red |
| A dictionary encodes as its value type | ✅ and falsified in both directions: disabling the encode-side resolution fails both round trips, pointing `DecodeRows` at the bind schema fails exactly the decode row |
| Managed Arrow and native Arrow hand the codec the same batch | ✅ `binding-abi-conformance`, 5 fixtures, byte identity, green on both platforms in CI. Falsified by flipping one corpus byte |
| Every proto-mapping type round-trips | ✅ 17 cases, 14 distinct Arrow types, with a coverage guard |

---

## DEBT

### D1 — one rule, two derivations, and only one nesting proven to agree

`DecodedSchema.Resolve` (C#) and `ResolveDictionaries` (C++) independently
implement the same rewrite: replace every dictionary node with its value type,
keeping the field's name, nullability and metadata. D-BIND-39 chose that
deliberately over an ABI entry point, on the ground that the rule is deterministic
and specified — which is sound, and is the same argument the round makes elsewhere
for not widening the boundary.

What is under-tested is the **agreement**. The managed side has a branch per
composite (`DecodedSchemaTests`: list, large list, fixed-size list, map value, two
deep). The native side has **no nested dictionary test at all** — the only
end-to-end nested case is `ADictionaryInsideAStructRoundTripsAsItsValueType`, which
covers exactly one shape.

So if `ResolveDictionaries` were wrong for a dictionary inside a **map value**,
nothing in the tree would say so. `DecodeRows` would build an array against a
schema the managed importer disagrees with, and `ImportRecordBatch` would either
throw at the caller or misread buffers. Both are bad; the second is worse.

**Owed to BIND-4/5**, where the subscriber makes nested decoding routine: one
end-to-end case per composite, or a native test that compares
`decoded_schema()` against the same expectations `DecodedSchemaTests` holds.

### D2 — `Encode` advances nothing on failure, proven only for a MANAGED fault

`AThrowingRefillComesBackAsItselfAndPublishesNothing` asserts `WrittenCount == 0`
after a refill throws. That is the managed-fault path. A **native** encode failure
— the shim refusing mid-row — is not reachable from the managed surface at all,
because `Encode` always supplies a `grow`, so the fixed-capacity `FL_PAYLOAD_TOO_LARGE`
path that `binding.h` documents has no managed caller.

Not a defect: the property holds by construction (`Commit` is called only after
`ThrowIfFailed` returns). But it is asserted on one of the two ways in, and the
unreachable one is the one the header talks about.

### D3 — the conformance suite's C# project restores unlocked

`RestorePackagesWithLockFile` lives in `dotnet/Directory.Build.props`, which does
not reach `integration-tests/binding-abi-conformance/dotnet/`. Its
`PackageReference`s are exact-pinned with `[...]`, so the named packages are fixed
and only transitives float — weaker than the unit-test lane's `--locked-mode`
restore, and a difference between two lanes that look alike.

### D4 — `ArrowArrayAbi` mirrors a foreign struct, and nothing would catch a drift early

`ExportedArray` mirrors the C Data Interface's `ArrowArray` to reach `release`,
which `Apache.Arrow` keeps internal. The layout is from `arrow/c/abi.h`, which is a
frozen interchange contract, and the mirror is exercised on every bind — a wrong
offset would call a garbage pointer and crash immediately rather than corrupt
quietly, which is the good failure mode. Still: the only thing standing between
this and a silent misread is that the spec does not move. Recorded as accepted
risk, not as a defect.

---

## NITs

### N1 — `FletcherCodec` calls itself immutable, and has a mutable property

The class doc says *"Immutable after construction, so any number of threads may
bind, encode and decode through one instance concurrently"*. `RefillFaultForTest`
is a settable property. It is `internal` and test-only and no shipped path writes
it, but the sentence above it is now not quite true, and the sentence is the thing
a BIND-4 author will read before sharing a codec across threads.

### N2 — `checked((int)window.Pos)` throws outside the documented taxonomy

A row larger than `int.MaxValue` raises `OverflowException` from
`GrowingWindow.Encode` rather than a `FletcherException`. Unreachable in practice —
the transport payload bounds are orders of magnitude lower — but every other
failure from this surface is a `FletcherException` or a rethrown user exception.

### N3 — the parity suite names two well-known types where the spec names thirteen

`MappedTypes` covers `Timestamp` and `Duration` by name. The eleven nullable
wrapper types are covered *implicitly*, as "a nullable scalar", and every case
carries a null row for that reason — the coverage is real. But a reader diffing the
case list against the spec's §"Well-Known Types" table sees thirteen rows and two
cases, and has to read the comment to learn why. A presentation gap rather than a
coverage one.

---

## What I did not find

No defect in: the handle lifetimes (the publisher→provider and rows→export pins
are both structural, and both are the only route to the handle); the containment
discipline (one thunk, outermost catch-all, enforced mechanically); the wire-format
path (byte identity against native Arrow holds over the whole corpus, on both
platforms); or the error taxonomy (rule 3 is checked first, the dispose is an
unconditional `finally`, and both are asserted).

The two things this item was most likely to get wrong — the borrow rule and the
thunk — are the two that are enforced by a type and by a test rather than by a
comment.
