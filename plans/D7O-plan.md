# D7O — Batch options — Plan and tracker

**Purpose (owner's terms):** a subscriber can ask for batches that close only when they are full or
when it unsubscribes, never on a clock; and a subscriber that asks for something nonsensical (fewer
than one row per batch, a negative timeout) is told so at once instead of silently getting something
else. It is the first of three rounds that give C# one batcher, C++'s ("we should only have one and
it should be the C++ one"); the shim round (domain 9) and the C# switch round (domain 10) follow.

- **Domain:** 7, Batch assembly ([spec](../docs/domains/d7-batch-assembly.md)), within one existing domain.
- **Locked decisions:** [D7O-locked-decisions.md](D7O-locked-decisions.md).
- **Owner rulings:** [RIX-rulings.md](RIX-rulings.md), the 2026-11-12 entries.
- **Test ladder:** [D7O-test-ladder.md](D7O-test-ladder.md). The stages below are generated from it:
  4 rungs, 3 stages; stage 2 carries rungs 2–3 for the reason the ladder gives.
- **Pre-round audit and census:** [D7O-pre-round-audit.md](D7O-pre-round-audit.md).

This file is both the `plan_path` (the tracker) and the `user_stories_path`.

## Tracker

Status: ⚪ not started · 🔴 in progress · 🟢 done (forcing test green, reviewed, logged).

| Item | Subject (what becomes true) | Inherits | Est. net prod. lines | Status |
|---|---|---|---|---|
| D7O-1 | A batched `Subscribe` with `max_rows` below 1 or a negative timeout is refused at once with an invalid-argument error and leaves nothing behind; every in-range value is used as given (`max_rows` 1 gives a batch per row; a zero timeout is accepted, with the 1 ms floor). | The D7F pipeline as built; census BC-8 (DEFINE: the one range check; delete the clamp `cpp:36`); audit F3–F5 (header and README text, for the refusal) | 10–20 | 🟢 |
| D7O-2 | A subscription with no timeout gets windows that close only when full or at closing, never on a clock; a window of only lost rows is reported in the `kClosing` batch. A timeout too long for the clock behaves as no timeout, never as an early or wrapped deadline. | D7O-1's range check (it must accept the no-timeout spelling); census BC-4 (EXTEND: `ArmTimer`, no deadline, no overflow); audit F1–F5 (no-timeout text, F2's disclosure); LC-1 (confirmed) | 15–40 | ⚪ |
| D7O-3 | A `max_rows` larger than any window will reach is used as given: rows are assembled and delivered, never dropped because of the size asked for. The BC-8 sweep proves "refused ⇔ out of range" over the whole option grid. | D7O-1 (clamp gone), D7O-2 (no-timeout arm); the BC-8 structural test (guard) | 5–15 | ⚪ |

Each item's forcing tests are named by the architect at design, from the subjects above and the
ladder's external paths. The BC-8 sweep is a declared guard in D7O-3.

## Items

### D7O-1 — Nonsensical options are refused

**User story:** as a subscriber, if I ask for fewer than one row per batch or a negative timeout, I
am told at once, and nothing is left running; anything sensible I ask for is what I get.
**Acceptance:** ladder rung 1; L15, L4, L5 (1 ms floor); D15; BC-8.

### D7O-2 — No timeout

**User story:** as a subscriber that only wants full batches, I can say "no timeout", and nothing
arrives on a clock: full batches as they fill, the rest when I unsubscribe, with any losses counted
then. A huge timeout means the same, never an instant one.
**Acceptance:** ladder rungs 2–3; L4, L5, L1, L6, L9; D14.

### D7O-3 — Any row count is used as given

**User story:** as a subscriber that asks for very large batches, I get my rows, never a stream of
drops because of the number I chose.
**Acceptance:** ladder rung 4 and the BC-8 sweep; L15, L8; BC-8.

## Definition of done (round)

- All three items 🟢.
- The default run of `pubsub-arrow` tests and `ctest -L heavy` are both green; the
  `pubsub-arrow-fastdds` integration suite is green.
- Domain 7's L4, L5 and L15 read `[built]`, with the tests that prove them; register BC-8 reads
  `[built]`; §2's deadline definition (F1) is amended with the owner's approval.
- No edit under `c-abi/` or `dotnet/`.
