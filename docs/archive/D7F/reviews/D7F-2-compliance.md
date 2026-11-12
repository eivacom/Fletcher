# D7F-2 — compliance review (step 4a)

## Cycle 1 — `7916105..dbaa2b6` — ISSUES(2): 1 blocking, 1 non-blocking (prose compressed in cycle 2)

Design `plans/D7F-2-schema-loss-at-deadline.md`; locked D-D7F-1..7. **Proof of read:**
`plans/RIX-rulings.md`, 169 lines, 14 entries, last "2026-11-10 — Queued windows are delivered
before the closing batch"; all tier 1. No tier-1 collision engaged. Touches L1, L5-L8, L13 and the
rulings "2. Fix both", "Yes on all accounts", D9 rewording; the code agrees. D-D7F-6: no index logic.

**F1 — BLOCKING. The ordered unimportable-schema guard cell was missing; d7 L8 claimed it impossible.**
- Design Risks: if `Publisher::CreateTopic` accepts an `OwnedSchema` that `ImportArrowSchema`
  rejects, add a fourth "undecodable" row to the guard. The condition holds: `DeclaredSchema::Encode`
  never throws (`schema_conflict.hpp:39-40, 54-58`), `CreateTopic` forwards (`publisher.cpp:51,
  82-87`), the Mock hands a copy on every `Publish`, `ImportArrowSchema` throws (`schema_import.cpp:20-25`).
- Untested as a result: §3's one narrowing (an import failure is decided once, permanently) and
  DEBT-1's condition (`import_failed` decides, a null return does not).
- Fix: add the state at both endings (null batch, `rows_dropped` 1); rewrite the L8 Tests bullet.

**F2 — non-blocking. False comment in the batched `Subscribe` (cpp:360-363):** "schema lazily from
the first decodable sample … without dropping the first window for want of a schema" contradicts §1,
L1, L7, D9 and the forcing test's W1. Tier-3 text; the text changes.

**Checked and conforming (cycle 1).**
- §1: `TimerLoop` has one `Flush(lk, kTimeout)`; `ArmTimer` and cadence untouched (L5); drops never
  close a window by count.
- §2: `ready_` (member, write, notify, read) deleted; no `cached_codec`, `Codec*` return,
  `unique_ptr<Codec>`, lifetime comment or second `codecs_` lookup survives.
- §3 / DEBT-1: `AcquireCodec` returns `TopicCodec` by value under `mu_`, codec a `shared_ptr`; the
  lambda decides on `arrow_schema || import_failed`; a null import decides nothing; `SetSchema`
  keeps `decoder_.reset()`; the per-row overload holds its codec by value.
- D-D7F-5 / P5: `import_failed` private; no friend or test names `TopicCodec`/`AcquireCodec`; public
  surface 0. (Observation: the flag rides every stored entry but is only ever set on a return.)
- D-D7F-7: one count entry, one closer, one delivery site. D-D7F-3: ruled null-batch wording intact.
- Header and README timeout sentences true (DEBT-5, a1); DEBT-2(c) clause dropped from both.
- Debts landed: D7F-2 DEBT-2(a)(b), DEBT-3; D7F-1 DEBT-3, -4, -5.
- Tests match the mapping table; none deleted or rewritten. d7 law sentences unchanged; ordered
  deletions (term **Ready**, L6 "Interim") done; Q1 marked resolved lawfully.

**RECORD (cycle 1).** (1) d7 L1's last bullet still called Q1 open. (2) d7 L1 Violation 1 said a
throw meant "no schema decided". (3) Design P3 not corrected per DEBT-1. (4) d7 line 6's
citation-drift note was stale. (5) Net production lines +28/−42 (net −14) vs declared +20/−40, for
the brief's "As landed". (6) design-debt: D7F-2 DEBT-2, -3 and D7F-1 DEBT-3, -4, -5 can be marked landed.

## Cycle 2 — `dbaa2b6..7170ca9` (item `e30f2aa..7170ca9`) — ISSUES(1): 0 blocking

**Proof of read:** `plans/RIX-rulings.md` re-read top to bottom: **169 lines, 14 entries**, last
"2026-11-10 — Queued windows are delivered before the closing batch" (unchanged since `6273822`).
No tier-1 collision engaged.

**F1 (cycle 1) — RESOLVED.**
- `kUnimportableSchema` is the guard's fourth state, at both endings under `SCOPED_TRACE`. It is
  declared through `Publisher(mock).CreateTopic`, and `ASSERT_ANY_THROW` on the import stops it
  passing vacuously.
- It goes beyond cycle 1's minimum, and the extra is real. After the first row the topic is
  re-declared with `TestSchema`: the Mock overwrites (test:40), and `pub` holds no prior entry, so
  there is no conflict. A well-formed row is then sent. Had the lambda decided on `arrow_schema` alone,
  that row would import (a failed import stores no `codecs_` entry, cpp:376-382) and decode, failing
  both endings. So the cell pins DEBT-1's `import_failed` half and §3/rung-1 "no recovery" (L8).
- It asserts nothing about a schema after *no* schema (L13 holds); d7 L8 Tests names it; law unchanged.

**F2 (cycle 1) — RESOLVED.** The stale text is gone (neither phrase greps), but see F3.

**F3 — non-blocking. The replacement comment (cpp:360-364) overstates the null batch.**
- It says rows before the schema "are counted dropped and reported with a null batch at the window's
  deadline or at close". Design §1 and L7 say a window that gains a usable schema before it closes is
  a *non-null* batch carrying those drops. `SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops`
  pins exactly that.
- "the first row that brings one" also omits that a released handle decides nothing (DEBT-1).
- Acceptable fix: "…counted dropped and reported in their window's `rows_dropped`, as a null batch
  when that window closes before a usable schema arrives…", and "the first row that brings a
  decidable one". Comment only; may ride the close commit without re-review.

**Checked and conforming (cycle 2).**
- The S1 helper `WaitForNullTimeoutReportsTotalling` departs from the mapping table's exact counts
  (W1 `WaitFor(1)` with `rows_dropped` 3; "still 1/2 deliveries"). It follows the design's own
  stall-proof principle (4b S1: W1's deadline is anchored at construction, per L5), and every
  forbidden corner case stays pinned:
  - an idle report (the count is unchanged after the idle sleep);
  - a double report (the total `EXPECT_EQ` plus `rows_dropped > 0`);
  - reports while open (`kTimeout`, null);
  - W2 in its own delivery.
  The red-before reason is unchanged. It gives up "W1 is one window" when nothing stalls; L5's cadence
  had no test before either (d7 L5).
- The production diff is one comment. Public surface 0. P5 holds: no test names `TopicCodec`,
  `AcquireCodec` or `import_failed`. D-D7F-7 is untouched, and no retired construct resurfaced.
- DEBT-6 (4b S2, carried to D7F-3) is accepted debt. The once-flag still makes a second decision
  unrepresentable.
- Evidence: `C:/Users/CTM/.conan2/p/b/fletcb202a7e197aca/b/build/Testing/Temporary/LastTest.log`
  shows 43/43 passed, with the guard at 570 ms. Its build sources contain the new helpers.
- Cycle-1 RECORDs (1)-(4) were corrected in `7170ca9`.

**RECORD (cycle 2).**
- RECORD: `D7F-progress-log.md` "paused" entry says the fix is "uncommitted and unverified" and
  blocked; `7170ca9` commits it and the build log shows 43/43.
- RECORD: design mapping table no longer matches the tests: the W1/W2 exact counts; the guard now has
  four states, and the unimportable deadline cell expects two reports. Annotate "As built", as was
  done for P3. Risks' "No in-tree provider does" is now false for the Mock.
- RECORD: d7 L8 Tests bullet says each unimportable cell reports its row "then declares a good
  schema"; at the close ending the single `kClosing` report comes afterwards, with `rows_dropped` 2.
- RECORD: cycle-1 (5) brief "As landed" and (6) design-debt landed marks are still owed at close.
