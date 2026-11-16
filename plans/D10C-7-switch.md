# D10C-7 — The switch and the deletion (design)

Round D10C, item D10C-7 = ladder rungs **7 and 8**, merged by ruling :660 ("(d) Merge D10C-8 in"). Branch `feature/csharp-switch`, HEAD
`2d43485`. **Proof of read:** `plans/RIX-rulings.md` top to bottom, 658 lines / 51 entries at design; re-read at revision: **667 lines,
52 entries**, last heading "## 2026-11-16 — D10C-7: the switch and the deletion are one stage" (:660). Binding: audit 0a (MUST-DELETE(5);
R1/M3 ruled at :650), design review DEBT-1..3, `design-debt.md` FWD-1, -1b, -1c. Cites: `h` = `binding.h`, `SA` = `SubscriberArrow.cs`, `Sub` = `Subscriber.cs`.

## Summary
`SubscriberArrow.SubscribeBatched`, `Unsubscribe` and `Dispose` stop using the managed batcher and ride the server shim's
`fl_subscriber_subscribe_batches` / `_unsubscribe_batches`. One batch thunk moves each window, imports it, hands the handler an owned
`RecordBatch` with copied attachments, and counts a handler fault. On the edge shim the call throws the shim's `NotSupported` plus one
sentence naming `Eiva.Fletcher.Server`. Options cross as given through one timeout site. **One stage (:660):** the switch (D1-D6), the
transport-conformance lane's server pass, and deleting C#'s own batcher with its proof (D7).

## Design
**D1 Mirror (L2).** `NativeMethods` imports the two entry points as `h:1102-1105`, `h:1128-1129`; `AbiTypes` gains a blittable `FlBatch`
with all seven fields, `failure` included (rung 11 adds a branch, not a signature); `fl_batch_fn` is `delegate* unmanaged[Cdecl]<nint,
FlBatch*, void>`. `Subscriber` (owner of the handle and the topic/options marshalling) gains two **internal** members:
`SubscribeBatches(topic, long maxRows, long timeoutMs, TopicOptions?, nint onBatch, nint ctx, ref bool accepted)` → `SubscribeResult`, and
`UnsubscribeBatches(Subscription)` (a no-op once disposed, as `Sub:569-574`); `Sub:425-447`'s options block is shared, not copied.

**D2 `SubscribeBatched`: one wrapper, options as given.** Order: disposed/null checks → a `BatchDelivery` (handler, owner, `Subscription`)
and its `GCHandle`, built before the claim (D10C-6 DEBT-1) → `SubscribeClaimed` (unchanged) around `SubscribeBatches(options.MaxRows, ToMs(options.Timeout))`.
`MaxRows` is `int64` end to end; `SA:254-267` (the BC-8 refusal and the clamp) is deleted, so domain 7 answers (L3, D22, D31).
- **Keyed on "native refused", never on a catch-all (review DEBT-2).** `SubscribeBatches` takes `ref bool accepted` and sets it the
  instant the native call answers OK, before any allocation. ONE frame (`SubscribeBatched`) frees the `GCHandle`, and
  `SubscribeClaimed` releases the claim, each in `catch when (!accepted)`: a claim refusal or a non-OK status (which never delivers:
  `h:1081`, :500). Anything thrown after OK (out of memory building `Subscription`/`SchemaArrival`, or recording `_claimed`) frees
  nothing and keeps the claim: the live subscription ends at `Dispose`, whose Closing frees. After OK only Closing frees (D3 step 6)
  and only `Unsubscribe` releases the claim. The subscribing thread then sets `delivery.Subscription` (volatile) and `Canceller`.
- **Edge sentence (L3, L6, D29, BC-10).** In `SubscribeBatches` only: on `NotSupported` with `fl_binding_variant() == Edge`,
  `Errors.ThrowIfFailed`'s new optional argument appends one sentence to the shim's verbatim message ("Batched delivery needs the server
  build of the shim: reference the Eiva.Fletcher.Server package."); number and origin unchanged. The one variant call in `dotnet/src`;
  it refuses nothing (the shim refused), so BC-10 holds and its scan must allow it. A provider's own `NotSupported` gets no sentence (T7).
- **Check order (FWD-1b):** claim first, then native. Row-first then batched is the same-topic refusal on both legs; batched-first on
  edge throws `NotSupported` and leaves no claim. Asking the variant first would be a refusal keyed on the variant (L6 forbids).

**D3 The batch thunk — one function: move, import, call, catch-count (L8, L9, L10, L13; BC-13 C# half).**
`[UnmanagedCallersOnly(Cdecl)] static void BatchThunk(nint ctx, FlBatch* b)` in `SubscriberArrow`, in `ThunkDisciplineTests`' scope:
1. `delivery` from the `GCHandle`. No delivery frame is pushed (BC-6), so rung 9's dispose-from-handler stays served (L11).
2. If `b->array != null`: **move** array and schema into C#-allocated structs (copy each struct, set the source's `release` to NULL;
   `h:1020-1023`), then `ImportSchema` and `ImportRecordBatch` against it. A dictionary arrives as `DictionaryArray`; C# converts nothing
   (D24, D32). Attachments: `CopyAttachments(new AttachmentsView(b->attachments[i]))` for each row — **the D10C-3 function, now its second
   caller** (`SA:471`; `Batcher`'s private copy goes with it, D7).
3. A failure in step 2: what was built is disposed, moved structs released once, and the handler gets a null batch with `RowsDropped =
   rows_dropped + num_rows` (residue a). `array == null` (d7 L7 / d9 L18) passes through: null batch, `rows_dropped` as given.
4. `BatchStatus((BatchReason)b->reason, dropped)`, cast 1:1 (L2, D34). `failure` is not read here (rung 11).
5. Handler in `try`; a throw → `ReportAbsorbed` (counted, raised; V13). The next batch still arrives (the thunk returns normally).
6. `finally`, when `reason == Closing`: mark the `Subscription` retired **if it is set** (a racing `Dispose` can deliver Closing before
   the subscribing thread sets it; DEBT-2) and free the `GCHandle`. **The only free after an accepted subscribe**: Closing is the last
   use of `ctx`, exactly once per accepted subscription, on every close path (`h:874-884`, :484).
7. Outer catch-all: nothing leaves the frame (L10). No lock, queue or thread anywhere on this path (L13).

**D4 `Unsubscribe` and `Dispose`.** `Unsubscribe`: ownership check unchanged; `quiet = _subscriber.InDeliveryOnThisProvider`, asked
before the close; route by the ABI's id rule (bit 63 = batched, `h:1071`) to `UnsubscribeBatches` or `Unsubscribe`; then release the
claim (D5). Outside a batch handler `UnsubscribeBatches` returns after Closing (`h:1109-1113`); from its own handler it returns at once
and Closing follows the handler (d7 L9). No lock is held across a native close. `Dispose`: the row-delivery refusal stays (L11); the
`_batchers` loop goes; the destroy ends every batched subscription with its Closing (`h:874-881`); an `Unsubscribe` meanwhile is a no-op.

**D5 The claim outlives a quiet close (FWD-1, FWD-1c).** A close made inside a row delivery on the provider leaves the transport
subscription "open and quiet" until destroy or a same-topic re-subscribe (`pubsub/.../subscriber.hpp:181-191`, `subscriber.cpp:720-745`).
So when `quiet`, `Release` keeps the entry with its kind and count 0; `Claim` reuses it for the same kind (the transport subscription
is that kind's) and refuses the other with the existing exception, its text naming the reason; "holds a per-row/batched" stays (D10C-6
DEBT-3). A batch callback is no row delivery: d7's `Unsubscribe` there tears the transport down (`subscriber_arrow.cpp:529-534`). The
question is the seam's own (`InsideDeliveryOn(provider)`), asked at C#'s existing site — no new check.

**D6 BC-14: one translation site.** The arithmetic of `SchemaArrival.ToMilliseconds` (`:216-232`) becomes one internal function:
`InfiniteTimeSpan` → `long.MaxValue`; `t ≥ 0` → whole ms, truncated, saturating; `t < 0` → `floor` ms, so −1 tick crosses as −1, never 0
(L12). The arrival keeps its managed refusal of negatives in front of it (D-BIND-20, behaviour unchanged); the batched path forwards.

**D7 Rung 8, merged: C#'s batcher is deleted in the same commit set (L4; D2, D3 end, D19; BC-9 EXTEND-AND-NARROW; census D12).**
Deleted: `Batcher` (`SA:592-1066`, with its own `CopyAttachments`), `DefaultWindowByteCeiling`, the internal ceiling constructor and
`_windowByteCeiling` (the public constructor stands alone), the now-unused `using`s, `FletcherCodec.DecodeBatchFramed` (`:283-331`) and
the `fl_decode_rows_framed` import in `NativeMethods` (d9 keeps the entry point: d10 Q8 default). No dead-code window and no bridge:
the switch and the deletion land together. The `SA:4-94` header is rewritten for the new shape (counted both ways). **Proof (rung 8's
forcing tests):** (i) a metadata scan (`System.Reflection.Metadata`) of `Eiva.Fletcher.dll` and `Eiva.Fletcher.Interop.dll`: no type
in `SubscriberArrow` references `System.Threading.Timer` or `Stopwatch`, and no `fl_decode_rows_framed` import; run on the build output
in the dotnet lane and on the assemblies unzipped from the packed `.nupkg`s in the pack job (one step); (ii) L4's source scan of
`SubscriberArrow.cs` (BC-9's structural test): no `Timer`, `Stopwatch` deadline arithmetic or row buffer. Both red today.

## Corner cases forbidden
- **A refused subscribe that delivers or leaks its context; a live one freed early** — unrepresentable: freed only when `!accepted` (D2).
- **A double free, or a free before the last delivery** — unrepresentable: only Closing frees; the subscribing thread never frees after OK.
- **A kept batch pointing into borrowed memory** — unrepresentable: moved before import; nothing borrowed outlives the call (L8).
- **Concurrent or out-of-order handler calls** — unrepresentable in C#: no thread, queue or lock of its own (L13, V1).
- **The other kind silently replacing a quiet subscription** — refused at the door (D5).
- **A negative timeout crossing as 0** — unrepresentable (`floor`, D6). **`MaxRows` narrowed** — unrepresentable (`int64` throughout).
- **The edge sentence on a provider's refusal** — unrepresentable: appended only when the build answers `Edge` (D2).
- **A row id closed as batched, or the reverse** — unrepresentable (bit-63 routing). **Dispose from a row delivery** — refused (L11).

**Handled residue:** (a) **a failure in C#'s own hand-over** (out of memory, an import refusal, a window longer than one managed batch):
null batch, the window's rows added to `RowsDropped`, reason kept. *Why not forbidden:* allocation can fail and L9 forbids silence (d9
L18 does the same one layer down); a failure in `BatchStatus` for it is rung 11's design. (b) **`HandlerFaulted.SubscriptionId` is 0 for a
batch delivered before `SubscribeBatched` returns.** *Why not forbidden:* the shim may deliver before `out_id` is written (`h:1077`).

## Premises and stop conditions
- **P1 (D24).** Apache.Arrow 23 imports a dictionary through C Data. Schema half evidenced in-repo (`DecodedSchema.cs:74,100`); array
  half **not executed at design**. **First step-3 action:** a leg in `ArrowCDataInterfaceTests` round-trips a batch with a `DictionaryArray`
  child (int32 → utf8, a null) through `ImportSchema` + `ImportRecordBatch`, the thunk's own path. **Red: STOP-AND-ASK the owner; never a C# conversion (L3).**
- **P2.** The importer moves out of a `CArrowArray` (`FletcherCodec.cs:266-279`) and `Free` releases only an unreleased struct. A leg calls
  the thunk with a C#-exported batch: source `release` NULL after, kept batch intact. A double release: STOP; no hand-rolled release.
- **P3.** Exactly one Closing per accepted subscription on every close path (d9 `[built]`). A C# test seeing 0 or 2 is a d9 defect: STOP.
- **P4.** Only a row-delivery-thread close leaves the transport quiet; if a batch-thread close does too, D5 widens to any handler; report.
- **P5.** Rung tests pick their leg from outside: the server file beside the test assembly (L5's own fact), never `NativeMethods`.

## Forcing-test mapping (SAT = `SubscriberArrowTests`)
| Test | Green by | Red before, for the right reason |
|---|---|---|
| Edge truth table, every batched subject (helper; ground rules) | D2 sentence | `SubscribeBatched` succeeds on edge (managed batcher) |
| NEW `EveryBatchArrivesOnOneThreadInWindowOrder` (d7 replay, server) | D3 (no C# thread) | Timeout flush on a timer thread, RowLimit on the delivery thread |
| NEW `AnIdleCloseDeliversOneEmptyClosing` + T25 rewrite (one empty Closing, then nothing) | D3/D4, P3 | today nothing at an idle close (V3) |
| T21, T36 rewritten (`DictionaryArray`, nulls and values kept) | D3, P1 | managed decode gives the value type |
| T7: server arm the provider's text, no sentence; edge arm truth table | D2's variant condition | both arms accept any `NotSupported` |
| NEW twins of T32 (empty attachment key) and T41 (throw counted, raised, next batch arrives) | D3 steps 2, 5 | no batched twin exists |
| NEW guard `AQuietCloseKeepsTheTopicForItsKind` (both kinds, server; edge: truth table) | D5 | other kind accepted, later re-subscribe gets nothing |
| NEW guard `AKeptBatchReadsIntactAfterClosing` (green both ways; catches a missing move) | D3 step 2 | declared guard |
| 21 re-anchors (T7, T12-T19, T22-T28, T33, T35, T37), T31 edge leg (M1), D10C-6's three (M4, FWD-1/-1b) | D2-D5 | — |
| Fast DDS reader profile, batched arm on the server shim; edge arm the truth table (M2) | D2 + the lane's server pass | lane runs the edge shim only |
| NEW (rung 8) `TheManagedBatcherIsGoneFromTheAssemblies` (metadata scan, build output and packed) | D7 | `Batcher` references `Timer`, `Stopwatch`; framed import |
| NEW (rung 8) L4 source scan of `SubscriberArrow.cs` (BC-9) | D7 | `Batcher`'s timer, clock and row buffer |
Wait for delivery, not straight after `Publish` (:140, L13): T12, T14, T17, T22, T23, T27. T26's handler ignores the Closing. Control: a
10 ms timeout gives `Timeout` within 2 s; each scan has a control (its pattern fed a line or type that holds it, found). **Deleted, never
re-anchored (subject forbidden):** T20, T30, T34, T38, T39, and T40 (pins the deleted ceiling; L4). T46: declared red to D10C-11 (:650 (2)).
The whole suite stays green on both legs after the deletion (rung 8's subject).

## Risks / Unknowns
- **Size (ruled :660; review DEBT-3 pricing agreed).** Switch +280-310 / −65-75 (net +215-245); lane +40-60; deletion +0-10 / −480-520
  (ladder rung 8). **Stage: +320-380 / −545-595, net −165 to −275**: inside the ceiling with margin. The lane = build `pubsub-arrow` and
  the server variant, stage `fletcher-c-abi-server` beside the conformance tests, a second `dotnet test` (both OSes). Large review by
  line count, mostly deletion in one file; the two scans prove the deletion mechanically.
- **D24 is unverified by execution at design** (P1). It is the stage's first gate.
- **New to the reviewed design:** D7 (rung 8's scope), DEBT-2's `accepted` flag (D2, D3 step 6), the M2 lane in-stage. D1-D6 otherwise unchanged.
- **D5 lengthens a ruled refusal** (L3 :575 (6)): after a quiet close the other kind is refused until `Dispose`. Same subject, longer
  duration; the alternative is the silent loss the ruling exists to prevent. Disclosed in the brief. BC-10's scan must allow D2's call.

## Files-to-touch
Production (8-10): `dotnet/src/Fletcher.Interop/NativeMethods.cs`, `AbiTypes.cs`; `dotnet/src/Fletcher/Subscriber.cs`, `SubscriberArrow.cs`,
`Errors.cs`, `SchemaArrival.cs`, `FletcherCodec.cs`; `.github/workflows/ci.integration-test.pubsub-transport-conformance.yml`,
`ci.dotnet.yml` (the packed-assembly scan step); the conformance csproj only if staging needs a property. Tests:
`dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`, `ArrowCDataInterfaceTests.cs`, a new scan test file (both rung-8 scans);
`integration-tests/pubsub-transport-conformance/dotnet/PubSubTransportConformance/TopicOptionsOverFastDdsTests.cs`. Docs (listed, not
counted): `SA` `///` docs (F16; `MaxRows` "below 1 mean 1" goes); d10 L3 Tests line and V12 per :650 (1); d10 marks L3 (D29), L4, L6,
L8, L9, L10, L12, L13, L14 → `[built]` (Windows by hand / devcontainer; CI unrun); register BC-9's C# half narrowed.

## Files-to-delete
- Tests T20, T30, T34, T38, T39, T40 — no replacement; behaviour gone by law (audit MUST-DELETE; T40 by L4, audit :23).
- `SA:254-267` (timeout refusal, `MaxRows` clamp) — replaced by domain 7's refusal passed through (BC-8).
- `SA:175` `_batchers` and its uses at `SA:275-278, 379-385, 438-448` — replaced by the native close and destroy.
- `Batcher` (`SA:592-1066`), `DefaultWindowByteCeiling`, the ceiling constructor and field, `DecodeBatchFramed`, the
  `fl_decode_rows_framed` import — replaced by domain 7 through the server shim (D7). Kept: `BatchOptions.MaxTimeout` (rung 10's).
