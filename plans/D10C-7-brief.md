# D10C-7 — Stage Brief (2026-11-16)

**In one sentence:** a C# app that has the server package now gets its batches from Fletcher's one batcher, the C++ one, and an app
on the small shim gets a clear "not supported here, add Eiva.Fletcher.Server" instead of the old C# copy's batches.
**Forcing test:** the batched tests on both shims — on the server shim batches arrive one at a time, in order, on one thread, with
exactly one closing delivery (empty when idle), re-folded dictionaries and their attachments; on the small shim the batched call fails
with the shim's own message plus one sentence naming the server package, and nothing is ever delivered.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| `SubscriberArrow.SubscribeBatched` | CHANGED | Server shim: windows closed by the C++ batcher, handed over as batches the handler owns and may keep. Small shim: named refusal. |
| its options | CHANGED | Passed as given: a row limit below 1 or a negative timeout is now the C++ batcher's "invalid argument" error (not clamped, not `ArgumentOutOfRange`); a timeout past 49.7 days is accepted. |
| `Unsubscribe`, `Dispose`, a subscription's `Dispose` | CHANGED | From outside a handler they return after the closing delivery; from inside the handler they return at once and the closing delivery follows it. |
| same-topic refusal | CHANGED | After an unsubscribe made inside a row delivery, the other kind stays refused on that topic until the subscriber is disposed (the network subscription is still open then). |
| `HandlerFaulted.SubscriptionId` | CHANGED | Batched ids are now the shim's (very large numbers); 0 for a fault in a batch that arrives before the subscribe call returns. |
No new public surface. The small shim's per-row path, publishing and decoding are untouched.

## Deleted
- Five tests whose subject the laws forbid (dictionaries as plain values, the 49.7-day refusal, overlapping batches, two C#-only
  window-size tests); the C++ batcher's own tests cover those cases (ported in D10C-1). C#'s own option checks: replaced by C++'s answer.
- C#'s batcher is **not** deleted here: nothing can reach it any more (no option, build or fallback leads to it), and D10C-8 deletes
  it. Dead code for one stage, not a bridge.

## Corner cases forbidden vs handled
**Forbidden:** a refused subscribe that later delivers; a batch freed before its last delivery or twice; a kept batch pointing at
memory the shim reclaims; two handler calls at once or out of order; one kind silently replacing the other on a topic; a negative
timeout crossing as zero; the "add the server package" sentence on a refusal that is not the build's.
**Handled:** if C# itself fails to take a batch over (out of memory), the handler gets "no batch" with that window's rows counted as lost.

## Decisions for you
1. **The switch is near the size ceiling. Where is it cut?**
   (a) Two steps: everything a C# app observes first; then the CI run that proves a Fast DDS reader profile reaches a batched
   subscription (by hand until then). *Users:* nothing ships mid-round; no test is red. *Architecture:* keeps the refusal with the
   first batched call, as the ladder requires; serves the purpose whole in one step.
   (b) The ladder's cut: attachments second. *Users:* nothing ships; four tests red for one stage. *Architecture:* saves ~10 lines, so
   it does not relieve the ceiling; a red window with no gain.
   (c) One stage, ~+260 net. *Users:* none. *Architecture:* over the 250 ceiling (accepted before at :643; overruns have run >50%).
   **Recommendation:** (a) — the round purpose lands whole with no red window. **Default:** (a).
   *Background: the attachments are reused from D10C-3 (one call); the separable bulk is the transport-conformance lane's server build.*

## Risks accepted / debt carried
- The dictionary import (C++ hands a dictionary column, C# must take it as-is) is checked first in implementation; if Apache.Arrow
  cannot import it, work stops for your decision; C# never converts it.
- The preview-bundle consumer's batched path stays red until D10C-11 (your ruling, :650 (2)).
- If the build flags the unreachable batcher code, the PM decides whether that piece's deletion moves up from D10C-8.

- **Verification:** full dotnet suite, edge and server legs (Windows); binding-abi-conformance, both passes;
  pubsub-transport-conformance (7b adds its server pass); Linux devcontainer (`fletcher-build:latest`).

## Numbers
Declared net lines: 7a +230-270 / −65-75 · 7b +40-60 / −0 · new public surface: 0 · design cycles used: 0/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
