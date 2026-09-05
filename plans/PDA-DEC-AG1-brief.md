# PDA-DEC-AG1 — Stage Brief (2026-09-05)

**In one sentence:** when a subscriber's message handler misbehaves — by failing, or by
calling back into Fletcher from inside itself — all three protocols now do the same
thing, and neither failure is ever blamed on another party.
**Forcing test:** `ProviderConformance.HostileCallbackNeitherEscapesNorIsChargedElsewhere`
— one handler that both fails *and* calls back in: the call-back-in is refused with a
distinct named error, the failure is contained where it happened, and the publisher on
the other side completes normally. All three protocols, in-process and across processes.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| A distinct "you called back in from inside my own callback" error | NEW | You allocated it 2026-09-03; tells that apart from "this protocol can't do it at all" |
| The sealed delivery handle a protocol uses to reach a handler | NEW | A handler failure cannot leak out, and no protocol can opt out |
| The wrapper every protocol entry point already uses | CHANGED | Now also refuses a call made from inside a delivery, before any lock is taken |

## Deleted
- Three protocol-local "catch whatever the handler threw" blocks — one shared behaviour replaces them.
- The old spelling of the entry-point wrapper — no compatibility form, so an unconverted site fails the build.
- The XRCE test pinning "cancelling from inside a handler works" — behaviour deliberately changed; replaced in place by one asserting the refusal.
- Two loopback comments documenting a deliberate deadlock, which stops being true.

## Corner cases forbidden vs handled
**Forbidden:** a subscriber's failure surfacing as a *publisher's* error (today it can
arrive as a bogus "payload too large"); an unwind through a transport's C code, which
today ends the process; any protocol shipping a private answer to either question; the
new refusal escaping Fletcher's own shutdown path; any "sometimes allowed" middle ground.
**Handled:** an application may catch and ignore the refusal handed to it — the seam
cannot force anyone to look at an error. A handler that never returns still stalls its
own delivery thread; that limit is already published and is not widened here.

## Decisions for you   (3)
1. **A handler calling back into Fletcher from inside itself — always refused, or allowed where a protocol can manage it?**
   Options: (a) always refused, identically everywhere · (b) each protocol allows what it safely can
   **Recommendation:** (a) — one rule a language binding can reproduce; (b) is the three-way disagreement this item exists to end.
   **Default if unanswered:** (a). Cost: an XRCE application that cancels from inside its handler stops working — loudly, with a named error.
   *Background: XRCE serves that call today via a recursive lock; the loopback deadlocks; Fast DDS does one of each.*
2. **A handler cancels its own last subscription; the transport subscription cannot be closed from inside it. What then?**
   Options: (a) leave it open and silent until the subscriber object goes away or the topic is used again, and publish that · (b) close it later on a thread we create
   **Recommendation:** (a) — no callbacks run either way, so nothing is unsafe; (b) invents a background thread the seam does not have.
   **Default if unanswered:** (a).
3. **What does a publisher learn when a *subscriber's* handler fails?**
   Options: (a) nothing — the publish succeeds; the failure is contained and logged where it happened · (b) the publisher is told
   **Recommendation:** (a) — it is not the publisher's failure, and (b) is a new contract for a party that cannot act on it.
   **Default if unanswered:** (a).
   *Background: today a subscriber's overflow reaches an unrelated publisher's Publish, and on XRCE a publisher's CreateTopic.*

## Risks accepted / debt carried
- A future third-party protocol could still bypass the shared handle by keeping its own copy of the handler; closing that needs a method-set change the round makes a stop-and-ask, so the runtime-driver round closes it in one shared adapter instead.
- Prior amendment items landed at 1.7–4x estimate. If the DDS conversions overrun, split (i) everything but the DDS protocols, (ii) the DDS protocols — never between the two failure kinds, which would let one mask the other.
- The XRCE recursive lock stays; the refusal removes its last external justification, but changing it re-enters code that already needed four fix cycles.

## Numbers
Declared net lines: +520 / −100 · new public surface: 2 (of 3) · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
