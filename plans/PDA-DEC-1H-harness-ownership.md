# PDA-DEC-1H — the harness proves it owns the Agent answering the port

**Status.** Executed on a **compressed cycle by PM decision (2026-09-02)**: no separate
architect step and no architecture-review cycles. The dispatch brief *was* the design, and this
document records it — written retrospectively, and labelled as such rather than presented as a
design that preceded the work. The independent scrutiny was the code review
([reviews/PDA-DEC-1H-codereview.md](reviews/PDA-DEC-1H-codereview.md)), which was explicitly
asked to weigh design judgement as well as implementation because it was the only review, plus
a compliance review at close.

**Why compressed.** A ~20-line harness guard with a crisp forcing test. A 300-line design doc
and two review cycles would have cost more than the item. The cost of that choice was paid at
the close gate, which needed this document and a Stage Brief produced after the fact — recorded
here as the honest price of the compression, not hidden.

## Why the item exists

PDA-DEC-1 built the XRCE conformance harness. Its Agent guard asserted `SpawnedAgentAlive()` —
true of the process the binary spawned, but **not evidence that that process is the one
answering the port**. A PDA-DEC-7 cycle-2 re-reviewer observed `conformance_xrce` report 25/25
PASSED while served by a foreign Agent.

Owner ruling 2026-09-02: fix **in-round**, before PDA-DEC-9 signs the parallelism handoff, since
both ABI rounds inherit this harness as their conformance oracle. Tracked as PDA-DEC-1H —
PDA-DEC-1's defect, found by PDA-DEC-7. Round denominator 9 → 10.

## The mechanism — measured, and it refuted the PM's hypothesis

The item's first obligation was to establish the mechanism rather than build on the guess.

- **Hypothesis (PM):** the spawned Agent fails to bind but does not exit, so liveness stays true.
  **Refuted.** An Agent that cannot bind logs `bind error` and **exits in ~876 ms**.
- **Actual cause: a race, and a true-but-stale predicate.** Windows `Spawn` has no reap loop
  (that loop is POSIX-only), so the first reachability probe answers from the *leftover* Agent
  within milliseconds while the doomed child still needs ~0.9 s to reach its bind error —
  `WaitForSingleObject(handle, 0) == WAIT_TIMEOUT` is still true when asked. Reproduced with the
  two probes answering **16 ms apart** against a child that dies ~900 ms later.
- On POSIX the reap loop *did* catch it, but told the operator to build a binary that already
  existed and never mentioned the port.
- The **interop fixture was worse**: no liveness check at all, so a leftover Agent satisfied it
  outright, no race required.

## The design

**Prove ownership of the endpoint being certified; do not infer it from liveness.**

`OwnedAgent::ProveOwnership()` requires the OS to record *this binary's child* as the holder of
the port: `GetExtendedUdpTable(UDP_TABLE_OWNER_PID)` matched against the child's pid on Windows;
`/proc/net/udp{,6}` inodes intersected with the child's `/proc/<pid>/fd` on Linux.

- **Foreign beats ours.** `ours` and `foreign` are accumulated over the whole table before
  deciding, so a shared-port `SO_REUSEADDR` case is *refused*, not guessed, and table iteration
  order cannot invert the verdict.
- **Liveness is demoted to a diagnostic**, distinguishing only "missing binary" from "lost the
  bind to a leftover" — which also unified two divergent platform messages.
- **No third state.** A platform with neither query is a **compile** refusal (`#error`); a
  runtime query failure **refuses**. Fix cycle 1 deleted an earlier `kUnprovable` that fell back
  to bare liveness and *returned success* — which would have re-admitted this item's own defect
  behind one line of stdout nobody reads.
- **Scope, stated rather than implied: the proof is bring-up-only.** A foreign Agent appearing
  *after* the proof is not caught. Recorded in both READMEs, both file headers and both doc
  comments.

**Duplicated, not shared, and the recorded reason matters.** The ownership block is byte-identical
in both harnesses. The first reason given — that a shared header would need four sparse-checkout
blocks and two path-filter edits — was **checked by the reviewer and is false**: both lanes
already sparse-checkout and path-filter the provider directories. The true reason is package
self-containment: sharing would mean exporting a test-only header from a shipped provider
package. The drift guard is behavioural — each harness carries its own equally-named forcing
test in its own CI lane, so breaking either copy reddens that copy's test.

## Forcing tests

| Test | Where | What reddens it |
|---|---|---|
| `ConformanceXrce.AForeignAgentDoesNotSatisfyTheHarness` | `pubsub-conformance` | reverting `ProveOwnership()` — the harness certifies a foreign Agent |
| `AForeignAgentDoesNotSatisfyTheHarness` | `fastdds-xrce-interop` | same, on its own fixture |
| the refusal-on-query-failure path | both | stubbing a runtime query failure — proved: ctest entry **Failed** where pre-fix it passed |

## Numbers

Declared: not declared up front (compressed cycle). **Actual +1263 / −246** across four harness
files plus two CMakeLists — `4d7d342` +449/−57, `5af2bfb` +474/−24, `ecc7b2c` +340/−165.
New public surface: **0** — harness-only, no provider, seam or clause change.

## Out of scope

No provider change; no delivery-contract or `ProviderConformance` clause change; no weakening,
skipping or deletion of any existing clause; nothing near `integration-tests/gateway-fastdds-ts`
(the only harness reproducing the receive-side data-sharing defect, protected by the 2026-09-01
ruling). No ABI, no config parser, no link-size check. Left as larger than this item: the 1 s
POSIX reap window, job-object / `PR_SET_PDEATHSIG` structural process cleanup, and the unbounded
`waitpid` in `Kill()` — all pre-existing.
