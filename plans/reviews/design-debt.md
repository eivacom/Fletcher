# Design-debt register

Debt accepted at architecture review, handed to the implementer. Full text lives
in the per-item review file; this register is the index, not a copy.

## PDA-DEC-1 — conformance suite (APPROVE-WITH-DEBT(6), cycle 2 of 2)

Review: [PDA-DEC-1-design-review.md](PDA-DEC-1-design-review.md).
Cycle-1 DEBT-1..11 were folded into the design revision (`154a1a2`) and are closed.
Cycle-2 items below are open and owed by the implementer.

| Id | Owed | Where |
|----|------|-------|
| DEBT-C2-1 | **Must land in this PR.** `pubsub/include/fletcher/pubsub/provider.hpp:68` still documents re-declaration rejection as optional ("may reject"); the owner's ruling makes it mandatory. One word plus one Files-to-touch line. | review §DEBT register — cycle 2 |
| DEBT-C2-2 | Key *retention* by provider but leave *schema_mode* per subject, or the promised PDA-DEC-3 handoff collides with rung-1 item 3. | ditto |
| DEBT-C2-3 | State that clause 12 observes overlap and cannot force it. | ditto |
| DEBT-C2-4 | Reuse the conflict comparison already shipping above the seam (`pubsub/src/publisher.cpp:46-78`); note the gateway cannot regress, because `Publisher` never forwards a conflicting or identical re-declaration to the provider. | ditto |
| DEBT-C2-5 | Two `Files-to-touch` entries owed, both one line. (`plans/PDA-decouple-interface.md` is already updated by the PM at `ea5287b`.) | ditto |
| DEBT-C2-6 | Informational, nothing owed — the sparse-checkout gotcha was mis-targeted in the design. | ditto |

## PDA-DEC-2 — copy-accounting oracle (APPROVE-WITH-DEBT(7), cycle 1 of 2)

Review: [PDA-DEC-2-design-review.md](PDA-DEC-2-design-review.md). All seven open
and owed by the implementer. No BLOCKERs; premise **P1** (`WriteBuffer::Data()` is
PDA-DEC-2's to add) is answered YES in the review — do not stop-and-ask on it.

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | The copy definition says "at or above the seam" but nothing above the seam is in a measured path (`Publisher`/`Subscriber` verified pass-through today, but unguarded). Either register a third subject that goes through `Publisher`+`Subscriber` (~25 lines) or narrow the definition and declare the above-seam boundary in the README beside the transport one. Same README line should name the DDS **publish-side** loan path as unmeasured — the existing `Loaned*` tests assert delivery, not absence of copies. | review §DEBT-1 |
| DEBT-2 | Make the row rule `delivered_data == encode_base && delivered_len == encode_len`, not containment. Containment admits an identity-preserving in-place `memmove` to the window base — the one false-pass shape that needs no UB, and `memcmp` cannot catch it. Both registered subjects already satisfy equality. | review §DEBT-2 |
| DEBT-3 | Add premise **P5**: the bytes `encode_base` names stay allocated and unfreed until the callback returns. That liveness is what makes provenance immune to allocator reuse, and PDA-ABI will register a driver-backed subject into this shape. | review §DEBT-3 |
| DEBT-4 | P3's stop condition is wrong: a by-value `Attachments` map copy would leave leg 2 **green** (the `Blob`s' `data()` is unchanged), which is the correct answer under the design's own definition. Restate the consequence — a map copy is an allocation finding raised in prose, not a red leg. | review §DEBT-4 |
| DEBT-5 | Pin the 4 KiB row to many sub-`kChunk` appends. A single `Append(4096)` takes `VectorWriteBuffer`'s bulk path, relocates nothing, and reports `refill_bytes == 0` — leaving Brief Decision 1 unevidenced and the 4 KiB leg indistinguishable from the 64 B one. | review §DEBT-5 |
| DEBT-6 | `Data()`'s lifetime doc must also name `VectorWriteBuffer::Finish()` as invalidating, and bound the defined range to `[Data(), Data() + Position())`. | review §DEBT-6 |
| DEBT-7 | Brief Decision 1 cites §3.1 clause 4, which does not say bytes may move. The clause that sanctions it is §3.1 **clause 1** ("must not move … **except inside a refill**"). Swap the citation so the owner sees that recommendation (b) ratifies the oracle rather than trading against it. PM-facing. | review §DEBT-7, §"question 5" |

## PDA-DEC-3 — crossing vocabulary (NEEDS-REWORK, 4 BLOCKERs, cycle 1 of 2)

Review: [PDA-DEC-3-design-review.md](PDA-DEC-3-design-review.md). The nine items
below are DEBT and do **not** loop the design; the four BLOCKERs are in the review.
Two rulings the implementer may rely on without re-asking: premise **P2** is
answered YES (§3.3 does *not* require reshaping `SharedSchema`), and the Fast DDS
loanable-read residue is an in-scope reduction, **not** a decision-7 stop-and-ask.

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | `PubSubError` can be constructed with `PubSubStatus::kOk`, so a C boundary translates a failed call to success — silently. Forbid it at construction. | review §DEBT-1 |
| DEBT-2 | "Forever is `milliseconds::max()`" overflows `condition_variable::wait_for` on common implementations. Specify `wait_until(steady_clock::time_point::max())` or clamp. | review §DEBT-2 |
| DEBT-3 | Decision 5 wants the rule in the header, but `core/include/fletcher/core/write_buffer.hpp` and `pubsub/include/fletcher/pubsub/owned_schema.hpp` are missing from `Files-to-touch`. Two lines. | review §DEBT-3 |
| DEBT-4 | `fastdds-pubsub-provider/benchmarks/` builds `Blob`s and reads `result.schema`, is not in `Files-to-touch`, and is outside CI — so it rots without going red. Migrate it or say it is knowingly left broken. | review §DEBT-4 |
| DEBT-5 | The new `SchemaMode` collides by name and enumerator with `fletcher::conformance::SchemaMode` (`conformance/subject.hpp:40`), which shadows it in `inprocess_main.cpp`. Rename the provider's axis. | review §DEBT-5 |
| DEBT-6 | P3's XRCE evidence is the wrong artefact — `ts.pending` holds *copies*; the shared-owner candidate is the per-sample local `payload` vector at `xrce_dds_pubsub_provider.cpp:178`. Premise holds; fix the citation. | review §DEBT-6 |
| DEBT-7 | When the pin flips to 0, the conformance README must keep saying the number is about the seam's capability, never about a transport's receive path (`Envelope::row` is still copied per sample). | review §DEBT-7 |
| DEBT-8 | Forward note for PDA-DEC-5: `SchemaMode` must arrive through §4.1's opaque document, not a second construction API. | review §DEBT-8 |
| DEBT-9 | "The triple *is* the C form" risks a layout reading of `shared_ptr<const void>`. One sentence: conceptual only, no layout compatibility, the boundary *constructs* a `Blob`. | review §DEBT-9 |
