# D7F-1 — Loss before a usable schema is reported at close — Design

Round D7F, item D7F-1, ladder rung 1. Branch `feature/d7f-batch-assembly`.
Citations: `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`, `hpp` =
`pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`, `test` =
`pubsub-arrow/tests/test_pubsub_arrow.cpp`, `d7` = `docs/domains/d7-batch-assembly.md`, `rul` =
`plans/RIX-rulings.md`, `ladder` = `plans/D7F-test-ladder.md`, `ipc` = `pubsub/src/in_process_provider.cpp`.

**Proof of read:** `rul` read top to bottom, 169 lines, 14 entries; last entry "2026-11-10 — Queued
windows are delivered before the closing batch". Every ruling was used as tier 1.

## Summary

Rows that reach a batched subscription while it has no usable schema are counted today
(`NoteDropped`, cpp:121-126) and then discarded unreported: `Flush` returns at `if (!ready_)`
(cpp:210), including the `kClosing` flush. This stage removes that gate, routes the no-codec row
through the batcher's one row entry, and deletes the dead row-limit flush in `SetSchema`. Closing
then delivers one `kClosing` batch, null, with `rows_dropped` = rows sent (L1 violation 1, L6, L7,
L9; D9). The header and README take D9's wording verbatim.

## Design

### What the code already has, and what this stage changes

The null-batch construction **already exists** in the window-finishing half of `Flush`, under `mu_`:
`batch` stays null when `decoder_` is null (cpp:216-232), and the drop count is moved out with it
(cpp:233-236). That is exactly where the ladder's note (ladder:62-65) wants it, and D7F-3 keeps
that half when it splits `Flush`. This stage builds nothing new there. It removes the one line that
stops the finishing half from being reached when no schema has arrived.

Three production changes, all in `cpp`:

1. **Remove the reporting gate (cpp:210).** `Flush` keeps one gate: `if (!HasPending()) return;`
   (L6: idleness gets no delivery). With no decoder, `HasPending()` is `dropped_ > 0`, so a
   schema-less window with drops is finished as a null batch carrying `dropped_`, and a schema-less
   window with nothing in it delivers nothing. The reason is whatever the caller passed. In this
   stage only `Stop` (`kClosing`) can reach `Flush` without a decoder: the timer still parks on
   `ready_` (cpp:194-199; D7F-2 deletes that), and `AddRow`'s row-limit flush is reached only with a
   decoder.

2. **One count entry for a transport row (census C2, D-D7F-7).** Delete `NoteDropped`. The batched
   subscription lambda (cpp:352-382) becomes: resolve the codec as now; **if** one was found, run
   the `call_once` that hands the batcher its schema; then call `batcher->AddRow(data, len, att)`
   **unconditionally**. A row with no codec reaches `AddRow`'s existing no-decoder branch
   (cpp:81-85), which counts it and arms the timer. That branch already serves the rejected-schema
   case (L8), so "awaiting a schema", "import failed" and "decoder refused the schema" now share one
   counting branch and one report (d7 Findings b1 resolved).

3. **Delete the dead branch in `SetSchema` (cpp:69-71; census C4, d7 Findings b5).** The decoder is
   brand new there, so it holds 0 rows, and `max_rows_ >= 1`. `SetSchema` keeps setting `ready_`
   and notifying `cv_` (the timer still waits on it until D7F-2). With no `Flush` call left, its
   `unique_lock` may become a `lock_guard`. Its comment (cpp:53-57) is rewritten to say that
   without a decoder every row is counted dropped and reported with a null batch.

The comment on the lambda (cpp:355-359) and on `Flush` (cpp:203-206) are corrected to the new
behaviour. No other production code changes.

### Ordering invariant the fold relies on

"No codec, yet a decoder" must never occur, or a row whose schema could not be imported would be
decoded against an older schema. It is unrepresentable by ordering, not by a check:
- `cached_codec` is monotone. It is stored once, non-null, and never reset (cpp:360-364).
- `SetSchema` (the only place that builds a decoder) runs only after a codec was obtained, inside
  the `call_once`.
- Deliveries are serialized per subscription by domain 3 (d7 §6), so no row overtakes that store.
Hence: no codec ⇒ the `call_once` has not run ⇒ `decoder_` is null ⇒ `AddRow` counts the row.
The race in d7 Findings a3 (`SetSchema(nullptr)`) also ends with a null decoder, and is now
reported instead of silent.

### Safety, concurrency, error paths

- **Locking.** Unchanged. All counting stays under `mu_`; `Flush` still releases `mu_` only around
  `cb_` (cpp:238-247). D7F-3 moves that delivery half; this stage does not touch it.
- **Timer interaction at close.** `Stop` sets `stopped_` before `Flush`, so a timer parked at
  cpp:198 that wakes while `cb_` runs sees `stopped_` and exits. Same as today's ready case.
- **Callback throws on the null batch.** Swallowed (cpp:243-246, L10). Unchanged.
- **Idempotence.** A second `Stop` is a no-op through `stopped_` (cpp:134). One closing report.
- **Conservation (L2).** Every row that reaches `AddRow` before `stopped_` is either appended or
  counted exactly once. With no decoder it is counted once at cpp:82, and the count leaves the
  window only through `Flush`'s move-out (cpp:235-236). The sum delivered equals rows sent.

### Documents (not counted as production lines)

- **hpp:103-104:** "a zero-row batch is still delivered" becomes "a zero-row (or null) batch is
  still delivered".
- **hpp:106-111** is replaced by D9's sentence, verbatim (identifiers may be code-formatted):
  "batch is null when no batch could be built for this window: no usable schema has arrived yet,
  the schema was rejected (permanent), or finishing the batch failed; rows_dropped counts the
  window's rows in every case". It is followed by L8, which stays true: a rejected schema stays
  rejected for the life of the subscription, with no later recovery and no partial decoding. A
  non-null batch may have zero rows. The paragraph after it (hpp:113-122, "What that costs depends
  on WHY…") keeps its meaning and is not touched.
- **README:94-98:** same replacement, in prose. README:85-90 gains "(or null)" beside "zero-row".
- **d7:** L1's build mark becomes "built for decode failures, internal failures, undecodable
  schemas and a schema that never becomes usable (D7F-1); violation 2 open (D7F-3)". L7's mark
  becomes `[built]` (the header now states what the code does). L9's test list gains the destructor
  closing test. Findings a2, b1 and b5 are marked resolved by D7F-1. Laws are not reworded.
- **Register:** BC-2 notes `NoteDropped` folded in (D7F-1); the retry copy is still D7F-3's. BC-4's
  dead branch is deleted.

## Corner cases forbidden

**Rung 1, unrepresentable:**
- **A row counted through two different entries.** `NoteDropped` is deleted. The subscription
  lambda has exactly one call into the batcher per row.
- **No codec while a decoder exists.** Unrepresentable by the ordering invariant above.
- **A null batch that means "no rows"** (forbidden by L7). With no decoder, pending ⇔ `dropped_ > 0`,
  and `HasPending()` gates finishing. A null batch therefore always carries `rows_dropped ≥ 1`.
  Guard G1 witnesses it.
- **A report suppressed because no schema arrived.** The only gate left in `Flush` is idleness (L6).
- **A row-limit flush when the schema arrives.** The branch is deleted. Its state (a new decoder
  holding rows) could not exist.
- **Two closing reports.** `stopped_` makes `Stop` idempotent (existing).

**Rung 2, refused at the door:** none new. A row that arrives after closing is ignored and not
counted (cpp:80). L9 states that this is lawful.

**Handled residue:** none.

## Premises and stop conditions

- **P1. Domain 3 hands null-schema rows to the batched lambda.** `Subscriber` does not filter rows
  whose `SharedSchema` is null (no such check in `pubsub/src/subscriber.cpp`). **STOP-AND-ASK** if
  the implementer finds a filter. The loss would then happen upstream of domain 7. That is a
  domain 3 L1 question for the owner, not something to patch around here.
- **P2. The Mock passes a null schema for an undeclared topic** (test:50-57). Verified.
- **P3. InProcess `as_declared` is the default.** It allows `Publish` to an undeclared topic
  (ipc:282) and latches the subscription's schema once at `Subscribe`, null for life if nothing was
  declared yet (ipc:236-239, 329-332). Verified. If either fails, use the Mock-only cases and report.
- **P4. `InProcessPubSubProvider` is reachable from `pubsub_arrow_tests`**, through
  `fletcher-pubsub-arrow`'s PUBLIC link to `fletcher-pubsub` (pubsub-arrow/CMakeLists.txt:25-26).
  If not, add the link in `tests/CMakeLists.txt`. That is test infrastructure and not counted.
- **P5. D7F-3 keeps `Flush`'s finishing half as is** (ladder:62-65). This stage changes nothing in
  the delivery half. **STOP-AND-ASK** if the implementer needs to touch cpp:238-247.

## Forcing-test mapping

All tests live in `test`, are driven only through `PublisherArrow`, `SubscriberArrow` and the
provider, and observe only the user callback (a `BatchSink`). `max_rows = 100000` and
`timeout = 10 min` keep the timer and the row limit out of the window. Each sends **N = 3** rows,
one carrying an attachment. Results are read straight after the ending returns. That read is lawful
under L9, because a closing call from outside delivers everything before it returns, and it stays
lawful after D7F-3 (rung 4). These tests need no re-anchoring later.

**F1–F4 (forcing, rung 1).** `SubscriberArrowBatchTest.` prefix:

| Test | Provider and setup | Ending |
|---|---|---|
| F1 `NoUsableSchemaDropsReportedAtUnsubscribe` | Mock; topic never declared; batched `Subscribe`; 3× `PublishDirect` | `Unsubscribe` |
| F2 `NoUsableSchemaDropsReportedAtDestruction` | same; the `SubscriberArrow` is scoped and the sink outlives it | `~SubscriberArrow` |
| F3 `SchemaDeclaredAfterSubscribeDropsReportedAtUnsubscribe` | `InProcessPubSubProvider{}` (`as_declared`); batched `Subscribe`; then `CreateTopic(kTopic, TestSchema())`; 3× `Publish(MakeRow…)` | `Unsubscribe` |
| F4 `SchemaDeclaredAfterSubscribeDropsReportedAtDestruction` | as F3 | `~SubscriberArrow` |

Each asserts: exactly **one** delivery; `reason == kClosing`; `batch == nullptr`;
`rows_dropped == 3`; `attachments.empty()` (L3: a dropped row takes its attachment). A shared
helper parameterized on provider and ending is the implementer's choice.
- **Turns green by:** design changes 1 and 2. The row reaches `AddRow`'s no-decoder branch, is
  counted, and `Stop`'s `Flush(kClosing)` is no longer stopped by `ready_`. F3/F4 show the class
  beyond the Mock: the schema exists at the topic but never reaches this subscription.
- **Red for the right reason (control):** on the pre-stage tree, each has **0 deliveries**
  (cpp:210 returns early), so `ASSERT_EQ(deliveries.size(), 1u)` fails. The implementer shows all
  four red before the change (ladder Controls, rung 1).

**G1 (guard): `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose`.** Mock, never declared,
batched `Subscribe`, no rows, `Unsubscribe`. Assert 0 deliveries. It forbids a null batch that
means "no rows" (L7) and any delivery for an idle window (L6). It is declared a guard, green before
and after. It fails if the fix removes the idleness gate along with the `ready_` gate.

**Regression:** the whole `pubsub_arrow_tests` suite stays green, notably
`NestedDictionarySchemaReportsEveryRowDropped` (same branch, rejected schema) and
`FinishFailureIsReportedNotFatal` (null on Finish failure, D9's third case). The FastDDS
integration suite is run once at stage close.

**Tier-1 audit of the tests named here:** F1–F4 assert L1, L3, L7, L9 and D9. G1 asserts L6 and L7.
None asserts or accepts nested closing (L9 revised), overlap (L12), or a null batch meaning "no
rows". No test is retired or re-anchored by this stage.

## Risks / Unknowns

- **Estimate.** The plan said 30–50 lines; this design is net negative. The null batch already
  existed in the right place. Only the gate in front of it, a duplicate entry and a dead branch go.
- **Coexistence window (declared).** Until D7F-2, an *open* schema-less subscription reports its
  loss only at close. The timer's wait-for-`ready_` branch (cpp:194-199) and the `ready_` member
  (cpp:68, 114, 194, 198, 263) remain, and **D7F-2 deletes them** (census C3). D7F-1 builds
  nothing that D7F-2 or D7F-3 deletes, so this is not a plan-shape finding.
- **BC-3's register text** says `AcquireCodec`'s import failure "becomes a state, not a
  `return nullptr`". This stage leaves `AcquireCodec` unchanged. The failure now reaches the
  batcher's single no-decoder branch and is reported, which meets BC-3's question ("is the schema
  usable?" is answered for reporting by the decoder's presence alone). Whether a state enum is
  still wanted once `ready_` is gone is the D7F-2 architect's call. Tier 3, not a stop.
- **Usability is still decided twice** (`ImportArrowSchema` and the `BatchDecoder` constructor,
  d7 b2). Both now lead to the same report, and their convergence is d7 Q7, out of scope.
- **Not touched:** the header's timeout wording (hpp:96-97, d7 Findings a1) sits in the same doc
  block but belongs to no D7F item. It is left as found.
- **No STOP-AND-ASK.** No tier-1 collision was found: L1, L6, L7, L8 and L9 (as revised) agree with
  D9 and with "2. Fix both".

## Files-to-touch

- `pubsub-arrow/src/subscriber_arrow.cpp`: changes 1–3 and the comments named above.
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`: doc comment, hpp:103-111.
- `pubsub-arrow/README.md`: lines 85-98.
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: F1–F4, G1; add
  `#include <fletcher/pubsub/in_process_provider.hpp>`.
- `docs/domains/d7-batch-assembly.md`: build marks and findings, as listed.
- `docs/domains/bounded-context-register.md`: BC-2 and BC-4 status notes.

**Declared net lines (production, `cpp` only): about +10 / −25, net ≈ −15.** Docs ≈ +20 / −15
(not counted). Tests ≈ +110 (not counted). New public surface: **0**.

## Files-to-delete

- `RecordBatchBatcher::NoteDropped` (cpp:119-126) → replaced by `AddRow`'s no-decoder branch
  (cpp:81-85), now the one entry for every transport row.
- The `kRowLimit` flush in `SetSchema` (cpp:69-71) → no replacement. It was unreachable, so no
  behaviour is lost.
- The `ready_` gate in `Flush` (cpp:210) → no replacement. Disclosed: the behaviour it carried, a
  silent discard of drops when no schema had arrived, is gone.
- The lambda's `NoteDropped(); return;` branch (cpp:365-368) → the unconditional `AddRow`.
- No files and no tests are deleted. No existing test's subject is forbidden, and none rides a
  construct this stage retires (step-0a audit: CLEAR).
