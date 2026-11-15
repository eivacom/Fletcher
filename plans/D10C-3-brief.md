# D10C-3 — Stage Brief (2026-11-15)

**In one sentence:** a C# app that receives Arrow rows one at a time now gets them straight from the small, Arrow-free native library, with no batching machinery involved, and every row it cannot read reaches its handler at once as "no data, 1 row lost", never skipped and never folded into a later delivery.
**Forcing test:** `SubscriberArrowTests.PerRowDeliversEachRowAloneOrCountsItAsOneDrop` publishes a mix of good and corrupt rows, rows sent before the topic was declared, and rows on a topic whose layout C# cannot read. It proves each row produces exactly one handler call, carrying either that row or one counted loss, and that the totals add up.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Per-row Arrow subscribe | CHANGED behaviour, same signature | Runs on the per-row native path, not on C#'s batcher (d10 L7, D20; ruling :536 (1)) |
| What a per-row handler sees for an unreadable row | CHANGED | At once, alone: no data, 1 row lost (ruling :575 (3); d10 L9, D30) |
| The "row limit" reason | Doc text only | It also labels every per-row delivery (see below) |

No new public type or member.

## What a user will notice
- **A bad row is reported when it arrives.** Today a corrupt row on a readable topic produces no call; its loss is added to the next good row's delivery. A topic C# cannot read, or a subscription made before the topic existed, today produces nothing until a one-minute timer fires or the app unsubscribes, then one combined "N lost". After this stage each such row is its own call with 1 lost.
- **Unsubscribing a per-row subscription delivers nothing further.** Today it could deliver one final "closing" call holding pending losses. Now nothing is ever pending.
- **A first row with no layout no longer disables the subscription for good.** On transports that send the layout with the data, later rows decode (ruling :258 (3)).
- **Every per-row delivery is labelled "row limit", losses included.** The laws settle this rather than a choice. C# may not invent a reason the native library does not have (d10 L2). "Closing" means "this is the last delivery" and "timeout" means "a deadline passed", so both would be false here. A one-row delivery is a full window of one.

## Deleted
Per-row subscribe's route through C#'s batcher (a batched subscribe of one row) is replaced by the per-row native path. No test or file is deleted. The twelve per-row tests the audit listed now run on the new path, with their text unchanged.

## Corner cases forbidden vs handled
**Forbidden:**
- A row skipped silently: the code has no route around the handler call.
- A loss folded into another delivery, or a delivery holding zero or several rows.
- A closing or empty delivery on a per-row subscription.
- A missing layout disabling the subscription for good.
- Decoding with an already-released decoder: it is released only after the last possible delivery.
- A handler failure counted where the app cannot see it.
- Disposing the subscriber from inside its own row delivery: refused before anything is torn down, as today, and now covered by a test.

**Handled:**
- A row whose attachments cannot be copied (out of memory) is counted as lost. Memory can run out on any row, and the only lawful outcome is a counted loss.
- If a handler fails before the subscribe call has returned (possible on networked transports), the failure event can carry subscription id 0. Native assigns the id only when the call returns. The failure count stays exact.

## Decisions for you
None:
- The labelling question the audit raised (T8) is settled by law L2 and the meanings of the other two reasons, shown above.
- No new public surface.
- No edits to the native library or the batcher (domains 9 and 7).
- No C# check repeats a lower layer's.
- No test is skipped or disabled.

## Risks accepted / debt carried
- C#'s batcher keeps its own copy of the attachment copy until it is deleted (stage 8). Batched subscribe stays on it until stage 7. Both are declared in the ladder.
- An unreadable layout is decided once per subscription. A decoder that fails from lack of memory, rather than refusing the layout, therefore counts as unreadable for good, the same gap domain 7 has registered.

## Numbers
Declared net lines: production about +85 / −3 in 2 files (about 88 touched, so not light-lane). Tests about +160 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
