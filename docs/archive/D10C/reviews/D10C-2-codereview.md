# D10C-2 code review (light lane)

Diff: `git diff HEAD` (base b07eb54 + 568ad93 paperwork) + untracked `AbiNumberTableTests.cs`. Ledger `plans/RIX-rulings.md` read in full: 603 lines, 45 `## ` entries, last `## 2026-11-15 — D10C ladder: the domain-7 port tests` (:595). Handover = 4 is ruled at :551 (3).

## Conformance
It matches. Production is 3 files, +28/-5 (`NM` +20, `FE` +5/-2, `SA` 3/3). The only new public surface is `FletcherOrigin.Handover`. `FlVariant` and `fl_binding_variant` are internal, and InternalsVisibleTo reaches only Fletcher, Fletcher.Tests and CopyOracle.Tests. `Errors.ThrowIfFailed` is unchanged and adds no range check, so nothing re-answers a lower layer (d10 L3). Nothing the brief kept was deleted, and no d9 file is edited.

## BatchReason consistency
Consistent. Every producer is a named member: `Flush(BatchReason.X)` at SA:458/463/551/578, and the comparison at SA:709. No code casts BatchReason to or from an integer, reads it from `fl_batch` (not imported yet) or serialises it, and tests compare it only by member. `GatewayBatchReason` (GatewayBatch.cs:72) stays 0-2. It mirrors the gateway wire, not `binding.h`, so it is outside L2 (nit below).

## CI
The copy item resolves to `<repo>/c-abi/include/fletcher/abi/binding.h`. A missing file fails the copy loudly (MSB3030) instead of skipping. Both test jobs check out `c-abi` (ci.dotnet.yml:91, :234). The pack job (:371-382) has no `c-abi`, but it only runs `dotnet restore` (an evaluation, with no copy) and `dotnet pack` of the four `src/` projects, so nothing builds the test project there. ci.format-check-cs uses a full checkout. The ci.pr.yml `dotnet` filter includes `c-abi/**`, so a header-only edit still runs the table test.

## Findings

**S1 — should-fix, high confidence. The header reader skips what it cannot parse, so one row can pass partly vacuously.**
`ReadEnum` keeps only matches of `NAME\s*=\s*-?\d+` and drops everything else silently:
- An enumerator with an implicit value (`FL_ORIGIN_FOO,`) or a symbolic one (`= FL_X + 1`) vanishes from the header side. A C# enum that also lacks it then passes.
- The value pattern has no anchor, so `= 0x10` reads as `0`.

`ReadDefines` has the same gap: a `#define FL_BATCH_X (4)` or `((int32_t)4)` is skipped. The `Count > 0` guard stops only a fully empty table, not a missing member.
Forbid it instead: split the enum body on `,` and require every non-empty entry to match `^\s*NAME\s*=\s*-?\d+\s*$`, otherwise fail. Likewise, fail on any `#define FL_BATCH_\w+` whose value is not a bare integer. The test exists to catch future header drift, so the parser must reject any input it does not understand.

**S2 — should-fix, medium confidence. Scaffolding from the red phase now hides compile-time proof.**
`FindInternalType("FlVariant")` and the reflection `GetMethod("fl_binding_variant")`/`Invoke` let the test tolerate a mirror that does not exist. That state can no longer happen: Fletcher.Tests already sees Interop internals (Fletcher.Interop.csproj:43). Use `typeof(FlVariant)` and call `NativeMethods.fl_binding_variant()` directly, with `Enum.IsDefined` in place of the hand-rolled `defined.Contains`. A rename then breaks the build instead of reaching a runtime assertion. The header comment at AbiNumberTableTests.cs:19-21 goes with it.

## Nits
- N1: `OriginFourIsHandover` compares `ToString()` with "Handover" (the red-phase form). `Assert.Equal(FletcherOrigin.Handover, error.Origin)` is now typed and stronger.
- N2: `StripComments` removes `/* */` before `//`, so a `// ... /*` line would swallow code up to the next `*/`. binding.h has no such line today.
- N3: `GatewayBatchReason` (public) is still 0-2 while `BatchReason` is 1-3. That is lawful because the gateway is not under L2, but the asymmetry is unrecorded in d10.

RECORD:
- d10 cites `NM:79` (:84, :199, :404), `NM:61-68` (:86, :116) and `NM:1-43` (:128). The 11-line `FlVariant` insertion shifted these, and the lines are now NM:90, NM:72-79 and roughly NM:1-34.
- The brief declared tests at about +95. The actual figure is about +187 (155 + 20 + 12).
