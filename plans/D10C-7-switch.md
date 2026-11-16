# D10C-7 — The switch (design)

Round D10C, item D10C-7 = ladder rung 7 (`plans/D10C-test-ladder.md`, row "| **7** |"). Branch `feature/csharp-switch`, HEAD
`2d43485`. **Proof of read:** `plans/RIX-rulings.md` top to bottom, **658 lines, 51 entries**, last heading "## 2026-11-16 — D10C-7
pre-design: T30 in d10; the preview-consumer red window" (:650). Binding: audit `plans/reviews/D10C-7-audit-0a.md` (MUST-DELETE(5),
R1/M3 ruled at :650), `design-debt.md` FWD-1, -1b, -1c. Cites: `h` = `binding.h`, `SA` = `SubscriberArrow.cs`, `Sub` = `Subscriber.cs`.

## Summary
`SubscriberArrow.SubscribeBatched`, `Unsubscribe` and `Dispose` stop using the managed batcher and ride the server shim's
`fl_subscriber_subscribe_batches` / `_unsubscribe_batches`. One batch thunk moves each window, imports it, hands the handler an owned
`RecordBatch` with copied attachments, and counts a handler fault. On the edge shim the call throws the shim's `NotSupported` plus one
sentence naming `Eiva.Fletcher.Server`. Options cross as given through one timeout site. **Lands as 7a (all C#) + 7b (one CI lane).**

## Design
**D1 Mirror (L2).** `NativeMethods` imports the two entry points exactly as `h:1102-1105`, `h:1128-1129`; `AbiTypes` gains a blittable
`FlBatch` with all seven fields, `failure` included, so rung 11 adds a branch, not a signature. `fl_batch_fn` is
`delegate* unmanaged[Cdecl]<nint, FlBatch*, void>`. `Subscriber` (owner of the handle and of topic/options marshalling) gains two
**internal** members: `SubscribeBatches(topic, long maxRows, long timeoutMs, TopicOptions?, nint onBatch, nint ctx)` returning a
`SubscribeResult` (batched id, arrival), and `UnsubscribeBatches(Subscription)` (a no-op once disposed, as `Sub:569-574`). The options
block of `Sub:425-447` is shared by both subscribes, not copied. No public surface is added.

**D2 `SubscribeBatched`: one wrapper, options as given.** Order: disposed/null checks → a `BatchDelivery` (handler, owner, `Subscription`)
and its `GCHandle`, built before the claim (D10C-6 DEBT-1) → `SubscribeClaimed` (unchanged) around `SubscribeBatches(options.MaxRows, ToMs(options.Timeout))`.
`MaxRows` is `int64` end to end; `SA:254-267` (the BC-8 refusal and the clamp) is deleted, so domain 7 answers (L3, D22, D31). On any
refusal (the claim, or a non-OK status) the `GCHandle` is freed at once (a refused call never delivers: `h:1081`, :500) and `Errors.ThrowIfFailed` throws. On OK the
subscribing thread never touches the handle again; it sets `delivery.Subscription` (volatile) and `Canceller = Unsubscribe`.
- **Edge sentence (L3, L6, D29, BC-10).** In `SubscribeBatches`, and only there: when the status is `NotSupported` and
  `fl_binding_variant()` answers `Edge`, `Errors.ThrowIfFailed` gains one optional argument that appends one sentence after the shim's
  verbatim message ("Batched delivery needs the server build of the shim: reference the Eiva.Fletcher.Server package."); number and
  origin unchanged. This is the one `fl_binding_variant` call in `dotnet/src`. It names a package; it refuses nothing (the shim refused),
  so BC-10's "no managed refusal keyed on the variant" holds. A provider's own `NotSupported` on the server build gets no sentence (T7).
- **Check order (FWD-1b), decided:** claim first, then native. Row-first then batched is the same-topic refusal on both legs (a true
  answer); batched-first on edge throws `NotSupported`, `SubscribeClaimed`'s catch releases the claim, and a per-row subscribe is accepted.
  Asking the variant first would be a managed refusal keyed on the variant (L6 forbids).

**D3 The batch thunk — one function: move, import, call, catch-count (L8, L9, L10, L13; BC-13 C# half).**
`[UnmanagedCallersOnly(Cdecl)] static void BatchThunk(nint ctx, FlBatch* b)` in `SubscriberArrow`, in `ThunkDisciplineTests`' scope:
1. `delivery` from the `GCHandle`. No delivery frame is pushed (BC-6), so rung 9's dispose-from-handler stays served (L11).
2. If `b->array != null`: **move** array and schema into C#-allocated structs (copy each struct, set the source's `release` to NULL;
   `h:1020-1023`), then `ImportSchema` and `ImportRecordBatch` against it. A dictionary arrives as `DictionaryArray`; C# converts nothing
   (D24, D32). Attachments: `CopyAttachments(new AttachmentsView(b->attachments[i]))` for each row — **the D10C-3 function, now its second
   caller** (`SA:471`; `Batcher`'s private copy is untouched, D3).
3. Any failure in step 2 (allocation, import): what was built is disposed, the moved structs released once, and the handler gets a null
   batch with `RowsDropped = rows_dropped + num_rows` (see residue). `array == null` (d7 L7 / d9 L18) passes through: null batch,
   `rows_dropped` as given.
4. `BatchStatus((BatchReason)b->reason, dropped)`, cast 1:1 (L2, D34). `failure` is not read here (rung 11).
5. Handler in `try`; a throw → `ReportAbsorbed` (counted, raised; V13). The next batch still arrives (the thunk returns normally).
6. `finally`, when `reason == Closing`: mark the `Subscription` retired and free the `GCHandle`. **The only free after an accepted
   subscribe**: Closing is the last use of `ctx`, exactly once per accepted subscription, on every close path (`h:874-884`, :484).
7. Outer catch-all: nothing leaves the frame (L10). No lock, queue or thread anywhere on this path (L13).

**D4 `Unsubscribe` and `Dispose`.** `Unsubscribe`: ownership check unchanged; `quiet = _subscriber.InDeliveryOnThisProvider`, asked
before the close; route by the ABI's id rule (bit 63 = batched, `h:1071`) to `UnsubscribeBatches` or `Unsubscribe`; then release the
claim (D5). Outside a batch handler `UnsubscribeBatches` returns after Closing is delivered (`h:1109-1113`); from its own handler it
returns at once and Closing follows the handler (d7 L9). No `SubscriberArrow` lock is held across a native close. `Dispose`: the
row-delivery refusal stays (L11); the `_batchers` loop goes; `_subscriber.Dispose()` destroys, which ends every batched subscription
with its Closing before returning (`h:874-881`). A handler's `Unsubscribe` during that destroy is a no-op (disposed).

**D5 The claim outlives a quiet close (FWD-1, FWD-1c).** A close made inside a row delivery on the provider leaves the transport
subscription "open and quiet" until destroy or a same-topic re-subscribe (`pubsub/.../subscriber.hpp:181-191`, `subscriber.cpp:720-745`).
So when `quiet`, `Release` keeps the topic's entry with its kind and count 0. `Claim` then reuses it for the same kind (correct: the
transport subscription is still that kind's) and refuses the other kind with the existing exception, whose text gains the reason
(closed inside a delivery; held until this `SubscriberArrow` is disposed); "holds a per-row" / "holds a batched" stays (D10C-6 DEBT-3).
A batch callback is not a row delivery: d7's `Unsubscribe` there tears the transport down (`subscriber_arrow.cpp:529-534`), so it
releases normally. The question is the seam's own (`InsideDeliveryOn(provider)`), asked at C#'s existing site — no new check.

**D6 BC-14: one translation site.** The arithmetic of `SchemaArrival.ToMilliseconds` (`:216-232`) becomes one internal function:
`InfiniteTimeSpan` → `long.MaxValue`; `t ≥ 0` → whole ms, truncated, saturating; `t < 0` → `floor` ms, so −1 tick crosses as −1, never 0
(L12). The arrival keeps its managed refusal of negatives in front of it (D-BIND-20, behaviour unchanged); the batched path forwards.

**D7 What of the managed batcher remains, and why it is not a bridge.** After 7a the source still holds `Batcher` (`SA:592-1066`, its own
`CopyAttachments`), `DefaultWindowByteCeiling`, the internal ceiling constructor and `_windowByteCeiling`, and
`FletcherCodec.DecodeBatchFramed` with its import. **None is reachable:** nothing constructs `Batcher`; the ceiling constructor only
stores a value nobody reads (T40 alone calls it, and dies with it at rung 8); `DecodeBatchFramed`'s one caller is `Batcher`. A bridge routes
work; no option, build, fallback or error path leads into the managed path. Dead for one stage; rung 8 (D10C-8) deletes all of it.

## Corner cases forbidden
- **A refused subscribe that delivers or leaks its context** — unrepresentable: freed on non-OK, which the shim promises never delivers.
- **A double free, or a free before the last delivery** — unrepresentable: only Closing frees; the subscribing thread never frees after OK.
- **A kept batch pointing into borrowed memory** — unrepresentable: moved before import; nothing borrowed outlives the call (L8).
- **Concurrent or out-of-order handler calls** — unrepresentable in C#: no thread, queue or lock of its own (L13, V1).
- **The other kind silently replacing a quiet subscription** — refused at the door (D5).
- **A negative timeout crossing as 0** — unrepresentable (`floor`, D6). **`MaxRows` narrowed** — unrepresentable (`int64` throughout).
- **The edge sentence on a provider's refusal** — unrepresentable: appended only when the build answers `Edge` (D2).
- **A row id closed as batched, or the reverse** — unrepresentable (bit-63 routing). **Dispose from a row delivery** — refused (L11).

**Handled residue:** (a) **a failure inside C#'s own hand-over** (out of memory, an import refusal, a window longer than one managed
batch can be): null batch, the window's rows added to `RowsDropped`, reason kept. *Why not forbidden:* allocation can fail, and d10 L9
forbids losing them silently; d9 L18 answers the same way one layer down. Whether this also carries a failure in `BatchStatus` is rung
11's design. (b) **`HandlerFaulted.SubscriptionId` is 0 for a batch delivered before `SubscribeBatched` returns.** *Why not forbidden:*
the shim may deliver before `out_id` is written (`h:1077`); blocking the batch thread on the caller would couple domain 7's thread to it.

## Premises and stop conditions
- **P1 (D24).** Apache.Arrow 23's C Data importer imports a dictionary array. Schema half evidenced in-repo (dictionary topics' schemas
  import as `DictionaryType` today, `DecodedSchema.cs:74,100`; the exporter carries dictionaries to `fl_rows_bind`); the array half is
  upstream's importer, **not executed at design**. **First step-3 action:** a leg in `ArrowCDataInterfaceTests` round-trips a
  `DictionaryArray` (int32 → utf8, with a null) through `CArrowArrayExporter`/`Importer`. **STOP-AND-ASK if red: an owner question;
  never a C# conversion (L3).**
- **P2.** The importer moves out of a `CArrowArray` (`FletcherCodec.cs:266-279` relies on it) and `CArrowArray/CArrowSchema.Free`
  release only an unreleased struct. A leg calls the thunk with a C#-exported batch and asserts the source's `release` is NULL after and
  the kept batch reads intact. If `Free` would double-release, STOP and report; do not hand-roll a release.
- **P3.** Exactly one Closing per accepted subscription on every close path, d9's `[built]` promise. A C# test seeing 0 or 2 is a d9
  defect: STOP (d9 production edits are out of scope).
- **P4.** Only a row-delivery-thread close leaves the transport quiet. If a server-leg test shows a batch-thread close does too, D5's
  condition widens to "inside any handler of this provider"; report it, same mechanism.
- **P5.** `TreatWarningsAsErrors` does not flag the unconstructed `Batcher` or the unread ceiling field (CS0414 is for constants;
  IDE0051/52 are suggestions). If it does: STOP-AND-ASK the PM. No suppression; the choice is to pull that member's deletion from rung 8.
- **P6.** Rung tests pick their leg from outside: the server file beside the test assembly (L5's own fact), never `NativeMethods`.

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
Wait for delivery, not straight after `Publish` (:140, L13): T12, T14, T17, T22, T23, T27. T26's handler ignores the Closing. Control: a
10 ms timeout gives `Timeout` within 2 s. **Deleted, never re-anchored (subject forbidden):** T20, T30, T34, T38, T39. Fast DDS batched
profile (M2): batched arm on the server shim (7a by hand, 7b in CI), edge arm the truth table. T46: declared red to D10C-11 (:650 (2)).

## Risks / Unknowns
- **Size → split (cut line differs from the ladder's).** Honest gross: 7a (all `dotnet/src`) **+230-270 / −65-75**, net ~+170-200;
  M2's lane **+40-60** YAML. Whole ≈ net +220-260 before any overrun; D10C-5/-6 overran by >50%. **7a** = D1-D7 and every test above;
  **7b** = the transport-conformance lane's server pass (build `pubsub-arrow` + the server variant; stage `fletcher-c-abi-server` beside the
  tests; second `dotnet test`), light lane. The ladder's cut (attachments to 7b) saves ~10 lines and leaves four tests red, so it does
  not relieve the ceiling. Window: for one stage the Fast DDS batched-profile arm runs by hand only (no red window). Owner: brief D1.
- **D24 is unverified by execution at design** (P1). It is the stage's first gate.
- **Plan shape:** the batcher's deletion is scheduled for the very next stage (D7). Merging rung 8 in (net ≈ −160 to −310; D7 and P5
  vanish; rung 8's scans and T40 move in) is offered to the owner as brief Decision 1 (d), recommended; 7a priced +215-245 (review DEBT-3).
- **D5 lengthens a ruled refusal** (L3 :575 (6)): after a quiet close the other kind is refused until `Dispose`. Same subject, longer
  duration; the alternative is the silent loss the ruling exists to prevent. Disclosed in the brief. BC-10's scan must allow D2's call.

## Files-to-touch
7a: `dotnet/src/Fletcher.Interop/NativeMethods.cs`, `AbiTypes.cs`; `dotnet/src/Fletcher/Subscriber.cs`, `SubscriberArrow.cs`, `Errors.cs`,
`SchemaArrival.cs`. Tests: `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`, `ArrowCDataInterfaceTests.cs`;
`integration-tests/pubsub-transport-conformance/dotnet/PubSubTransportConformance/TopicOptionsOverFastDdsTests.cs`. Docs (listed, not
counted): `SA:4-94, 104-114, 148-162, 235-246` header and docs (F16; `MaxRows` "below 1 mean 1" goes); d10 L3 Tests line and V12 per :650 (1);
d10 marks L3 (D29), L6, L8, L9, L10, L12, L13, L14 → `[built]` (Windows by hand / devcontainer; CI unrun). 7b:
`.github/workflows/ci.integration-test.pubsub-transport-conformance.yml` (and the conformance csproj only if staging needs a property).

## Files-to-delete
- Tests T20, T30, T34, T38, T39 — no replacement; behaviour gone by law (audit MUST-DELETE).
- `SA:254-267` (timeout refusal, `MaxRows` clamp) — replaced by domain 7's refusal passed through (BC-8).
- `SA:175` `_batchers` and its uses at `SA:275-278, 379-385, 438-448` — replaced by the native close and destroy.
- Kept for rung 8, unreachable (D7): `Batcher`, the ceiling constructor and constant, `DecodeBatchFramed`; `BatchOptions.MaxTimeout` (rung 10).
