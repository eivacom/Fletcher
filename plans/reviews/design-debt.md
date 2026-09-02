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

### Cycle 2 (`078ed4a`) — APPROVE-WITH-DEBT(9), no BLOCKERs

B1/B2/B3 closed; B4 closed by the PM (waiver 7 additions / 5 simultaneous
retirements). Cycle-1 DEBT 1-7 and 9 are folded into revision 2 and **closed**;
DEBT-8 is carried forward below as C2-9. The nine items below are open and owed by
the implementer.

| Id | Owed | Where |
|----|------|-------|
| C2-1 | **Must land in this PR.** Replace "latched at its first delivery" with the rule stated beside it — *a declaration made after a subscription exists never reaches that subscription; the schema is fixed when `Subscribe` returns and equals what its `SchemaArrival` reports*. **And add a named test:** no subject reaches this path (`InProcessLocal` is subject-axis `kAbsent`, so the loopback is never handed a real schema; only the gateway exercises it, and it has no subject). ~20 lines in `seam_vocabulary.cpp`. | re-review §B3, §C2-1 |
| C2-2 | `Resolve(nullptr)` is not refused, so `kOk`+null — reserved for schema-less transports — is reachable from a carrying provider, re-opening B1's conflation. Forbid: `Resolve` refuses a null schema; `Ready(nullptr)` stays the only producer. | re-review §C2-2 |
| C2-3 | Enum comment claims "8/9 are §2 outcomes, never thrown", but `PubSubError` refuses only `kOk`/`kPending`. Refuse `kSubscriptionEnded` too, or drop the claim. | re-review §C2-3 |
| C2-4 | Negative timeout is refused only "at a C boundary"; a negative `std::chrono::milliseconds` silently polls. Make `Wait` refuse it with the same status. | re-review §C2-4 |
| C2-5 | Say whether `wait`'s `*out` owner handle is a new reference the caller releases or a borrow. Not an interop hazard (each round writes both sides of its own boundary) — an idiom gap. | re-review §C2-5 |
| C2-6 | Reconcile "C form, pinned" with §1's "their C shapes may differ freely": semantics pinned, spelling illustrative, each round picks its own names and layout (decision 2). | re-review §C2-6 |
| C2-7 | `ImportArrowSchema` is now public API; state its behaviour for a null or release-less schema (`ImportFromNano` returned `nullptr`). | re-review §C2-7 |
| C2-8 | `kOk`+null does not say whether `*out` is written null or left untouched, unlike the other outcomes. One word. | re-review §C2-8 |
| C2-9 | Carried forward (cycle-1 DEBT-8): forward note for PDA-DEC-5 — `SchemaCarriage` must arrive through §4.1's opaque document, not a second construction API. | review §DEBT-8 |

## PDA-DEC-4 — provider registry (APPROVE-WITH-DEBT(10), cycle 1 of 2)

Review: [PDA-DEC-4-design-review.md](PDA-DEC-4-design-review.md). No BLOCKERs. Two
rulings the implementer may rely on **without stop-and-asking**: premise **P2** is
answered — §4 clause 2 is satisfied under *both* readings, because `SetPathResolver`
lands and is exercised in this item, so PDA-ABI adds no registry method (review §A);
and rung-1 item 1's "unrepresentable" is an overclaim whose *substance* nevertheless
holds (review §B). Surface waiver (5 vs 3) is the PM's; the config already names this
item as the expected exception.

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | **The substantive one.** The resolver seat has no lifetime rule, while the design sanctions destroying the registry with providers still live. One normative sentence beside `SetPathResolver`: a resolver must keep everything the returned provider depends on — including a loaded module — alive as long as that provider lives, independently of the registry's and the resolver's lifetime. Otherwise PDA-ABI's natural module cache unloads a `.so` under a running provider. | review §DEBT-1 |
| DEBT-2 | Make the §4 clause 2 amendment freeze the **whole** registry surface (`Create`, `Register`, `SetPathResolver`; PDA-ABI adds no method, it calls one), and rewrite P2 to record the ruling instead of a stop-and-ask. | review §DEBT-2 |
| DEBT-3 | Pin the whole signature, not the return type: `static_assert(std::is_same_v<decltype(&ProviderRegistry::Create), std::shared_ptr<PubSubProvider> (ProviderRegistry::*)(const ProviderSelector&, const ProviderConfig&) const>)`. A return-type assert cannot see a defaulted third parameter or a dropped `const` — the exact widening this item exists to prevent. | review §DEBT-3 |
| DEBT-4 | A second `SetPathResolver` silently replaces the first. The design's own case-10 reasoning applies verbatim — refuse it with `kInvalidArgument`, or say why replacement is legal. | review §DEBT-4 |
| DEBT-5 | State the **selector's** C form (§3.5: pointer + length, borrowed, length authoritative) as the design already does for `document` — §4 selection is binding-visible surface — and refuse an embedded NUL at `Parse`, or a length-carrying binding hands PDA-ABI's `dlopen` a truncated path that loads a different library with no signal. | review §DEBT-5 |
| DEBT-6 | Soften rung-1 item 1: the classification rule is public and pure, so a caller can re-derive name-vs-path (== built-in-vs-loaded) in one line, and RTTI on the returned provider is always available. Claim instead that the seam offers no way to ask and no caller has cause to; what decision 3 guarantees is that the built-in → loaded move is a **config** edit, never a caller edit. | review §DEBT-6 |
| DEBT-7 | Forbidden case 5's machine check is "no **transport SDK** is reachable", not "no provider header": `in_process_provider.hpp` is inside `fletcher-pubsub` and PDA-DEC-5 will link it into this very suite. | review §DEBT-7 |
| DEBT-8 | `kNotSupported`'s message should say the selector was classified as a *path* and name the first offending character/offset — trailing whitespace or a CRLF from a config file is the realistic misclassification, and "this build cannot load drivers" reads as an infrastructure fault rather than a typo. | review §DEBT-8 |
| DEBT-9 | Forward note for PDA-DEC-7: the seam's `domain_id` is `uint32_t`, `XrceConfig::domain_id` is `uint16_t`. Refuse out-of-range, never narrow — a truncated domain id is a wrong answer with no error. | review §DEBT-9 |
| DEBT-10 | Records: (a) "providers are constructed at four sites in tree (§10)" is false — §10 counts four *config-consuming files* and predates PDA-DEC-1/2/3; PDA-DEC-6/7 should re-measure. (b) `provider.hpp:68-70` still documents configuration as "a provider-specific Options struct"; update here or book against PDA-DEC-6/7. | review §DEBT-10 |

## PDA-DEC-5 — InProcess as a built-in (APPROVE-WITH-DEBT(5), cycle 1 of 2)

Review: [PDA-DEC-5-design-review.md](PDA-DEC-5-design-review.md). No BLOCKERs.
Two rulings the implementer may rely on **without stop-and-asking**: premise
**P1 does not fire** — `TranslateSeamFailure` rethrows a `PubSubError`
untouched (`core/status.hpp:136`) and `Registry.AFactoryThatFailsIsReportedAsA
TypedSeamFailure` already asserts it, so the bad-document refusal arrives as
`kInvalidArgument` (the reader must throw `PubSubError`, not a `std::` type);
and premise **P3's stop-and-ask does not fire** — the loopback reading its own
document inside `fletcher-pubsub` honours decision 8, the provider does **not**
move to its own component, and the reader must stay unshared and dependency-free
in `in_process_provider.cpp` (review §B).

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | **The substantive one.** B.1's named mutation "register `inprocess` in `carried` mode → the schema/publish cases go red" is false: every publish in `end-to-end.test.ts` is preceded by `createTopic(topic, SCHEMA)` (lines 225/237, 335/353, 408), and the one undeclared-subscribe case (170-189) asserts the *absence* of a schema, which a pending `kCarried` arrival also satisfies. The battery cannot see the mode. Correct the claim — the load-bearing mutation is "drop the registration"; the mode proof is `Registry.InProcessCarriageComesFromTheDocument`. If the gateway's mode is to be pinned, one ~10-line case (publish to a never-declared topic produces no error frame) is the only difference the battery could see. | review §C, §F-1 |
| DEBT-2 | Pin the `--provider bogus` staleness assertion to wording the pre-change binary cannot emit (`available:` / `no built-in provider named`) — "names the registered providers" is satisfied by the old `unknown provider: … (expected inprocess\|fastdds)`. Give the new block's `READY` case its own port; `TEST_PORT` and `TEST_PORT+3` are held by the two `describe.each` contexts. | review §C, §F-2 |
| DEBT-3 | Rung-2 case 6 lists four refusals but names three test inputs; add a duplicate key (`schema_carriage=as_declared\nschema_carriage=carried`) to `InProcessRefusesAnUnrecognisedDocumentEntry`, or that rule is asserted by nothing. | review §F-3 |
| DEBT-4 | Undisclosed narrowing: a Fast DDS construction failure escapes `main` uncaught today (no try around `main.cpp:109-116`) and aborts; after this item it exits 2 with a message. An improvement, but "observably unchanged" should say it. Put `Parse`, both `Register` calls and `Create` inside the one `try`. | review §F-4 |
| DEBT-5 | Record that the gateway has no route for a provider document (it passes `""`), so charter requirement (b) is unreachable from `gateway.exe` after this item. Correct here; PDA-DEC-6 cannot move Fast DDS QoS into the document without that surface, and no item currently owns it. | review §F-5 |
