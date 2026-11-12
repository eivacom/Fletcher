# D7O-2 — Stage Brief (2026-11-12)

**In one sentence:** a C++ subscriber can ask for "no timeout". Full batches then arrive at once,
and whatever is left arrives when it unsubscribes, never on a clock. A timeout too long for the
clock to measure means the same thing. Before, such a timeout produced a batch about 1 ms after the
first row.
**Forcing tests:** `NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing` proves that full batches arrive
and nothing else arrives while the subscriber waits. Unsubscribing, or destroying the subscriber,
then delivers the rest, and any rows lost meanwhile are reported in that last batch.
`TimeoutTooLongForTheClockBehavesAsNoTimeout` proves that the largest timeouts behave exactly as
"no timeout" does, never early. **Control:** the same scenarios with a 10 ms timeout do get their
timed batch, which shows the "nothing arrives" checks can fail.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batch options (C++): a named "no timeout" value | NEW | Ruling "Add 'never' to C++"; the round's one new item |
| Batched subscribe with a huge timeout | CHANGED: no deadline, never an early one | L5 (confirmed "Keep it") |
| Header and README | CHANGED: what "no timeout" does; losses are then reported at unsubscribe | Audit F2–F5 |

"No timeout" is the largest timeout value, given a name. A C caller passes it as an ordinary 64-bit
number, so the shim round can pass it straight through.

## Deleted
The early batch that very large timeouts produced (behaviour gone, disclosed), and one stale note
in the domain document. No code file, type or test is deleted.

## Corner cases forbidden vs handled
**Forbidden:**
- "no timeout" together with "5 seconds": there is only one timeout setting;
- "no timeout" as a negative number (refused, L15) or as zero (zero keeps its "about 1 ms" meaning);
- a batch that arrives earlier than its timeout says;
- a second place that judges the options: that check is unchanged.

**Handled:** a timeout of nearly 300 years may give a deadline in one window and none in a later
one. Refusing it would refuse a value the law says is used as given; no user can tell the two apart.

## Decisions for you
Made without asking: "no timeout" is the largest timeout value, not a separate on/off switch. A
switch would allow "no timeout, 5 seconds", which needs a second refusal or a silent winner.
1. **May the domain's definition of "deadline" say that some windows have none?**
   (a) Amend it: "…for a timeout the clock can reach; otherwise none (L5)". Users: no change, and the
   document matches what they get. Architecture: agrees with L5; closes audit F1, as the round's
   definition of done requires. · (b) Leave it. Users: no change, but the text claims every waiting
   batch has a deadline, which is false. Architecture: contradicts L5; F1 stays open.
   **Recommendation:** (a), because L5 governs. **Default if you don't answer:** (b), with F1
   reported open at round close. *Background: d7 §2, "Deadline".*

## Risks accepted / debt carried
- A no-timeout subscription that never gets a usable schema reports its losses only at unsubscribe
  (ruled, D-D7O-5; disclosed).
- C# writes "infinite" as −1 ms, which C++ refuses: the C# switch round translates it. No C# change.

## Numbers
Declared net lines: +10 / −2 (production; tests about +170; documentation not counted) · new public
surface: 1 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
