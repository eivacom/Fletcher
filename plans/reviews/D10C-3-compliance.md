# D10C-3 compliance review: per-row `SubscriberArrow.Subscribe` on the native path (cycle 1)

**Verdict: ISSUES (2 blocking, 1 non-blocking).** Diff `5b311af`..working tree (`dotnet/`, `docs/`); no `c-abi/` or
`pubsub-arrow/` edit; new public surface 0 (`RowDelivery`, `CopyAttachments` private; the `Subscriber` overload internal).
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **603 lines, 45 `## ` entries, last "## 2026-11-15 — D10C ladder:
the domain-7 port tests" (:595)**. Also: design, brief, design review, debt D10C-3, 0a, 0b, d10 §3/§5, register BC-2/BC-16. Ran: `SubscriberArrowTests|ReentrancyTests|ThunkDiscipline`, edge shim, 62/62 on net8.0 and net10.0.

## Blocking

**F1. `DisposeRelease` swallows uncounted (`Subscriber.cs:997-1007`): a new silent absorption site.** Not in the design;
DEBT-2 asked for an ORDER ("after `Self.Free()`, or in a `finally`"), not a catch. Against: d10 L9's principle, "Nothing is
lost or absorbed silently" (the principle governs, not only its handler-failure clause); D-BIND-19 (5) "never silent" and
(2) "exactly one containment site" per direction, "a second containment site → STOP-AND-ASK" (d10 D18 keeps D-BIND-19; L10).
On the `Exit` path it duplicates L10's thunk catch (`:867`). On `Unsubscribe`, `Dispose` and the failed subscribe it hides
a failure from a managed caller. In-file precedent counts what it swallows (`SA:362-366`). Dead code today: the only
release is `RowDelivery.Dispose` = a lock + `FletcherCodec.Dispose` → SafeHandle → `void fl_codec_close`. d10 L3 is not
engaged today (no native answer to forward), but a catch-all would eat one if a release ever had it.
*Fix:* delete the try/catch. Keep the call after `Self.Free()` (that half of DEBT-2 is met). State on the internal
overload: "`release.Dispose` must not throw", as `fl_batch_fn` must not. A throwing release then shows up, never silently.
If the architect wants containment anyway, that is D-BIND-19 (2)'s STOP-AND-ASK and goes to the owner, not kept.

**F2. Survived: `SA:43-44`, "Per-row delivery is the same thing with MaxRows = 1 (D-BIND-59): a row arrives as a one-row
batch, flushed on the delivery thread as it lands."** It is the delegation's own remark (Files-to-delete: "`Subscribe`'s
delegation … and its remarks"). It states the premise :536 (1) amended (D20), and it became false at this rung, not at
the switch: d10 Findings (a)3 does not list it under rung 7's `SA:4-94`. Tier 3 against tier 1 means it changes.
*Fix:* replace both lines with "Per-row delivery (`Subscribe`) does not use the batcher: see `RowDelivery`." Not counted.

## Non-blocking (fix in the same cycle)

**F3. One exit from `OnRow` still skips the handler (`SA:445`).** `delivered = [attachments!]` allocates outside the
guarded `try`. On OOM after a good decode, the row gets no call and no `RowsDropped`, its batch leaks, and the throw is
counted on the hidden inner `Subscriber` (SUB:857). The design calls both outcomes unrepresentable (:69, :76-77); its
residue (:82-83) makes an allocation failure a counted drop. *Fix:* build `delivered` inside the first `try`, after
`Decode`; the catch then disposes a non-null `batch` before nulling it.

## Pressure-tested, conforms

- Design shape: one `OnRow`; `batch is null` ⇔ (RowLimit, 1) with `[]`, else (RowLimit, 0) with `[copy]`; a handler
  throw goes to `SubscriberArrow.ReportAbsorbed` (L9); the codec is decided once under the gate by the first non-null
  schema, and the gate is never held across decode or handler; release at `TryFree` and the failed subscribe; no
  `SubscribeBatched` call, nothing in `_batchers`; `Canceller` kept, as `SubscribeBatched` sets it.
- :575 (3) / D30 a null batch, `RowsDropped = 1`, alone. :575 (5) / D32 domain 2's value type, nothing converted.
  :536 (1) / D20 edge proven, server leg rung 4. BC-16: one classify-and-count site; the `MaxRows = 1` route is gone
  from code. :258 (3) / :551 (4): a null schema decides nothing (untested by recorded PM decision).
- Implementer (3), `RowDelivery.Dispose` sets `_decided`: lawful. Unreachable under P2 (the GCHandle is freed first). It
  stops an undecided subscription opening a codec never released, and matches the design review's accepted "late
  decode → counted drop". No recovery path for a forbidden state.
- Implementer (4), two `CopyAttachments`: predicted (design :45-48, :127-128; ladder rung 3), not a drift; `Batcher`'s copy
  unedited (D3). DEBT-1 resolved (leg: delivery +1, deferred 0; design corrected `7104ae5`). DEBT-2: order met,
  containment is F1. DEBT-3: restated. Tests: 3-arm forcing theory with 0b's fixture checks, 2 guards, 1 leg, T8 comment
  only. Nothing deleted; the twelve re-anchored tests are unchanged.

## RECORD
- RECORD: "177 net" counts 40 doc-comment lines; by the ladder's rule production is +144/−2 = **142 net**, inside 130–160.
- RECORD: d10 L9's per-row mark reads `[built]`; L7 and BC-16 read `[built on the edge shim]` (server leg rung 4). Match L9.
- RECORD: d10 §4 strikes "…on both packages" whole while the server half is open; strike only the edge half.
- RECORD: d10 §2 "Per-row Arrow subscribe" row still says "today `SubscribeBatched` with `MaxRows = 1` (`SA:206-207`)".
- RECORD: d10 citations drifted by this edit: L9 `SA:335-350`→`:353-367`, `SA:729-736`; L11 `SA:310-316`; `Subscriber.cs:812-865`.
- RECORD: no D10C-3 evidence row yet for `TopicOptionsOverFastDdsTests…OnBothForms`'s per-row half (design :118-119).
- RECORD: the brief says "the laws settle" RowLimit; 0b classed it UNMOORED (a PM decision under L2, not a ruling).
