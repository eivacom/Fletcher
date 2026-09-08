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
  **Refuted.** An Agent that cannot bind logs `bind error` and **exits within tens of
  milliseconds**.
- **The ~876 ms / ~0.9 s figure first published here was wrong, and wrong in the instructive
  way: it was a measurement artifact, not a mistyped digit.** It came from
  `Measure-Command { Start-Process -Wait }`, which times PowerShell's launch-and-poll wrapper.
  That wrapper reports **~1,025-1,122 ms around `cmd /c exit`**, a process that does nothing,
  so ~1 s is the instrument's floor and was never the Agent's lifetime. **What is actually
  measured, and how:** the doomed child's own OS lifetime, `Process.ExitTime -
  Process.StartTime`, read after `WaitForExit()` on a `Start-Process -PassThru` handle with an
  incumbent Agent holding the port — **28-89 ms across nine trials in two independent
  sessions** (28 / 33.6 / 36.7 / 40.4 / 44.8 ms in the compliance review's four; 66.9 / 67.5 /
  71.5 / 71.6 / 89.3 ms in fix cycle 2's five, where bare process start-up on the same box
  costs 69-82 ms). It agrees with the Agent's own log, 2.7 ms from `bind error` to `server
  stopped`. The lesson is kept rather than the number quietly swapped: **a plausible number
  from the wrong instrument is worse than no number**, and nobody asked how the first one was
  taken until the compliance review (F2) did.
- **Actual cause: a race, and a true-but-stale predicate.** Windows `Spawn` has no reap loop
  (that loop is POSIX-only), so the first reachability probe answers from the *leftover* Agent
  within milliseconds while the doomed child is still on its way to its bind error —
  `WaitForSingleObject(handle, 0) == WAIT_TIMEOUT` is still true when asked. Reproduced with
  the two probes answering **16 ms apart**, and the doomed child still alive at that mark in
  **7 of 9 trials**. The correction sharpens the finding: the race is **~10-90 ms wide, not
  ~900 ms**, so the pre-existing XRCE greens were a **coin flip**, not a near-certainty — a
  materially different risk statement, and the one a reader will quote.
- On POSIX the reap loop *did* catch it, but told the operator to build a binary that already
  existed and never mentioned the port.
- The **interop fixture was worse**: no liveness check at all, so a leftover Agent satisfied it
  outright, no race required.

## The design

**Prove ownership of the endpoint being certified; do not infer it from liveness.**

`OwnedAgent::ProveOwnership()` requires the OS to record *this binary's child* as the holder of
the port: `GetExtendedUdpTable(UDP_TABLE_OWNER_PID)` matched against the child's pid on Windows;
`/proc/net/udp` inodes intersected with the child's `/proc/<pid>/fd` on Linux.

- **IPv4 only, on both platforms.** The Windows query passes `AF_INET`; the Linux one reads
  `/proc/net/udp` and **not** `/proc/net/udp6`. It used to read both, which made the Linux copy
  strictly stricter than the Windows copy while both comments claimed one rule — with our
  child on `127.0.0.1:P` and a stranger on `[::1]:P` v6-only, Linux said `kSomeoneElses` where
  Windows said `kOurs` (compliance review F1, reproduced under WSL). A false *refusal* and never
  a false pass, so nothing was certified wrongly; but two blocks presented as one rule have to
  BE one rule, and a narrower guard that is genuinely uniform beats a wider one that silently
  differs. The `udp4` spawn is what makes the narrow rule the right one.

- **Foreign beats ours.** `ours` and `foreign` are accumulated over the whole table before
  deciding, so a shared-port `SO_REUSEADDR` case is *refused*, not guessed, and table iteration
  order cannot invert the verdict.
- **Liveness is demoted to a diagnostic**, distinguishing only "missing binary" from "lost the
  bind to a leftover" — which also unified two divergent platform messages.
- **No third state.** A platform with neither query is a **compile** refusal (`#error`); a
  runtime query failure **refuses**. Fix cycle 1 deleted an earlier `kUnprovable` that fell back
  to bare liveness and *returned success* — which would have re-admitted this item's own defect
  behind one line of stdout nobody reads.
- **And that deletion is now guarded, which it was not.** The `#error` arm has a compiler behind
  it; the runtime arm had only inspection, because nothing in either harness reached
  `kQueryFailed` — re-introducing the fallback reddened **nothing**, in the very item that
  exists to fix an unfalsifiable guard (compliance review F3). The query is therefore reached
  through **one function-pointer indirection**, `g_udp_port_ownership_query`, defaulting to the
  real query; `AFailedOwnershipQueryDoesNotSatisfyTheHarness` in each harness points it at a
  stub that fails and requires the bring-up to refuse an Agent that is genuinely alive and
  genuinely ours. No build option and no environment variable can reach the seam — that test
  is its only assignment in the tree. **Proved rather than asserted:** re-introducing the
  fall-back-and-pass (`case kQueryFailed: if (Alive()) return {};`) in both harnesses made both
  copies' tests fail at the `proven()` assertion (`Value of: agent.proven() Actual: true`), and
  removing it again made both pass.
- **Scope, stated rather than implied: the proof is bring-up-only.** A foreign Agent appearing
  *after* the proof is not caught. Recorded in both READMEs, both file headers and both doc
  comments.

**Duplicated, not shared, and the recorded reason matters.** The ownership block is byte-identical
in both harnesses. The first reason given — that a shared header would need four sparse-checkout
blocks and two path-filter edits — was **checked by the reviewer and is false**: both lanes
already sparse-checkout and path-filter the provider directories. The true reason is package
self-containment: sharing would mean exporting a test-only header from a shipped provider
package. The drift guard is behavioural — each harness carries its own equally-named forcing
test in its own CI lane, so breaking either copy reddens that copy's test — **once those
lanes run, which on this branch they have not.** Both integration lanes are `workflow_call` from
the PR-triggered `ci.pr.yml` and opening the PR is the owner's step, so
`gh run list --branch feature/protocol-driver-abi` is empty: by design, not breakage (compliance
review F4). Until a lane runs, the pairing is checked locally and the Linux `/proc` half is
verified by **local compilation only** — g++ 13.3 under WSL, `-Wall -Wextra` clean, correct
verdicts on six machine states including a dead pid (evidence supplied by the compliance
reviewer, re-run in fix cycle 2 after the `/proc/net/udp6` removal).

## Forcing tests

| Test | Where | What reddens it |
|---|---|---|
| `ConformanceXrce.AForeignAgentDoesNotSatisfyTheHarness` | `pubsub-conformance` | reverting `ProveOwnership()` — the harness certifies a foreign Agent |
| `AForeignAgentDoesNotSatisfyTheHarness` | `fastdds-xrce-interop` | same, on its own fixture |
| `AFailedOwnershipQueryDoesNotSatisfyTheHarness` | both | re-introducing the fall-back-and-pass on the `kQueryFailed` arm: the test forces the query to fail through `g_udp_port_ownership_query` and requires a refusal from a bring-up whose Agent is alive and ours |

## Numbers

Declared: not declared up front (compressed cycle).

**Net diff, which is the figure to quote: +1309 / −94** over `integration-tests/`
(`39512da..worktree`), across four harness files plus two CMakeLists. The **+1263 / −246**
first published here was the SUM of per-commit churn — `4d7d342` +449/−57, `5af2bfb`
+474/−24, `ecc7b2c` +340/−165 — which double-counts every line a later commit rewrote.
Both are defensible; only one was labelled (compliance review RECORD). Fix cycle 2 accounts
for +324 / −126 of that net (test_interop.cpp +145/−59, xrce_main.cpp +132/−55, the two
READMEs +47/−12); its edits to these `plans/` documents are counted nowhere above, since the
net is scoped to `integration-tests/`.

New public surface: **0** — harness-only, no provider, seam or clause change. The one new
non-static symbol is the file-scope `g_udp_port_ownership_query` inside each harness's own
anonymous namespace, which no other translation unit can name.

## Out of scope

No provider change; no delivery-contract or `ProviderConformance` clause change; no weakening,
skipping or deletion of any existing clause; nothing near `integration-tests/gateway-fastdds-ts`
(the only harness reproducing the receive-side data-sharing defect, protected by the 2026-09-01
ruling). No ABI, no config parser, no link-size check. Left as larger than this item: the 1 s
POSIX reap window, job-object / `PR_SET_PDEATHSIG` structural process cleanup, and the unbounded
`waitpid` in `Kill()` — all pre-existing.
