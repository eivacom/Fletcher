# D7F-2 — compliance review (step 4a), cycle 1

**Verdict: ISSUES(2): 1 blocking, 1 non-blocking.** Diff `7916105..dbaa2b6`. The design is
`plans/D7F-2-schema-loss-at-deadline.md`; the locked decisions are D-D7F-1..7.

**Proof of read:** I read `plans/RIX-rulings.md` top to bottom: **169 lines, 14 entries**. The last
entry is "2026-11-10 — Queued windows are delivered before the closing batch". I treated every entry
as tier 1. No tier-1 collision is engaged. This item touches L1, L5, L6, L7, L8 and L13, and the
rulings "2. Fix both", "Yes on all accounts" and the D9 rewording. The code agrees with all of them.
D-D7F-6 holds: the diff has no index logic.

## Findings

**F1 — BLOCKING. The design ordered a test on a condition that holds, and the test was not added. d7
L8 now records a false claim instead.**
- **What the design ordered** (Risks): "If `Publisher::CreateTopic` accepts an `OwnedSchema` whose
  format string `ImportArrowSchema` rejects, the implementer adds that as a fourth 'undecodable'
  row of the guard."
- **The condition holds:**
  - `DeclaredSchema::Encode` "never throws: an unencodable schema becomes `encodable == false`"
    (`schema_conflict.hpp:39-40, 54-58`).
  - `Publisher::CreateTopic` then forwards the schema to the provider (`publisher.cpp:51, 82-87`).
  - The Mock stores a `DeepCopy` of it and hands it out on every `Publish` (test:35-41, 54-58).
  - `ImportArrowSchema` throws when `arrow::ImportSchema` fails (`schema_import.cpp:20-25`).
  - A test can build such a schema by hand, the same way `pubsub/tests/test_publisher_subscriber.cpp:180-187`
    builds its schemas (`ArrowSchemaInit` plus a format string Arrow does not know).
- **The cell was not added.** The guard has three states, not four. This leaves untested the one
  behavioural narrowing in this item: an import failure is decided once and is permanent (§3). It
  also leaves untested the condition DEBT-1 asked for (`found.import_failed` decides, a null return
  does not).
- **The d7 L8 Tests bullet is false.** It says "No test forces an import failure: no in-tree
  provider can hand over an unimportable schema." The design's condition was about
  `Publisher::CreateTopic`, not about providers, and both the Mock and InProcess pass such a
  schema through.
- **Fix:**
  - Add a fourth state, "unimportable schema", to `EveryAssemblyStateReportsItsDropsAtEitherEnding`.
    Expect a null batch with `rows_dropped` 1 at both endings.
  - Rewrite the L8 Tests bullet to name that cell.
  - If writing the cell shows the schema is refused before import, stop and record that evidence
    in L8 instead.

**F2 — non-blocking (fix it in F1's cycle). A false comment survives in the batched `Subscribe`
(cpp:360-363).**
- **The comment:** "The batcher receives the schema lazily from the first decodable sample above,
  so this works subscriber-first … without dropping the first window for want of a schema."
- **Why it is false:**
  - Design §1 makes exactly that window a null batch of drops, and the forcing test's W1 asserts
    it.
  - The schema is taken from the first row that *brings* one, not from the first decodable sample.
- **Why it must change:** the comment contradicts L1, L7 and D9, and it sits in a function this item
  rewrote. It is tier-3 text, so the text changes, not the law. The design's comment list
  (cpp:53-56, 331-334, 341-354) missed it.

## Checked and conforming

- **§1 timer.** The `ready_` branch and its wait are gone. `TimerLoop` now has a single
  `Flush(lk, kTimeout)` call (cpp:179-181). `ArmTimer` and the cadence are untouched (L5). Drops still
  never close a window by count (cpp:108).
- **§2 `ready_`.**
  - The member, its write in `SetSchema`, the notify, and the read in `AddRow` are all deleted.
  - Grepping the code finds no `ready_`, no `cached_codec`, no `Codec*` return, no
    `unique_ptr<Codec>` in `SubscriberArrow`, and no "SubscriberArrow's lifetime" comment.
    `codecs_.find` appears once, inside `AcquireCodec`.
  - The second lookup inside `call_once` is gone.
- **§3 and DEBT-1.**
  - `AcquireCodec` returns `TopicCodec` by value, and the copy is made under `mu_`. Its `codec` is a
    `shared_ptr`.
  - The lambda decides on `found.arrow_schema || found.import_failed`. A null import result is
    "not a decision" (cpp:379).
  - `SetSchema` keeps its `catch` with `decoder_.reset()`.
  - The per-row overload holds its codec by value.
- **D-D7F-5 and the `import_failed` field.**
  - The field is private, and P5 still holds: no friend and no test names `TopicCodec` or
    `AcquireCodec`.
  - Public surface is 0. `BatchOptions`, `BatchStatus`, `RecordBatchCallback` and the signatures
    are unchanged.
  - DEBT-1 left the mechanism open ("for example"), so this choice is within it.
  - Observation, not a finding: every stored `codecs_` entry carries a flag that only a return
    value uses. A {schema, `import_failed`=true} value can be represented but is never built.
- **D-D7F-7.**
  - One count entry: the no-decoder branch of `AddRow`.
  - One closer: `Flush`. One delivery site: `cb_` in `Flush`.
  - The timer still has exactly one `Flush` call. No new site exists.
- **D-D7F-3.** The ruled null-batch wording in the header (hpp:108-111) and the README (:97-100)
  is unchanged.
- **Header and README timeout sentences (D7F-1 DEBT-5, d7 a1).** Both sentences are now true:
  - The header says `timeout` after the previous flush, never sooner than 1 ms after the first row
    or drop, schema or not. This matches `ArmTimer` and the timer.
  - The README says the same in its own words.
- **DEBT-2(c).** The clause "lost rows never count towards `max_rows`" was dropped from both the
  header and the README. The phrase "rows have been decoded" is the design's own §5 wording, and
  the review accepted it.
- **Debts.**
  - D7F-2 DEBT-2(a) landed: the BC-3 row names both the once-flag and the `schema_decided` cache,
    and "nothing in the batcher" is accurate.
  - DEBT-2(b) landed: the L8 edit sits under *Holds at*, and the law's sentence is unchanged.
  - DEBT-3 landed: `ExpectLateSchemaRowsAssembled` checks order within each delivery and the set
    {1, 2, 3} across deliveries.
  - D7F-1 DEBT-3 (the race), DEBT-4 (forcing test plus leg) and DEBT-5 also landed.
- **Tests.**
  - The four tests match the design's mapping table: options, W1, idle, W2, late schema, Σ = 7,
    the P2 caveat comment, the guard's 3 × 2 cells with `SCOPED_TRACE`, and the leg's shape.
  - No test was deleted or rewritten, as declared.
- **d7 law sentences.**
  - The sentences of L1, L5, L6, L7, L8 and L13 are unchanged. Only marks, *Holds at*, test lists
    and status bullets were edited.
  - The design's ordered deletions are done: the §2 term **Ready** and L6's "Interim" line are gone.
- **d7 Q1 marked resolved: judged lawful.**
  - Q1's owner column gives "how" to the fix round, and gives it to the owner only if the
    interface changes.
  - The interface did not change beyond the owner-ruled D9 wording and the owner-confirmed laws
    L1, L5 and L6.
  - The design's §5 did not order this edit, and it left one stale sentence (RECORD).

## RECORD
- RECORD: d7 L1's last bullet still says that how pre-schema drops are reported "is a design
  question for that round (Q1)", but Q1 is now marked resolved.
- RECORD: d7 L1's Violation 1 bullet says "no schema has been decided (… or `ImportArrowSchema`
  throws)". Since D7F-2, a throw *is* a decision (L8 *Holds at*). It should read "no usable schema".
- RECORD: design P3 was not corrected per DEBT-1. It still says a null return from
  `ImportArrowSchema` is a failure.
- RECORD: d7 line 6 says cpp citations are "off by up to 14 lines", but cpp shrank a further 19
  lines. For example, L8 cites `call_once` at cpp:372, and it is now at cpp:342.
- RECORD: net production lines, counting code only (cpp + hpp), are +28 / −42 (net −14), against
  the declared +20 / −40 (net ≈ −20). This is for the brief's "As landed".
- RECORD: design-debt register. D7F-2 DEBT-2 and DEBT-3, and D7F-1 DEBT-3, -4 and -5, can be marked
  landed. D7F-2 DEBT-1's code half has landed; its P3 correction is still owed.
