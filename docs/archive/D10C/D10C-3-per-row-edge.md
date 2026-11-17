# D10C-3 — per-row `SubscriberArrow.Subscribe` on the native per-row path (rung 3)

Ladder rung 3 (`plans/D10C-test-ladder.md:48`, ground rules :21-40); citations as the ladder; `rul` read in
full (**603 lines, 45 entries, last "## 2026-11-15 — D10C ladder: the domain-7 port tests" :595**);
`SUB` = `dotnet/src/Fletcher/Subscriber.cs`. 0a: `plans/reviews/D10C-3-audit-0a.md` (CLEAR).

## Summary

`SubscriberArrow.Subscribe` stops being `SubscribeBatched(MaxRows = 1)` (SA:206-207) and becomes a row handler
over the inner `Subscriber` (`fl_subscriber_subscribe`) decoding with `fl_decode_rows`. Each row offered gives
exactly one handler call: one decoded row, or null with `RowsDropped = 1` (rul:575 (3); d10 L7, L9, D20, D30;
BC-16 DEFINE). No `Batcher`, window or timer, so it runs on the edge shim (rul:536 (1), :363 (4)).

## Design

**One per-row state per subscription** (private nested `RowDelivery`): its `SubscriberArrow`, the handler, the
id, a small gate and the codec decision. The inner `Subscriber` calls it on the transport thread inside the
existing thunk and frame push (SUB:812-864), so L10 and L11 hold unchanged.

**The one classify-and-count site (BC-16): `RowDelivery.OnRow(row, schema, view)`.** Straight-line, no
early return, so every row reaches exactly one handler call:
1. Inside one `try` that catches everything: copy the attachments (below), then get the codec (below),
   then `codec.Decode(row)` (`FletcherCodec.Decode` = `fl_decode_rows`, count 1; refuses bad or trailing
   bytes, domain 2's answer). Any throw, or no codec, leaves `batch = null`.
2. `batch is null` ⇔ drop. A drop disposes any copied attachments and calls
   `handler(null, [], BatchStatus(RowLimit, 1))`; a row calls `handler(batch, [copy], BatchStatus(RowLimit, 0))`.
   The handler owns both (D-BIND-59, unchanged).
3. The handler call is wrapped: an exception is `SubscriberArrow.ReportAbsorbed` (SA:335-350), so T41's
   count lands on the `SubscriberArrow`, never on the hidden inner `Subscriber` (L9).

**The codec decision** (follows d7 L8/L13 as rul:258 (3) requires of "a C# subscription"): under the
gate, while undecided, a row whose schema handle is null decides nothing (that row is a drop); the
first non-null schema decides once: `new FletcherCodec(schema.ToArrowSchema())`, or `null` if that
throws (an unopenable schema: every later row is a drop, d7 L8's permanence). The gate is never held
across decode or the handler. Decodes on one codec may run concurrently (premise P1).

**Codec release, made ordering-safe.** The codec must outlive every delivery of its subscription,
including after an in-handler `Unsubscribe` (the D-BIND-50 carve-out, which does not drain). The inner
`Subscriber` already has the one exactly-once point after which no delivery of a subscription can run:
`SubscriptionState.TryFree` (SUB:954-974), reached from the drained cancel, the last delivery out, the
deferred free and `Subscriber.Dispose`. `Subscriber` gains an **internal** `Subscribe` overload taking an
`IDisposable` released there (and on the failed-subscribe branch, SUB:439-443); the public overloads pass
null. `RowDelivery.Dispose` disposes the codec. No reference counting, no "is anyone still decoding" check.

**The attachment copy: one function, two callers** (ladder accumulation; census D16). A private static
`SubscriberArrow.CopyAttachments(AttachmentsView)` outside `Batcher`: copies every pair through
`AppendDelivered` (empty keys cross, T32), disposes the partial copy and rethrows on failure. Rung 7's
batch thunk is its second caller. `Batcher`'s own copy (SA:467-485) is not edited (D3) and dies at rung 8.

**Per-row `BatchReason` is `RowLimit` on every call — determined, not chosen.** L2 forbids a C# reason
the C surface does not publish (`BatchReason` = `FL_BATCH_*`, D34), and a new public member is on this
round's STOP list. `Closing` means the last delivery of a subscription (L14, D17) and `Timeout` a passed
deadline; both would be false here. `RowLimit` ("the window is full at its one row") is the one true value.
T8's assertion (SA test :284) is therefore lawful and stays; its doc cites L2 instead of the vehicle.

**No closing delivery on a per-row subscription.** Nothing is pending (no window), and BC-16 requires every
call to carry exactly one row or one drop, so an empty `Closing` call is forbidden. `Unsubscribe` and
`Dispose` keep their code (SA:261-282, :303-333): with no `Batcher` registered, they reduce to the inner
`Subscriber`'s cancel. The id is the inner `Subscriber`'s, so T29 holds (P3).

**`Subscribe`:** the disposed and null checks, a new `RowDelivery`, the internal overload (`OnRow`, `topicOptions`,
the delivery as release), then the id. No `SubscribeBatched` call, nothing in `_batchers`.

**Docs (not counted):** SA:201-207; SA:105-110 (per-row null clause only; the rest is rung 7's F16); SA:118
(`RowLimit` covers every per-row call); d10 L7/L9 per-row marks, L11 Tests, Findings (c), §4; reg BC-16 status.

## Corner cases forbidden

- **A row skipped silently** — unrepresentable: `OnRow` has no exit that avoids the handler call.
- **A drop folded into another delivery, or a call with zero or several rows** — unrepresentable: the
  only two `BatchStatus` values built are (RowLimit, 0) with one row and (RowLimit, 1) with null.
- **A `Closing` or empty delivery on a per-row subscription** — unrepresentable: no window, no stop path.
- **A schema-less first row deciding "undecodable for life"** (V4) — unrepresentable: only a schema decides.
- **The codec used after it is closed** — unrepresentable by ordering: released only at the subscription's
  exactly-once free, after which the seam begins no delivery (P2).
- **A handler exception counted on the hidden inner `Subscriber`** — unrepresentable: caught in `OnRow`,
  reported on the `SubscriberArrow`; classification failures are caught before the handler runs.
- **A per-row reason C# invents** — forbidden by L2 (see above).
- **Disposing the `SubscriberArrow` inside a row delivery** — refused at the door (SA:310-316, unchanged; L11).

**Handled residue:**
- *Attachment copy fails (allocation)* → counted as a drop. Why not forbidden: allocation can fail on any
  row; L9 allows only a counted outcome (D-BIND-66's rule, kept).
- *`HandlerFaulted`'s subscription id reads 0 for a fault in a delivery that precedes `Subscribe`'s return*
  (a carrying transport). Why not forbidden: the id is native's, known only when the call returns, and
  `RowHandler` carries none; changing that is `Subscriber`'s public shape. The count is exact; the
  `Batcher` has the same residue (SA:241).

## Premises and stop conditions

- **P1** `fl_decode_rows` on one codec is safe from concurrent threads (the managed batcher already decodes
  one codec from several threads, D-BIND-64). *If a reviewer finds it is not: STOP-AND-ASK (d9 question).*
- **P2** After `SubscriptionState.TryFree` no delivery of that subscription runs (D-BIND-50; the GCHandle is
  freed there). *If a delivery is observed after it: STOP — a `Subscriber` use-after-free, not designed around.*
- **P3** `fl_subscriber_subscribe` ids are numbered per `fl_subscriber` (0a, T29). *If not: STOP-AND-ASK;
  T29 then needs a new anchor, not a deletion.*
- **P4** `inprocess` delivers on the publishing thread before `Publish` returns (the suite relies on it).
  *If not: the forcing test waits with `Sink.WaitFor` and a one-sided "nothing more" window.*
- **P5** The edge shim exports `fl_subscriber_subscribe` and `fl_decode_rows` (used today). *If not: STOP (d9).*

## Forcing-test mapping

**Forcing (new, `SubscriberArrowTests`): `PerRowDeliversEachRowAloneOrCountsItAsOneDrop`**, a theory over
three arms, edge shim (and server too from rung 4): (a) declared topic, a seeded mix of good rows (some
with attachments) and malformed raw rows (`[0x00]`, a truncated encoding); (b) subscribed before the
declaration on default `inprocess` (every row schema-less); (c) a schema the codec refuses (T24's
dictionary-of-struct). Asserts: handler calls = rows offered; each call is either one row with
`RowsDropped = 0`, its value in publish order and exactly its attachments, or null with `RowsDropped = 1`
and no attachments; every reason `RowLimit`; Σ rows + Σ `RowsDropped` = offered; nothing after `Unsubscribe`.
**Green by:** `OnRow`'s one site. **Red today for the right reason:** in (a) a malformed row makes no call of
its own (its drop rides the next good row's call, `RowsDropped = 1` beside a row); in (b) and (c) no call
per row, then one null `Closing` carrying N at `Unsubscribe`.

**Guards (green both ways, declared):** `DisposingInsideAPerRowDeliveryIsRefusedAndNothingIsTornDown` (L11,
d10 Findings (c)); `APerRowDictionaryColumnArrivesAsItsValueType` (D32; distinct subject from the batched
test rung 7 deletes). **Leg (mechanism, not sole proof):** `APerRowHandlerThatUnsubscribesItselfStopsCleanly`
drives the codec release on the departing delivery's exit, not the deferred branch (DEBT-1; 0b) (no fault, nothing after, `Dispose` clean).
**Re-anchored, text unchanged** (they now ride the native path): T1-T5, T8-T11, T29, T32, T41; T7's and
`TopicOptionsOverFastDdsTests.ASubscriberArrowForwardsItsReaderProfileOnBothForms`'s per-row halves (lawful,
0a; batched halves are rung 7's). T8's doc comment cites L2. **Internal observation dropped:** the ladder's
"batcher count" leg would need a production hook rung 8 deletes; arm (a)/(b) already fail on the managed path.

## Risks / Unknowns

- **Review focus:** the overload touches D-BIND-50's lifetime code (one field, a call in `TryFree` and in the
  failed-subscribe branch); on a transport thread it runs inside the thunk's outer catch.
- **Coexistence (ladder-declared):** `Batcher`'s copy duplicates the new one until rung 8; `SubscribeBatched`
  stays managed until rung 7. No bridge routes per-row to it.
- **Visible changes (brief):** a bad row is reported at once, alone; a per-row subscription never delivers a
  final closing call; a schema-less first row does not latch (defensive only: spec §7 cl. 1 makes it unreachable; no test).
- **Inherited gap:** a codec failing on allocation, not rejection, decides "unopenable" (as d7 BC-3's gap).
- **No STOP-AND-ASK.** No tier-1 disagreement: L2, L7, L9, L11, BC-16, D20, D30, D32 and rul:536 (1),
  :575 (3) agree. No d7/d9 edit; no new public member; no lower-layer re-check; nothing skipped.

## Files-to-touch

- `dotnet/src/Fletcher/SubscriberArrow.cs` (`Subscribe`, `RowDelivery`, `CopyAttachments`, docs);
  `dotnet/src/Fletcher/Subscriber.cs` (the internal overload, the release at `TryFree`).
- `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` (forcing test, two guards, one leg, T8 comment);
  d10 and the register (marks, not counted).

**Declared net lines (production):** about **+130–160 / −3** in 2 files (restated at design review, DEBT 3; was +85).
Tests about +160. New public surface: **0**.

## Files-to-delete

- `Subscribe`'s delegation to `SubscribeBatched(MaxRows = 1)` and its remarks (SA:202-207) → `RowDelivery`.
- No test or file: no rung-3 test's subject is forbidden (0a), all twelve re-anchor; the `Batcher` is still
  the batched path until rung 7 and is deleted whole at rung 8.
