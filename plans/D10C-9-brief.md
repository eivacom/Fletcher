# D10C-9 — Stage Brief (2026-11-16)

**In one sentence:** a C# app may close its whole subscriber from inside its own batch handler and gets the remaining batches, then one closing batch; bad batch options reach C# as Fletcher's own refusal, word for word; and the old "about 49.7 days" limit disappears from the public API.
**Forcing test:** `BatchOptionsTests.TheTimeoutCapIsGoneFromThePublicSurface`: `BatchOptions` has no `MaxTimeout`. It fails today because the member exists. It is the stage's only red-first test. Everything else below already passes after D10C-7 and is a **declared guard**, each with its control.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| `BatchOptions.MaxTimeout` (public static) | **DELETED** | Ruled :536 (3), "the 49.7-day cap goes"; no limit exists to name. Code that reads it stops compiling. |
| Anything else public | none | `MaxRows`, `Timeout`, `SubscribeBatched` and `Dispose` keep their shapes. No new type or member. |

## Deleted
`BatchOptions.MaxTimeout` (behaviour gone, disclosed; its only test use is T31, re-anchored below). No test is deleted. **T31** (`AWindowAtTheMaximumTimeoutWaits`) has a lawful subject, "a very long timeout never fires early" (d7 L5), so it is **re-anchored** (audit 0a): two cases, 100 years and `TimeSpan.MaxValue`, on both legs, with "timer's maximum" removed from its name and summary.

## Corner cases forbidden vs handled
**Forbidden:** C# checking, clamping or capping a batch option (L3; BC-8 source scan) · a second place that turns a `TimeSpan` into milliseconds (L12; BC-14 scan) · −1 tick becoming 0 ms, i.e. "no wait" (the one conversion rounds negatives down, so −1 tick crosses as −1 and domain 7 refuses it) · naming a 49.7-day limit (the member is deleted) · disposing a `SubscriberArrow` inside a *row* delivery (still refused; existing test).
**Handled:** two batch handlers that dispose or close each other's subscribers wait for each other and hang. Domain 9 already documents this. *Why not forbidden:* the wait is the shim's rule (d9), and refusing it in C# would mean C# answering a question a lower layer owns (L3).

## How
- **Production:** remove `MaxTimeout` from `dotnet/src/Fletcher/SubscriberArrow.cs`, 2 lines. Rung 9 needs no code: on domain 7's batch thread no delivery frame is open, so `SubscriberArrow.Dispose` passes its row-delivery check and calls the native destroy, which serves it (d10 L11; ruling :174 (1)). `Dispose` holds no lock while closing.
- **Rung 9 guard, server leg** (`SubscriberArrowTests`): two cases, a row-limit window (V14's case) and a timeout window. `MaxRows = 1`. The handler blocks on its first window while two more rows are published, then calls `Dispose`. Expected: `Dispose` returns inside the handler within the wait limit; then the two queued row-limit windows arrive, then exactly one `Closing`, then nothing. *Control:* a flag set when `Dispose` returns is checked against the wait limit, so a `Dispose` that waits for its own `Closing` fails the test instead of hanging. Edge leg: the BC-10 truth table.
- **Rung 10 grid guard** (new `BatchOptionsTests.cs`): BC-8's 6 x 7 grid from public C#. `MaxRows` takes {long.Min, −1, 0, 1, 2, long.Max}. `Timeout` takes {TimeSpan.MinValue, −1 tick, 0, 1 ms, `InfiniteTimeSpan`, 100 years, TimeSpan.MaxValue}; −1 tick stands in for d9's −1 because −1 ms *is* `InfiniteTimeSpan` in C#.
  - *Server leg:* 27 cells refused, each `FletcherException(InvalidArgument, Seam)` with domain 7's message byte-equal, built from **literal** expected millisecond values (−1 tick → "got -1 ms": **the −1-tick arm**). Nothing is delivered, and the topic is free again. The 15 accepted cells deliver as in d9's `EveryBatchOptionReachesDomain7AsGiven`; the no-deadline cells deliver nothing within 2 s and then exactly one `Closing`. *Control:* a 10 ms subscription gives `Timeout` inside the same window.
  - *Edge leg:* every cell, bad or not, gets the edge `NotSupported`. That is the outside proof that C# checks nothing before the shim.
- **Scans** (same file; they reuse `ManagedBatcherGoneTests`' code-line reader, made internal):
  - BC-8: no `<`, `>`, `Math.Clamp`, `Math.Min` or `Math.Max` on `.MaxRows` or `.Timeout` in `dotnet/src/Fletcher`; equality with `InfiniteTimeSpan` is allowed.
  - BC-14: every ticks-to-milliseconds expression is inside `SchemaArrival.ToNativeMilliseconds`, and `SubscribeBatched` calls it.
  - Each scan has pattern controls that feed it the deleted clamp and cap lines.
- **Docs (domain 10's own):** build marks for d10 L3, L11, L12 and L14; §4 :438 and :471; register BC-8 and BC-14 status. No change to d7 or d9.
- **Verification**, as the D10C-7 evidence rows: from `dotnet/`, `dotnet build Fletcher.slnx -c Release -m:1 -p:FletcherNativeShim=<edge> -p:FletcherProbeShim=<probe> -p:FletcherServerShim=<server>`.
  - *Edge leg:* `dotnet test Fletcher.slnx -c Release --no-build -m:1`.
  - *Server leg:* the server file copied under its own name into `tests/Fletcher.Tests/bin/Release/{net8.0,net10.0}`, then `dotnet test tests/Fletcher.Tests -c Release --no-build -f <tfm>` (removed afterwards, checked gone).
  - Before the change: the forcing test is red and the guards are green. After it: all green.

## Decisions for you
None. Removing `MaxTimeout` is already ruled (:536 (3)), nothing else public changes, and I found no conflict between the authorities.

## Risks accepted / debt carried
- If the rung 9 guard is red (a hang, or a refusal), the fault is in C# (≤15 lines, 1 file, as the ladder prices it) **unless** it traces to the shim or to domain 7, which is a STOP: no edits to d7 or d9.
- The grid pins domain 7's message text in a C# test. If domain 7 rewords its message, this test changes with it. That is accepted, because "byte-equal" is the register's own test.
- `GatewayBatchOptions.MaxTimeout` and its range check remain (domain 5, out of scope; BC-8 and BC-9 deferred).

## Numbers
Declared net lines:
- **Production:** +0 / −2 in 1 file; worst case +15 / −2 in 2 files if a guard turns out red. **Fits the light lane.**
- **Docs:** about +12 / −12 in 2 files.
- **Tests:** about +230 to +300 in 3 files: the 42-cell grid on two legs, two scans with controls, rung 9's two cases and T31. This is where an overrun would come from, so it is priced here.

New public surface: 0 (1 member retired) · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
