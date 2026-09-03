# PDA-DEC-A4 — Stage Brief (2026-09-03)

**In one sentence:** cancelling a subscription now really cancels it — once the call
returns, that handler is not running and will not run again — and cancelling something
already gone is quietly accepted instead of raising an error.
**Forcing test:** `CallerTier.NoCallbackAfterUnsubscribeReturns` — proves a handler is
never entered after its cancellation returned, even while another delivery is mid-flight.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Cancelling a subscription (client-facing) | CHANGED | now waits for any delivery in progress; guarantees no later delivery |
| Cancelling an unknown/already-cancelled subscription | CHANGED | was an error, becomes accepted and does nothing |
| Contract text (the cancellation clause + who inherits which test suites) | CHANGED | says the guarantee applies to the layer other languages wrap, not only the transport layer |

## Deleted
- The published statement that "one final message after cancelling is intentional and by
  design" — no replacement: it was the defect, contradicting the frozen contract.
- The two in-tree tests asserting that late delivery and that error — each replaced by a
  test asserting the opposite.

## Corner cases forbidden vs handled
**Forbidden** (cannot occur, by construction): a handler entered after its cancellation
returned, or later in the *same* delivery round after being cancelled during it; a
cancellation deadlocking against the handler that issued it; teardown raising an error; a
handler running after the subscriber is destroyed.
**Handled**, with why not forbidden: (1) cancellation waits while a handler runs — the
contract *is* that wait; (2) a handler that never returns blocks it forever — nothing can
bound foreign code, and the transport layer already carries that exposure; (3) an
identifier means something only to the subscriber that issued it — global uniqueness needs
process-wide state your isolation ruling keeps out. All three published in the README.

## Decisions for you   (3)
1. **Should cancelling a subscription that is already gone be an error, or accepted silently?**
   Options: (a) accepted, does nothing · (b) keeps raising an error
   **Recommendation:** (a) — a C#/Rust cleanup path calls it unconditionally and a finaliser
   cannot let an error escape. Cost: a typo'd identifier is ignored. **Default:** (a)
   *Background (skippable): the tiers disagree — `provider.hpp` says no-op, `subscriber.cpp` throws `kInvalidArgument`.*
2. **Should cancelling wait for a delivery already in progress?**
   Options: (a) wait, so the caller may free its handler state the moment it returns ·
   (b) return immediately, requiring every client to keep handler state alive indefinitely
   **Recommendation:** (a) — (b) is what causes the crash class this item exists to remove.
   Cost: shutdown can pause as long as the last handler takes. **Default:** (a)
3. **How much of the contract do we prove at the layer the language bindings wrap?**
   Options: (a) this guarantee and cancellation only, remaining gap published ·
   (b) the whole delivery contract re-run at that layer now
   **Recommendation:** (a) — (b) is a stage of its own and would likely uncover more
   divergences to fix mid-round; matches four prior narrow-claim rulings. **Default:** (a)

## Risks accepted / debt carried
- The other delivery clauses stay proven only at the transport layer — published as a limit, not implied.
- The contract's blind-spot list is *not* edited: outside this item's authorisation; raised to you if review disagrees.
- A transport found to deliver after its own cancellation returns is a stop-and-ask, not a local fix.

## Numbers
Declared net lines: +≈410 / −≈45 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
