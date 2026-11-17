# D10C-7 — Stage Brief (2026-11-16)

**In one sentence:** a C# app with the server package gets its batches from the C++ batcher, an app on the small shim gets a clear
"add Eiva.Fletcher.Server" refusal, and C#'s own batcher is deleted in this same stage (ruled :660).
**Forcing test:** batched tests on both shims (server: one at a time, in order, one thread, exactly one closing delivery, re-folded
dictionaries, attachments; small shim: the named refusal), and scans showing C#'s batcher gone from the source and the packed assemblies.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| `SubscriberArrow.SubscribeBatched` | CHANGED | Server shim: windows closed by the C++ batcher, handed over as batches the handler owns and may keep. Small shim: named refusal. |
| its options | CHANGED | Passed as given: a row limit below 1 or a negative timeout is now the C++ batcher's "invalid argument" error (not clamped, not `ArgumentOutOfRange`); a timeout past 49.7 days is accepted. |
| `Unsubscribe`, `Dispose`, a subscription's `Dispose` | CHANGED | From outside a handler they return after the closing delivery; from inside the handler they return at once and the closing delivery follows it. |
| same-topic refusal | CHANGED | After an unsubscribe made inside a row delivery, the other kind stays refused on that topic until the subscriber is disposed (the network subscription is still open then). |
| `HandlerFaulted.SubscriptionId` | CHANGED | Batched ids are now the shim's (very large numbers); 0 for a fault in a batch that arrives before the subscribe call returns. No new public surface; per-row, publishing and decoding untouched. |

## Deleted
- **C#'s own batcher** (windows, row counts, timer, deadline clock, window-size ceiling, its framed decode): replaced by the C++
  batcher through the server shim; no bridge, no dead code. Its option checks: replaced by the C++ batcher's answer. Six tests the laws
  forbid (dictionaries as plain values, the 49.7-day refusal, overlapping batches, three C#-only window sizes; C++ covers the window-size cases).

## Corner cases forbidden vs handled
**Forbidden:** a refused subscribe that delivers; a batch freed early, twice, or kept on memory the shim reclaims; two handler calls at
once or out of order; one kind silently replacing the other; a negative timeout as zero; the package sentence on another refusal.
**Handled:** if C# itself fails to take a batch over (out of memory), the handler gets "no batch" with that window's rows counted as lost.

## Decisions for you
1. **Ruled 2026-11-16: (d)** (ledger :660). **The switch is near the size ceiling. Does C#'s own batcher go in the same stage, and if not, where is the switch cut?**
   (d) **Merge D10C-8 in:** switch, CI lane and deleting C#'s batcher in one stage, net about −160 to −310. *Users:* nothing ships
   mid-round; C#'s copy is gone when batches start coming from C++; no red test, no dead code. *Architecture:* the purpose's "C#'s own
   copy is gone" (:566) and L4 (no window, count, timer or batch thread in C#) land with the switch; D10C-8 moves in whole (the sixth
   forbidden test, BC-9's narrowing, its forcing scans of the packed assemblies and the source). Cost: one review of ~300 lines written
   and ~600 deleted, mostly one file; the round drops to 10 stages.
   (a) **Two stages, batcher kept:** all C# first, then the CI run proving a Fast DDS reader profile reaches a batched subscription.
   *Users:* nothing ships; no red test. *Architecture:* the switch lands whole, but C#'s copy stays in source, unreachable, until D10C-8 (L4).
   (b) **The ladder's cut, attachments second.** *Users:* nothing ships; four tests red for one stage. *Architecture:* saves ~10 lines
   (attachments reuse D10C-3's copy), so it does not relieve the ceiling: a red window for no gain.
   (c) **One stage, switch only, about +255 to +305 net.** *Users:* none. *Architecture:* over the 250 ceiling; :643 accepted D10C-5's
   overrun but kept 250 for later stages, so this needs a new exception.
   **Recommendation:** (d): it serves the round purpose better, since "C#'s own copy is gone" becomes true in the stage where batches
   start coming from C++, with no dead-code window. Its only cost is review size, which breaks no tie here. **Default:** (d).

## Risks accepted / debt carried
- Dictionary import checked first: if Apache.Arrow cannot take C++'s column as-is, work stops for you (C# never converts it). The
  preview-bundle consumer's batched path stays red until D10C-11 (:650 (2)).
- **Verification:** dotnet suite, edge and server legs (Windows); binding-abi-conformance, both passes; pubsub-transport-conformance
  with its new server pass; the "batcher is gone" scans of the source and of the packed assemblies; Linux devcontainer (`fletcher-build:latest`).

## Numbers
Declared: **+320 to +380 / −545 to −595, net −165 to −275** (switch net +215 to +245, reviewer's pricing agreed; CI lane +40 to +60;
deletion +0 to +10 / −480 to −520) · new public surface: 0 · design cycles used: 1/2. Counted: code and `//` comments (the
`SubscriberArrow` header's rewrite counts both ways); `///` doc comments, Markdown and domain-doc marks are listed, not counted (ladder :14).

---
**As landed (2026-11-16):**
Switch + D10C-8's deletion in one stage (ruling :660). Counted prod+CI +328/−555, net −227 (declared net −165..−275); tests +1351/−212; 6 tests deleted. D24 and P1 hold (a C++ dictionary imports). Not predicted: hand NULL at `release` via `Marshal.OffsetOf` (the P2 cross-check is no longer independent); an idle Closing may be a null batch (d7 L6); 10 ms control bound widened 2→5 s (PM); pack job builds Fletcher.Tests. Design cycles 2, fix 1. Green on Windows and the local Linux devcontainer; CI unrun.
