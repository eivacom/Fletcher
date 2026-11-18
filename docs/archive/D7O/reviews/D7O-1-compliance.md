# D7O-1 — conformance review (step 4a), cycle 1

**Verdict: CONFORMS — ISSUES(1, 0 blocking).** Diff `555d899..9abb0c7`, against
`plans/D7O-1-refuse-bad-options.md` (approved at `555d899`), D-D7O-1..6, archreview DEBT-1.
**Proof-of-read:** `plans/RIX-rulings.md` was read top to bottom: **285 lines, 23 entries, last heading
"2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows"**. The diff contradicts no ruling. No
tier-1 collision, tier-2-vs-tier-1 conflict or principle-vs-detail conflict needs the owner.

## Verified conformant (do not re-derive)
- **One predicate, one throw** in `RecordBatchBatcher`'s constructor (`cpp:40-46`). The clamp is gone.
  A grep finds no other clamp or range check on batch options in C++. `schema_arrival.cpp:67` is
  domain 3's `Wait`, which answers a different question.
- **One construction site** (`cpp:380`), before `Start` (`:382`) and `subscriber_->Subscribe` (`:392`).
  A refusal starts no thread and subscribes nothing. Nothing writes `max_rows_` or `timeout_` after
  construction. The refusal ends the call: it never becomes a recovery path.
- **Production +11/−2 vs the declared +6/−1:** `#include <string>` for `std::to_string`, plus a 3-line
  message that names both rules and both values, as design line 36 requires. The −2 comes from the
  split `provider_(provider) {}` line. No construct beyond the design. Conformant.
- **Files-to-touch match exactly** (cpp, hpp, README, test, d7, register), plus the 0b audit record.
  `c-abi/` and `dotnet/` are untouched (D-D7O-6, premise 5). No public type, method or enum changed.
- **Tests:** the arms, the `weak_ptr` token checked straight after `catch`, the counter going 0→1, the
  200 ms one-sided window, the positive control and both guard arms are as designed. Only the status
  is asserted. DEBT-1 is folded in: on the red path only the `milliseconds::min()` arm is
  unsubscribed, after its `weak_ptr` check, and the other arms stay live. The test diff is +165/−0,
  so nothing was retired or re-anchored. This matches the design's "none".
- **Docs:** hpp F3/F4 match the design text. README F5 is there. d7 L4's "Until D7O" note and L15's
  "Today" line are deleted. §(c)'s bullet is struck. §4's sentence is kept. BC-8's single-site text,
  the clamp copy marked deleted, and the status "check `[built]` (D7O-1); sweep owed by D7O-3" match
  design lines 89-92 and archreview (a)-(c).
- **Build-state marks:** d7 L15's `[built]` is limited to the refusal, and huge values stay
  `[designed-not-built]`. BC-8 claims only the check; the sweep is still owed. **Neither claims the
  huge-value part or the sweep.** One unmarked clause remains (RECORD 1).

## N1 (non-blocking): the header says "used as given" for values the tree still mishandles
- `hpp:110` (design line 82, word for word) says "Any other value is used as given." On `9abb0c7`
  that is false for two values the check accepts:
  - `max_rows = INT64_MAX`: `Reserve` throws in `SetSchema` (`cpp:79-84`), the decoder is reset, and
    every row is dropped for the life of the subscription.
  - `timeout = milliseconds::max()`: it wraps at `cpp:196` into a `kTimeout` about 1 ms after the row
    (ladder rung 3).
- Both are what D7O-2 and D7O-3 fix. The design's own rule (line 66: "neither refuses them nor changes
  their behaviour") and d7's L15 mark are accurate. Only this public sentence describes the target
  behaviour as if it already holds.
- It is not blocking because the round's definition of done keeps the branch off `main` until D7O-3.
  **Fix (PM's choice):** (a) gate: D7O-1 is never merged or cherry-picked to `main` on its own, and
  D7O-3's close confirms the sentence is then true; or (b) qualify the sentence now ("very large
  values: D7O-2/D7O-3") and remove the qualifier at D7O-3.

## RECORD
- d7 L15's mark says nothing about its binding clause ("a binding passes the refusal through"). C#
  still answers the question itself (BC-8 copies, `SubscriberArrow.cs:228-237`) until the switch
  round. Mark that clause `[designed-not-built]`, owned by the C# switch round.
- d7's header (lines 3-6) still says the L15 parts are `[designed-not-built]` until D7O. L15's refusal
  is now built.
- d7 L15's "Tests:" line says "rungs 1-2" (the design's forbidden-case rungs). In d7, "rung" reads as
  the D7O test ladder, whose rung 2 (no timeout) is not built. Write "rung 1".
- d7 L15 cites `cpp:33-45`; the constructor is at `cpp:34-47`. This edit added about 9 lines, so the
  L4/L5 "Holds at" citations (`cpp:108`, `92-94`, `184-191`, ...) are now about 9 lines off.
- DEBT-1 says "record the counter value actually observed" on the red run. That value is recorded
  nowhere in the tree: the evidence ledger is empty and the 0b audit does not mention it. The test
  prints it to stderr instead. Whether that stderr line should stay is step 4b's call.
