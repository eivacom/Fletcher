# PDA-DEC-AG1 — Stage Brief (2026-09-05, revision 2)

**In one sentence:** when a subscriber's message handler misbehaves — by failing, or by
calling back into Fletcher from inside itself — all three protocols now do the same thing:
publishing, declaring and subscribing from inside a handler work everywhere, cancelling
from inside one is refused by name, and neither failure is blamed on another party.
**Forcing test:** `ProviderConformance.HostileCallbackNeitherEscapesNorIsChargedElsewhere`
— one handler that both fails *and* calls back in: the call-back-in is refused with a
distinct named error, the failure is contained, and the publisher on the other side
completes normally. All three protocols, in-process and across processes.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| A distinct "you called back in from inside my own callback" error | NEW | You allocated it 2026-09-03; tells that apart from "this protocol can't do it at all" |
| The sealed delivery handle a protocol uses to reach a handler | NEW | A handler failure cannot leak out, and no protocol can opt out |
| What the in-process protocol allows from inside a handler | CHANGED | Publish / declare / subscribe start working there, as they already do on both DDS protocols |

## Deleted
- Three protocol-local "catch whatever the handler threw" blocks — one shared behaviour replaces them.
- The in-process protocol's deliberate deadlock (and the two comments documenting it) — replaced by an ordered hand-off, so a handler can publish without freezing.
- The XRCE test pinning "cancelling from inside a handler works" — behaviour deliberately changed; replaced in place by one asserting the refusal. And the shutdown path's silent swallow of that refusal: it now stops the program instead of leaking a subscription with no signal.

## Corner cases forbidden vs handled
**Forbidden:** a subscriber's failure surfacing as a *publisher's* error (today it can
arrive as a bogus "payload too large"); an unwind through a transport's C code, which today
ends the process; any protocol shipping a private answer to either question; a handler
entered from inside another handler; tearing down a subscriber from inside a delivery on
the same protocol instance (your 2026-09-05 ruling, now a stated rule).
**Handled:** an application may catch and ignore the refusal handed to it — the seam cannot
force anyone to look at an error. A handler that never returns still stalls its own
delivery thread; that limit is already published and is not widened here.

**Already answered by you (2026-09-05), not re-asked:** only cancelling is refused from
inside a handler, with publish / declare / subscribe made to work everywhere; and tearing
down a subscriber from inside a delivery is forbidden, and said so.

## Decisions for you   (2)
1. **A handler cancels its own last subscription; the transport subscription cannot be closed from inside it. What then?**
   Options: (a) leave it open and silent until the subscriber object goes away or the topic is used again, and publish that · (b) close it later on a thread we create
   **Recommendation, and default if unanswered:** (a) — no handler runs either way, so nothing is unsafe; (b) invents a background thread the seam does not have.
2. **What does a publisher learn when a *subscriber's* handler fails?**
   Options: (a) nothing — the publish succeeds and the failure is contained where it happened, counted so a test can see it · (b) the publisher is told
   **Recommendation, and default if unanswered:** (a) — it is not the publisher's failure, and (b) is a new contract for a party that cannot act on it.
   *Background: today a subscriber's overflow reaches an unrelated publisher's Publish, and on XRCE a publisher's CreateTopic.*

## Risks accepted / debt carried
- A program that tears down a subscriber from inside a delivery now **stops with a named error** rather than hanging (two protocols) or leaking silently. That is the loud-over-silent trade you have chosen repeatedly, but it is a behaviour change for anyone doing it today.
- Making publish-from-a-handler work in-process means reworking the in-process delivery path — code close to what needed four fix cycles in an earlier item. A second defect of the same kind there is the stop condition you set on 2026-09-04.
- The item roughly doubled against revision 1 (+520 → +1050). If it overruns again, it splits at a named threshold into (i) everything but the two DDS protocols, (ii) the DDS protocols — never between the two failure kinds, which would let one mask the other.
- A future third-party protocol could still bypass the shared handle by keeping its own copy of the handler; closing that needs a method-set change the round makes a stop-and-ask, so the runtime-driver round closes it in one shared adapter instead. Separately, deleting the per-protocol catch blocks removes the only log line a failing handler produces today, and there is no logger in that layer, so a counter a test can read replaces it.

## Numbers
Declared net lines: +1050 / −150 · new public surface: 2 (of 3) · design cycles used: 2/2

---
*As landed — 2026-09-05, `61e6dc7`:*
**+2028 / −243** vs declared +1050/−150 — **93% over the declared adds**, a close finding owed a note: both owner decisions landed as recommended, but ruling 52 widened the refusal from `Unsubscribe` to all four methods on all three providers after the numbers were declared, and 12 doors cost what 3 were budgeted for. Public surface **3** (declared 2): `AbsorbedCallbackFailures()`, the `DeliveryChannel` ctor pair, `RawToken`. Retired: the process-wide counter, the base-cast normative paragraph, the ruling-48 delivery gate. Design cycles **2/2** · fix cycles **3** · implementer launches **≥4** (one interrupted launch is not in the PM's dispatch record). Debt out: AG1-DEBT-20, -21 → PDA-ABI.
