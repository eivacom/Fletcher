# D10C-4 design review — cycle 1 NEEDS-REWORK (1 BLOCKER) · cycle 2 **APPROVE-WITH-DEBT(2)**

## Cycle 1 (`9bab11f`) — NEEDS-REWORK (1 BLOCKER, 4 DEBT)
Proof of read: `plans/RIX-rulings.md` top to bottom, **603 lines, 45 `## ` entries, last `## 2026-11-15 — D10C ladder: the
domain-7 port tests` (:595)**. No tier-1 collision.
- **B1 — BLOCKER (unstated premise).** The design deleted `NL:70-76` on the claim that the folders the runtime searched were
  already candidates. That holds only if `RuntimeInformation.RuntimeIdentifier` (`NL:85`) names the package's folder. On a
  source-built .NET 8+ (Ubuntu's and RHEL's own, both named in the floor at `NL:157-158`) it is a distro RID. A NuGet app's
  `runtimes/linux-x64/native/` (`PreviewConsumer/Program.cs:72`) would then be missed, and the app would stop. Fix: key the
  folder on OS + `ProcessArchitecture`, or use the host's native search folders, and state the premise.
- **DEBT-1** Decision 1 is settled (R-1 (a), d10 L5/D28, BC-10), so make it a disclosure. **DEBT-2** keep
  `env -u LD_LIBRARY_PATH` at conformance `:123`. **DEBT-3** "per process" really means per `AssemblyLoadContext`.
  **DEBT-4** the documented full-suite commands need `-p:FletcherServerShim=`.
- **Checked, no finding:**
  - :575 (1) is met exactly: `Resolve` never returns zero, there is no loop, and the only by-name import left is test-only
    (`CopyOracleTests.cs:340,344`).
  - The cached refusal is correct: a failed cctor is permanent, and there is no race.
  - The edge→edge narrowing is licensed by "no fallback branch".
  - The module-list host is a legitimate outside observation (`PreviewConsumer:73-78` does the same).
  - The server leg follows the ladder, with P5's STOP. The conformance pass is equivalent and stronger.
  - 7a: both documents at their caps, not padded. 7b: priced ≈80-115 net against 250, so no split.

## Cycle 2 (HEAD) — **APPROVE-WITH-DEBT(2)**, no BLOCKER stands
Ledger re-checked: unchanged, 603 lines, 45 entries, last :595.
**Resolved.**
- **B1:** `PackageRid(OSPlatform, ProcessArchitecture)` now keys the candidates, `PlatformFloor` and the RID messages
  (:30-36). `NL:85` is in Files-to-delete, and P6 carries a musl STOP. The reason for rejecting `NATIVE_DLL_SEARCH_DIRECTORIES`
  (a second folder source, computed by the host) is sound under L5/BC-10.
- **DEBT-1:** brief :34-40, now a disclosure with "Decisions for you: None". **DEBT-2:** :61-62 and :148.
  **DEBT-3:** :18-22 and the table row at :80. **DEBT-4:** :135-137.

**The distro-RID guarantee is a PROMISE.** Brief :16 ("kept") and :35-36 tell the owner that packaged apps, "including those
on a distro's own .NET", are unaffected. The platforms are the floor's own. The case is reachable, unlike D10C-3's
schema-less case, which was ruled unreachable (progress log :53). So law b applies: the internal leg
`ADistroBuiltRuntimeStillFindsTheLinuxX64Folder` (:115) cannot be its only proof (ladder :27-28).

**An outside proof is feasible without a distro runtime, so this is not an accepted gap:**
1. Stage the forcing theory's `runtimes` arm (:108) at the literal package folder `runtimes/linux-x64/native/` or
   `runtimes/win-x64/native/`, as `PreviewConsumer/Program.cs:72` does, never at a runtime-computed RID. The Linux leg's host
   run then proves from outside that the package's folder is found.
2. Extend the guard `NoNativeLoadOutsideTheResolver` (:116) to also fail on `RuntimeInformation.RuntimeIdentifier` anywhere in
   `dotnet/src/Fletcher.Interop`. A scan is a proof form this ladder already admits (rung 4/BC-10, 8, 10). Its vacuity
   control must find `PackageRid`.

The runtime's RID is the only input a distro runtime changes, so (1) and (2) together cover every runtime. A run on Ubuntu
24.04's own `dotnet-runtime-8.0` in an `ubuntu:24.04` container (control: the host prints a RID other than `linux-x64`) is also
feasible but optional.

**Classed DEBT-5, not a BLOCKER:** it is two test rows, with no production line and no redesign, so step 3 can fix it.
**Binding:** 4a/4b refuse to close the stage while the distro promise still rests on the leg alone.

**DEBT-6 (disclosure, new).** A single-file app published with `IncludeNativeLibrariesForSelfExtract=true` extracts the shim
outside the app folder, and `NL:73` found it through the host's native search folders. Such an app now stops, loudly, with the
folders listed. The setting is opt-in, not the default. Brief :35 ("published apps … are unaffected") should name this
exception, or P1 should list it as unsupported. No code change.

**Budget.** The design is 150/150 and the brief 53/55; new surface 0. Declared +125/−48 (net ≈+77). My price is unchanged at
≈80-115 net against 250: it fits, with no split.
