# D9S-1 — code review (light lane)

**Diff:** `git diff 5f573ea..fcb7be6`. Date 2026-11-13. Rulings ledger read: 393 lines, last heading "2026-11-13 — D9S pre-round: a failed handoff is reported; corrections approved".

**Verdict: nothing blocking, nothing should-fix.** 0 blocking / 0 should-fix / 4 nit.

## What I checked
- C ABI hygiene: the enum uses a trailing comma (allowed in C99) and has explicit values. The declaration is `(void)` and carries `FL_ABI_EXPORT`. `binding_version.c` is already in both the shipped target (`CMakeLists.txt:81`) and the probe target (`:182`), so it gets the export define. `exports.map`'s `fl_*` covers the new function. The calling convention is the default, like every other entry point. Returning an enum by value follows the precedent of `fl_status`.
- Containment and any thread: the function is plain C that returns a constant. It is outside `Contained`/`PoisonedRefusal`, so it answers in a poisoned shim by construction.
- BC-10: only `binding_version.c:16` decides the build. Repo-wide grep: no other `FL_VARIANT_*` producer, and no `#if` on a variant.
- `dotnet/`: exactly one source line changed (`NativeLoader.cs:34`). `NativeLoaderTests` derive from the constant, so no test literal 7 is left behind.
- Tests: the release barrier spins only and has no timing assertions. Counts are per thread and asserted after `join`. `TEST_PREFIX "probe."` keeps ctest names unique. The first-call claim depends on per-row processes (`DISCOVERY_MODE PRE_TEST`), and the row's comment says so.
- Docs build-state marks: `[built]` is claimed only for the query and the bump, on edge and probe. Refusals, the server half, the per-target constant and the export-set check stay `[designed-not-built]`. None of them claims more than is built.

## Nits
- n1 (high): the header promises "still answers in a shim that refuses every other call", but no row asserts it. One row under `test_containment.cpp`'s `PoisonedForThisTest` would test the promise instead of relying on construction. The version has the same gap.
- n2 (medium): `fl_variant`'s size is the compiler's choice (`-fshort-enums`, or an AAPCS bare-metal toolchain, on the embedded edge targets). The same exposure already exists for `fl_status`. When domain 10 declares the import it should marshal as `int`. Optionally, a header note could say "int-sized; values fit in int32".
- n3 (medium): the CI export check `ci.c-abi.yml:247` asserts only `fl_binding_abi_version` by name. D9S-2 (which already edits the workflow) could add `fl_binding_variant`.
- n4 (low): `example.c` declares the same "known build" set twice (the ternary and the `if`). This is fine for a smoke consumer.

## Conformance
**Matches the brief.** The surface, the single site, the bump to 0.8 in the same commit as the one C# line, the test wiring and the doc edits all landed as specified. Nothing was slated for deletion, and nothing survived that should not have. D-D9S-1/6/8 and the 2026-11-13 rulings (D23, D27) are honoured.

## RECORD
- RECORD: The header grew by 15 lines, so the d9 doc's `h:` line cites after `h:172` are now off by 15. Examples: §4 "Version (`h:158-172`)" is now 158-187, "Status and errors (`h:174-316`)", and BC-13's `h:1061-1066`/`h:455-473`.
- RECORD: `ci.c-abi.yml:25` still says the ABI "exports one function". This is already carried to D9S-2 by the brief.
