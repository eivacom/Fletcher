# D7O-3 — Any row count is used as given (design)

Round D7O, domain 7, branch `feature/d7o-batch-options`, tree `f587e36`. Story: `plans/D7O-plan.md`
§ D7O-3. Ladder: `plans/D7O-test-ladder.md` rung 4 and "Tests other than rungs" (the BC-8 sweep).
Locked: D-D7O-3, -4, -6. Step 0a: `plans/reviews/D7O-3-audit-0a.md` (CLEAR). Rulings ledger read in
full: 295 lines, 24 entries, last heading "2026-11-12 — D7O-2: the deadline definition says some
windows have none".
Citations: `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`, `hpp` = its header, `test` =
`pubsub-arrow/tests/test_pubsub_arrow.cpp`, `d7` = `docs/domains/d7-batch-assembly.md`, `reg` =
`docs/domains/bounded-context-register.md`, `bb` = Arrow's `buffer_builder.h` (Conan package). Line
numbers are those of `f587e36`.

## Summary

The batcher pre-sizes its decoder from `max_rows`, and a failed pre-size in `SetSchema` (`cpp:80-85`)
discards the decoder, so a decodable schema is treated as unusable for life. Two changes remove the
whole class. (1) **The verdict on a schema is the decoder constructor's alone.** The pre-size runs
after it, outside the verdict, and a failure is ignored, as `Close` already ignores it. (2) **The
pre-size is bounded.** It sets aside room for at most `kMaxPreSizeRows` (65 536) rows, so no
user-chosen number ever sizes an allocation. `max_rows` keeps deciding when a window closes, exactly
as given. One helper serves both pre-size sites. The BC-8 sweep lands as the register's guard.

## Design

**The class.** "The number asked for decides something other than when a window closes." Today it
also decides (a) whether the schema is usable, through the `catch` in `SetSchema`, and (b) how much
memory Arrow is asked for at the start of every window, through `Reserve(max_rows_)` (`cpp:82`,
`cpp:240`). (a) is the defect the ruling names ("a failed pre-size never makes a decodable schema
unusable"). (b) is the same defect one step earlier. Arrow's size arithmetic is unchecked:
`TypedBufferBuilder::Resize` computes `new_capacity * sizeof(T)` (`bb:287`) and wraps for huge
counts. So for the largest values, whether the pre-size fails or succeeds depends on wraparound
inside another library. A subscriber asking for 50 million rows also has gigabytes set aside at the
start of every window, even a window that closes on its timeout with 3 rows. Removing (a) alone
would leave (b). Bounding the pre-size removes both. Owner Decision 1 (brief) confirms the visible
consequence.

**SetSchema: one verdict, then a hint.** Sketch (the exact shape is the implementer's):
```cpp
void SetSchema(std::shared_ptr<arrow::Schema> schema) {
    std::lock_guard<std::mutex> lk(mu_);
    if (decided_ || stopped_) return;
    decided_ = true;
    if (!schema) return;  // an import failure: unusable for life (L8)
    try {
        decoder_ = std::make_unique<BatchDecoder>(std::move(schema));
    } catch (...) {
        return;           // undecodable: no decoder, for life (L8)
    }
    PreSize();            // a hint, never a verdict
}
```
`SetSchema` has no `decoder_.reset()` at all. Once the decoder exists, nothing in it can null it.
Only `Loop`'s end releases it (`cpp:284`), after the last delivery.

**One pre-size helper.** It is called from `SetSchema` and from `Close` for a non-closing window
(`cpp:238-243`, which it replaces). Caller holds `mu_`.
```cpp
// Room set aside for the next window: a hint, never a limit or a verdict. Bounded, so the number a
// subscriber asks for never sizes an allocation (L15: max_rows decides only when a window closes).
// A failure is ignored: the next Append() grows the builders.
void PreSize() {
    try { decoder_->Reserve(std::min(max_rows_, kMaxPreSizeRows)); } catch (...) {}
}
static constexpr int64_t kMaxPreSizeRows = 65536;
```
- **Why 65 536.** Every `max_rows` up to it keeps today's exact pre-size. That covers the default
  8000, the benchmarks' {1, 1000, 8000}, and every test value except the 100 000 helpers. And
  2^16 × the widest fixed element Arrow allows (`fixed_size_binary`, 2^31 − 1 bytes) is below 2^47,
  so `bb:287`'s product cannot overflow for any schema.
- **Not a clamp, and not a range check (D-D7O-3, D-D7O-4).** `max_rows_` is stored and compared
  unchanged (`cpp:118`). The bound refuses nothing and alters no window. It sizes a buffer the
  subscriber never sees. BC-8's one check (`cpp:42`) is untouched. Rung 4's guard arm (c) fails if
  an implementation lets the bound leak into the window size.
- **Concurrency.** `PreSize` runs under `mu_`, as both of today's call sites do. It adds no state,
  lock or thread.
- **Error paths.** `PreSize` lets nothing escape (L10). A partly applied `Reserve` (some columns
  grown, then a throw) leaves a decoder with zero rows and some spare capacity (premise 1).

**What a user sees.**
- `max_rows` ≤ 65 536: nothing changes.
- Larger: the batches are the same, but room is no longer set aside for the whole batch up front. A
  big window grows as rows arrive, with a few reallocations while it fills.
- The largest values (rung 4): rows are delivered instead of reported lost.

**Unchanged:**
- BC-3's site. "Can rows be assembled?" is still decoder presence. That now means the
  constructor's verdict alone, as BC-3 will say (one allocation residue remains: Risks).
- L8's permanent cases (an undecodable or unimportable schema).
- `BatchStatus`, its reasons, `RecordBatchCallback`, `Unsubscribe` and `BatchDecoder` (D-D7O-6).

**Docs (not counted).**
- `hpp:78`: `max_rows` gains "Any larger value is used as given, however large." plus one
  disclosure sentence: a window that outgrows memory before it closes is lost and counted in
  `rows_dropped` (L1, L10; Decision 2).
- `hpp:121`: "Any other timeout is used as given" is restored to "Any other value is used as given"
  (4a N1, inherited).
- `pubsub-arrow/README.md:79-83`: one sentence: any `max_rows` of 1 or more is used as given.
- `d7`:
  - header blockquote (`d7:7`): the huge `max_rows` case becomes `[built]` (D7O-3);
  - L15: the "used as given" clause becomes `[built]` for `max_rows`; "Holds at" gains the pre-size
    helper; Tests gain the two below;
  - L8: "Holds at" gains "the constructor alone decides; a failed pre-size decides nothing (ruling
    2026-11-12, 'Fix in D7O')";
  - §(c), last bullet: resolved.
- `reg` BC-8: the sweep becomes `[built]`, named in the test column. BC-3: one clause, "decoder
  presence is the constructor's verdict alone (D7O-3)".

## Corner cases forbidden

Rung 1 (unrepresentable):
- **A pre-size deciding the schema's verdict.** `SetSchema` has no reset path after construction.
  The only way to "no decoder" is the constructor throwing.
- **A subscriber's number sizing an allocation, or overflowing Arrow's size arithmetic.** The
  pre-size is `min(max_rows_, 65 536)`, so no larger value reaches Arrow.
- **Two pre-size sites that disagree** (one fatal, one not, as today). There is one helper.
- **A clamp or second range check disguised as the bound.** The bound feeds only `Reserve`.
  `max_rows_` is never rewritten. Rung 4 (c) proves it from outside.

Rung 2 (refused at the door): nothing new. BC-8's refusal is unchanged; the sweep proves it.

Handled residue:
- **A bounded pre-size that still fails** (memory pressure, or a very wide fixed-size column). It is
  ignored, and `Append` grows the builders. *Why not forbidden:* allocation failure is a property
  of the machine, and `Reserve` belongs to domain 2, which this domain cannot make infallible.
  Dropping the pre-size entirely would forbid it, but the default path would lose its measured
  allocation profile (`arrow-bridge/benchmarks/README.md:296-299`) to save one `try`.
- **A window that outgrows memory** (a huge `max_rows`, no timeout, a fast stream of fixed-width
  rows). `Append` fails, and the window is dropped whole and counted (`DiscardPending`, L1, L10), as
  happens today for any window that exhausts memory. *Why not forbidden:* closing a window early on
  memory pressure would be a fifth closing event, which is a change to L4 and the owner's call.
  It goes to the owner as brief Decision 2 and is disclosed in the header.

## Premises and stop conditions

1. **A failed `BatchDecoder::Reserve` leaves the decoder usable**: zero rows, appendable. `Close`
   already relies on this (`cpp:236-243`). A failed Arrow `Resize` leaves the builder's capacity
   unchanged. **STOP-AND-ASK** if any test shows otherwise: that needs a domain 2 contract
   statement, not an edit to `BatchDecoder` here.
2. **`Reserve` is only a hint**: `Append` grows the builders by itself. `cpp:236` says so, and the
   benchmarks measure growth beyond `Reserve(N)`. **STOP** if `Append` needs a prior `Reserve`.
3. **Rung 4 is red on `f587e36`.** `Reserve(INT64_MAX)` on `TestSchema()` (int32 + utf8) is expected
   to throw: the int32 byte count wraps to −4 at `bb:287` (`int64 × size_t`, an unsigned, defined
   wrap), and Arrow refuses a negative size before touching the builder's capacity. Record what is
   observed, and count a crash as red. **STOP and report** if arm (a) or (b) passes on `f587e36`.
4. **The Mock delivers synchronously** (`test:41`), so `AddRow` has run by the time `Publish`
   returns.
5. **No edit under `arrow-bridge/`, `c-abi/` or `dotnet/`.** STOP-AND-ASK if the build forces one.

## Forcing-test mapping

Both tests go in `test`, suite `SubscriberArrowBatchTest`, in a new section "D7O-3 (L15, L8,
BC-8)". They are driven from outside through `MockProvider`, `PublisherArrow`, `BatchSink`,
`MakeRow`, `XAt`, `Observe` and `Describe` (all exist; `test:491`, `:2509-2585`). Every sink is
declared before its `SubscriberArrow`. Observation windows are the 200 ms one-sided
`kObservationWindow`.

**Rung 4, forcing: `MaxRowsLargerThanAnyWindowIsUsedAsGiven`.** Declared `TestSchema()`.
- **(a)** `max_rows` = `INT64_MAX`, timeout 10 ms, 3 `Publish` (x = 0, 1, 2). Wait (2 s) until the
  deliveries hold 3 rows in total, then `Unsubscribe`.
  - Every delivery is non-null, `kTimeout` or `kClosing`, with `rows_dropped` 0.
  - Σ`num_rows` = 3, and x reads 0, 1, 2 in order.
  - There is at least one `kTimeout`.
  - *Why a sum:* a slow machine may split the rows across two deadlines, as with D7O-2's DEBT-1.
    The sum is one-sided.
  - *Red on `f587e36`:* a null `kTimeout` batch with `rows_dropped` 3.
- **(b)** `max_rows` = `INT64_MAX`, `kNoTimeout`, 3 `Publish`.
  - Nothing arrives within the observation window.
  - `Unsubscribe` delivers exactly one batch: `kClosing`, 3 rows (x = 0, 1, 2), `rows_dropped` 0.
  - *Red:* one null `kClosing` batch with `rows_dropped` 3.
  - Arm (a) is (b)'s control: the same body with a finite timeout does get a timed delivery.
- **(c) GUARD, green both ways:** `max_rows` = 131 073 (2^17 + 1, above the bound; the comment
  names `kMaxPreSizeRows`), `kNoTimeout`, `max_rows + 1` `Publish`.
  - `WaitFor(1)`: exactly one `kRowLimit` batch of 131 073 rows, `rows_dropped` 0. Its first and
    last x are checked.
  - `Unsubscribe`: one `kClosing` batch with 1 row.
  - This forbids a fix that clamps `max_rows` to the bound, or lets the bound close a window.
- *Turned green by:* the verdict/hint split in `SetSchema` and the bounded `PreSize`. Either alone
  turns (a) and (b) green on this schema. Both are required by the ruling and by the class
  (Design).

**BC-8 sweep, GUARD: `EveryBatchOptionIsRefusedExactlyWhenOutOfRange`.** The grid is the ladder's:
`max_rows` {`INT64_MIN`, −1, 0, 1, 2, `INT64_MAX`} × timeout {`milliseconds::min()`, −1 ms, 0, 1 ms,
`kNoTimeout`, 100 years, `duration_cast<milliseconds>(steady_clock::duration::max())`}.
`milliseconds::max()` *is* `kNoTimeout`, so it is one column labelled with both names: 42 cells.
Each cell gets a fresh Mock, publisher and subscriber. The test's oracle is L15 written literally:
refused ⇔ `max_rows < 1 || timeout < 0`.
- **Refused cells (27):**
  - `PubSubError(kInvalidArgument)`;
  - the callback's `weak_ptr` token has expired straight after the catch, as in `test:2376-2406`;
  - the Mock's `subscribe_with_options_count` is 0;
  - one `Publish` follows. One observation window covers all refused cells at the end, and every
    refused sink is empty.
- **Accepted cells (15):** no throw, and the count is 1. One `Publish` (x = 7, timestamped just
  before it), then `Unsubscribe`.
  - Σ`num_rows` = 1 and Σ`rows_dropped` = 0, every batch non-null (L15 "used as given"; stronger
    than the ladder's sum, lawfully).
  - With `max_rows` 1, the one batch is `kRowLimit`.
  - With timeout ≥ 100 years or `kNoTimeout`, no `kTimeout` arrives.
  - Any `kTimeout` arrives ≥ 1 ms after the timestamp (L5 floor). This needs a timestamped sink
    local to the test, as in `test:2471-2489`.
- *Declared:* green both ways except the 5 accepted `INT64_MAX` cells. Those are red on `f587e36`
  for rung 4's reason (null batch, `rows_dropped` 1), and that overlap is declared, not hidden.

**Order of the red work.** Add both tests and run them on `f587e36`. Rung 4 (a) and (b) are red,
(c) is green. The sweep is red only in its 5 `INT64_MAX` accepted cells. Then implement, and all go
green.

**Existing tests:** none retired or re-anchored (step 0a CLEAR).
- `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` and `InRangeBatchOptionsAreUsedAsGiven`
  overlap the sweep but stay. They are lawful proofs, and the second asserts the floor on a
  measured path.
- `NestedDictionarySchemaReportsEveryRowDropped`, the BC-3 guard's undecodable and unimportable
  cells, and `FinishFailureIsReportedNotFatal` stay as they are. They pin L8's lawful permanent
  cases and the decoder's reuse after a failure, which this design keeps.

## Risks / Unknowns

- **Out of class, reported, not designed:** an *allocation* failure inside the `BatchDecoder`
  constructor (not a rejection) also makes a decodable schema unusable for life, through the same
  `catch` (`cpp:83`). That is a different cause from the ruling's "failed pre-size", so fixing it
  here would add a value subject. It goes to the PM as a possible register note under BC-3 / L8.
  Separating "rejected" from "failed to build" would need domain 2's exception contract
  (`batch_decoder.hpp:28-41`).
- **Stale comment in domain 2:** `arrow-bridge/benchmarks/bench_arrow_codec.cpp:388-390` describes
  `Reserve(max_rows_)` in `RecordBatchBatcher::Flush`. It has been stale since D7F renamed `Flush` to
  `Close`, and it stays literally true for its N ≤ 8000. It is not edited (outside domain 7). Noted
  for domain 2's next round.
- **The bound is in rows, not bytes.** A very wide fixed-size column can still make a 65 536-row
  pre-size large or failing. That is handled (ignored on failure), the same as today at
  `max_rows` 8000. A byte-based bound needs domain 2 (`Reserve` by bytes). Not pursued.
- **Decision 2's residue** (a window that outgrows memory is lost and counted) is pre-existing for
  every batch size. Rung 4 does not make it newly reachable, only easier to ask for.
- **No coexistence window.** `Close`'s inline pre-size is replaced in place. Nothing in a later
  stage is touched: this is the round's last item.
- **STOP-AND-ASK: none.** L15, L8, the ruling "Fix in D7O", D-D7O-3/4/6 and BC-8 agree. No tier-2
  clause contradicts them. The header's narrowed sentence is the inherited 4a N1, restored.

## Files-to-touch

- `pubsub-arrow/src/subscriber_arrow.cpp`: `SetSchema` (`:71-87`, with its comment), `Close`'s
  pre-size (`:236-243`), the new `PreSize` and `kMaxPreSizeRows` (beside `kMinWindow`, `:308`).
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`: comments only (`:78`, `:121`).
- `pubsub-arrow/README.md`: `:79-83`.
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: the two tests (about +190).
- `docs/domains/d7-batch-assembly.md`: the header, L8, L15, §(c).
- `docs/domains/bounded-context-register.md`: BC-8, plus one clause in BC-3.

**Net lines (production, `cpp` only): +16 / −10.** Tests are about +190. Docs are about +15 / −6
and are not counted. New public surface: 0. The round's 1 is spent, and this design needs none.

## Files-to-delete

- **The verdict-by-pre-size:** `SetSchema`'s `decoder_->Reserve(max_rows_)` inside the
  constructor's `try`, with `catch (...) { decoder_.reset(); }` (`cpp:80-85`). It is replaced by
  the constructor-only verdict plus `PreSize`. Behaviour gone, disclosed: a decodable schema is no
  longer made unusable by a failed pre-size.
- **`Close`'s inline `try { decoder_->Reserve(max_rows_); } catch (...) {}`** (`cpp:238-243`). It
  is replaced by `PreSize()`, the one pre-size site.
- **An unbounded pre-size.** Room set aside per window no longer grows past 65 536 rows. Behaviour
  changed, disclosed (brief Decision 1).
- **No file, type or test is deleted.** Every test in scope is lawful (0a CLEAR). The stage retires
  a behaviour and a duplicated block. It adds no construct beyond the one helper that replaces
  them.
