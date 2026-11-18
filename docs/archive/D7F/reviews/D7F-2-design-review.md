# D7F-2 — design review, cycle 1 of 2

**Verdict: APPROVE-WITH-DEBT(3).** No BLOCKERs. No STOP-AND-ASK.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: 169 lines, 14 entries. The last entry
is "2026-11-10 — Queued windows are delivered before the closing batch". Every entry was treated as
tier 1. No ruling, no d7 law (L1, L2, L5, L6, L8, L13), D9 and no locked decision (D-D7F-1..7) is
contradicted.

**Budget.** (a) The design is 251 lines (cap 300). New public surface is 0: `TopicCodec` and
`AcquireCodec` are private (hpp:155-176). Net lines are declared. (b) The implied work adds 0 types
and 0 verbs. It swaps one member (`cached_codec` for `schema_decided`), changes one private return
type and one private field type, and deletes one member and one wait. I price it at −15 to −25 net
production lines, which matches the declared −20 and is far under the 250 ceiling. No split is needed.

## Verified against the tree (the implementer can rely on these)

- **P1 holds** (test:51-58): a null `SharedSchema` before `CreateTopic`, the real one after it.
- **P4 is plausible.** `Codec::DecodeRow` is `const` (codec.hpp:61-62).
- **P5 holds.** No friend and no test names `SubscriberArrow::TopicCodec` or `AcquireCodec`.
  `PublisherArrow::TopicCodec` is a separate type (publisher_arrow.hpp:58).
- **P3 half holds.** A failed copy or import throws `PubSubError(kInternal)` (schema_import.cpp:20-25).
  The null return is not a failure (DEBT-1).
- **`ready_`** is written at cpp:67 and read only at cpp:110, 181 and 185. `ready_ &&` at cpp:110 is
  redundant. cpp:77 returns when there is no decoder, and only `SetSchema` resets `decoder_`, on the
  same serialized delivery thread.
- **`SetSchema`'s `catch` is not a no-op.** If `Reserve` throws after the assignment, a decoder is
  left behind. The design is right to keep `decoder_.reset()`.
- **The guard's cells are sound.** A single `0x00` row is malformed under `TestSchema`
  (test:641-644), so the assembling cell gets a non-null zero-row batch with 1 drop. The undecodable
  cell reuses test:1003-1005. Only awaiting×timeout is red before: undecodable has `ready_` true
  today, so its timer already flushes.
- **The red-before is right.** With no decoder and `ready_` false, the timer parks at cpp:185, so
  W1's `WaitFor` times out.
- **F1-F4 and G1 stay green unchanged.** They use a 10-minute timeout (test:695-700), so the timer
  never reaches their windows.

## BC-3: is building no state enum a deviation? No.

- **Tier.** BC-3 is a census output, so it is tier 3. D-D7F-7 locks C1, C2 and C4, not C3. The
  register lets a round change a single site if the round says so. The design says so (§2) and
  rewrites the row.
- **The census's two concrete asks are met.** `ready_` no longer gates anything (it is deleted).
  An import failure becomes a decided state instead of a `return nullptr` that is retried forever.
- **Its structural test is built** (States × endings), as the census wrote it.
- **The state is three facts:**
  - decoder presence (batcher);
  - decided (subscription lambda);
  - `stopped_` (batcher).

  The combination "undecided, decoder present" is unreachable, because `SetSchema` is called only
  inside the decision. An enum would restate decoder presence inside the batcher, which would be a
  second site for the same fact. Not building it is the narrower shape.
- **One correction.** The lambda *does* read awaiting vs undecodable: it uses it to decide whether to
  try acquisition again. So §2's "no behaviour reads it" holds for the batcher only. The register
  text has to say this accurately (DEBT-2a).

## The narrowing (an import failure is decided once and is permanent): lawful

- **L8 states the principle** (owner-confirmed): "Once a schema has been rejected, every later row is
  dropped, and there is no recovery." D9, which was ruled, names "the schema was rejected
  (permanent)" as one meaning of a null batch. An import that throws rejects the delivered schema.
  Today's per-row retry is tier-3 code that departs from L8's principle, and the design brings the
  code into line with the law. Census C3 commissioned exactly this.
- **It cannot be observed in the tree.** A topic's schema cannot change from one non-null value to
  another, because a conflicting re-declaration is refused (D7F-1 review §BC-3). A schema that fails
  to import would fail on every retry.
- **It is disclosed plainly** in the brief. No owner question is owed. Its edges are DEBT-1 and
  DEBT-2b.

## DEBT

**DEBT-1 (must land in this PR): decide on what `ImportArrowSchema` says, not on handle presence.**
- The lambda decides on `found.arrow_schema || schema`. But `ImportArrowSchema` returns null,
  without throwing, for a non-null handle whose `release == nullptr`. It documents that case as
  "nothing to import", the same as a null handle (schema_import.hpp:31-34, schema_import.cpp:14-18).
- Under the design, such a row makes the subscription unusable for life. A usable schema arriving
  later is then never used, against rung 2 and L13.
- **Not reachable in the tree.** Every `SharedSchema` comes from `MakeSharedSchema`, which returns
  null for an empty schema (owned_schema.hpp:126-130).
- **Not silent.** Every row is still counted in `rows_dropped`.
- **Fix.** Only a throw from `ImportArrowSchema` decides "unusable". For example, `AcquireCodec`
  reports that its `catch` ran, and a null return stays "not arrived". Correct P3, which calls a null
  return a failure. This is one condition, with no redesign.

**DEBT-2: documentation accuracy, three items.**
- **(a) BC-3 register row.** §2 names "the once-flag, one site", but §3 reads `schema_decided` on
  every row, with the once-flag behind it. Name both: the once-flag is the decision, and
  `schema_decided` caches it per row. Keep the once-flag. It also stops a second `SetSchema` from
  replacing a decoder that holds pending rows.
- **(b) d7 L8.** Record "an import failure is decided once" under L8's *Holds at*. Leave the law's
  own sentence unchanged: the owner confirmed it, and it already covers this case.
- **(c) Header and README sentence (§5).** "Lost rows never count towards `max_rows`" publishes a
  rule that d7 L4 does not state and that the design says "stays unpinned". The cheapest fix is to
  drop that clause. If it stays, pin it: a guard cell with `max_rows` 1 and two drops expects one
  `kTimeout` and no `kRowLimit`.

**DEBT-3: the forcing test's cross-delivery order claim is not stall-proof while L12 is open.**
- If a stall of about 100 ms splits x = 1, 2, 3 across `kTimeout` (timer thread) and `kClosing`
  (`Unsubscribe` thread), the two callbacks can overlap. `Stop` does not wait for an in-flight timer
  callback (cpp:123, cpp:224-233). The sink can then record `kClosing` first, and "concatenated in
  order" fails. This is rare and fails loudly.
- **Fix.** Assert the order of rows within each delivery. Across deliveries, assert x = 1, 2, 3 each
  exactly once, with the attachment on x = 2. Cross-delivery order is rung 3's subject (L12), so
  this test never needs to change at D7F-3.
