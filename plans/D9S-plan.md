# D9S — The shim's server build — Plan and tracker

**Purpose (owner's terms):** C# — and any other binding — can get columnar batches straight from
Fletcher's one batcher through the shim, while the build that runs on embedded devices stays exactly
as small and Arrow-free as today. Owner: "we should only have one and it should be the C++ one";
"Arrow C++ should never be in the native library, because it needs to be able to run on embedded
devices"; "Let's build one shim in two variants".

- **Domain:** 9, the C ABI shim ([spec](../docs/domains/d9-c-abi-shim.md)); this round creates it
  (Gate 2, shape 2) — the document was specified and owner-confirmed 2026-11-13 before any stage.
- **Locked decisions:** [D9S-locked-decisions.md](D9S-locked-decisions.md).
- **Owner rulings:** [RIX-rulings.md](RIX-rulings.md), the 2026-11-12/13 entries.
- **Test ladder:** [D9S-test-ladder.md](D9S-test-ladder.md): 10 rungs, 7 stages.
- **Pre-round audit and census:** [D9S-pre-round-audit.md](D9S-pre-round-audit.md).

This file is both the `plan_path` (the tracker) and the `user_stories_path`.

## Tracker

Status: ⚪ not started · 🔴 in progress · 🟢 done. Lane: **L** = light-lane candidate by size (≤60
touched lines, ≤3 files; ejects on overrun), **F** = full loop.

| Item | Subject (what becomes true) | Rungs | Inherits | Est. prod. lines / files | Lane | Status |
|---|---|---|---|---|---|---|
| D9S-1 | A caller can ask the loaded shim which build it is; today's shim and the probe shim answer "edge"; the ABI version is bumped once and C# still loads it. | 1 | D-D9S-6 (the one `NativeLoader.cs` line); BC-10 single site | 15–30 / 3 | L | 🟢 |
| D9S-2 | A second build from the same sources says "server", behaves identically on the edge surface and exports the same set; CI measures the edge shim by package identity, not file name. | 2 | D9S-1; audit T3, T5, T19 re-anchors; Q5a's file-name half (shim side only); `ci.c-abi.yml:25` stale note; D9S-1 4b n1 (variant answers in a refused shim) and n3 (CI export check names `fl_binding_variant`) | 90–140 / 4–5 | F | ⚪ |
| D9S-3 | The edge build is shown, by name not size, to carry no Arrow C++. | 3 | D9S-2; BC-11 | 40–70 / 2 | L | ⚪ |
| D9S-4 | On the server build a C caller receives each window domain 7 closes as an importable Arrow batch with reason, loss count and attachments, in order; a window domain 7 could not build arrives as no-batch with rows counted; the edge and probe builds refuse with not-supported. | 4 | D9S-1..3; BC-9, BC-10; Q6, Q7, Q12 due | 150–220 / 5–7 | F | ⚪ |
| D9S-5 | A batch is the caller's only for its callback unless moved; closing delivers the rest then the closing batch, and works from inside the callback; options reach domain 7 exactly as given (no-timeout, refusal). | 5, 6, 7 | D9S-4; BC-13, BC-5, BC-8 grid through the ABI, BC-14 | 25–75 / 2–3 | F | ⚪ |
| D9S-6 | A batch the shim cannot hand over still reaches the caller as a no-batch delivery with its rows counted. | 8 | D9S-4..5; BC-12; test-only fault hook (D-D9S-4); Q12 | 40–75 / 2–4 | F | ⚪ |
| D9S-7 | Edge and server in one process give one working shim; the C# conformance lane passes against both builds. | 9, 10 | D9S-1..6; BC-15 | 15–40 / 1–2 | L | ⚪ |

## Definition of done (round)

- All seven items 🟢; d9 L12–L18 (and L6's bump, L8's check) read `[built]` with their tests;
  register BC-10–BC-15 `[built]` or scoped.
- c-abi tests, binding-abi-conformance (both builds), and the integration suites that load the shim
  green; the edge shim's size and dependency checks green.
- No edit under `dotnet/` beyond the one `NativeLoader.cs` line; none to domain 7 or domain 2 code.
