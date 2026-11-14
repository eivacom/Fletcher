# D9S-5a — Close from outside, borrow-and-move, options as given (design)

Round D9S, item D9S-5a; rungs 5, 6 (outside close only), 7. Tree `7f5ff65`. Full loop. Read: d9 §1,
§3, §4, §7; d7 L4-L7, L9, L15; register BC-5/8/13/14; locked decisions; 0a audit (classifications
stand); D9S-4 design l.236-241. **Ledger** `plans/RIX-rulings.md` top to bottom: **491 lines**, highest
entry l.478 **"2026-11-14 — D9S-5: every close ends with one closing delivery; D9S-5 splits"**. No
tier-1 disagreement is open.

## Summary

On the server build, `fl_subscriber_unsubscribe_batches` ends one batched subscription by forwarding
to domain 7's `SubscriberArrow::Unsubscribe`. Called from outside a callback, it returns once every
finished batch, then the closing batch, has been delivered (d7 L9). The edge build and the probe shim
refuse it, as they refuse the subscribe. The shim adds no wait, flag or check of its own. Rungs 5 and
7 are already true of the shim; 5a proves them through the ABI, as guards with controls.

## Design

**Surface** (one item; ABI stays 0.8, D-D9S-6): `FL_ABI_EXPORT fl_status
fl_subscriber_unsubscribe_batches(fl_subscriber* subscriber, uint64_t subscription_id, fl_error* err);`
**The header comment says:**
- **Outside any batch callback it BLOCKS.** Every finished batch, then the closing batch with the
  pending rows, arrives on domain 7's thread before it returns. Nothing runs afterwards, so `ctx` may
  be freed then; hold no lock across it that a callback takes.
- **From its own callback, or from a row delivery, it does not wait** (d7 L9). Closing *another*
  subscription inside a callback does wait, so two callbacks closing each other hang (BC-5 res. 1).
- **The idle and in-callback cases are written once, in `fl_subscriber_destroy`'s batched paragraph**
  (`binding.h:867-877`), the one paragraph D9S-5b rewrites (ruling 2026-11-14). 5a therefore writes
  no idle-close sentence that 5b would have to undo.
- **Ids:** bit 63 clear → `FL_INVALID_ARGUMENT`, nothing closed. A tagged id that is not live →
  `FL_OK`, a no-op. Other subscriptions are untouched.
- **Edge build:** `FL_NOT_SUPPORTED`.
- **Two text-only edits, no answer changed (L13):** the subscribe's "only fl_subscriber_destroy
  ends … for now", and `fl_subscriber_unsubscribe`'s batched-id sentence, now name the close.

**The door (`binding.cpp`).** A file-local `RequireServerBuild(entry)` reads `fl_binding_variant()`
(BC-10's single site) and throws `kNotSupported`, naming the edge build and `fletcher-c-abi-server`.
Both batched entry points call it first. It replaces the subscribe's inline refusal, so there is one
refusal site and no `#if`. Then, inside one `Contain(err, FL_ORIGIN_SEAM, …)`:
1. A NULL subscriber → `kInvalidArgument`.
2. Bit 63 clear → `kInvalidArgument` ("not a batched subscription id").
3. Otherwise call `internal::UnsubscribeBatches(*subscriber, id & ~kBatchedIdTag)` (`handles.hpp`).
   The server backend reads the host pointer under `batches_mu` (that read only, never across
   `Unsubscribe` or a callback, L11): no host → return; else `host->arrow.Unsubscribe(raw)`. The edge
   backend throws `kInternal`, unreachable behind the door.

**Step 2 validates an argument; it is not a third L4 answer.** Bit 63 is the shim's own spelling of a
batched id (D9S-4 B1); domain 7 has no such concept. It refuses a malformed C argument as the NULL
checks do, and is no range check on options (BC-8). The alternatives fail: forwarding closes whichever
batched subscription shares the raw number (both counters start at 1); a no-op reports a false close.

**Test-only oracle (rung 7).** Under `variant=server` only, `c_abi_tests` links `fletcher-pubsub-arrow`,
which the recipe already requires. A helper refuses the cell's options through a `SubscriberArrow`
over the test's own `InProcessPubSubProvider`, and returns the refusal's `what()`. It computes the
expected string only; every exercise is `fl_*`. Rung 7 names this oracle. The edge graph is
unchanged (BC-11).

## Corner cases forbidden

**Rung 1, unrepresentable:** the shim choosing when the closing batch comes (no flag, wait or thread;
BC-9 scan); a lock across a close or callback; a moved batch released or an unmoved one kept (the
`release` pointer is the only state); an `fl_schema` in a callback; the edge half-serving (door first).
**Rung 2, refused at the door:** non-server build → `FL_NOT_SUPPORTED`; NULL subscriber or untagged id
→ `FL_INVALID_ARGUMENT`.

**Handled:**
- **H1. A tagged id that is not live → `FL_OK`, a no-op.** *Why not forbidden:* "cancelling what is
  not live is a no-op" is domain 3's rule, forwarded by d7 (`subscriber_arrow.cpp:479-512`). Refusing
  it would answer that rule again (L4), and teardown closes unconditionally.
- **H2. Another subscriber's id may close a same-numbered subscription here.** *Why not forbidden:*
  the seam scopes ids per subscriber, and row ids carry the same obligation (`subscriber.cpp:267`). A
  registry of issued ids would answer a seam question (L4).
- **H3. Until 5b, an idle or in-callback close has no guaranteed closing batch (d7 L6).** *Why not
  here:* ruling 2026-11-14 gives it to 5b. No 5a test asserts it.

## Premises and stop conditions

- **P1.** `inprocess` delivers synchronously inside the publish (`in_process_provider.hpp:24`).
  *If false:* F1 first waits one-sidedly for the full windows, and reports it.
- **P2.** `SubscriberArrow::Unsubscribe` of an unknown id is a no-op. *If it throws:* STOP-AND-ASK,
  and never translate.
- **P3.** Domain 7 refuses with `PubSubError(kInvalidArgument)`, and `Capture` copies `what()`
  unchanged. *If not:* STOP-AND-ASK.
- **P4.** The server tests-on graph exposes `fletcher-pubsub-arrow` to the tests. *If not:* add it for
  server tests only. *If that would touch the edge graph:* STOP.
- **P5.** A topic-option refusal crosses domain 7 unwrapped. *If not:* assert the status only, and
  record a d7 finding.

## Forcing-test mapping

All tests live in `c-abi/tests/test_batches.cpp` and call only `fl_*`. Waits are one-sided, and
every "nothing arrives" window has its control inside the same window. **No test asserts what an
idle close delivers:** subscriptions already idle are torn down without assertion.

| Test | Turns green by | Red before / controls |
|---|---|---|
| **F1** `Batches.ClosingFromOutsideDeliversTheRestThenTheClosingBatch` (server; rung 6) | Runs once with each closer: the new close, then `fl_subscriber_destroy`. With `max_rows` 2 and timeout `INT64_MAX`, publish 5 rows and close at once. **When the call returns**, exactly ROW_LIMIT, ROW_LIMIT, CLOSING (row 5) have arrived, in order. Two more rows published afterwards deliver nothing within 2 s. Close variant only: a second close of the id gives `FL_OK`, and there is still exactly one CLOSING (ruling: "exactly one"); and a second batched subscription, on another topic, still gets a ROW_LIMIT window. | **Red:** no close, so it does not compile. **Built-in control:** 2 rows into a live subscription give ROW_LIMIT. A close that ends the whole host turns the second-subscription arm red. |
| **F2** `Batches.TheBuildAnswersABatchedCloseAsItAnswersTheQuery` (all three artefacts; guard e) | **Edge and probe:** `FL_NOT_SUPPORTED`, `FL_ORIGIN_SEAM`, and a message naming "edge" and `fletcher-c-abi-server`, even for a NULL subscriber. A live row subscription on another topic keeps delivering after the batched close is called with its id, bare and tagged. **Server (control):** with one row pending, `FL_OK` and one CLOSING before return. NULL → `FL_INVALID_ARGUMENT`. The bare raw id → `FL_INVALID_ARGUMENT`, and the row subscription with that number keeps delivering. | **Red:** does not compile. Without the door, the edge answers `FL_INTERNAL`. |
| **G1** `Batches.ABatchIsTheCallersOnlyIfMoved` (server; rung 5; **guard**) | Windows as in F1. W0: both moved and stored. W1: both left in place, wrapped by a counting `release`; the original is moved into a stash the test holds. W2: array moved, schema wrapped. Close, then destroy subscriber and provider, in both orders. On a new thread, import W0 and W2's array, compare them cell for cell, and release them. Each wrapper ran **exactly once**, after its callback returned, on the batch thread. The wrapper's state is the test's, so a second call is counted rather than a use-after-free. | Green on `7f5ff65` apart from compiling the close, hence a **guard**. **Mutation controls, run once and recorded:** delete `ReleaseUnmoved` → count 0. Release unconditionally → a call through NULL, so a crash. Release twice → count 2. |
| **G2** `Batches.EveryBatchOptionReachesDomain7AsGiven` (server; rung 7, BC-8, BC-14; **guard**) | d7's 6 x 7 grid, in `int64_t`. `max_rows`: `INT64_MIN`, −1, 0, 1, 2, `INT64_MAX`. Timeout: `INT64_MIN`, −1, 0, 1, `INT64_MAX`, 100 years, the clock's maximum in ms. One topic per cell, plus a 10 ms control; one row each; observe for 2 s. **Refused (27)** exactly when `max_rows < 1 \|\| timeout < 0`: `FL_INVALID_ARGUMENT` at SEAM, the message byte-equal to the oracle's, OUT sentinels untouched, nothing delivered. **Accepted (15):** `max_rows` 1 gives ROW_LIMIT; timeouts 0 and 1 give TIMEOUT. The 6 cells with no deadline in reach (100 years never fires early, d7 L5; `INT64_MAX` is BC-14) deliver nothing in the window, then exactly one CLOSING with the row when the close returns. A topic option with `max_payload_bytes` 1 gets the row subscribe's status and message. | Green on arrival apart from the close, hence a **guard**. **Timing control:** 10 ms gives TIMEOUT inside the window. **Mutation:** a range check or clamp in the shim breaks the byte-equality, or the "refused exactly when out of range" rule. |

**Guards re-run:** `check_shim_exports.sh` (the new export must be in both builds), `c99_probe.c`,
`check_edge_arrow_free.sh`, and `ShimBatchesNothing.*`.

## Risks / Unknowns

- **No STOP-AND-ASK, no owner decision.** No edge entry point changes its answer, and there is no new
  status, window, timer or range check. Nothing is touched in domain 7, domain 2 or `dotnet/`. The
  one append rides the unreleased 0.8.
- **Step 2 is read as argument validation.** If review reads it as a third L4 exception, it becomes
  an owner decision, and the recommendation is to refuse.
- **Rung 5's observation is substituted.** BC-13 names an allocation-counting pool, but the shim's
  Arrow pool is a hidden copy, reachable only through a shipped seam (L7 forbids one). G1 instead
  counts each struct released exactly once, which is the C Data meaning of "released". An ASan/LSan
  leg, once Linux CI runs, is the stronger proof; it is registered, not built.
- **The oracle against the ground rule** ("`SubscriberArrow` never called by a rung test"): rung 7
  names the oracle, and the exercise stays `fl_*`.
- **The close from a row-delivery thread** (d7 case d) moves to **D9S-5b**, with cases a, b, c and e.
- **Files:** 5 production files against the 2-3 estimated. The two-backend pattern needs
  `handles.hpp` and both backends; the lines are within the estimate.
- **G2's cost:** about 3-5 s and 15 batch threads, under the 30 s TIMEOUT.

## Declared lines (raw diff)

- **Production: +80 / −9; about +52 / −6 counted; about 89 lines touched.**
  - `binding.h`: +4 code, plus doc +24 / −4 (not counted).
  - `binding.cpp` +24 / −5, `handles.hpp` +5, `batches_server.cpp` +15, `batches_edge.cpp` +6.
- **Tests: +490 / −3** (`test_batches.cpp` about +480 / −3, `tests/CMakeLists.txt` +10).
- **Docs, not counted:** about +30 / −20.

## Files-to-touch

- **Production**, under `c-abi/`: `include/fletcher/abi/binding.h`, `src/binding.cpp`,
  `src/handles.hpp`, `src/batches_server.cpp`, `src/batches_edge.cpp`.
- **Tests:** `c-abi/tests/test_batches.cpp`, `c-abi/tests/CMakeLists.txt` (the oracle link).
- **Docs:**
  - d9: L12, L13 and §4 (the close); L14 and L15 tests `[built]`; Q7.
  - Register: the BC-8, BC-13 and BC-14 shim halves `[built]`; BC-5 outside `[built]`, the in-callback
    half left to 5b.
  - `c-abi/README.md`.

## Files-to-delete

None. 5a builds D9S-4's designed close and proves laws the code already meets; nothing is
superseded. The header's "only fl_subscriber_destroy ends … for now" is edited in place, with no
coexistence window.
