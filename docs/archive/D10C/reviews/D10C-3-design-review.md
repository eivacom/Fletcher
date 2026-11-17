# D10C-3 design review: per-row `SubscriberArrow.Subscribe` on the native path (cycle 1 of 2)

**Verdict: APPROVE-WITH-DEBT(3).** No BLOCKER, no STOP-AND-ASK. Design `plans/D10C-3-per-row-edge.md` (149/150), brief (54/55).
**Proof of read:** `plans/RIX-rulings.md`, 603 lines, 45 `## ` entries, last "## 2026-11-15 — D10C ladder: the domain-7
port tests" (:595). Also read: d10 §3 L1-L15, §5 D1-D34, §8; register BC-2, BC-16; ladder rung 3 and ground rules; 0a.

## The six questions: each holds

1. **Codec lifetime: sound.** `TryFree` (SUB:954-974) runs exactly once (`Interlocked.Exchange`), only when no delivery of the
   subscription is past `TryEnter`: `Retire` after a draining cancel, the last delivery's `Exit` (self-cancel; the native gate
   serialises one subscription's deliveries, SUB:910-914), `FreeAfterDrain` after the second drain, or `Dispose`. `OnRow` runs
   inside `TryEnter`..`Exit` and uses the codec only before the handler, so the release always follows the last decode.
   Backstop, verified: `fl_decode_rows` takes `CodecHandle`, a SafeHandle (NM:195-196), so even in D-BIND-50's
   Dispose-vs-deferred corner the native close waits for an in-flight decode; the worst case is a managed
   `ObjectDisposedException`, counted as a drop. Batches never hold the codec (SA:601-602); a failed subscribe ran no delivery.
   One exactly-once point beats a second in-flight count, which would re-answer D-BIND-50. The departure from the ladder's
   "reuses `Subscriber.Subscribe` as it is" (tier 3) is justified and declared.
2. **Per-row reason: not an owner question.** The ladder gives it to the architect (ground rules :39-40); no ruling covers it.
   L2 forbids a fourth member, the STOP list forbids new surface, and `Closing` (L14, D17) and `Timeout` would be false: that
   leaves `RowLimit`. For a drop it cannot mean d7's "max_rows good rows reached" (d7 counts good rows only, d10 L14 note),
   so the SA:118 doc must define it per row ("the one row offered"), as the design's doc line already does.
3. **Visible changes: all licensed.** (a) Reported at once, alone: :575 (3), BC-16 (one row or one drop per call), L9. (b) Nothing
   after a per-row unsubscribe: L14, D17 and :484/:500 govern batched subscriptions only. BC-16's per-call shape forbids an
   empty `Closing`, and L7 (no window) leaves nothing pending. (c) No schema-less latch: :258 (3), :551 (4), D7, D27.
4. **Dropped internal leg: the forcing test is enough from outside.** Ground rules (:27-28) make a leg extra, never the only
   proof. All three arms are red on the managed path for the stated reason. At SA:707-714 a malformed row is put back and
   rides the next good row's `RowLimit` flush. Rows with no schema, or a schema that cannot be opened, wait for `Timeout`
   or `Closing`. Rung 8's scans prove the batcher is gone.
5. **L3 and scope: clean.** No range, format or schema check added; domain 2 (`fl_decode_rows`) refuses a bad row; the codec
   verdict is cached only as ruled (:258 (3), d7 L8). Files-to-touch is `dotnet/src/Fletcher` only (no `c-abi/`, `pubsub-arrow/`).
6. **Size: set by the subject, not the cap.** Some repetition (corner cases ↔ design body, Risks ↔ brief), no padding; the gaps
   below are omissions, not filler. The brief's length goes on four visible changes and two residues.

**7b, priced:** `RowDelivery` (5 fields, constructor, `OnRow` about 30 lines, codec decision about 20, `Dispose`),
`CopyAttachments` about 20, `Subscribe` about 12, the `Subscriber` overload with its field and two release calls about 15.
That is about 130-160 gross lines in this file's Allman style. It fits the 250 ceiling, so no split. See DEBT-3.
**P1 is not load-bearing** for one `RowDelivery` (the native gate serialises its deliveries, SUB:910-914); harmless.
**Tree claims checked:** SA:206-207, :241, :310-316, :335-350, :467-485; SUB:439-443, :812-864, :954-974; T8's assert at
`SubscriberArrowTests.cs:284`; tests have `InternalsVisibleTo` (`Fletcher.csproj:31`); ledger 603/45/:595.

## DEBT (in `plans/reviews/design-debt.md`)

**DEBT-1 (step 3, test): the leg names the wrong branch.** `APerRowHandlerThatUnsubscribesItselfStopsCleanly` never reaches
the deferred free: a self-cancel frees on `TryFree(Delivery)`, every time (`ReentrancyTests.cs:135-143` asserts Deferred = 0).
Restate the leg as that branch, optionally bracketing `FreedByDeliveryCount`. Only a sibling's cancel reaches the deferred
branch (`ReentrancyTests.cs:378-402`); add that arm only if the deferred release should be proven too.

**DEBT-2 (step 3, code): where the release sits in `TryFree`.** After `Self.Free()`, or in a `finally`; otherwise a throwing
release can skip the GCHandle free or, on the Canceller branch, abort `Subscriber.Dispose`'s loop (SUB:720-729) with
subscriptions still live. Today's only caller (`CodecHandle`) does not throw, but the overload accepts any `IDisposable`.

**DEBT-3 (PM, brief): re-book the line count.** The declared +85/−3 is thin against what the design asks for (7b above:
about 130-160). It is inside 250. But earlier rounds took overruns above 50% to the owner (:313, :408, :465). Restate the
figure in the brief's Numbers before the owner reads it, so the close does not read as an overrun.
