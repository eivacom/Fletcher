# BIND-5 — conformance against the acceptance

**Subject:** BIND-5's acceptance bullets in `BIND-csharp-bindings.md` ("### BIND-5"), as amended by
D-BIND-58 through D-BIND-63. Each bullet was checked against the tree, not against a commit message.

**Reviewed at:** `0670ad2`. Code review: `BIND-5-codereview.md` (9 BLOCKERs, 5 questions, 23 DEBT,
17 NITs).

**Why a separate file from the code review:** that pass asks what the code does, and this one asks
whether the bullets are met. Where a ruling *changed* a bullet during the item, this pass states
what the bullet says now and which ruling changed it.

**Outcome: 5 of 11 live bullets CONFORM; 5 PARTIAL; 1 NOT MET. BIND-5 does not close.**
- **What conforms:** the copy oracle, which was the item's hardest bullet. The instrument is placed
  as ruled, and the Arrow tier folds into `Publisher`.
- **What does not:** the batch-first subscriber and its port of `test_pubsub_arrow`. The port's
  tests hid three behaviours the C++ original has (code review B3, B4, B7), and it carries a
  deadlock (B2).

---

## The live bullets

| # | Bullet | Verdict |
|---|---|---|
| 1 | `SubscribeBatched` is the primary shape (D-BIND-25): borrowed rows copied, N per native call, parallel attachments, `BatchStatus`, defaults 8000 / 1 min, per-row = `MaxRows 1`. **Amended by D-BIND-59:** one `RecordBatchHandler` for both forms, owned `AttachmentsBuilder` copies, a handler-owned `RecordBatch?`, `TopicOptions` and the schema watch forwarded | 🟨 **PARTIAL.** The shape is present as amended (`SubscriberArrow.cs:124-205`). Behaviour falls short of the C++ it mirrors: corrupt rows count toward `MaxRows` (B3); rows split across messages decode misaligned (B4); a decodable topic can get the NULL the contract reserves for an unopenable schema (B7); a timer-thread cancel deadlocks (B2). "N per native call" holds in code but no test shows it |
| 2 | `PublisherArrow` folds into `Publisher` | ✅ No `PublisherArrow` in `dotnet/src`; its cases map to `PublisherTests` and the new mismatch mirrors |
| 3 | Dictionary re-folding deferred to DICT (D-BIND-8) | ✅ Delivered as the value type; `ADictionaryColumnArrivesAsItsValueType` and `…KeepsItsNulls` |
| 4 | The `test_pubsub_arrow` file set ported over `inprocess`: 33 cases at D-BIND-57 | 🟨 **PARTIAL.** The mapping is complete **by name**: 33 of 33 named, 32 mapped, 1 excluded by D-BIND-8/59 with its reasoning. It is not complete **by property**. Three ports cannot see the C++ behaviour they claim (B3, B4 through T-D5, and T-D1's reuse case). Four more are weaker than their notes say: T-D2, T-D3, T-D4, T-D5 |
| 5 | **Added by D-BIND-60:** the shim refuses rows bound under a schema other than the topic's | 🟨 **PARTIAL.** It holds on Linux, and the tests pass on both platforms. On Windows the refusal is thrown from C-linkage code under `/EHsc`, which is undefined behaviour (B1, reproduced as a fail-fast in a standalone build). The check is also silently skipped for dictionary schemas (N-D3, Q1). The lock-across-throw finding D-BIND-60 recorded as open now has a cause, pending B1's confirming experiment |
| 6 | **Rows (D-BIND-58, D-BIND-61):** C#'s real `Publisher.Publish` into the probe scores `encode_copies == 0`; a staging publish and a copied export each score exactly 1 | ✅ `CopyOracleTests` 5/5 on Linux and Windows × net8 and net10, in runs 36443379137 and 36447834274. `c_abi_probe_tests` 7/7 inside c-abi's 86/86. Two reviewers found no path to a false zero. Weakness: the multi-row form is not measured (T-D7) |
| 7 | **Attachments (D-BIND-58):** a handler's `AttachmentsView` carries the transport's own bytes, by address | ✅ `AttachmentsViewHandsTheHandlerTheTransportsOwnBytes` asserts `parked == delivered` and `attachment_copies == 0`. Its control is weak (NIT: it does not assert the loan arrived), and the arena reuses the loan's slot on the 4th publish (N-D5). Neither affects the single-publish cases the claim rests on |
| 8 | **D-BIND-62:** the instrument lives in a test-only `fletcher-copy-probe` shared with pubsub-conformance; the probe shim builds only under `with_probe_shim`; release packages are built without it | ✅ **As narrowed** (only the lanes that load the probe build it; recorded in the digest). The option defaults off and stays in the package ID, and no cd workflow builds c-abi or copy-probe. Debt: the lanes no longer build the default configuration at all (T-D9), and nothing stops `fl_test_*` reaching the shipped export table (N-D2) |
| 9 | **D-BIND-63:** the window splits at C++'s ceiling, as `RowLimit`; a row alone past it is dropped and counted; a failed copy is a counted drop | 🟨 **PARTIAL.** Rules 1 to 3 are implemented, and the tests can fail. Rule 4 holds only for the row copy in `OnRow`. The flush's own copies and the attachment copy are not covered, and one of them can take the timer thread down (B5). The claim that the raw bound implies the column bound is false for null slots (M-D2) |
| 10 | The rulings D-BIND-58 to 63 landed in all four places | 🟨 **PARTIAL.** 58 to 62: digest, tracker and development plan ✅. **63: missing from the development plan** (T-D11). The architecture diagrams have not caught up with 5a or 5b, which is Q5 |
| 11 | The public-surface note agrees with the code | ❌ **NOT MET, narrowly.** The `SubscriberArrow` class diagram lacks `HandlerFaulted` and `BatchReason`, and `AttachmentsBuilder` lacks its three accessors and the empty-key refusal (T-D12). The tables are right; the diagrams are not |

## The bullets that changed, and who changed them

1. **The copy-oracle bullet** was amended by **D-BIND-58**: rows through a test-only probe shim,
   attachments as the delivery view. **D-BIND-61** then made the rows half scorable (by source) and
   **D-BIND-62** placed it. All three are maintainer rulings recorded in the digest.
2. **The shape bullet** was amended by **D-BIND-59**: one handler, owned attachments, a nullable
   batch.
3. **D-BIND-60** added a bullet mid-item, and **D-BIND-63** added one after the item's code was
   done. The C++/C# sequence review found the gap while 5b's CI ran.

## Carried forward

- The author's own findings from the sequence review:
  - T-D7, the multi-row publish is not oracle-measured;
  - T-D8, the `publish_rows` range is not checked up front;
  - a codec failure probably reports a transport-dependent status (not verified);
  - C++'s staging copy is not on the surface note.

  The first two are confirmed here; the last two are still open as recorded.
- D-BIND-60's lock-across-throw finding: B1 gives a cause, and the confirming experiment is owed.
- The intermittent XRCE and gateway failures from BIND-4 have not recurred in this range's CI.

## Re-graded at close-out (`812d8c2`, 2026-09-29)

Every BLOCKER is fixed and recorded in the code review's resolution log. The five questions were
ruled as D-BIND-64 to D-BIND-67 plus the diagrams, and D-BIND-68 came out of fixing B3 and B4. Each
bullet was re-checked against the tree, not against the log.

| # | Bullet | Was | Now |
|---|---|---|---|
| 1 | The primary shape, as amended by D-BIND-59 | PARTIAL | ✅ **CONFORMS.** B2 (D-BIND-64), B3 and B4 (D-BIND-68) and B7 (D-BIND-66) are fixed, each with a test that fails when its fix is removed. "N rows per native call" holds in code: one `fl_decode_rows_framed` per flush. No test counts the native calls, as before |
| 2 | `PublisherArrow` folds into `Publisher` | ✅ | ✅ |
| 3 | Dictionary re-folding deferred (D-BIND-8) | ✅ | ✅ |
| 4 | The `test_pubsub_arrow` file set | PARTIAL | 🟨 **PARTIAL, narrower.** The three ports that could not see their C++ behaviour are fixed where the code was wrong: B3 now has `ACorruptRowDoesNotCountTowardMaxRows`, C++'s own `max_rows = 2` case, and B4 has `RowsSplitAcrossMessagesAreDroppedNotMisaligned`. **Still open, all test-quality:** T-D1 (`WindowsFollowOneAnother` checks counts, not values in order), T-D2 (`BatchesAreWellFormed` lacks the dictionary and list columns), T-D3 (the options cases assert only `NotSupported`), T-D4 (the schema-watch port checks neither "no data subscription" nor the release), T-D5 (the corrupt-row port uses `[0x00]`, not a truncated row). Each is a port weaker than its C++ case, and none hides a code defect found so far |
| 5 | D-BIND-60: the shim refuses rows of another schema | PARTIAL | ✅ **CONFORMS.** B1 is fixed and confirmed as the cause of D-BIND-60's open finding, and c-abi builds with `/EHs`. D-BIND-65 checks dictionary schemas instead of skipping them, and D-BIND-67 extends the check to topics this publisher did not declare |
| 6 | Rows through the copy oracle | ✅ | ✅. Re-run at 0.7: 5/5. Weakness T-D7 stands (the multi-row form is not oracle-measured) |
| 7 | Attachments, the delivery view | ✅ | ✅. N-D5 and its NIT stand; neither affects the single-publish claim |
| 8 | D-BIND-62's placement | ✅ | ✅. T-D9 and N-D2 stand as debt for BIND-9 |
| 9 | D-BIND-63's ceiling | PARTIAL | ✅ **CONFORMS.** Rule 4 now holds everywhere: D-BIND-66 contains every copy, and the flush no longer copies the window at all. M-D2's false claim is corrected in the digest |
| 10 | Rulings in the four places | PARTIAL | ✅ **CONFORMS.** D-BIND-58 to 68 are each in the digest, the tracker, the development plan and the architecture diagrams, checked by search |
| 11 | The public-surface note agrees with the code | NOT MET | ✅ **CONFORMS.** The class diagram gains `HandlerFaulted`, `BatchReason`, `MaxTimeout` and `AttachmentsBuilder`'s three accessors; the table row gains the empty-key rule. Validated with mermaid + jsdom |

**Outcome: 10 of 11 bullets CONFORM; 1 PARTIAL (bullet 4, five test-quality gaps). No bullet is NOT
MET.** Whether BIND-5 closes with bullet 4 partial, or after T-D1 to T-D5, is the maintainer's call.

**Local evidence at `812d8c2`:** c-abi 93/93, managed 326/326 and the copy oracle 5/5 on net8 and
net10, copy-probe 13/13. The transport suite is 25 of 33, as before; the 8 `xrce` cases need an
Agent this machine does not have. **None of this is on CI yet.** Ten commits are unpushed, and this
is the first CI run for ABI 0.7, `/EHs` and the probe-shim lanes' changes.
