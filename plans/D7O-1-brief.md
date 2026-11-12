# D7O-1 — Stage Brief (2026-11-12)

**In one sentence:** a C++ subscriber that asks for batches of fewer than one row, or for a negative
timeout, now gets an "invalid argument" error straight away and nothing is left running. Before,
it silently got one-row batches, or a timeout of about 1 ms.
**Forcing test:** `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` proves this. Each such
request is refused. The callback it was given is let go before the error reaches the caller, no
transport subscription is made, and nothing is ever delivered to it. A normal subscription on the
same topic afterwards works as usual.
**Guard:** `InRangeBatchOptionsAreUsedAsGiven` proves the other side. Asking for one row per batch
gives one batch per row. A zero timeout is accepted, and its batch still arrives no sooner than 1 ms
after the row.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batched subscribe (C++) | CHANGED: refuses `max_rows` below 1 or a negative timeout with an invalid-argument error | L15 / "Refuse both": refused, never adjusted |
| Batch-options and subscribe documentation, header and README | CHANGED: states the accepted ranges, the refusal, and that a zero timeout means the 1 ms floor | Audit F3–F5, refusal part only |

No new public type, field or method. Stage 2's "no timeout" still has the round's one slot of new
public surface.

## Deleted
- The silent adjustment of fewer than one row per batch up to one row. Replaced by the refusal.
- The silent treatment of a negative timeout as about 1 ms. The behaviour is gone, and the change is
  disclosed above.
- No test is deleted or rewritten. No existing caller or test passes such values (step-0a audit).

## Corner cases forbidden vs handled
**Forbidden** (cannot exist, or refused at the door with one error):
- a batch assembler holding a row limit below 1 or a negative timeout, which no code path can build;
- a refused request that leaves a thread, a transport subscription or the user's callback alive;
- a second place in C++ that decides whether batch options are acceptable. Any future batched
  subscribe goes through the same one check.
- the arithmetic overflow that the most negative timeout used to reach.

**Handled:** none. "One row per batch" and "zero timeout" are ordinary accepted values and get no
special treatment.

## Decisions for you
None. One ordering choice was made without asking. A request that is both out of range *and* made
from a thread where subscribing is not allowed gets "invalid argument" rather than "re-entrant
call". Both are refusals that leave nothing behind, and checking the options first is the simpler
order.

## Risks accepted / debt carried
- Visible change for C++ callers: a request that used to be adjusted now throws. No caller in this
  repository is affected.
- C# is not affected until the C# switch round. That round will see `MaxRows` below 1 refused, as
  the "Refuse both" ruling foresaw.
- Very large row counts and timeouts pass this check, as the law requires. What they *mean* is
  built by stages 2 and 3. Until then they behave as they do today.

## Numbers
Declared net lines: +6 / −1 (production; tests about +110; documentation not counted) · new public
surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
