# D7O-3 — architecture review, cycle 1

**Verdict: APPROVE-WITH-DEBT(4).** No BLOCKER, no STOP-AND-ASK. Reviewed `plans/D7O-3-any-row-count.md` and
`plans/D7O-3-brief.md` at `00dacb8` (production tree `f587e36`).
**Ledger proof-of-read:** `plans/RIX-rulings.md` read top to bottom: **295 lines, 24 entries, last heading
"2026-11-12 — D7O-2: the deadline definition says some windows have none"** (line 287). The design contradicts
no ruling. DEBT-1 and DEBT-2 are PM-facing and must land **before the owner reads the brief**.

## Point 1: the bounded pre-size is lawful. It is not a clamp and not a second range check
- **Principle** (L15, D-D7O-3, "Refuse both"): an accepted value is never adjusted. Its meaning is L4's: a window
  closes at `max_rows` rows. The header promises only that ("Flush at this many rows", `hpp:78`). No law,
  ruling, header line or register row gives `max_rows` a second meaning as a reservation size. `min` changes
  nothing the option means and refuses nothing. `max_rows_` is still stored and compared unchanged (`cpp:38`,
  `cpp:118`). No principle-vs-detail collision.
- **D-D7O-4 holds. Rely on this at 4a/4b.** `PreSize` answers neither "acceptable?" (BC-8) nor "when does a
  window close?" (BC-4). Rung 4 (c) is the right outside guard: a clamp, or a bound that closes a window, fails it.
- **The bound is what the ruling needs, not just something it allows.** `bb:287` multiplies `int64` by `size_t`, which
  wraps without UB. NumericBuilder resizes its data buffer first (`builder_primitive.h:149-154`). So for
  some legal values the pre-size half-succeeds (DEBT-1). The bound removes every wrap (2^16 × 2^31 < 2^47). It
  also removes the latent `length() + INT64_MAX` in `ArrayBuilder::Reserve` (`builder_base.h:140`) on `Close`'s
  failed-`Finish` path.
- A record is owed so that a census by operation does not count `min(max_rows_, …)` as a BC-8 copy: DEBT-4(b).

## Point 2: the brief's Decisions
- **Shape:** both Decisions have two options, each with a Users line and an Architecture line, plus a
  recommendation and a default. **Content:** Decision 1's option (b) hides an L10 break (DEBT-1). Decision 2 is
  already answered by tier 1 (DEBT-2).

## Point 3: a failed decoder construction. Correctly left out; record it in this stage
- The cause differs from the ruling's ("a failed pre-size"). L10's letter holds: no thread dies and the rows are
  counted (L1). L8 is stated positively. The "only an undecodable schema" reading is the ladder's gloss, not
  the law's text. Telling "rejected" from "failed to build" needs domain 2's exception contract, and
  D-D7O-6 keeps `BatchDecoder` unchanged.
- **Where to record it:** in this stage's own doc edits, which already touch BC-3 and L8 (DEBT-4(a)). Not "to the PM as a
  possible note", where it can be lost.

## Point 4: the declared reds and the overlap
- **Rung 4 (a)/(b) red, verified by reading.** `Reserve(INT64_MAX)` gives an int32 byte count of −4. Arrow
  refuses the negative size before `ArrayBuilder::Resize` touches capacity, so `Reserve` throws, `cpp:84` resets, and
  `AddRow`'s no-decoder branch counts the row. The red is deterministic and has no UB, so premise 3 holds. On a slow
  machine (a) may show `rows_dropped` 2 + 1 across two deadlines; it is still red on Σ`num_rows`. Observe it; do not stop.
- **(c) is green both ways.** `Reserve(131 073)` is small on the old tree.
- **Sweep:** 6 × 7 = 42 cells (`milliseconds::max()` is `kNoTimeout`, `hpp:88`); 27 refused (3×7 + 3×2), 15
  accepted. The overlap comes from the design itself. It strengthens the ladder's Σ`num_rows` +
  Σ`rows_dropped` = 1 to Σ`num_rows` = 1 and Σ`rows_dropped` = 0. Under the ladder's literal sum, the 5
  `INT64_MAX` cells would be green on `f587e36` (null batch, `rows_dropped` 1). The strengthening is lawful
  (L15, "used as given"; the ladder's sum is a floor) and declared. The forcing test is still rung 4. Accepted.

## Ceiling (7b) and budgets: fits, no split
- **Priced.** `SetSchema` is rewritten at about the same length. `PreSize` plus `kMaxPreSizeRows` add about 7
  lines with the comment. `Close`'s block goes from 6 lines to 1. That is about +6 to +10 net against 250, and the
  declared +16/−10 is consistent. There is no new type, verb, thread, lock or fixture, and `<algorithm>` is
  already included (`cpp:9`). Tests (about +190) use only existing helpers.
- **Budgets.** Design 264/300. Brief 60/60: it is at the cap, so the DEBT-1/2 edits must not grow it. Public
  surface 0: `kMaxPreSizeRows` is a private member of the private `RecordBatchBatcher` (`hpp:154`).
- **Files-to-delete** is real (`cpp:80-85`'s reset, `cpp:238-243`'s block). There is no coexistence bridge.

## Tree claims verified (rely on these; do not re-derive them)
- `Reserve` is called at only two sites in `pubsub-arrow` (`cpp:82`, `cpp:240`). `max_rows_` is read only at
  `cpp:38/42/45/82/118/240`.
- `kMinWindow` is at `cpp:308`. `Loop` releases the decoder at `cpp:284`.
- `BatchDecoder::Reserve` calls `ArrayBuilder::Reserve` once per top-level column (`bd:600-602`, `:516`).
- `MockProvider` is at `test:41` and `subscribe_with_options_count` at `test:108`. Helpers: `BatchSink`
  `test:491`, `Observe` `:2562`, `Describe` `:2570`, `XAt` `:2583`. The `weak_ptr` pattern is at
  `:2376-2406`. A malformed row via `PublishDirect` is at `test:680`.
- The 100 000 `max_rows` helpers are at `test:570` through `:1307` (7 sites).

## DEBT-1: Decision 1's option (b) hides a crash, and (a)'s line overclaims (PM-facing, before the owner)
- (b) holds L15 just as (a) does, so (a)'s "follows L15" misleads, and (b)'s Architecture line names no law.
- What (b) breaks is **L10**. Take 2^62 rows on an int32 column: `bb:287` gives 0 bytes, so the data buffer
  allocates. Then `ArrayBuilder::Resize` records capacity 2^62 before the 2^59-byte bitmap fails (Arrow's
  `builder_base.cc`, not on disk). Kept, the decoder's first `Append` writes through an unallocated bitmap.
  Today `SetSchema` discards that state. Rung 4's `INT64_MAX` misses it, because −4 is refused first.
- **Fix (cheapest, a forbid):** strike (b) and make Decision 1 a disclosure, since the bound is forced by L10.
  Or give (b) "Architecture: holds L15, breaks L10: a size that wraps in Arrow crashes the delivery thread."

## DEBT-2: Decision 2 is already answered by the laws (PM-facing)
- A window that exhausts memory is lost whole and counted. That is L10 ("an internal failure becomes a drop"),
  L1 (reported), §1 Refuses ("a lost row is counted and never recovered"; no back-pressure) and L4's closed list
  of four events. With no timeout, the count arrives in the `kClosing` batch (L1's no-timeout clause, already in
  the header). Option (a) is the law, not a choice.
- Only (b) is genuinely the owner's, and it is an L4 amendment: a proposal, not a stage decision.
- **Fix:** move it to "Risks accepted" as a one-line disclosure citing L1 and L10. If (b) should be weighed, add a
  d7 §7 open question (owner). The header's disclosure sentence stays as designed.

## DEBT-3: the verdict/hint split has no test (step 3, tests)
- The design says so: "Either alone turns (a) and (b) green." A bound-only build keeps the `reset` and passes
  everything, and under the bound rung 4's pre-size never fails. The ruling's own clause is proven by nothing.
- **One cell pins it, on any machine.** Declare a schema with a `fixed_size_binary(2^31−1)` field and set
  `max_rows` ≥ `kMaxPreSizeRows`, so the pre-size asks for about 2^47 bytes, beyond any x64 user address space.
  Send one malformed row (`PublishDirect`, `AppendByte(0x00)`, as at `test:680`). After `Unsubscribe`, expect
  one **non-null** zero-row `kClosing` batch with `rows_dropped` 1: a decoder exists (L6, L7). It is red on
  `f587e36` and under a bound-only build. If the schema is refused upstream, or the allocation succeeds,
  record that and drop the cell.

## DEBT-4: records (step 3, docs; not counted)
- **(a)** Point 3's residue: a "Registered gap" in BC-3 (as BC-2 has) and a d7 Findings line by L8. A
  constructor that fails on allocation, not rejection, still decides "no decoder" for life (`cpp:83`). The fix
  needs domain 2's exception contract (`batch_decoder.hpp:28-41` names no allocation outcome).
- **(b)** BC-8 gains one clause: `PreSize`'s `min(max_rows_, kMaxPreSizeRows)` is a capacity hint, not a copy of
  the range check (D-D7O-4).
- **(c)** Premise 1 ("a failed Arrow Resize leaves capacity unchanged") is false when the bitmap fails after the
  data buffer grew (DEBT-1). Under the bound, only exhaustion between two small allocations reaches that case,
  as in every `Append`'s growth. Put it in the domain 2 note beside the benchmark comment; it is not a stop.
