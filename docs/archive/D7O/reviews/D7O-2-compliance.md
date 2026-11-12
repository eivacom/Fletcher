# D7O-2 — conformance review (step 4a), cycle 1

**Verdict: ISSUES(2, 1 blocking).** Diff `920b3ec..7a0cc64` (working tree = `7a0cc64`, clean) against
`plans/D7O-2-no-timeout.md` (approved `53a37da`, APPROVE-WITH-DEBT(2)), the brief, D-D7O-1..6, and
the owner's Decision 1 ruling (`920b3ec`).
**Proof-of-read:** `plans/RIX-rulings.md` was read top to bottom: **295 lines, 24 entries, last heading
"2026-11-12 — D7O-2: the deadline definition says some windows have none"**. The diff contradicts no
ruling. No tier-1 collision, no tier-2-vs-tier-1 conflict, and no principle-vs-detail conflict needs the owner.

## B1 (blocking): the README's F2 disclosure is wider than the design, and it misstates L1
- `pubsub-arrow/README.md:75-77` says: "Rows lost for want of a usable schema (or because they are
  malformed) never count toward `max_rows`, so with no timeout **they** are reported only in that
  last batch."
- That is false whenever good rows follow in the same window and fill it. A malformed row, then
  `max_rows` good rows, gives a `kRowLimit` batch with `rows_dropped` 1 (`TryAppend` adds to `dropped_`,
  and `Close` hands it over). The same holds for pre-schema drops when the schema arrives mid-window
  (`SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops`).
- The design (README = "the same, in user terms" as the header, F2), the header (`hpp:115-117`) and
  d7 L1/L6 all scope the rule to **a window of only lost rows**. The README drops that scope. It
  tells users where L1's loss report appears, and gets it wrong.
- **Fix (one sentence, docs only):** "A window holding only lost rows (no usable schema yet, or
  malformed rows) never reaches `max_rows`, so with no timeout it is reported only in that last batch."

## N1 (non-blocking): the header's "used as given" sentence was narrowed beyond the design
- `hpp:121` changes "Any other **value** is used as given" to "Any other **timeout** is used as given".
- The design (Docs, `hpp:110`) kept the sentence: it "becomes true for every timeout … stays untrue
  for `max_rows` until D7O-3" (D7O-1 4a N1, logged, branch off `main`).
- The edit is honest about what is built. But it stops stating L15 ("Every value it accepts is used
  as given") for `max_rows`. D7O-3's ladder lists no header docs (rung 4: "Docs: none beyond d7
  L15's 'Holds at'"), so the narrowed text would survive the round.
- **Fix:** either revert to "value", as the design says, or record "restore 'Any other value is used
  as given' in `hpp`" in D7O-3's Files-to-touch. The PM chooses. This does not gate D7O-2.

## Verified conformant (rely on these)
- **Production code.** `ArmTimer` (`cpp:199-213`) matches the sketch:
  - `static_assert(ratio_less_equal_v<period, milli>)` is not weakened (premise 1). The optional
    `ratio_less_v` (DEBT-2(c)) was not taken, which is allowed.
  - `constexpr kReach`, then `timeout_ > kReach`, then `last_flush_.time_since_epoch() > max − window`.
  - The sum `last_flush_ + window` is formed only after both comparisons pass.
  - `now + kMinWindow` is unguarded, as premise 2 says.
  - Nothing else changed: `<ratio>` and the comment are the only other additions.
- **What survived that should not: nothing.**
  - The overflowing `last_flush_ + timeout_` is gone. A grep finds no other deadline sum in C++.
  - `timeout_` is read only by the constructor's check and message (`cpp:42,46`) and by `ArmTimer`.
  - There is **one range check**: the constructor's `timeout_ < zero()`, untouched (D-D7O-4).
    `timeout_ > kReach` answers BC-4's per-window question, not "acceptable?" (archreview Point 1).
  - There is **one "no deadline" state**, `has_deadline_ == false`. Its readers are `AddRow`
    (`cpp:102`) and `Loop` (`cpp:271`). There is no flag, enum or second idle path.
  - d7 L5's "Until D7O … overflows" note is deleted. No "until D7O-2" claim survives in docs,
    headers or the README.
- **D-D7O-1 holds.** `kNoTimeout = milliseconds::max()`, a `static constexpr` member. `BatchOptions`
  is still an aggregate, so the README's designated-initialiser example compiles in form. Its
  `count()` is INT64_MAX as a 64-bit `rep` on MSVC, libstdc++ and libc++, so a C caller passes a plain
  `int64_t`. Rung 3's first arm (`milliseconds::max()` written as a literal) pins that the C form
  behaves as no timeout. `c-abi/binding.cpp:943` already relies on the same identity.
- **d7 §2 uses the ruled wording verbatim:** "…, for a timeout the clock can reach; otherwise none
  (L5)". The `(cpp:176)` citation is replaced by `` (`ArmTimer`) ``. The design's longer draft was
  correctly not used.
- **Build-state marks are scoped to what is built.**
  - The d7 header and L4/L5 mark no timeout and the unrepresentable case `[built]`.
  - L15 marks "used as given" `[built]` **for timeouts only**, with a huge `max_rows`
    `[designed-not-built]` until D7O-3.
  - BC-8's status is unchanged: check `[built]`, sweep owed by D7O-3.
  - No mark claims rung 4.
- **Tests match the design.**
  - Rung 2 has 4 arms: (a) ended by Unsubscribe, (a) ended by the destructor, (b), (c).
  - Rung 3 has 3 arms, the 100-year arm declared a GUARD.
  - The control obeys DEBT-1: a `kTimeout` exists, its content is ≥1 row or ≥1 drop, and rows are
    conserved after Unsubscribe. No row identity is asserted.
  - All use Mock, a 200 ms one-sided window, and sinks declared first. Tests are driven from
    outside only, so the batcher is not reached.
  - No existing test was retired or re-anchored, and the finite stand-ins stay (design: "none").
- **Files-to-touch match:** cpp, hpp, README, test, d7, register (BC-4 only), plus the 0b record.
  `c-abi/` and `dotnet/` are untouched (premise 6, D-D7O-6). `BatchStatus`, its reasons, the callback
  and the per-row `Subscribe` are unchanged.
- **F1–F5 are closed in substance.**
  - F1: §2 is amended under the ruling.
  - F3 and F4: the hpp text matches the design.
  - F5: the README is updated, with the example line. The B1 wording still needs fixing.
  - F2: disclosed in the header, L1 and L6.
- **DEBT-2(a) is done.** BC-4 names `Close` and `ArmTimer`.

## RECORD:
- d7 L5 Tests: the claim that the 100-year arm shows "a timeout the clock can reach keeps its
  deadline" is untestable. The arm observes only "accepted, never early". Reword it.
- d7 L15 Tests: no D7O-2 test is cited beside the new "`[built]` for timeouts". Rung 3 and its
  guard arm belong there. §(c) points to "tests under L5".
- Register BC-8: its question "…and what do they mean?" still names only the constructor. The
  timeout's meaning (deadline or none) is now answered at BC-4's `ArmTimer` (design § "Where 'what
  the timeout means' is answered"). Narrow the question to "acceptable?" or cross-reference BC-4.
- Register BC-4 Status column: it has no D7O-2 entry. Only the site and test columns changed.
- d7 Mode line: it does not record D7O-2's shift of about 12 lines after `cpp:193`. L4's
  `cpp:251-252` and L5's `cpp:200`/`cpp:285` no longer point at the deadline or `Close`.
- Ladder line 71 ("stage 1's range check is extended (not replaced) by stage 2") is superseded. The
  design adds no extension.
- The pre-stage red observations are not yet in the progress log or evidence: rung 3's two red arms,
  the guard green, the control green, rung 2's compile failure (premise 4, DEBT-2(b)). The PM
  captures them at close.

## Cycle 2

**Verdict: CONFORMS.** Diff `7a0cc64..6e5cd89`: docs and records only (cpp, hpp, tests untouched).
- **B1 closed.** The false README sentence is deleted and reduced to a pointer (`README.md:75-76`) to
  the header's `Subscribe` doc. That doc (`hpp:114-117`) scopes the rule correctly to "a window of
  only lost rows". F2 and F5 are now disclosed through the pointer, and nothing in the README
  misstates L1. d7 L1 ("the header says so, and the README points to it") matches the tree.
- **N1 closed as booked.** `hpp:121` keeps "Any other timeout is used as given". D7O-3's tracker row
  (`plans/D7O-plan.md`) now owes the restoration of "value", with its cause cited.
- **RECORD corrections confirmed.**
  - L5's 100-year wording now reads "accepted and never fires early".
  - L15's Tests line cites rung 3.
  - BC-4's Status has a D7O-2 entry.
  - BC-8 points to `ArmTimer` for meaning.
  - d7's Mode line notes the shift.
  - Ladder :71 carries an as-landed note.
  - The red-run evidence is deferred to the close progress log, as stated.

RECORD:
- d7 L5 "Holds at" `cpp:200` now lands inside `ArmTimer`, so it duplicates the `ArmTimer` citation.
  L5's anchor to the previous flush (`last_flush_ = now` in `Close`, `cpp:222`) is not cited.
