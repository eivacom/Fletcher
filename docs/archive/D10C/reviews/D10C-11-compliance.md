# D10C-11 compliance review (step 4a), cycle 1

**Verdict: ISSUES. 1 blocking (F1), 2 non-blocking.** No tier-1 collision, no tier-2 text against tier 1, no STOP-AND-ASK.
**Proof of read:** `plans/RIX-rulings.md` read top to bottom: 684 lines, 54 `## ` entries. The last is "## 2026-11-17 — Fletcher Linking Exception: legal approval; pack it" (:677).
**Scope:** `git diff 924adcb` plus the working tree: 9 files modified, none untracked. I ran `check_readme_snippets.py` as
`ci.dotnet.yml:488` runs it: 7 snippets match. The protoc README check passes too.

## Blocking
**F1. What survived: the bundle's own README still describes the bundle without the server package.**
- `scripts/preview/README-preview.md:11` lists `nuget/` without `Eiva.Fletcher.Server` and without the two
  `fletcher-shim-server-<rid>.conan.lock` files. The bundle carries both: `assemble_preview_bundle.sh:34` requires the package,
  `ci.dotnet.yml:579,638` ships the lockfiles, and `README.package.md:291` sends testers to them in the bundle's `nuget/`.
- `:33-35` tells a tester which packages to reference and never names the server package. A tester who follows it gets only the NotSupported.
- This is tier-3 text against :605 (1) and d10 L15. This diff fixed the same table in `PREVIEW-TESTERS.md:10` but not this one,
  which ships in every bundle (`assemble_preview_bundle.sh:30`). It is a design gap, not drift: audit 0a (4) and Files-to-touch omit it.
- *Fix:* add `Eiva.Fletcher.Server` to `:11` (the server build of the shim; reference it only for batched Arrow subscriptions),
  with the two server lockfiles. Add to `:33-35`: "and `Eiva.Fletcher.Server` beside it for batched Arrow subscriptions".
  Add the file to the design's Files-to-touch as a delta.

## Non-blocking
**F2. The below-floor "no fallback" premise is false as written.**
- `minimal.sh:78` says "so a fallback would run", and design :39 says it "would also leave the app running". But the edge shim has
  the same floor (L15), so Debian 12 refuses it too, and a fallback could not run there.
- The arm still forbids everything it can observe (`That file is the edge build` absent, the server sentence present). L5 and
  :575 (1) are carried by the at-floor exactly-one check and D10C-4's loader tests. No forbidden state is accepted.
- *Fix:* reword both: "the edge shim shares the floor, so a fallback would show only as a refusal naming the edge file".

**F3. Stale tier-3 text.**
- `Program.cs:15-16` still says the shim is the one "Eiva.Fletcher.Interop's package put under runtimes/<rid>/native/".
  In `server`, it comes from `Eiva.Fletcher.Server`.
- d10 L6's from-packages line names `minimal.sh` only in its qualifier, not as an L6 test, though its at-floor base run asserts
  the refusal. DEBT-4 asked for both T46 and `minimal.sh`. *Fix:* one clause in each place.

## Pressure-tested and conformant
- **Purpose, from packages.** `server` gets its batches with exactly one shim loaded, `fletcher-c-abi-server` from
  `runtimes/<rid>/native/`; C#'s batcher has been gone since D10C-7. `base` (the README's csproj, untouched) publishes and
  subscribes per row on the edge shim and has no `*fletcher-c-abi-server*` file in its output. Its batched subscribe gets
  NotSupported/Seam with the shim's text and the `Eiva.Fletcher.Server` sentence (:551 (1), :575 (2)).
- **Audit 0a.** (1) T46 re-anchored, DEBT-1 applied (`Program.cs:171-172`). (2) The shim check is per configuration: it collects
  both names and requires exactly one. (3) `minimal.sh` covers both builds, at and below the floor. (4) All four docs written,
  plus the L5 and L6 lines (DEBT-4).
- **Audit 0b.** (a) `' not found` kept. (b) `grep -E`, dots escaped. (c) The reason is commented at `minimal.sh:91-97`. The PM's
  decision holds: `check_shim_dependencies.sh:59` proves the floor's value on the server shim (`ci.dotnet.yml:189,379`).
- **Forbidden states.** No rows arm survives in base: a base success is a failure, and so is any other exception. An edge load in
  `server` fails the exactly-one check. A wrong argument fails both ways; a missing one exits 2. A no-op `dotnet add` fails at
  `run.sh:97`. There is no fallback branch.
- **T46 re-anchored, not deleted.** `NativeBatch` and its `native-batch` region are unchanged. Files-to-delete (none) holds.
- **Marks.** L7 moves to `[built]` with a qualifier that matches the evidence ledger: local bundle, Linux devcontainer with the
  .NET 8 builds rolled forward, Windows, `minimal.sh` through the recorded wrapper, CI unrun. L5's and L6's earlier marks are
  unchanged. No mark lifts on CI (:408 (2)).
- **Scope.** Nothing changed in `dotnet/src`, d7, d9, `c-abi` or `.github`. No new public surface. The design's anchors
  (`run.sh:51,96-98,103`; `Program.cs:49,164`) match the tree.

## RECORD
- RECORD: brief :31 still holds the dead tripwire ("STOP-AND-ASK if the two builds' floor sentences ever differ"), and :33 still
  says "owed at the close". The design re-pointed both (DEBT-2, DEBT-3).
- RECORD: `D10C-progress-log.md` does not yet hold the PM's `3\.4\.3[12]` decision (0b (c): log it at close).
- RECORD: `preview-bundle-consumer/README.md:69` says "the README all four packages carry". The server package carries it too.
## Cycle 2 — PASS (no blocking items)
All three findings and the RECORDs are closed in the tree. F1: `README-preview.md:11,33-35`, and the file is in the design's Files-to-touch (:107). F2: `minimal.sh:77-80` and design :38-40 now say the edge shim shares the floor. F3: `Program.cs:15-17`; d10 L6 cites `minimal.sh`'s at-floor base runs. The consumer README :69 says "five", and the brief :31/:33 is fixed (`3e8ec66`). Re-checked: the snippet check still matches 7 of 7. Nothing changed in `dotnet/src`, d7, d9, `c-abi` or `.github`. `read -r` changes no arm, and the evidence rows record the re-runs. Non-blocking note: `README-preview.md:11`'s "with their symbol packages" now covers `Eiva.Fletcher.Server`, which has none (`assemble_preview_bundle.sh:11`).
