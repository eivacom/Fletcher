# D10C-4 compliance review (step 4a), cycle 1: **ISSUES (1 minor, ruling corner)**

Proof of read: `plans/RIX-rulings.md` read top to bottom, **603 lines, 45 `## ` entries; the last is `## 2026-11-15 — D10C ladder:
the domain-7 port tests` (:595)**. No tier-1 collision: :575 (1) names and revises :363 (4); :536 (2) retires :525 by its own terms;
d10 L5/D28 and BC-10 agree with :575 (1). Diff base `ff86539`, working tree plus 4 untracked paths.

## F1 (minor, blocking until routed): a present server file whose presence check fails falls back to the edge (:575 (1))
- `NL:125` decides presence with `File.Exists`. That call returns `false`, with no error, when the caller cannot stat the
  path (a candidate folder without search/list permission: EACCES on Linux, ACCESS_DENIED on Windows). So with a server file in
  `runtimes/<rid>/native/` that cannot be stat'ed and an edge file flat, the edge file loads silently. That is a present server
  file the app cannot load, and it ends in the edge build: the refusal has become a recovery path. (Dangling symlinks are safe:
  .NET falls back to `lstat`.) The design chose "`File.Exists` only" (:39), so the design is the tier-3 text that is wrong.
- **Fix (about 5 lines):** treat only `FileNotFoundException`/`DirectoryNotFoundException` from `File.GetAttributes(candidate)`
  as absent. Any other failure means *chosen*, so the one `NativeLibrary.Load` refuses and names the file with the OS reason.
  Leg: on Windows, deny list access on a folder holding the server file, valid edge next; the refusal names the server file.
- **Alternative (owner, not PM):** accept it as disclosed residue. That narrows :575 (1), so it needs the owner's word.

## DEBT-5: the outside proof, given that Linux has not run
- **Part 1** (literal `runtimes/{win,linux}-x64/native`, `LoadedShim.cs:40-41`; LLT theory and guard arms) is present. It ran
  green on **win-x64 only**. **Part 2** (`NoNativeLoadOutsideTheResolver`: `\bRuntimeIdentifier\b|RUNTIME_IDENTIFIER` in Interop,
  vacuity control `PackageRid(`) is present and green. It scans source, so it is evidence on any OS.
- So the promise no longer rests on the leg alone; design review :50's letter is met and 4a does not block on it. But the
  **linux-x64 host arm has never run**, the only outside observation on the OS the promise concerns (P2 and P4 unverified too).
- A `[built]` mark is honest **only in its qualified form** ("Windows, by hand; CI and Linux unrun", :408 (2)). Record DEBT-5
  as "tests in place; Linux evidence owed". Lift the qualifier only with a green `ci.dotnet` Linux run ID (audit 0b).

## Checked, no finding
- **:575 (1), every route.** Resolver: for ours, `Resolve` returns `Decision.Value`, never zero; the `Lazy`
  (`ExecutionAndPublication`) caches the throw. Defaults: no `TryLoad` and no `DllImportSearchPath` attribute in `dotnet/src`.
  ALC: `LoadUnmanagedDll`/`ResolvingUnmanagedDll` run only after the resolver returns zero, which it never does. Probe shim:
  CopyOracle's edge-name staging and test-only imports (`CopyOracleTests.cs:340,344`) unchanged, LAWFUL per audit 0a.
  Edge→edge narrowing disclosed (brief :29-30).
- **Files-to-delete:** gone (grep): `TryLoad` by name, the continue-past-refused loop, the `RuntimeIdentifier` key, the
  conformance `cp`/`cmp`/`EDGERUN` lines on both OSes. `env -u LD_LIBRARY_PATH` kept, header rewritten. T42 re-anchored
  (8/4/4). No test deleted.
- **:536 (2):** distinct names, server first. :525's renamed copy: no survivor in code, CI or conformance README; D16 struck.
- **G1** `AShimOnlyOnTheOsSearchPathIsNeverLoaded` (LLT:114; host run with PATH/LD_LIBRARY_PATH; red at `ff86539` per evidence).
  **G2**: `ci.dotnet` fails with no output folder and runs `test -f` per TFM; conformance runs `test -f` on both OSes.
  **G3**: `RUNTIME_IDENTIFIER` in the scan, control recorded. **G4** left to rung 7, as the register says.
- **Design shape:** one `Lazy` per ALC, commented as the design asks. `CandidatePaths` lists server then edge, same folder
  order. `PackageRid` keys the candidates, the floor and the messages. `LoadFromCandidates` signature kept; T43 unchanged.
  The implementer's message change (no library name, no "Also probed" in the refusal) fits "today's wording … then the floor";
  the path names the file (D28), and the inner exception is attached.
- **Scope:** no d7/d9/c-abi/pubsub-arrow edit. No new public surface: only `NativeLoader.cs` changed in `dotnet/src`, every new
  member internal. TestHost `IsPackable=false`, no IVT, public API only (ladder ground rule). Files-to-touch match, plus only
  `plans/D10C-evidence.md`. DEBT-6 disclosed (brief :34-38, `dotnet/README.md:163-174`). Decision 1 (settled, not asked) is
  consistent with :575 (1), L5/D28 and BC-10.

## RECORD
- RECORD: d10 L5 "Settled (:575 (1))" (:216) and §4 (:446) say `[built] (D10C-4)` without the "Windows, by hand; CI and Linux
  unrun" qualifier that L5's headline and BC-10 carry. The PM's note says all four carry it.
- RECORD: d10 §2 "Edge shim / server shim" row (:84) still reads "C# today loads only the first (`NM:90`)".
- RECORD: d10 `NL:` references shifted: §2 Loader `NL:25-273` (now 27-303); Handshake/L1 `NL:253-272` (now 283-302); L15
  `NL:155-162` (now 189-196).
- RECORD: register BC-10, column 3 ("today `NativeLoader`'s single library name") and column 5 ("delete … when the loader
  lands"), both past.
- RECORD: d9 :370 (L13 Tests) and :600 (§6) still describe the renamed copy. d9 is read-only by design, recorded only at design
  :124; carry it into the round's close findings.
