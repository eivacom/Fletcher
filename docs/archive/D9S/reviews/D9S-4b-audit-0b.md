# D9S-4b step-0b test-authority audit (new-test): CLEAR

**Verdict: CLEAR.** Nothing is FORBIDDEN or FORBIDDEN-BY-TOLERANCE, so nothing has to be deleted before building. Nothing schedules a restoration and no tier-1 texts disagree. Working tree on HEAD 023152b, light lane.

**Proof of read:** `plans/RIX-rulings.md` has 476 lines. The highest entry, at line 470, is "2026-11-13 — D9S: tighter document caps for the remaining stages".

**Checked against:** every ruling, especially 2026-11-13 "Batch owner: Borrowed; keep by moving" (D26) and "Accept until stage 6". Also d9 L11, L13, L14 and L15, d7 L3, binding.h A2 ("THE ORDER IS PART OF THE VALUE: ascending unsigned-byte order of the key bytes"), and the ground rules and rung 4 of the ladder.

**Scope (the working-tree diff only):**
- `Batches.EachBatchRowCarriesItsOwnAttachments` (`c-abi/tests/test_batches.cpp`:487), with helpers `SetPtr` and `MakeSet` (:448-472). **LAWFUL.**
  - Law (a): every assertion agrees with the laws.
    - One set per row, in row order: d7 L3, "`attachments[i]` belongs to batch row `i`".
    - The sink copies keys and bytes inside the callback and reads no borrowed pointer afterwards: L11 and D26.
    - The expected entries follow A2's byte order: `origin` < `trace`.
    - The test asks the shim for no count, window or check of its own: L15.
    - "Empty set, never NULL" settles Q6, which was open. No law forbids it.
  - Law (b): the test drives everything from outside. It uses only `binding.h` entry points: `fl_attachments_builder_*`, `fl_blob_create`, `fl_publisher_publish_rows` with `atts_per_row` (NULL entries are documented as allowed), `fl_subscriber_subscribe_batches` and the `fl_attachments_*_at` readers. It observes and does not arrange. No new value subject is smuggled in: "each row's attachments" is named in rung 4's subject, and the ladder's 4a/4b split assigns it to 4b ("4b: attachments filled"). It is not a new rung.
- `Seen` and `BatchSink::Record` (the shared fixture). **LAWFUL; 4a's proofs are unchanged.** The new code only records data and adds no assertion to 4a's tests. The shim builds `fl_batch out = {}` (`batches_server.cpp`:109), so before 4b's production code the new field is NULL, and the fixture reads nothing. Recording NULL sets as a count instead of hiding them is correct.
- Controls: none separate, and the test's own values are enough. Window 1 ends with the attachment row and window 2 starts with one. Each set has a different size (0, 1 or 2 entries) and different bytes, one of them with an embedded zero. So a NULL list, all-empty sets, identical sets, a shift by one row or a swap each fails a distinct `EXPECT`.

**Notes (no finding against the test):**
1. The brief says the test is red because it "does not compile". With the binding.h field already added, it compiles and fails at runtime instead, on `had_attachments` false. That is still a valid red.
2. The header's "NULL iff num_rows == 0" half, for the no-batch window, has no test. That is a gap, not a violation. Optionally, 4a's no-batch test could assert `!had_attachments`.
3. Closing by `DestroySubscriber` is inherited from 4a's test. It is d7 L9 behaviour, so it is lawful. It is rung 6's subject, but nothing new is introduced here.
