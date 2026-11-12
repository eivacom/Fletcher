# D7F-1 — compliance review (step 4a), cycle 1

**Verdict: CONFORMS.** No blocking conformance items. No STOP-AND-ASK. No tier-1 collision.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: 169 lines, 14 entries. The last
entry is "2026-11-10 — Queued windows are delivered before the closing batch". All entries were
treated as tier 1. Diff reviewed: `git diff b2ef5ae a2c8b97` (7 files).

## Checked, conforms
- **Design changes 1–3** (`cpp`). The `if (!ready_) return;` gate is gone from `Flush`, and the one
  gate left is `HasPending()` (cpp:197). `NoteDropped` is deleted. The lambda runs `call_once` only
  `if (codec)` and calls `AddRow` unconditionally (cpp:355-369). The `kRowLimit` flush in `SetSchema`
  is deleted, and its lock is now a `lock_guard`. The delivery half of `Flush` is untouched (P5).
- **D-D7F-3.** The header (hpp:107-110) and README (:94-97) use D9's words unchanged. The only change
  is code formatting on `batch` and `rows_dropped`, which the design allows. "(or null)" was added at
  hpp:104 and README:89. The "What that costs…" paragraph is untouched.
- **D-D7F-5.** The hpp diff changes only comments. New public surface: 0.
- **D-D7F-7.** The lambda calls into the batcher once per row. `cb_` is called at one site
  (cpp:230). `Flush` is the only function that closes a window.
- **Survivors grep.** Production code no longer has `NoteDropped`, the `ready_` reporting gate, the
  `SetSchema` flush, or the lambda's `NoteDropped(); return;`. The `ready_` uses that remain
  (cpp:67, 110, 181, 185, 249) are the declared interim and belong to D7F-2.
- **Corner cases forbidden.**
  - Two count entries: unrepresentable.
  - No codec with a decoder: guarded by ordering, since `if (codec)` comes before `call_once`.
  - A null batch meaning "no rows": a null batch is reached only through `HasPending`, so it carries
    `rows_dropped ≥ 1`. G1 guards this.
  - Two closing reports: prevented by `stopped_`.
  - A row after closing: not counted (cpp:76).
  - No refusal was turned into a recovery path.
- **Tests.** F1–F4 and G1 have exactly the names, providers, endings and assertions the design
  specifies (N=3, one attachment, `max_rows=100000`, 10 min timeout). `in_process_provider.hpp` is
  included. No test was deleted or rewritten.
- **Files-to-touch** match the design. `tests/CMakeLists.txt` was not needed (P4). The only extra
  file is the step-0b audit record.
- **DEBT-1 landed.** The BC-3 note has all four parts: gate removed; import failure counted
  through the no-decoder branch; state and `ready_` left to D7F-2; one usability decision waits on
  Q7.
- **DEBT-2 landed.** The L1 mark reads "violation 1: closing half built (D7F-1), deadline half open
  (D7F-2); violation 2 open (D7F-3)".
- **Law texts.** No law sentence was reworded. The d7 changes are:
  - build marks for L1 and L7;
  - test lists for L1 and L9;
  - "Resolved by D7F-1" notes on findings a2, b1 and b5;
  - the §(c) list, including the PM's two record corrections at d7:72 and d7:328-329.

  The register edits to BC-2, BC-3 and BC-4 are status notes only.

## Findings
None.

## RECORD
- RECORD: d7:64-67 (L1, Violation 1) still describes `NoteDropped` and the `cpp:210` gate as
  present. The closing half is fixed by D7F-1.
- RECORD: d7:122-127 (L7 body) still says the header's narrower claim "is the part that is
  violated" and that the test "contradicts the header". Both now contradict the `[built]` mark.
- RECORD: d7:118 (L6, "The idle case has no test") and d7:327 (§c) are stale. G1
  `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose` now covers the schema-less idle window at
  close.
- RECORD: d7:263 Q3 is still listed as open, although D9 settled it and D7F-1 applied it.
- RECORD: d7:295 (a3, "unreported drops") and d7:314 (b2, "whose failure goes unreported") are
  stale. Both cases are now reported at close.
- RECORD: d7's `cpp:NN` citations after old line 69 are now 3–14 lines off. For example, L1 "Holds
  at cpp:81-85" is now cpp:77-81, and L6 cpp:211 is now cpp:197.
- RECORD: the L6 mark `[built]` overclaims during the interim, the same way DEBT-2 found for L1. An
  open schema-less window gets no delivery at the deadline until D7F-2. This predates D7F-1, which
  narrowed it.
- RECORD: actual cpp numstat is +25 / −37 (net −12), against the declared +10 / −25 (net ≈ −15).
  Re-indenting the `call_once` block inflates the gross figures. This is for the brief's "As landed".
- RECORD: the tree holds no evidence that F1–F4 were red on the pre-stage tree, which the design
  requires. Reading the code supports it: the old `Flush` returned at `!ready_`, so there were 0
  deliveries and `ASSERT_EQ(size, 1u)` failed for the right reason. The implementer's handback should
  carry the evidence.
