# D10C-4 — step 0a test-authority audit (corpus) — **CLEAR**

Proof of read: `plans/RIX-rulings.md` read top to bottom, **603 lines, 45 `## ` entries, last `:595` "2026-11-15 — D10C ladder: the domain-7 port tests"**. Yardstick: d10 §3 L1, L5 (as revised by :575 (1); :363 (4)'s "references" superseded), L6, L7; :525, :536 (2), :575 (1); register BC-10 (deferred half), BC-15. Spec (d9) read-only context. No tier-1 disagreement: :536 (2) retires :525 by its own terms, and L5/D28 match :575 (1).

**Scope covered (HEAD b317201).** `NativeLoaderTests.cs` (9 rows, :44-239; no `Skip`); `NativeLoader.cs` callers; the conformance lane's server pass (`ci.integration-test.binding-abi-conformance.yml:119-127` Linux, `:211-235` Windows; README :77-92); `ci.dotnet.yml` staging (`:138-162`, `:279-302`, edge only, no rename); `CopyOracle.Tests.csproj:36-38` (probe staged under the edge name); `PreviewConsumer/Program.cs:71-81`; gateway-crosslang and transport-conformance shim lookups; `design-debt.md`. No test asserts or tolerates a server-to-edge fallback, and none asserts a silent default probe (the only fallback-capable loop, `LoadFromCandidates` continuing past a refused candidate, `NL:119-126`, is not asserted by any row).

| Test | Class | Disposition |
|---|---|---|
| T42 `TheShimIsLookedForBesideTheAssemblyThenBesideTheApplication` (:159) | LEGITIMATE-BUT-ADJACENT | RE-ANCHOR (rung schedules it). The subject, the directory order, is lawful. `Assert.Equal(4, …)` over a list built from one name pins "the server file is never a candidate", against L5 "loads the server shim when it is present". Rewrite that assertion; do not preserve it. |
| T47 conformance server pass, renamed copy | LEGITIMATE-BUT-ADJACENT | RE-ANCHOR. The subject (C# over the server build, d9 L13) is lawful. The vehicle, :525, retires by :536 (2). The `cmp "$OUT/…fletcher-c-abi…" "$SERVER"` lines pin the rename and change with it. |
| T43 handshake ×2, version, marker, resolver, refused-shim (:193), no-shim (:224) | LAWFUL | KEEP. :193 is the base for :575 (1)'s loud failure. |
| `EachShippedRidStatesTheFloorOfItsBuildProfile` (:239) | UNMOORED | KEEP. It pins D-BIND-172 (tier 2), and no law speaks to it. |
| CopyOracle probe-as-edge-name staging | LAWFUL | none. It is not :525's copy, and no server file is beside it. |
| PreviewConsumer :71-81 (edge-named module, exactly one) | LAWFUL | Base-only case, rung 12's (T46). |

**Scheduled restoration:** none. Nothing plans to keep or re-point the renamed copy (`D10C-plan.md:14` retires it).
**Laws vs tests:** L1 `[built]` has 3 tests. L5 `[designed-not-built]` has no test, and nothing asserts it as built (no false green). BC-10's `NativeLibrary.Load*`/`TryLoad` scan does not exist yet. Today only `NL:73` and `NL:121` would match, both in `NativeLoader`.
**Watch for 0b (the new rows must not be FORBIDDEN-BY-TOLERANCE):**
- (a) Once both files are staged, the re-anchored T47 `cmp` and the dotnet server leg prove only that a file is present, not that it loaded. A silent fallback to edge would stay green, because per-row is green on both builds until rung 7. Each leg must observe `SERVER` (variant or module), or it tolerates what :575 (1) forbids.
- (b) The no-fallback control must stage {unloadable server, valid edge}. Today's :193 stages {missing, unloadable}, which cannot show a fallback.
