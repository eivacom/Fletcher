# D9S-5a — code review (c7220e2..ea49aa3)

**Verdict: APPROVE.** 0 blocking · 1 should-fix · 2 nits. Read only, not built.

## Checked and clean
- **Lock scope.** `subscriber.batches` is set once, under the lock, and is reset only by `~fl_subscriber`. The raw `host` read under `batches_mu` therefore stays valid for as long as the handle does. A close racing a destroy *from another thread* breaks the quiescence the destroy already requires, so it is not the shim's problem. A close from any thread other than the subscribing one is safe: the host is stable and d7 `Unsubscribe` takes its own `mu_`.
- **No host:** FL_OK with no effect, which matches H1. **Bit 63:** checked after the door and the NULL check, then stripped. Domain 7's ids live in their own `Subscriber`, so a tagged id can never reach a row subscription. **Contain:** d7 throws only `PubSubError`, and the catch-all maps anything else.
- **G1 against the C Data Interface:** "Move" is copy-then-NULL `release`. "Leave" moves the original into the test's stash, then installs a fake whose `release` only counts and sets NULL, which the spec requires. The children stay owned by the stash, and the stash is released once (import, or by hand for W1). The happens-before for the non-atomic fields comes from the blocking close and d7's join. Correct.
- **Waits:** F1's two 2 s windows (about 4 s in total) check that nothing arrives, so they cannot be shorter. Each window's control is checked inside the same window. F2's row wait is one-sided.
- **Linking:** domain 7 is linked only under `variant=server`, and only into `c_abi_tests`. The probe binary is pinned to edge (1) and the edge graph is untouched.

## Should-fix
**S1 — A close made inside a callback while the subscriber is being destroyed reads `batches` mid-destruction.** Confidence: medium.
How it happens: thread A calls `fl_subscriber_destroy` from outside. `~shared_ptr<void>` then runs `~SubscriberArrow`, which delivers the queued batches. Meanwhile a callback calls `fl_subscriber_unsubscribe_batches` on its own id. `UnsubscribeBatches` locks `batches_mu`, which is still alive, and calls `.get()` on a `shared_ptr` whose destructor has already started. It then calls `Unsubscribe` on a `SubscriberArrow` that is partway through its own destructor.
This works today only because neither MSVC nor libstdc++ clears `_Ptr`, and because d7 deliberately snapshots `batchers_` (subscriber_arrow.cpp:343). Formally it is use after the object's lifetime has ended, and nothing in the header rules the pattern out.
**Fix:** make this state unreachable. Give `fl_subscriber` a destructor that moves `batches` out under `batches_mu`, then resets it outside the lock: `std::shared_ptr<void> b; { lock_guard l(batches_mu); b = std::move(batches); } b.reset();`. This still runs before `subscriber` is destroyed. A close made during teardown then takes the existing no-host path (FL_OK), and the destructor closes everything anyway.

## Nits
- G2 checks its positive cells (ROW_LIMIT, TIMEOUT, control) after a fixed 2 s sleep rather than a condition wait. Fix: wait one-sidedly on each positive sink first, then sleep only for whatever is left of the window before checking the negative cells.
- G1's provider-order loop adds no assertion: every unmoved struct is already released inside the callback frame during the close, before any destroy. Fix: keep one order, or say in the doc comment that the loop is a smoke test.

## Conformance
It matches the brief: door, NULL, bit 63, forward, no-host return; oracle linked for server only; F1/F2/G1/G2 as mapped. F1 extends its control to the destroy variant (DEBT-2). Nothing the brief said would go has survived.

## RECORD:
- tests/CMakeLists.txt TIMEOUT comment says "the longest measured row takes about 2 s"; F1 now takes about 4 s and G2 more than 2 s.
- The binding.h close paragraph says the closing batch is delivered with no condition attached. For an idle window none is delivered until 5b (H3). It should point to the `fl_subscriber_destroy` paragraph for the idle case, as it already does for the in-callback case.

## Cycle 2 (1a52718..4cb0144)

**Verdict: APPROVE.** 0 blocking · 0 should-fix · 1 nit. Read only, not built.

### S1: closed
- **Order.** `~fl_subscriber`'s body runs before any member is destroyed, so the host goes while `subscriber`, `provider` and `batches_mu` are all alive. That is the same order the member declaration gave before, now explicit. When the body ends, `batches` is empty, and its implicit destructor does nothing.
- **No deadlock.** The lock covers only the move. `host.reset()` runs after the `lock_guard` scope has ended. When `~SubscriberArrow` joins a batch thread whose callback calls the close, that close takes `batches_mu` (now free), reads null and returns FL_OK. It never re-enters the host that is mid-destruction. d7's own destructor then closes that subscription anyway, so nothing is lost.
- **Race.** `UnsubscribeBatches` reads the raw pointer under the same mutex. Either it reads before the move, and the host is still fully alive because the reset comes later on the destroying thread, or it reads null. It can no longer observe a `shared_ptr` that is mid-destruction. (Taking the pointer before the move and calling `Unsubscribe` after `reset` would still need a racing close from a non-callback thread, which breaks destroy's quiescence rule. That is unchanged from cycle 1 and not the shim's problem.)

### G2: as asked
The positive-cell predicate (`max_rows >= 1 && timeout >= 0 && (max_rows == 1 || timeout <= 1)`) matches the ROW_LIMIT and TIMEOUT assertion branches exactly. A negative `left()` makes `wait_for` test the predicate and return, which is harmless. `sleep_until(window_end)` keeps the 2 s that the negative cells need.

### Record edits: each new sentence is true
- binding.h close: the conditional CLOSING matches today's d7 L6, and destroy's paragraph (l.867-870) does say "an idle one delivers nothing".
- d9 §2 / §4 "50": the script's `^FL_ABI_EXPORT` + `sort -u` method gives 50. The raw `grep -c` gives 54 because it also counts the four `#define` lines, so naming the script is the right citation.
- d9 "or idle" (§2 row, L15) and BC-5 "and the idle close": these match ruling 2026-11-14 (RIX-rulings l.478) and the D9S-plan 5b row.
- d9 §4 source line: "tier 3; no owner ruling exists" is true (design review "Rulings asked for": "No owner decision is needed"). The dropped §1 citation was the wrong one (§1 l.29 lists no subscription ids), and D9S-4 B1 remains.
- CMakeLists: "about 4 s (F1 …; G2 just over 2 s)" is consistent with both tests.

### Nit
- (medium) A *subscribe* from a batch callback during destroy now finds `batches` null and builds a second host. The join gives happens-before, so the member destructor reaps it after the body, while the provider is still alive: clean, but caller misuse that the door could refuse if 5b wants it explicit.
