# D7F-2 — Loss before a usable schema, reported at each deadline (design)

Round D7F, item D7F-2, ladder rung 2. Branch `feature/d7f-batch-assembly`, HEAD `4f4eec7`.
Citations: `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`, `hpp` = its header, `test` =
`pubsub-arrow/tests/test_pubsub_arrow.cpp`, `d7` = `docs/domains/d7-batch-assembly.md`, `reg` =
`docs/domains/bounded-context-register.md`. Line numbers are at `4f4eec7`.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **169 lines, 14 entries**; the last is
"2026-11-10 — Queued windows are delivered before the closing batch".

## Summary

An open batched subscription whose rows have no usable schema now gets one `kTimeout` report per
window at the window's deadline: `batch == nullptr`, `rows_dropped` = that window's rows. A usable
schema that arrives later gives normal batches. Today the timer parks on `ready_` until a schema
arrives (cpp:181-186), so the loss is told only at close (D7F-1). The fix deletes that wait and the
`ready_` flag. It also resolves DEBT-3 (the codec-lookup race), DEBT-4 (a drops-then-good-rows test)
and DEBT-5 (the header's timeout sentence). Net production lines are negative.

## Design

### 1. One rule for the deadline, in every state (L1 violation 1, L5, L6)

`TimerLoop` flushes at every deadline it reaches. The `if (ready_) … else { wait for ready_ }`
branch (cpp:181-186) becomes the single line `Flush(lk, BatchStatus::Reason::kTimeout);`. `Flush`
already builds the null batch when there is no decoder (D7F-1, cpp:202-222), and it already
returns without delivering when nothing is pending (cpp:197, L6). Nothing is added to `Flush`.

The deadline arithmetic (`ArmTimer`, cpp:160-167) is untouched, and so is the cadence (L5):
`max(first event + 1 ms, previous flush + timeout)`. A drop arms it exactly as a decoded row does,
which `AddRow`'s no-decoder branch already does (cpp:77-80). The schema-less window therefore
follows the same cadence as every other window. That is the user story's "same cadence as any
other window".

**The batch's shape is decided when the window closes, by the state at that moment.** If there is no
decoder, the batch is null. If there is one, the batch is non-null, possibly with zero rows. A window
that starts without a schema and gains one before it closes is a normal batch: its decoded rows plus
the earlier drops in `rows_dropped`. L7 holds, because a batch *could* be built for that window. L2
holds per window.

**Drops never close a window by count, in any state.** The count trigger is decoded rows only
(`decoder_->num_rows() >= max_rows_`). That is today's rule for an assembling window full of
malformed rows, and it now holds the same way with no schema. So a schema-less window closes at the
deadline or at close, never as `kRowLimit`. This is the ladder's "a `kTimeout` null batch … once per
window". Code review N3 asked whether D7F-2 would change this; it does not, on purpose.

### 2. `ready_` is deleted, not renamed (census C3, BC-3)

`ready_` has three uses: the timer branch (deleted above), `AddRow` cpp:110 `ready_ &&`, and
`SetSchema`'s `cv_.notify_all()`. At cpp:110 it is redundant, because the `!decoder_` early return
at cpp:77 means a decoder exists whenever line 110 runs. It becomes
`if (decoder_->num_rows() >= max_rows_)`. The notify in `SetSchema` existed only for the deleted
wait. With all three gone, the member is deleted.

**Why there is no state enum** (D7F-1 Risks left this call to D7F-2). BC-3 asks for one answer to
"which state is this subscription in, and is its schema usable?". After this change, the answers
are:
- *Can rows be assembled?* Decoder presence, one site. "Awaiting a schema" and "undecodable" are
  the same state for the batcher, because they behave the same at every ending. Making them
  indistinguishable to the reporting code is the fix itself: there is no longer a flag that
  reporting could be gated on.
- *Has the schema been decided?* The subscription's once-flag, one site (§3). It is set at the first
  row that brings a schema, whether usable or not, and never again. This one decision is what
  makes "undecodable" permanent (L8).
- *Closed?* `stopped_`, unchanged.

An enum would carry a distinction (awaiting vs undecodable) that no behaviour reads. Not built. The
register's BC-3 single site is rewritten to name these sites (Files-to-touch).

### 3. The schema handed to the batcher is the one that was found or built (DEBT-3)

Today the batched lambda calls `AcquireCodec`, checks the `Codec*` for null only, and inside the
`call_once` looks `codecs_[key]` up a second time (cpp:356-362). A concurrent last-`Unsubscribe` on
the same topic can erase the entry between the two lookups (d7 finding a3). The batcher then gets
`SetSchema(nullptr)` and drops every row for the subscription's whole life.

**Make the race unrepresentable:**
- `TopicCodec::codec` becomes `std::shared_ptr<Codec>` (hpp:161-164).
- `AcquireCodec` returns `TopicCodec` **by value**, copying both owners under `mu_`. It is
  empty when no entry exists and the row brings no schema, or when the row's schema cannot be
  imported. A caller holds what it found, whatever happens to the map afterwards. There is no
  second lookup left to lose.
- The batched lambda becomes:
  ```
  if (!schema_decided->load(std::memory_order_acquire)) {
      TopicCodec found = AcquireCodec(key, schema);
      if (found.arrow_schema || schema) {   // known, or brought by this row (even if unimportable)
          std::call_once(*schema_set, [&] { batcher->SetSchema(std::move(found.arrow_schema)); });
          schema_decided->store(true, std::memory_order_release);
      }
  }
  batcher->AddRow(data, len, att);
  ```
  `schema_decided` (`std::atomic<bool>`) replaces `cached_codec` (`std::atomic<Codec*>`), which only
  ever served as a "decided yet?" flag. Its comment (cpp:331-333, "Codecs live in `codecs_` for the
  SubscriberArrow's lifetime") is false (finding a3) and is deleted.
- **An import failure becomes a decided state (BC-3), not a retry on every row.** A row that brings
  a schema `ImportArrowSchema` cannot read is decided with a null `arrow_schema`. `SetSchema(nullptr)`
  then means "this subscription's schema is unusable", and L8 makes that permanent. It is the same
  door, and the same permanent result, as a schema `BatchDecoder` rejects. Today an import failure
  is retried on every row and is never final, which deviates from L8 (tier 3, changed).
- `SetSchema(schema)`: a null schema returns at once (the decoder stays null). Otherwise it builds
  the decoder and `Reserve`s, as today. The `catch` keeps `decoder_.reset()`. Code review N2 called
  that a no-op, but it is not: if `Reserve` throws after the assignment, a decoder is left behind.
  The `ready_ =` line and the notify go.
- The per-row overload takes the same by-value result: `TopicCodec found = AcquireCodec(...);
  if (!found.codec) return; … found.codec->DecodeRow(...)`. A consequence, not a new subject: a
  per-row decode now holds its codec alive across a concurrent erase (finding a3's per-row half).
  How the per-row path handles a row it cannot decode is unchanged (BC-2's registered gap).

### 4. Concurrency and lock order

Nothing new. `AcquireCodec` takes `SubscriberArrow::mu_` alone. `SetSchema` takes the batcher's
`mu_` alone, never nested. The timer and `AddRow` serialize on the batcher's `mu_` as today. A
deadline that falls between a window's drops and `SetSchema` closes that window as a null batch,
and the next window assembles. Conservation holds either way, because each row is counted once
in `AddRow`. `Flush` still releases `mu_` around `cb_`. Moving delivery onto the batch thread is
D7F-3's job (the timer thread becomes the batch thread), and this stage adds nothing D7F-3 must
delete.

### 5. Documentation (not counted as production)

- **hpp:95-98 (DEBT-5, d7 finding a1).** "…or `options.timeout` elapses since the first row/drop of
  the current batch" was false twice over: for a schema-less open subscription until this stage, and
  always for the anchoring (L5). It is replaced with L5's rule: *flushed when `options.max_rows`
  rows have been decoded, or at the window's deadline, `options.timeout` after the previous flush
  but never sooner than 1 ms after the window's first row or drop, whichever comes first. This
  holds whether or not a usable schema has arrived; lost rows never count towards `max_rows`.*
- **README:66-68.** The same sentence, in the README's words.
- **d7.**
  - L1 mark becomes "violation 1 built (closing: D7F-1; deadline: D7F-2); violation 2 open (D7F-3)",
    with the new tests listed.
  - L6: delete the "Interim (until D7F-2)" line.
  - L13: the Mock forcing test is its first batched subscriber-first test.
  - L8: an import failure is decided once and is permanent.
  - §2: delete the term **Ready**.
  - Findings: a1 resolved; a3 resolved; b2 noted (two decisions, one door, both permanent); (c)
    pre-schema drops at the deadline now tested.
- **reg BC-3:** new single sites (§2) and status `[built]`, D7F-1 and D7F-2. "One usability
  decision" still waits on d7 Q7.

## Corner cases forbidden

Rung 1, unrepresentable:
- **The timer parking without a deadline check.** The wait-for-`ready_` state is deleted, so every
  reached deadline flushes or finds nothing pending.
- **Reporting gated on schema arrival.** No flag exists to gate on. Decoder presence decides only
  the batch's shape, never whether the loss is reported.
- **`SetSchema(nullptr)` caused by a race.** No second lookup exists, and `AcquireCodec` returns
  owners. A null schema reaches `SetSchema` only when a delivered schema could not be imported.
- **A second schema decision for one subscription** (once-flag plus decided flag). This covers an
  unusable schema later "recovering" (L8), now for import failures too.
- **A dangling `Codec*`** in either overload: callers hold `shared_ptr`s.
- **Reporting a window twice, or reporting an idle window.** `Flush` resets `dropped_` and clears
  the deadline under `mu_`, and `!HasPending()` returns early (L6). Both are pinned by the forcing
  test's idle check.

Rung 2: none needed.

Handled residue:
- **A deadline that splits a window around the schema's arrival** gives a null batch, then normal
  batches. *Why not forbidden?* Forbidding it would mean holding a window open past its deadline
  for a schema that may never come. That is the very silence this stage removes (L1, L5).

## Premises and stop conditions

- **P1.** The Mock hands a null `SharedSchema` for a topic never declared, and hands the real one
  on every `Publish` after `CreateTopic` (test:51-58, verified). *If false, STOP-AND-ASK. Do not
  move the forcing test to another provider without saying so.*
- **P2.** `InProcessPubSubProvider` (`as_declared`) never gives a subscription made before the
  declaration a schema (audit 0a, ladder ground rules). *If it does, the in-process variant will
  show normal batches: STOP-AND-ASK. Do not weaken the assertion.* That test pins L1/L6/L7 for "no
  usable schema", not "a late schema is unusable". Its comment must say so (audit 0a caution).
- **P3.** `SharedSchema` is tested for null as `AcquireCodec` already does, and `ImportArrowSchema`
  signals failure by throwing or returning null (cpp:390-397). *If a third failure shape exists,
  STOP-AND-ASK.* **As built (DEBT-1, `dbaa2b6`):** only a throw decides "unusable"; a null return
  (a released or empty handle) decides nothing, so a later schema is still used.
- **P4.** Sharing one `Codec` between per-row subscriptions on one topic is today's arrangement.
  `shared_ptr` changes who owns it, not who calls it. *If `Codec` holds per-call mutable state that
  shared ownership would newly expose, STOP-AND-ASK. Do not add a lock.*
- **P5.** No caller outside `SubscriberArrow` names `TopicCodec` or `AcquireCodec`; both are private
  (hpp:155-176). `PublisherArrow`'s `TopicCodec` is a separate type. *If a friend or test reaches
  either, STOP-AND-ASK. That would be public surface.*

## Forcing-test mapping

All are external (`PublisherArrow` / `SubscriberArrow`). They read results only through
`sink.WaitFor` or after an outside `Unsubscribe` (L9), so they survive D7F-3's move unchanged.

| Test | Kind | Asserts | Red before, for the right reason |
|---|---|---|---|
| `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` (Mock) | **forcing** (rung 2; DEBT-4 across windows; L13) | `max_rows` 100000, `timeout` 100 ms; `Subscribe` before any `CreateTopic`. **W1:** `PublishSchemaLessRows` (3 rows, one with an attachment). `WaitFor(1, 2 s)`: `kTimeout`, null, `rows_dropped` 3, no attachments. **Idle:** sleep 3×timeout, still 1 delivery. **W2:** one `PublishDirect`. `WaitFor(2)`: `kTimeout`, null, `rows_dropped` 1. **Late schema:** `CreateTopic(TestSchema)`, `PublishWellFormedRows` (x = 1, 2, 3), `Unsubscribe`. Every later delivery is non-null, with `rows_dropped` 0 and `attachments.size() == num_rows`. Their rows, concatenated, are x = 1, 2, 3 in order, with the attachment on x = 2. Over all deliveries, Σ`num_rows` + Σ`rows_dropped` = 7. These checks are stall-proof: a slow machine may split the late-schema rows across `kTimeout`/`kClosing` and stay green. | W1's `WaitFor` times out, because the timer parks on `ready_` (cpp:185). |
| `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` (InProcess) | **forcing** (ladder variant) | Same options. `Subscribe`, then `CreateTopic`. **W1:** `PublishWellFormedRows`. `WaitFor(1)`: `kTimeout`, null, 3. **W2:** one `Publish`. `WaitFor(2)`: `kTimeout`, null, 1. `Unsubscribe`: still 2 deliveries. | W1's `WaitFor` times out. |
| `EveryAssemblyStateReportsItsDropsAtEitherEnding` (Mock) | **guard** (BC-3/C3 structural test, ladder "States × endings") | A table of {awaiting: topic never declared; undecodable: nested-dictionary schema, as test:1003-1005; assembling: `TestSchema`} × {timeout: 100 ms, then `WaitFor(1)`, then `Unsubscribe`; closing: 10 min, then `Unsubscribe`}. Each cell sends one 0x00 row. Each cell gets exactly one delivery, whose reason matches the ending and whose `rows_dropped` is 1. The batch is null for awaiting and undecodable, and non-null with 0 rows for assembling. `SCOPED_TRACE` per cell. | Only the awaiting×timeout cell is red. The other five are green before and after (declared). It fails if any state suppresses reporting. |
| `SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops` (Mock) | **leg** (DEBT-4, code-review S2 shape; L2/L3/L7 in the transition window) | 10 min timeout. `PublishSchemaLessRows`, `CreateTopic`, 2× `Publish` (one with an attachment), `Unsubscribe`. Exactly one `kClosing`, non-null, `num_rows` 2, `rows_dropped` 3, 2 attachments aligned. | Green before and after (declared). The forcing test carries the promise; this pins the mixed window's shape. |

**As built (`dbaa2b6`, `7170ca9`):**
- The forcing test does not assume one delivery per window. It waits (2 s) until null `kTimeout`
  reports add up to exactly 3 lost rows for W1, then exactly 4 for W2, and checks every report in
  between as null and non-empty (L6). Σ`num_rows` + Σ`rows_dropped` = 7 is checked at the end.
- The guard has four states, not three: awaiting, undecodable, **unimportable** and assembling, each
  at both endings (eight cells). The unimportable state is a struct whose child has a format string
  Arrow rejects. `Publisher::CreateTopic` accepts it and the Mock hands it on, so
  `ImportArrowSchema` throws (the Risks line below is corrected).
- Each unimportable cell then declares `TestSchema` and sends a well-formed row, to pin L8. So the
  deadline cell gets two reports (`kTimeout`, null, `rows_dropped` 1 and 1), and the close cell gets
  one (`kClosing`, null, `rows_dropped` 2).

Every test was checked against tier 1:
- None asserts drops counting towards `max_rows`, either way. That stays unpinned, as the rung does
  not name it.
- None asserts that a late schema is unusable.
- The existing F1-F4 and G1 stay lawful and unchanged (audit 0a).

No new value subject.

## Risks / Unknowns

- **No STOP-AND-ASK.** L1, L5, L6, L8 and L13 agree with D9, with "2. Fix both" and with the rung's
  wording. The header's timing sentence (tier 3) contradicts L5 and is corrected.
- **Import failure made final (§3)** is a narrowing that only a provider handing one subscription an
  unimportable schema and later an importable one could observe. The Mock can (corrected
  after build; see "As built" above). It follows L8, and the brief discloses it. `PublisherArrow` cannot declare an unimportable schema.
  If `Publisher::CreateTopic` accepts an `OwnedSchema` whose format string `ImportArrowSchema`
  rejects, the implementer adds that as a fourth "undecodable" row of the guard. Otherwise the
  case stays untested and disclosed. It goes through the same `SetSchema` door as the guard's
  nested-dictionary cell. **As built:** it does accept one, so the guard has the fourth state.
- **D7F-3 interplay.** This stage only removes code from `TimerLoop`, which D7F-3 turns into the
  batch loop. It adds nothing D7F-3 deletes (ladder:62-65). The four tests are not in the
  re-anchoring list.
- **Test time.** About 0.4 s (forcing, Mock) + 0.25 s + 0.35 s (guard timeout cells), well under
  the 30 s ctest TIMEOUT.
- **Coexistence window:** none. D7F-1's declared window (loss told only at close) closes here.

## Files-to-touch

- `pubsub-arrow/src/subscriber_arrow.cpp`: §1-§3. This covers `SetSchema`, `AddRow`:110,
  `TimerLoop`, the `ready_` member, both `Subscribe` lambdas, `AcquireCodec`, and the comments at
  cpp:53-56, 331-334 and 341-354.
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`: `TopicCodec::codec` type,
  the `AcquireCodec` declaration and comment (private), doc hpp:95-98.
- `pubsub-arrow/README.md`: lines 66-68.
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: the four tests above.
- `docs/domains/d7-batch-assembly.md`, `docs/domains/bounded-context-register.md`: as in §5.

**Declared net lines (production: `cpp` plus header code lines): +20 / −40, net ≈ −20.**
Docs ≈ +25 / −15 and tests ≈ +160, not counted. New public surface: **0**.

## Files-to-delete

- The timer's wait-for-`ready_` branch (cpp:181, 183-186). No replacement. Disclosed: the behaviour
  it carried, silence at the deadline until a schema arrived, is gone. That is the fix.
- `RecordBatchBatcher::ready_` (cpp:249), its write and notify in `SetSchema` (cpp:67-68), and its
  read at cpp:110. No replacement: decoder presence already answers the question.
- `cached_codec` (`std::atomic<Codec*>`, cpp:334) is replaced by `schema_decided`
  (`std::atomic<bool>`).
- The second `codecs_` lookup inside the `call_once` (cpp:357-362) is replaced by the schema
  `AcquireCodec` returns.
- The `Codec*` return of `AcquireCodec` and `TopicCodec`'s `std::unique_ptr<Codec>` are replaced by
  `TopicCodec` by value with a `shared_ptr<Codec>`.
- The false lifetime comment (cpp:331-333). No replacement.
- d7 L6's "Interim (until D7F-2)" line and the d7 §2 term **Ready**. No replacement.
- Tests: none are deleted or re-anchored (audit 0a: all lawful).
