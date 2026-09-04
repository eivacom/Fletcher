# PDA-DEC-A4 — Stage Brief (2026-09-04, revision 2)

**In one sentence:** cancelling a subscription now really cancels it — once the call
returns, that handler is not running and will not run again — and cancelling something
already gone is quietly accepted instead of raising an error.
**Forcing test:** `CallerTier.NoCallbackAfterUnsubscribeReturns` — proves a handler is
never entered after its cancellation returned, even while another delivery is mid-flight.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Cancelling a subscription (client-facing) | CHANGED | waits for a delivery in progress; guarantees no later delivery — your 2026-09-04 ruling |
| Cancelling an unknown/already-cancelled subscription | CHANGED | was an error, now accepted and does nothing — your 2026-09-04 ruling |
| Contract text (cancellation clause + who inherits which test suites) | CHANGED | says the guarantee applies to the layer other languages wrap, not only the transport layer |

## Deleted
- The published statement that "one final message after cancelling is intentional and by
  design" — no replacement: it was the defect, contradicting the frozen contract.
- The two in-tree tests asserting that late delivery and that error — each replaced by a
  test asserting the opposite.

## Corner cases forbidden vs handled
**Forbidden** (cannot occur, by construction, *at this layer*): a handler entered after
its cancellation returned, or later in the same delivery round after being cancelled
during it; two handlers on the SAME subscriber cancelling each other hanging one
another; teardown raising an error; a handler running after the subscriber is destroyed.
**Handled**, with why not forbidden: (1) cancelling waits while a handler runs — that
*is* the guarantee you ruled for; (2) a handler that never returns blocks it forever —
nothing can bound foreign code; (3) a cancellation issued **from inside a handler** does
not wait, so in that shape the application must not free handler state on return — the
carve-out you approved, published rather than implied; (4) an identifier means something
only to the subscriber that issued it. All four published in the test-suite README.

## Decisions for you
**None.** The two questions the last brief asked, you answered on 2026-09-04, and both
are built as you ruled. The third (how wide to claim the new evidence) is settled by the
narrow-claim preference your 2026-09-03 ruling licensed us to apply without asking.

## Risks accepted / debt carried
- **Published residue, per your 2026-09-04 ruling:** two handlers on *different*
  subscriber objects that cancel each other can still hang. You chose this over the wider
  alternative, on the reasoning that a loud hang beats a silent use-after-free. It is
  written into the test-suite README as a numbered limit, not implied.
- Delivery gets marginally slower: cancelling safely needs a lock the delivery path
  previously avoided. Accepted without measuring it — no mechanism that waits is free.
- The other delivery guarantees stay proven only at the transport layer — published as a
  limit, not implied.
- What a *transport* does when a handler cancels during its own delivery is untouched and
  stays with the separate re-entrancy item; two of three hang there today, as before.
- A transport found to deliver after its own cancellation returns is a stop-and-ask, not a
  local fix.

## Numbers
Declared net lines: +≈360 / −≈45 · new public surface: 0 · design cycles used: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, fix cycles used>.
