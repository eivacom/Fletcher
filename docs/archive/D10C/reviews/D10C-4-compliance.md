# D10C-4 compliance review (step 4a): cycle 1 ISSUES (1 minor) · cycle 2 **PASS**

Proof of read: `plans/RIX-rulings.md` read top to bottom, **603 lines, 45 `## ` entries; the last is `## 2026-11-15 — D10C ladder:
the domain-7 port tests` (:595)**. No tier-1 collision: :575 (1) names and revises :363 (4); :536 (2) retires :525 by its own
terms; d10 L5/D28 and BC-10 agree with :575 (1). Diff base `ff86539`, the working tree.

## Cycle 1: ISSUES (1 minor)
- **F1 (:575 (1)).** `NL:125` decided presence with `File.Exists`, which returns false, with no error, for a path it cannot
  examine. A server file in an unreadable folder was therefore skipped, and the edge build loaded: a refusal had become a
  recovery path. The design (:39) was the tier-3 text at fault. Fix: only not-found counts as absent. The alternative, residue,
  was the owner's call.
- **DEBT-5.** Part 1 (literal `runtimes/{win,linux}-x64/native` arms) ran green on win-x64 only. Part 2 (the
  `RuntimeIdentifier`/`RUNTIME_IDENTIFIER` scan with a `PackageRid(` control) scans source, so it is evidence on any OS. The
  distro promise therefore no longer rests on the leg alone, and design review :50 is met. But the linux-x64 host arm has never
  run. `[built]` is honest only with the qualifier (:408 (2)). Lift it only with a green `ci.dotnet` Linux run ID.
- **Checked, no finding.**
  - :575 (1) in every route: the resolver never returns zero, and the `Lazy` caches the throw. There is no `TryLoad` and no
    `DllImportSearchPath`. The ALC hooks are unreachable. The probe shim is unchanged (audit 0a LAWFUL).
  - Files-to-delete are gone. :525's copy has no survivor. G1, G2 and G3 are applied; G4 is rung 7's. T42 is re-anchored
    (8/4/4).
  - No d7/d9 edit, no new public surface. DEBT-6 and Decision 1 are disclosed and consistent with :575 (1), L5/D28 and BC-10.
- **RECORD (cycle 1):** five doc items: the d10 qualifiers, §2 :84, the `NL:` refs, BC-10's tense, and d9 :370/:600. The
  first four are fixed in cycle 2. The fifth stays open.

## Cycle 2: **PASS**
Ledger re-checked: unchanged (603 lines, 45 entries, last :595; no diff since `ff86539`).
- **F1 resolved.**
  - `IsPresent` (`NL:178-192`) calls `File.GetAttributes`. `FileNotFoundException` and `DirectoryNotFoundException` read as
    absent; anything else chooses the file, so the one `Load` names it. Grep: no `File.Exists`, `TryLoad` or
    `RuntimeIdentifier` left in Interop code.
  - The test `AServerFileWhosePresenceCannotBeCheckedIsChosenNotSkipped` is an outside proof: a host run, server first, a
    valid edge file next, and a premise assertion that `File.Exists` is false. Its Windows arm (a deny ACL with the file
    present) was red before and green now. Its Linux arm (a self-symlinked folder, ELOOP) is unrun.
  - The stricter outcome, where an unexaminable edge candidate also stops the app, is covered by the disclosed edge→edge
    narrowing (brief :29-30).
- **The 4b fixes do not drift from the design or the rulings.**
  - S1: the refusal still names the file and the OS reason (D28) and now adds which build it is and the remedy. T43 :193
    still holds.
  - S2: path matching survives aliases. N2: exact shim names only. N5: a constructor that fails cleans up after itself.
  - S3: NuGet-cache hosts are disclosed in `dotnet/README.md:173-176`. They were never documented, so P1's STOP-AND-ASK
    (supported layouts only) is not triggered. Resolving them would need the second folder source the design rejected
    (BC-10).
- **The cycle-1 RECORD items are fixed:**
  - d10 :217 and :446 now carry the qualifier.
  - §2 :84 is rewritten.
  - The `NL:` refs match the tree: 27-340, 320-339, 33-39, 226-233.
  - BC-10's column 3 and column 5 are in the past tense.
- **The `[built]` marks are honest pending Linux.** All four D10C-4 marks (d10 L5 :199, :217, §4 :446, BC-10) carry "Windows, by
  hand; CI and Linux unrun". That is :408 (2)'s form, and it matches the evidence row. The Linux evidence still owed is the
  linux-x64 host arms (DEBT-5), F1's ELOOP arm, G1 on `LD_LIBRARY_PATH`, P2/P4, the Linux server leg, NativeAOT and
  `check_dotnet_packages.sh`. The qualifier lifts only on a green run ID that covers them.

## RECORD (cycle 2)
- RECORD: design said "T43 unchanged", but N1 changed `NativeLoaderTests.cs:314`'s literal from "Also probed:" to "Searched:".
  The wording changed and the subject did not. Cite it as re-anchored (4b N1) in the brief's "As landed".
- RECORD: the owner-facing brief's "Disclosed" section (:34-38) names PATH and single-file only. "As landed" should add the
  NuGet-cache hosts (S3).
- RECORD: `design-debt.md` DEBT-5 still reads "refuse close without both". Mark it "tests in place; Linux evidence owed (run ID)".
- RECORD: d9 :370 (L13 Tests) and :600 (§6) still describe the renamed copy. It is recorded only at design :124; carry it into
  the round's close findings.
