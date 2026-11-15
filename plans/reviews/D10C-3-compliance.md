# D10C-3 compliance review: per-row `SubscriberArrow.Subscribe` on the native path (cycle 1)

**Verdict: ISSUES (2 blocking, 1 non-blocking).** Diff `5b311af`..working tree (`dotnet/`, `docs/`); no `c-abi/` or
`pubsub-arrow/` edit; new public surface 0 (`RowDelivery`, `CopyAttachments` private; the `Subscriber` overload internal).
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **603 lines, 45 `## ` entries, last "## 2026-11-15 — D10C ladder:
the domain-7 port tests" (:595)**. Also: design, brief, design review, debt D10C-3, 0a, 0b, d10 §3/§5, register BC-2/BC-16. Ran: `SubscriberArrowTests|ReentrancyTests|ThunkDiscipline`, edge shim, 62/62 on net8.0 and net10.0.

## Blocking
**F1. `DisposeRelease` swallows uncounted (`Subscriber.cs:997-1007`): a new silent absorption site.** Not in the design.
DEBT-2 asked for an ORDER (after `Self.Free()` or in a `finally`), not a catch. It breaks d10 L9's principle ("Nothing is
lost or absorbed silently") and D-BIND-19: (5) "never silent"; (2) one containment site per direction, a second →
STOP-AND-ASK (d10 D18; L10). On `Exit` it duplicates L10's thunk catch; on every other path it hides a failure from a
managed caller. Dead code today: the only release is a lock + SafeHandle over `void fl_codec_close`. d10 L3 not engaged.
*Fix:* delete the try/catch, keep the call after `Self.Free()`, and state on the internal overload that `release.Dispose`
must not throw (as `fl_batch_fn` must not). If containment is wanted anyway, that is D-BIND-19 (2)'s STOP-AND-ASK, to the owner.

**F2. Survived: `SA:43-44`, "Per-row delivery is the same thing with MaxRows = 1 (D-BIND-59)…".** It is the delegation's
own remark (Files-to-delete: "… and its remarks"). It states the premise :536 (1) amended (D20), it is false from this
rung, and it is not in d10 Findings (a)3's rung-7 list. *Fix:* replace with "Per-row delivery (`Subscribe`) does not use the batcher: see `RowDelivery`."

## Non-blocking (fix in the same cycle)
**F3. One exit from `OnRow` skips the handler (`SA:445`).** `delivered = [attachments!]` allocates outside the guarded
`try`. On OOM after a good decode: no call, no `RowsDropped`, a leaked batch, and a throw counted on the hidden inner
`Subscriber` (SUB:857). The design calls both unrepresentable (:69, :76-77; residue :82-83 makes this a counted drop).
*Fix:* build `delivered` inside the first `try`, after `Decode`; the catch disposes a non-null `batch`.

## Pressure-tested, conforms
- Design shape: one `OnRow`; null ⇔ (RowLimit, 1) with `[]`, else (RowLimit, 0) with `[copy]`. A handler throw goes to
  `SubscriberArrow.ReportAbsorbed` (L9). The codec is decided once under the gate by the first non-null schema; the gate
  is never held across decode or handler. Release at `TryFree` and the failed subscribe. No `SubscribeBatched`, nothing in `_batchers`.
- Rulings: :575 (3)/D30; :575 (5)/D32 (nothing converted); :536 (1)/D20 (edge; server rung 4); BC-16 one site;
  :258 (3)/:551 (4) (a null schema decides nothing; untested by recorded PM decision).
- Implementer (3), `_decided` on Dispose: lawful. Unreachable under P2, it prevents an unreleased codec; no recovery path.
  (4), two `CopyAttachments`: predicted (design :45-48, :127-128); `Batcher`'s copy is unedited (D3).
- DEBT-1 resolved (leg: delivery +1, deferred 0). DEBT-2: order met, containment is F1. DEBT-3: restated. Tests: 3-arm
  forcing theory, 2 guards, 1 leg, T8 comment; nothing deleted; the twelve re-anchored tests are unchanged.

## RECORD
- RECORD: "177 net" counts 40 doc-comment lines; by the ladder's rule production is 142 net, inside 130–160.
- RECORD: d10 L9's mark `[built]` vs L7/BC-16's `[built on the edge shim]`; §4 struck whole; §2 row "today `MaxRows = 1`";
  citations drifted (L9, L11, `Subscriber.cs:812-865`). No evidence row for the FastDDS per-row half (design :118-119).
  The brief's "the laws settle" RowLimit vs 0b's UNMOORED.

## Cycle 2
**Verdict: PASS.** Working tree against `5b311af`. Ledger unchanged: 603 lines, 45 entries, last :595. Same filter, 63/63 on net8.0 and net10.0.
- F1 closed. `DisposeRelease` and its catch are gone (grep finds no hit). `Release?.Dispose()` runs after `Self.Free()` in `TryFree` (`SUB:991`) and directly on the failed subscribe (`:453`). The overload states "MUST NOT THROW" (`:380`). No new silent or second containment site.
- F2 closed. `SA:43` now points to `RowDelivery`. No per-row `MaxRows = 1` claim is left in `dotnet/src`; d10 keeps it only as the amended premise (L7, D20).
- F3 closed. `delivered` is built inside the first `try`, after `Decode` (`SA:427`), and the catch disposes a decoded batch (`:433`). Outside the `try` remain only `AttachmentsBuilder.Dispose` (cannot throw) and a struct.
- New leg `ReentrancyTests.TheReleaseRunsExactlyOnceWhereverTheSubscriptionIsFreed` (4b S1). It covers the drained, self-cancel (none during the handler), `Dispose` and refused paths, each exactly once. A lawful leg on the internal overload, not the rung's proof.
- Cycle-1 RECORD fixed in d10: L9 mark, §4 server half left open, §2 row; the citations checked (`SUB:824-877`, `SA:352-367`, `:442-449`, `:327-333`). Brief basis fixed (`779dbda`).
- RECORD: `ReentrancyTests.cs` is outside the design's Files-to-touch (a 4b-requested leg). Production is now +129/−4 = 125 net (doc comments excluded), under the declared 130–160. The FastDDS per-row evidence is still owed at close.
- Note (non-blocking): the overload's "a throw would surface on whichever path freed" is not so on the departing delivery's `Exit`, where L10's thunk catch absorbs it. The contract conforms; only that wording is loose.
