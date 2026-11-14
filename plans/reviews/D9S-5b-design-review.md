# D9S-5b design review — cycle 1

**Verdict: NEEDS-REWORK** — 1 BLOCKER (STOP-AND-ASK), 3 DEBT (register, under D9S-5b).
**Proof of read:** `plans/RIX-rulings.md`, 491 lines, read top to bottom. The highest entry is at line 478:
"2026-11-14 — D9S-5: every close ends with one closing delivery; D9S-5 splits". The 2026-11-10/11 L9
rulings and the 2026-11-13 "Accept until stage 6" were applied. Reviewed at `3c7e1d0`.

## B1 — A refused Subscribe can still deliver a CLOSING; the design calls that forbidden (STOP-AND-ASK)
The code contradicts forbidden #2 ("unrepresentable"), the brief's "cannot happen by construction" and Risks'
unverified "practically unreachable". `subscriber.cpp:597-599` publishes the entry before the options-conflict throw
(`:324-325`), and only `:633` retires it, after the unwind. A busy topic's delivery there runs `AddRow`, and
`StopOnThrow`'s `Stop` (`subscriber_arrow.cpp:416-421`) queues a `kClosing` holding that row (any provider).
Outside a delivery, the CLOSING reaches `on_batch` before `FL_INVALID_ARGUMENT` returns. Under 5b's new `binding.h`
("CLOSING is the last use of `ctx`") plus "written only on success", a conforming binding frees `ctx` twice. Inside a row
delivery, `Stop` detaches (`:136-140`) and the CLOSING arrives after the refusal: a use-after-free. Neither is signalled.
*Acceptable fix:* delete the false claims, state this path, and make `binding.h` say what a refused call may have
delivered. Forbidding the case is cheaper (~5 lines: discard an unregistered batcher's window and queue), but it leaves
rows unreported against d7 L1, which is outside the L6 widening. So it is one owner question, not a Risks line.

## Pressure tests (no further findings)
1. **Scope.** In scope. L9's wording edit takes in the same ruling, which itself settles the collision with L9's
   2026-11-11 text. D1 and Findings (c) are records. D18 is the decision of record that d7's header requires for a law
   change. It adds law only if it says more than the ruling, e.g. the registration ordering (DEBT-2).
2. **Throw after rows reached the batcher.** B1: reachable, and verified against the code.
3. **P3 holds.** `Loop` pushes no `DeliveryScope`. On the batch thread, `~fl_subscriber` runs `~SubscriberArrow`, whose
   own `Stop` returns at `loop_id_` (`:136`). Both `~Subscriber`s see `InsideDeliveryOn` false, so the provider's
   `Unsubscribe` is entered, not refused, and that thread holds no gate. The thunk captures only `(on_batch, ctx)`
   (`batches_server.cpp:100`). Nothing terminates and nothing hangs.
4. **Shape.** Settled by L6 and L7; not a principle question. With a decoder, L7 forbids a null ("never 'no rows'").
   Without one, no batch can be built and no schema invented. One point of precision for the owner (DEBT-1): "knows its
   layout" means "has decoded a row" (L13), so a declared topic closed before its first row gets the null shape.
5. **Caps.** Neither document is padded to its cap; the corpus list and the texts to correct are required content. The
   cap shows only where the throw-after-rows case was squeezed into a parenthetical (B1). The brief has 5 lines of room
   left for As-landed.

## Checks
- **7b, priced:** one flag, one setter, one condition, one call site, one comment: about +9 net production lines (about
  +2 more if doc comments are counted). Far under 250, so no rung is needed; tests are excluded.
- **Audit 0a, carried whole:** the 1 delete with its L6 citation; the 7 + 11 re-anchors at the audit's lines, with the
  tolerance re-anchors first; both watches; the stress tightening; every text the audit lists (d7 L6, L9, D1, Findings
  (c), D-row; README; hpp; cpp comment; `binding.h` at both sites; c-abi test comments; d9 L1, L15, §1, §2; BC-5; ladder).
- **Tree claims verified:**
  - The disarm is at `:450`, after `batchers_`.
  - The shim does only noexcept steps after `SubscribeBatches` (`binding.cpp:904-914`).
  - G1 is refused from inside a delivery, so m2 does turn it red.
  - P1's witness asserts a non-null zero-row `kClosing`.
- **Locked decisions:** D-D9S-2, -3 and -4 (the interim) are honoured. D-D9S-8 still lists only the fPIC exception; the
  2026-11-14 widening is not annotated there (DEBT-3).
