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

## PDA-DEC-6 — Fast DDS by document (NEEDS-REWORK, 3 BLOCKERs, cycle 1 of 2)

Review: [PDA-DEC-6-design-review.md](PDA-DEC-6-design-review.md). The eleven items
below are DEBT and do **not** loop the design; the three BLOCKERs are in the review.
Two rulings the implementer may rely on without re-asking: **P1's API half is
verified** — `get_participant_qos_from_xml` / `get_datawriter_qos_from_xml` /
`get_datareader_qos_from_xml` all exist in fast-dds/3.4.0 with the `profile_name`
overload and the documented `RETCODE_BAD_PARAMETER`-for-both contract, so do not
re-derive that; and **`<propertiesPolicy>` for `loan_publish` / `max_schema_bytes`
is approved** (review §R2) — it honours decision 8 rather than blurring it, and both
settings are kept (Brief decision 3(a)).

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | **Must land in this PR.** The forcing table has no row for the *most common* document: an anchor-only, non-empty document (`fletcher_participant` present, no writer/reader profile) must resolve to **Fletcher's** built-in QoS, not Fast DDS's. Every `fletcher.*`-property document has this shape. A provider that fell back to `DATAWRITER_QOS_DEFAULT` whenever the document is non-empty passes all three rows as written and loses rows in production. One extra row: anchor-only document → discovered TRANSIENT_LOCAL + RELIABLE. | review §"one sentence", §R4-1 |
| DEBT-2 | Assert **discovery observed** per row (a latch with a hard timeout) before comparing values. Row 2 expects VOLATILE, which equals a default-constructed `DurabilityQosPolicy`, so if that one row's callback never fires the row is vacuous while the test stays green. Rows 1 and 3 are immune, so only the per-row flake hides. | review §R4-2 |
| DEBT-3 | Convert P1's global-state half from a bet into a measurement **in this item** (~15 lines): two providers alive at once, same profile names, different values, each must resolve its own. The Conan package is binary-only, so no header can prove "registers nothing process-wide", and the failure mode is not always loud — a stale singleton entry hands instance B instance A's QoS, a wrong answer rather than a refusal. PDA-DEC-8 is too late to learn this. | review §R1 |
| DEBT-4 | Dropping `transitive_headers=True` breaks `fastdds-pubsub-provider/benchmarks/` — all four TUs (`exp_zero_copy.cpp`, `bench_pub_sub_type.cpp`, `example_arrow_roundtrip.cpp`, `legacy_fletcher_topic_type.hpp`) include `<fastdds/...>` directly, and `benchmarks/conanfile.py` neither requires fast-dds nor does `benchmarks/CMakeLists.txt` find/link it. Add both files to `Files-to-touch` (`self.requires("fast-dds/3.4.0")`, `find_package(fastdds)` + link) or say the benchmarks are knowingly left broken. Recurrence of PDA-DEC-3 DEBT-4 — benchmarks are outside CI, so this rots without going red. | review §R5 |
| DEBT-5 | Two build files missing from `Files-to-touch`: `fastdds-pubsub-provider/tests/CMakeLists.txt` (the new TU `test_profile_document.cpp` must be added to `fastdds_pubsub_tests`; it already links `fastdds` explicitly, so nothing else changes) and `integration-tests/pubsub-conformance/CMakeLists.txt` (see DEBT-6). | review §R5 |
| DEBT-6 | `conformance_registry`'s link line is **deliberately narrow** — "it needs no fixture, no subject and no child, and a narrower link line is a stronger check" (`pubsub-conformance/CMakeLists.txt:205-213`), which is PDA-DEC-4 DEBT-7's "no transport SDK is reachable" guard. Registering `fastdds` there links the Fast DDS SDK into that TU and weakens it. Put `Registry.FastDdsResolvesAsABuiltIn` in `conformance_fastdds` (already links the provider, already `RESOURCE_LOCK`ed with a 180 s timeout) or in its own executable, and keep `conformance_registry` narrow. | review §R5 |
| DEBT-7 | Stale-comment cleanup missing from `Files-to-touch`, all naming the retired type: `pubsub/include/fletcher/pubsub/provider.hpp:72-77` (**booked against this item by PDA-DEC-4 DEBT-10(b)**), `fastdds-pubsub-provider/src/internal/{data_writer_listener,data_reader_listener,sample_writer}.hpp`, and `integration-tests/{pubsub-arrow-fastdds,fastdds-xrce-interop}/README.md`. | review §R5 |
| DEBT-8 | `<propertiesPolicy>` honesty residues: (a) say whether the two `fletcher.*` properties are stripped before `create_participant` or left in the QoS (Fast DDS ignores unknown property names, so either is safe — but the round's idiom is "consume what you own"); (b) README warning that `<propagate>true</propagate>` on a `fletcher.*` property puts a Fletcher key into DDS participant discovery data. Neither is a wire-byte change under decision 13. | review §R2 |
| DEBT-9 | The mandatory anchor makes **H4 universal**: every non-empty document must carry a participant profile, and an anchor empty of policies drops the `FletcherParticipant` name. Verified diagnostic-only — nothing in the tree keys on that name (only `fast_dds_pubsub_provider.cpp:200` sets it) — so one README line, but H4 should stop reading like an exotic case. | review §R3 |
| DEBT-10 | M7 is not a standalone mutation: if the provider ignored the document entirely, editing the README block apart would still leave both instances equal and green. BLOCKER B3's whole-struct fix makes it valid unconditionally; correct the mutation column when B3 lands. | review §R4-3 |
| DEBT-11 (taken in rev 2) | Correct spec §10's table, do not merely mark it done: it says 4 files / 19 occurrences, and both halves are now wrong — `test_interop.cpp` carries 3 occurrences (not 9) and four further files construct the provider (both conformance subjects, `test_package/src/example.cpp`, `benchmarks/exp_zero_copy.cpp`). PDA-DEC-7 will cite the same table for XRCE. Closes PDA-DEC-4 DEBT-10(a). | review §R5 |

### Cycle 2 (`2456e76`) — APPROVE-WITH-DEBT(5), no BLOCKERs

B1, B2 and B3 are **closed**; cycle-1 DEBT-1…11 are all folded into revision 2 and
**closed**. Three rulings the implementer may rely on without re-asking: the
whole-struct transcription equality is **total** (`DataWriterQos::operator==` and
`DataReaderQos::operator==` each compare 22 of 22 members, and `RTPSEndpointQos::operator==`
carries `history_memory_policy`, so the `CanLoanSamples` gate is covered);
`TwoInstancesResolveTheirOwnDocuments` **does** detect process-global registration in
both its failure modes; and the `<domainId>0</domainId>` residue is **accepted** —
it cannot be forbidden and the alternative makes documents non-portable across
domains. The five items below are open and owed by the implementer.

| Id | Owed | Where |
|----|------|-------|
| C2-1 | **Must land in this PR. The fifth silence: the policies a supplied profile omits** — i.e. Brief decision 2 itself, which no test in the item can currently distinguish an answer to. (a) Mandate the implementation form in §2: *every `get_*_from_xml` call is made into a freshly default-constructed QoS; the built-in default is applied only on the not-found branch, never as the call's input.* Correct under either substrate behaviour, so it needs no seventh premise. Without it, the natural ladder (`qos = MakeFletcherDefault…(); if (get_…(doc, qos, name) != OK) keep;`) silently implements answer **(b)**, merge semantics, if the API overlays onto what it is handed — and nothing goes red. (b) One in-process assert beside the transcription test: a minimal writer profile setting only `durability = VOLATILE` must resolve to `history()` = Fast DDS's `KEEP_LAST(1)`, **not** Fletcher's `KEEP_ALL`. That assert is the only thing in the item that tells (a) from (b). | re-review §C2-1 |
| C2-2 | **Must land in this PR. The sixth silence: the reader's.** All four omission-guards read *publication* data; nothing asserts that a document supplying a writer profile (or only the anchor) leaves the READER on `MakeFletcherDefaultReaderQos()`, whose `data_sharing().off()` is the one line holding back the measured receive-side row-loss defect (`qos_defaults.cpp:45-68`). `gateway-fastdds-ts` does not cover it either — it runs with an empty document. One row: writer-only document → the reader's discovered `data_sharing` is OFF and durability TRANSIENT_LOCAL. Available: `SubscriptionBuiltinTopicData` carries all three (lines 74/89/142) and the test already listens on `on_data_reader_discovery`. | re-review §C2-2 |
| C2-3 | Sharpen P6's mutation-column sentence. "If the P6 row cannot be made red, P6 is false" has two readings (red under mutation M8, versus red against the unmutated build). Both lead to "stop", so it is not dangerous — but a stop-condition should not offer a choice. Say instead: *this row must PASS against the correct implementation; if it fails, the document was accepted, P6 is false — STOP-AND-ASK, and do not delete or weaken the row.* | re-review §B1 |
| C2-4 | State the domain rule in the README **positively**, not as a residue to infer: *the deployment's domain always wins; an anchor's `<domainId>` must either match it or be absent, and an explicit `0` cannot be told from absent.* Consider having the rung-2 case 7 refusal message say the same, since that is where an operator meets the rule. | re-review §B2 |
| C2-5 | §3's new promise — the two `fletcher.*` properties are stripped before `create_participant` and "every other property reaches Fast DDS untouched: security plugins need that" — is asserted by nothing. An over-reaching strip silently drops `dds.sec.*` and the participant comes up unsecured with no error. The strip is a testable unit (the test TU already puts `../src` on the include path, `tests/CMakeLists.txt:12-13`): one assert that a foreign property survives and the two `fletcher.*` ones do not. | re-review §"fifth silence" |

## PDA-DEC-7 — XRCE by document (NEEDS-REWORK, 1 BLOCKER, cycle 1 of 2)

Review: [PDA-DEC-7-design-review.md](PDA-DEC-7-design-review.md). The nine items below
are DEBT and do **not** loop the design; the single BLOCKER (B1 — four of six document
keys have no guard that an accepted value lands anywhere) is in the review.

Five things the implementer may rely on **without re-asking**, all verified against the
tree in review cycle 1 — do not re-derive them:
- **P5 holds.** `max_payload` is read nowhere: header declaration + comment,
  `README.md:21,53,66`, `test_xrce_provider.cpp:52,63,71`, and **nothing in `src/`**. Same
  for `serial_device` / `serial_baudrate` (header + `README.md:64-65`). Delete both.
- **P3 holds, exactly.** `kPayloadBytes<64*1024>` is 65536 (`payload_bound.hpp:56-58`),
  `XrceConfig::payload_bound`'s default is that value (`…hpp:37`), and **no in-tree caller
  sets it** (`xrce_main.cpp:71-79`, `test_interop.cpp:107-117`, `xrce_peer_main.cpp`). So
  `max_payload_bytes == 0 → 65536` reproduces `FletcherTypeName(65536)` bit-for-bit. No
  wire change, no stop-and-ask.
- **P2 holds.** `TranslateSeamFailure` rethrows a `PubSubError` untouched
  (`core/status.hpp:136`) and turns any other `std::exception` into `kInternal` (`:145`);
  `PubSubError` derives from `std::runtime_error` (`:97`), so the surviving
  `EXPECT_THROW(…, std::runtime_error)` rows do stay green. `kNotSupported` (6) and
  `kTransportFailure` (5) exist and are frozen.
- **§4.1's disclosure clause: nothing is owed.** All three deferral routes were checked
  (Agent-dependent key, transport failure on first publish, anything the factory reaches
  after construction). No XRCE key is topic-scoped; `domain_id` is typed core, not a
  document key. The design's structural claim is correct as written.
- **The unshared reader is forced, not convenient.** Decision 8 verbatim
  (`PDA-decouple-locked-decisions.md:68-74`) plus the in-tree precedent at
  `in_process_provider.cpp:44-47` (the same call already made for a one-line helper).
  Do **not** propose a shared reader; it is a stop-and-ask.

**P1 was not machine-verifiable here** (the client is FetchContent'd, no source on disk).
Everything reachable is consistent with it. Confirm it in step 3's first ten minutes,
before writing the listener — its stop condition changes the item's shape.

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | **The substantive one.** `PublishedDefaultsAreExact` reads `README.md` off disk, but `xrcedds-pubsub-provider/conanfile.py`'s `exports_sources` does **not** include `README.md` and `conanfile.py` is not in `Files-to-touch` — so under `conan create` (the CI build mode, and the design's own mandated full run) the file does not exist. PDA-DEC-6 hit this and fixed it exactly once: `fastdds-pubsub-provider/conanfile.py:33-38`, *"README.md is exported for the TESTS, not for documentation"*. Add the export and the `Files-to-touch` line. **And mandate the failure mode:** unreadable → hard failure with the path in the message (`test_profile_document.cpp:595-598` is the shape), never `GTEST_SKIP`, and read at **run time** — a `configure_file`/`file(READ)` bake-in re-creates PDA-DEC-6's held-copy defect the disk read exists to kill. | review §B1 area, §DEBT |
| DEBT-2 | §6's row 2 is a **harness control, not a build guard**, and the mutation column overstates it: "M2 hard-code tcp+port → row 2 red" is unachievable, because the port is ephemeral and chosen at run time, so no build can hard-code it. A build that hard-coded *tcp* while reading the address from the document passes rows 1-3. State row 2's real job (no stale accept latch, no cross-row or process-global state) and let row 1 carry the falsifiability — it does, strongly. Also note the row is environment-sensitive: an Agent on UDP 2018 (the interop suite's port) changes what row 2 *does*, though not what it asserts. | review §DEBT |
| DEBT-3 | §1's "includes only `<fletcher/pubsub/provider.hpp>`" is wrong: `ProviderConfig` and `ProviderRegistry` are declared in `provider_registry.hpp` (`:100`, `:136`), which is why `fast_dds_pubsub_provider.hpp:19-20` includes both. And **keep `payload_bound.hpp`** if the new header keeps advising `kPayloadBytes<N>` — dropping it while keeping the advice is PDA-DEC-6 review 4a F7 verbatim, an out-of-tree TU that takes the advice fails to compile (`fast_dds_pubsub_provider.hpp:13-18` records the fix). | review §DEBT |
| DEBT-4 | The one-colon `agent` rule makes **IPv6 unrepresentable** (`[::1]:2018` and `::1` both refused). This matches today's behaviour — the client is initialised `UXR_IPv4` at `xrce_dds_pubsub_provider.cpp:362,369` — so it is not a regression, but as written it is an **undisclosed** foreclosure sitting oddly beside H1's "Fletcher does not know what that client's resolver accepts". One README/H-list line: IPv4 literals and hostnames only; IPv6 is a separate change that also has to move off `UXR_IPv4`. | review §DEBT |
| DEBT-5 | "M1/M3 also redden all 27" over-claims. The 3 interop tests run their Agent on the **default** port 2018 with the default transport (`test_interop.cpp:69-71`), and their only distinguishing setting is `domain_id` — which is **typed core, not the document**. So the document's end-to-end witness is the 24 `conformance_xrce` cases (Agent on 2019, `xrce_main.cpp:42-43`), and only they. Correct the count to 24 and say what the interop three actually witness (the typed core and the type name). | review §DEBT |
| DEBT-6 | Give built-in **registration** an Agent-free witness: route `AgentUnreachableIsATransportFailure` through `RegisterXrceProvider(registry)` + `Create("xrce", cfg)` rather than a direct constructor call. Registering under the wrong name then reddens a row in the provider's own CI instead of only in the Agent-gated suite, and it makes M8's "arrives as `kInternal` through the factory" literally true. `Registry.XrceResolvesAsABuiltIn` keeps the row-delivery half. ~5 lines. | review §DEBT |
| DEBT-7 | **PM-facing, before the owner is asked.** All three brief decisions are answerable from the ledger/spec: (1) option (b) is new behaviour, outside the 2026-09-01 split ruling's "prepare, don't develop"; (2) the brief itself cites the 2026-09-02 PDA-DEC-4 ruling that answers it; (3) is **already in spec §4.1** as landed, and its non-default option would force a spec amendment and split one format into two dialects. Strike or reframe as confirmations. | review §"brief" |
| DEBT-8 | Net-lines realism: 1400 is the right order of magnitude and honestly derived, but the two largest line items are each ~1.5× under for this tree's comment density — the reader (~180 vs the loopback's ~90 for a *single* key) and the refusal table (~200 for 15-20 rows, against `test_profile_document.cpp`'s 1089 lines for the comparable job). Expect **~1900-2100** including B1's fix, the `conanfile.py` export and the extra `conformance_xrce` case. Also: the "11 → ~17 ctest entries" figure is high — 10 − 4 retired + 6 new + the MSVC nodiscard probe is 13 unless the refusal table splits per key. | review §DEBT |
| DEBT-9 | Forcing-test wall clock. Row 1 should set `connect_timeout_ms=0` (→ 0 retries, one attempt: `xrce_dds_pubsub_provider.cpp:385-387`) or it costs ~3 s after the accept. Row 2 **cannot** shorten its own budget — an empty document sets nothing — so it pays the full default 3000 ms of `uxr_create_session_retries`. Budget the case at ~4-5 s and say so, since the retired `SerialTransportNotImplemented` / `ConstructorThrowsWithoutAgent` pair the README calls out at ~1 s each is what it replaces. | review §DEBT |

### Cycle 2 (`c50f7e5`) — APPROVE-WITH-DEBT(7), no BLOCKERs

**B1 is closed** and cycle-1 DEBT-1…9 are all folded into revision 1 and **closed**.
Four rulings the implementer may rely on without re-asking, on top of cycle 1's five:
the whole-struct row `EveryKeySetNonDefaultLandsWholeStruct` **does** close the reader
boundary for all four keys (the defaulted `operator==` is total over the fields that
exist, and the row's document sets all four away from their defaults); the
`stream_history` / `run_loop_ms` **deletion is approved and adequately disclosed** — do
not re-propose keys for them, and do not treat their in-provider reads as a reason to
stop (see C2-1); the demotion of the empty-document row to a harness control is
**correct** — row 1's ephemeral port carries the falsifiability alone; and
`Registry.XrceResolvesAsABuiltIn` belongs in `conformance_xrce`, following
`subjects/fastdds_main.cpp:68`, never in `conformance_registry`.
The design is at its 300-line cap, so **every item below lands in code, tests or the
README — not in the design doc.**

| Id | Owed | Where |
|----|------|-------|
| C2-1 | **Must land in this PR — as written, P5 tells step 3 to stop on a deletion the design already reasoned through.** P5 groups all four names as "set nowhere and **read nowhere** in `src/`" and orders **STOP-AND-ASK** "on any read or caller". That is false for two of them: `stream_history` is read at `xrce_dds_pubsub_provider.cpp:352` (buffer sizing) and `:393`/`:396` (the `history` argument to `uxr_create_{output,input}_reliable_stream`), and `run_loop_ms` at `:412`. Split it: (a) `max_payload`, `serial_*` — set and read nowhere (verified cycle 1), so deleting is a no-op; (b) `stream_history` / `run_loop_ms` — **read** by the provider at those lines, but set only by the two tests this item retires (`test_xrce_provider.cpp:64-65`, which echo the field back) and observed by nothing, so they become same-valued constants: a disclosed narrowing, not a no-op. **STOP-AND-ASK only if a production caller sets one or a test observes one.** | re-review §B1 leg 1 |
| C2-2 | **The one mutation the revision still lets through.** `connect_timeout_ms` is asserted only at the reader boundary: a build whose `ParseXrceDocument` is correct but whose constructor passes the old constant `3000` to `uxr_create_session_retries` (`:388`) is green everywhere — the whole-struct row never reaches the constructor, §6 row 1 still accepts and still fails `kTransportFailure` (just ~3 s later), and the 24 conformance cases set 5000 only as headroom. ~3 lines: have row 1 bound its own wall clock — with `connect_timeout_ms=0` the retry count is 0 (`(0-1)/1000`, `:385-387`), so the constructor must fail well inside the ~3 s an ignored document costs (assert elapsed < ~1.5-2 s, generously). Same item: carry the retiring header's granularity note (`…hpp:39-41`, "retries in ~1000 ms increments, so effective granularity is one second") into the README, since the key now advertises a millisecond knob whose bottom 1000 ms are indistinguishable. | re-review §B1 leg 3 |
| C2-3 | The demoted empty-document control is now the **only** place an empty document is constructed, and it asserts only "no connection" — so H2 ("empty document = every published default", §4.1's fixed meaning) is asserted by nothing, and a build that refused an empty document with `kInvalidArgument` passes every row. Loud rather than silent, hence debt; one line, zero extra cost: the control row also asserts the failure is **`kTransportFailure`**, not `kInvalidArgument`. That makes the demoted row carry H2's only witness. | re-review §"demotion" |
| C2-4 | §7's "a field added later and left unassigned is caught without editing the row" is **false** — a fifth field the parser forgets compares default-against-default in the expectation and stays green. A defaulted `operator==` is total over the fields that *exist*; what polices growth is §2's rule that a key arrives **with** its witness. Reword rather than rely on it. | re-review §B1 leg 3 |
| C2-5 | `AgentUnreachableIsATransportFailure` must name an unreachable port explicitly. The row it replaces uses `127.0.0.1:19999` + `connect_timeout_ms=200` (`test_xrce_provider.cpp:122-129`); the design does not say what document the replacement uses. With the default (`127.0.0.1:2018`) an Agent on 2018 — the interop suite's own port — makes construction **succeed** and the row goes red for the wrong reason, and it pays 3 s instead of ~1 s. Use `agent=127.0.0.1:19999` + `connect_timeout_ms=0`. | re-review §"mutations" |
| C2-6 | **Owner-facing.** Recorded decision 1's authority — "the round's delete-first lean default" (design §2, §8.1; Brief decision 1) — names no oracle: grepped, it appears nowhere in the spec, the locked decisions or the rulings ledger, only in review prose about coexistence bridges. The decision is authorized; cite what actually authorizes it: cycle 1's verified P5 (nothing in `src/` reads `max_payload`, so nothing observable changes) **and** the 2026-09-01 split ruling ("It should not do any development on any of the ABIs, only prepare for them"), which puts "make the cap real" outside the round. Fix the Brief before the owner reads a norm as a ruling. (§8.3's "as landed by PDA-DEC-6" → PDA-DEC-5 was corrected in place.) | re-review §"three decisions" |
| C2-7 | `xrcedds-pubsub-provider/README.md:101`'s test-duration note names both retired tests and attributes ~1 s to `SerialTransportNotImplemented`, which cannot cost that — the serial arm throws at the transport switch (`:375-376`) before any session attempt; the ~1 s is `ConstructorThrowsWithoutAgent`'s single UDP attempt. The README rewrite replaces that note with the new wall-clock story (the 4-5 s forcing case). | re-review §NITs |

## PDA-DEC-8 — multi-instance proof (NEEDS-REWORK, 3 BLOCKERs, cycle 1 of 2)

Review: [PDA-DEC-8-design-review.md](PDA-DEC-8-design-review.md). The six items
below are DEBT and do **not** loop the design; the three BLOCKERs are in the review
(B1 unequal payload bounds make the two instances undiscoverable on any domain, so
M1 cannot redden and the positive control cannot cross; B2 an oversized row is not
refused on the configured publish flow, so §2's assertion 3 is red on the
unmutated tree; B3 the mutation gate has no clean-environment precondition and two
rows plausibly kill the process, orphaning an shm segment).

Nine things the implementer may rely on **without re-asking**, all verified against
the tree in cycle 1 — do not re-derive them:
- **The oracle citation is exact.** §4's third normative item is verbatim as the
  design quotes it (`docs/pubsub-interface-spec.md:359-362`), and §4 clause 2's own
  prose ("**Clause 3** sanctions destroying a registry while its providers run",
  `:335`) confirms the spec numbers these items as clauses.
- **Placement is right and smuggles nothing.** `conformance_registry` links
  `fletcher-pubsub` + gtest only (`CMakeLists.txt:209-213`); `conformance_fastdds`
  already links the provider (`:221-229`) and already carries `RESOURCE_LOCK
  conformance_fastdds` + `TIMEOUT 180` (`:296-297`), applied per discovered entry —
  so **P3 holds**.
- **The suite arithmetic is right.** 81 discovered entries across the six
  `build/*_tests-Release.cmake` files + 1 `add_test(conformance_xrce)` = 82 today;
  three cases into a `gtest_discover_tests` target = 85 entries. Entries and cases
  are not conflated.
- **The domain census is accurate.** The tree uses 0, 7, 43, 91–99, 137, 145,
  151–153; 154–158 are unused. Do not re-survey.
  **PM correction 2026-09-04 (A5 implementation, verified in the tree): 154 is NOT unused** —
  `integration-tests/gateway-end-to-end/test/end-to-end.test.ts:360` takes it. The census was
  wrong, and "do not re-survey" is what made it load-bearing. Unused: **155–158**. A5 took 155
  (Fast DDS) and domain 153 with session base `0x55000000` (XRCE).
- **P2 holds:** `test_profile_document.cpp:551-552` really does stand up two
  providers in one process.
- **Forbidden case 5 holds:** `ProviderRegistry` has no static member, no free
  function with storage, and `Create` is `const`
  (`provider_registry.hpp:277-287`).
- **The scratch buffer is correctly identified.** `fast_dds_pubsub_provider.cpp:382`
  is the provider's only `thread_local` and the only caller of
  `internal::JoinSegmentsInto`.
- **The control cannot rot into flakiness.** Default writer/reader QoS is RELIABLE +
  KEEP_ALL + TRANSIENT_LOCAL (`qos_defaults.cpp:23-42`), so a late-matching reader
  still gets the backlog; and with one shared `kSettle` the only pressure on the
  number is toward widening, which strengthens the isolation claim.
- **`PayloadBytes()` is a Fast DDS extension**, not on `PubSubProvider` — a
  base-typed handle cannot ask a provider for its bound.

| Id | Owed | Where |
|----|------|-------|
| DEBT-1 | §2's claim that the alternating single-thread sequence "is **the only** arrangement that reaches `Publish`'s `static thread_local` scratch buffer" is **false**, and M5's stated mechanism is wrong. `test_profile_document.cpp:556-557` already publishes to two different topic names through two different instances on one thread, so M5 is caught by an existing test; and with append semantics the first failure in the new sequence is **A's own second publish** (`pdadec8/shared` + `pdadec8/only-a` → `kTopicNotDeclared`), not B's lookup. Keep the row (it costs nothing) but correct both sentences, or the recorded evidence will not match the observation. | review §"row-by-row" |
| DEBT-2 | M2/M3/M4's "Must redden" cells predict crossed markers; all three will actually redden by **typed refusal at declaration/subscription time** — `create_topic`/`register_type` returning null → `kTransportFailure` (M2), `kSchemaConflict` from `CreateTopic`'s different-IPC compare or "already subscribed to" from `Subscribe` (M3, M4). P5 is satisfied by any named assertion going red, but a mismatch between the predicted and observed mechanism is exactly what an implementer reads as "the mutation survived". Correct the column. | review §"row-by-row" |
| DEBT-3 | M5 mutates `pubsub/include/fletcher/pubsub/internal/segments.hpp`, which **every** provider uses, so that row reddens most of the tree's pub/sub suites at once. Say so where the mutation set is described, and require the recorded evidence to name *these three cases'* failure rather than "the tree went red". | review §"row-by-row" |
| DEBT-4 | §8's published claim names two exclusions (cross-host, vendor process-wide state) and should name a third: **intra-process delivery**. Locked decision 12 is explicit that "Fast DDS serves same-process endpoints over intra-process delivery, bypassing data-sharing and much of the transport", so what this arrangement proves isolated is the matching/routing layer, not the transport's shared-memory segments — and two instances in *separate* processes on one host do share those segments. One clause in the claim paragraph and one line in the README. If Brief decision 3 is put to the owner, it must carry this, because it changes what option (a) claims. | review §"the rest" |
| DEBT-5 | The concurrent case must **join both publisher threads before either provider is destroyed** — §6 clause 5 requires quiescence ("no call in flight"), and a crash there is precisely the shm-orphaning path §7 claims the item adds none of (see BLOCKER 3). §7 currently covers subscriptions and callbacks but not the item's own threads. While there: keep the assertions on the main thread after the join — a gtest `ASSERT_*` inside a spawned thread only records a failure, it does not stop the case. | review §"the rest", B3 |
| DEBT-6 | Declared **+460/−0** is optimistic once B1's fourth arrangement and B2's fix land; expect **~+540**. Not scope creep and no extra guards are being demanded — book the number so the close does not read as an overrun. | review §"budget" |

### Cycle 2 (`02bc51b`) — APPROVE-WITH-DEBT(6), no BLOCKERs

**B1, B2 and B3 are closed** and cycle-1 DEBT-1…6 are all folded into revision 1 and
**closed**. Eight things the implementer may rely on **without re-asking**, on top of
cycle 1's nine — all verified in cycle 2, do not re-derive them:
- **B1 is closed at the class.** With one `kBound`, `domain_id` is the *only*
  wire-visible difference: the data type name is `fletcher_<bound>`
  (`payload_bound.hpp:63-65`), the schema companion type name is the constant
  `SchemaBytes` and is explicitly bound-independent (`:67-68`), topic names are
  identical by construction, **no partitions are set anywhere**, and both empty
  documents make participant/writer/reader QoS byte-identical (`qos_defaults.cpp`).
- **The round's reading of locked decision 13 is the product code's.**
  `fast_dds_pubsub_provider.cpp:183-186` pairs the two verbatim, so P1b's citation is
  accurate: the header states the fact, decision 13 freezes it.
- **The same-domain crossing M1 and the control depend on is observed, not theorised.**
  `test_profile_document.cpp:536-565` stands two participants on one domain and both
  are discovered; `test_roundtrip.cpp:50`'s four tests share domain 137 *and* topic
  names and do interfere.
- **B2's drop is real.** The overflow is caught in `serialize()`
  (`fletcher_sample_pub_sub_type.hpp:106-116`), `payload.length = 0` + `return false`,
  so the change never enters history, `write()` returns non-OK, and `SampleWriter`
  only logs (`sample_writer.hpp:59-93`). Dropped — not truncated, not late, not
  refused elsewhere — and `Publish` returns normally.
- **"A row after a dropped one is still delivered" is new coverage.**
  `DataSharingOversizedRowDoesNotThrow` has no subscriber, so it pins no-throw only.
  §2a's third row is a gain, and it reddens loudly if it is ever false.
- **M6 reddens in either construction order** — low-bound first loses the high-bound
  journal's middle marker; high-bound first gives the low-bound journal a third
  marker. The row does not depend on which instance is built first.
- **All six mutation mechanisms are confirmed in the tree**: `kSchemaConflict` at
  `fast_dds_pubsub_provider.cpp:321-324`, `kInvalidArgument` "already subscribed to"
  at `:433-435`, `kTransportFailure` on a second `register_type` under one name at
  `:233-235`.
- **Domains and counts.** 159/160 are unused (no match anywhere); 82 ctest entries
  today reconciles exactly to 12+12+4+2+7+19+25+1 and 86 after; `conformance_fastdds`
  goes 25 → 29 gtest cases; the design's `conformance_xrce` "1 / 27" is right and the
  CMakeLists comment is the stale one. Entries and cases are not conflated anywhere.

The design is at its 300-line cap, so **every item below lands in code, tests or the
README — not in the design doc**, except items 1, 2 and 4, which are corrections to
sentences already there and must not grow the file.

| Id | Owed | Where |
|----|------|-------|
| C2-1 | **Must land in this PR — it is what gets published.** §8's claim folds §2a into the non-exchange claim ("with different domains — **and, separately, with different payload bounds** — exchange no rows"), but for that pair non-exchange is guaranteed by the type name (P1b) and the control's measured window licenses only the equal-bound pair; the design's own P1b says §2a "claims none". True but unearned, and it is the B1 defect surviving in prose. Split the paragraph: the domain pair claims no crossing inside the measured window, the bound pair claims **each instance honours its own bound** (a row over one instance's bound is dropped there and delivered on the other). Same split in the README. | re-review §DEBT-1 |
| C2-2 | §3 ("differing only in that both instances sit on domain 156") and rung-1 case 1 ("differ only in `domain_id`, so nothing but the domain can be keeping them apart") are contradicted by the design's own forbidden case 4, which says the control uses **one** shape while the isolation case uses two. **Verified harmless — rely on this, do not re-derive it:** the schema shape is not part of the discovery key (the data type name is `fletcher_<bound>` alone), and the reader performs no row-against-schema validation — a schema only releases `OrderedDelivery`'s pre-schema backlog (`internal/data_reader_listener.hpp`). Record it as a premise beside P1b and correct the two "only" sentences. | re-review §DEBT-2 |
| C2-3 | **Must land in this PR. The one residue that could fail silently.** Each journal is appended on a Fast DDS listener thread and compared on the main thread after `kSettle`. Unguarded, `push_back` racing the comparison is UB, and a foreign marker arriving during the read can be missed — a **green the arrangement did not earn**, which is this item's own defect class. `registry.cpp`'s `Journal` is unsynchronised only because its probes deliver synchronously on the caller's thread; the cross-thread idiom in this harness is `include/fletcher/conformance/fixtures.hpp:130`'s `std::mutex mu_`. One mutex per journal; compare a snapshot taken under it. | re-review §DEBT-3 |
| C2-4 | M5's mechanism is still one step off. `Publish`'s scratch is `static thread_local` and persists across cases in the same binary and thread, and the 12 clause cases publish before the `Registry.` cases run — so under append semantics the first failure is likely A's **first** publish, not its second. Immaterial to P5 (any *named* assertion reddening satisfies it), but say so, or the recorded evidence reads as a missed prediction. | re-review §DEBT-4 |
| C2-5 | README placement. `## The Registry suite` opens "in its own binary (`conformance_registry`), 19 entries", and the two existing `Registry.*ResolvesAsABuiltIn` cases that live in the provider binaries are not documented there at all. Introduce the four new cases under a sub-heading that names `conformance_fastdds` (PDA-DEC-6 DEBT-6's concern in the other direction), or that section's own count becomes misleading. | re-review §DEBT-5 |
| C2-6 | Informational, nothing owed by this item. `integration-tests/pubsub-conformance/CMakeLists.txt:304-306` says `conformance_xrce` is 26 gtest cases; it is 27 (24 clause cases + 2 `ConformanceXrce` + 1 `Registry`), and the design's number is the correct one. Fix wherever it is cheapest — PDA-DEC-9 restates these counts anyway. Not a reason to grow this item's `Files-to-touch`. | re-review §DEBT-6 |

## PDA-DEC-9 — seam spec, taxonomy, handoff (NEEDS-REWORK, 6 BLOCKERs, cycle 1 of 2)

Review: [PDA-DEC-9-design-review.md](PDA-DEC-9-design-review.md). The eight items below are
DEBT and do **not** loop the design; the six BLOCKERs are in the review (B1 the totality
`static_assert` cannot fail on an append, so the item's one new guard does not do what it
claims; B2 "ships with the package" is false — `package()` copies only `*.hpp`/`*.cmake`;
B3 §12 rows 2 and 5 are labelled `mechanical` over checks that are partial or by-reading;
B4 §10's per-site ledger and three bare counts survive into the frozen spec, against the
design's own F1; B5 `append-only` names no actor or allocator and drops §4.1's stop-and-ask;
B6 Brief decision 2 as worded reverses the owner's 2026-09-01 *Ship the guard, hunt
elsewhere* ruling — STOP-AND-ASK).

Verified against the tree in cycle 1 — **rely on these, do not re-derive them**: §10's recipe
really does yield **96 occurrences / 21 files** (`plans/`, `build/`, `node_modules/` excluded;
`docs/` contributes 0); `architecture-overview.md:164` really does describe a
`SubscriptionResult` carrying an `OwnedSchema` while the type is
`struct SubscriptionResult { SchemaArrival schema; }` (`provider.hpp:39-41`); `README.md:155`
is already correct; both docs' `std::make_shared<fletcher::FastDDSPubSubProvider>()` examples
exist (`README.md:142`, `architecture-overview.md:258`) and still compile
(`fast_dds_pubsub_provider.hpp:104`); C2-6 is discharged
(`pubsub-conformance/CMakeLists.txt:306-308` reads 27); `status.hpp:67-83` is ten
per-enumerator asserts over values `0..9`; `provider_registry.hpp:292-297` pins the
member-pointer type of `Create`; `gateway/src/main.cpp:209-227` names no concrete provider in
selection; no `FastDDSProviderOptions`/`XrceConfig` survives except comments and gtest suite
names (38 occurrences, all inert); `ci.pr.yml` is `pull_request`-triggered with **9 component +
11 integration** reusable-workflow lanes; `core_tests` exists and the `core` lane runs it in the
Conan cache on both platforms (`ci.core.yml:55,107`); the P2 precedent is exactly as cited
(`xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`). The tree has **no** global `/W4 /WX` or
`-Wall -Werror`; warnings-as-errors are per-target only (`pubsub-arrow/tests/CMakeLists.txt:57-59`,
`xrcedds-pubsub-provider/tests/CMakeLists.txt:66`) — B1's fix must set its own flag.

| Id | Owed | Where |
|----|------|-------|
| C1-1 | **`README.md:315` is a wrong correction — do not make it as written.** The roadmap bullet promises two things: runtime plugin *loading* (not shipped, PDA-ABI) and "pick a transport **without compile-time coupling to its library**" (also **not** shipped — an application still links the provider library). What shipped is runtime *selection among linked-in providers*. Files-to-delete proposes to delete exactly the clause that is still a true unshipped promise. Keep the coupling clause; narrow the bullet to loading and note that selection by name already ships. Deleting the wrong half would understate PDA-ABI and is the "wrong correction is worse than the original error" pattern this item exists to end. | review §DEBT C1-1 |
| C1-2 | The drift guard covers **name + number only**. The published table's "meaning" and "who raises it" columns are unguarded prose, and "meaning" duplicates `status.hpp`'s own doc comments — a second copy of the semantics one column over from the copy F2 forbids. Say in the README section which columns the test enforces, or drop "who raises it" (it rots the moment PDA-ABI ships a driver that raises `kNotSupported`). | review §DEBT C1-2 |
| C1-3 | §12's frozen list **paraphrases** sections ("§3's ownership rules", "§5.1's mapping rules"). §3.5's empty-segment refusal and §5.3's callback-must-not-throw rule are normative and are neither ownership nor mapping rules — only the default clause catches them, after the reader has already wondered. List by section number with named exceptions instead. | review §DEBT C1-3 |
| C1-4 | Say whether "§7's clauses are `frozen`" forbids PDA-ABI from **adding conformance cases**. The clause *text* must not change; the *suite* must stay extendable — the 2026-08-31 ruling wants it to pressure-test the ABI, and §9's new row hands it to both rounds as their oracle. One sentence distinguishing contract text from test set. | review §DEBT C1-4 |
| C1-5 | D7 states the evidence and issues no instruction, which is the difference between a handoff and a record of embarrassment. Add the actionable sentence Brief option 1(a) actually promises: both rounds treat Linux as unverified; the first PR of either round runs the lanes; a Linux-only failure **of seam behaviour** is a stop-and-ask against the spec, not a local fix. | review §DEBT C1-5 |
| C1-6 | The new test must fail **loudly** on a missing or unreadable `README.md` and on a zero-row parse. A "for each row, compare" loop is vacuously green over zero rows; the row-count equality closes that only if the expected count is asserted rather than derived from the parse. Follow the precedent's shape (`#error` on a missing define, empty string on a missing file, caller asserts). | review §DEBT C1-6 |
| C1-7 | §5.1 still names numbers in prose after the change (`kOk = 0`, "taxonomy entry 4"). Under F2 either cite the table for both or state that these two are deliberate, cited exceptions. | review §DEBT C1-7 |
| C1-8 | **Brief decision 3 is already answered** — locked decision 1 ("not a change landed inside an ABI round") and spec §1 ("Neither ABI round may change the seam") settle it verbatim, and option (b)'s observations annex is a *deviation from a locked decision*. Either strike the question or label (b) as requiring a lock change, so the owner is not offered a locked-decision breach as an ordinary alternative. | review §DEBT C1-8 |

## PDA-DEC-9 — cycle 2 (APPROVE-WITH-DEBT(10), no BLOCKERs stand)

Review: [PDA-DEC-9-design-review.md](PDA-DEC-9-design-review.md) §Cycle 2. All six cycle-1
BLOCKERs are closed and all eight cycle-1 DEBT items are folded into the design. The ten items
below do **not** loop the design; **C3-1 and C3-2 are the two an implementer must not skip.**

Verified in cycle 2 — **rely on these, do not re-derive**: `core/conanfile.py:24-29`
`exports_sources` is `("CMakeLists.txt", "include/*", "cmake/*", "tests/*")` and `package()`
(`:58-66`) copies `*.hpp` + `*.cmake` only; `core/tests` contains **no** `switch` today (two
sources, `core/tests/CMakeLists.txt:6-8`); `provider_registry.hpp:292-297` is an
`is_same_v<decltype(&ProviderRegistry::Create), …>` whole-member-pointer assert; spec §4.1:414-417
matches D3's quote **verbatim**; spec §1:59-62 and locked decision 1 match the brief's citation
verbatim; the 2026-09-01 *Ship the guard, hunt elsewhere* ruling exists under that title and its
*Applies-to* is item-scoped; `shared_future` survives at exactly three `*.hpp`/`*.cpp` sites, all
comments (`schema_arrival.hpp:7`, `subscriber_arrow.hpp:51`, `seam_vocabulary.cpp:120`);
`README.md:38`/`:315` and `architecture-overview.md:15`/`:86`/`:164` are word for word as D5
describes; `FastDDSProviderOptions|XrceConfig` now matches **36 times across 9 `*.hpp`/`*.cpp`
files**, of which **7 are the live helper `XrceConfigFor(...)`** and the rest comments or gtest
suite names.

| Id | Owed | Where |
|----|------|-------|
| **C3-1** | **Priority; must land in this PR.** `docs/pubsub-interface-spec.md` §10:838-846 — the *"Consumers of the vocabulary change"* paragraph — survives D5, Files-to-touch and Files-to-delete, and says **in the present tense** that "`SubscriptionResult` and its `shared_future` are consumed by 10 sites outside `provider.hpp` — `subscriber.cpp` (5), `subscriber_arrow.cpp` and its header (4), `gateway/src/{main,ws_session}.cpp` (3), plus …". It is false about the seam (the `shared_future` was retired by the 2026-09-01 *One mechanism only* ruling; `SubscriptionResult` is `{ SchemaArrival schema; }`), its rows sum to **12** against its stated **10**, and after this item §10 is `frozen` by D3's default, so correcting it later is a stop-and-ask. **Zero design lines needed:** extend the existing Files-to-delete bullet to name it. And do the general thing, not the named thing — **read §10 end to end for present-tense claims and counts**; this defect class has now appeared in both review cycles at different addresses. | review §Cycle 2 B4 |
| **C3-2** | **The whole B1 guard hangs on one compiler flag whose failure mode is silent green.** If `/we4062` (or `-Werror=switch`) does not take, appending `kFoo = 10` and touching nothing else leaves part 1 silent (fallthrough returns `""`), part 2 green (all ten rows still match) **and part 3 green** (`StatusName(cast(rows.size()))` *is* `""`) — the original B1 defect restored, with condition 3 labelled `mechanical` in a frozen document. P3b's stop condition ("does not fire") is not observable from a clean build. **Redden it once:** append a throwaway enumerator, confirm `core_tests` fails to *compile*, revert, and record the compiler error text in the PR — the design's own restated §8.1 asks for exactly that, and it is two minutes. | review §Cycle 2 B1 |
| C3-3 | The flag precedent is cited as `pubsub-arrow/tests/CMakeLists.txt:57-59`, which is `target_compile_options` on a dedicated `EXCLUDE_FROM_ALL` OBJECT target (`discard_probe_tu`, `:45-60`) — precedent for *narrow-scope promotion*, which is the load-bearing half, but **not** for `set_source_files_properties`. Fix the citation or drop the word "precedent" there. Implementer note: source-file `COMPILE_OPTIONS` are **directory-scoped**, so the `add_executable` entry and the property must both sit in `core/tests/CMakeLists.txt` (Files-to-touch already puts them there). | review §Cycle 2 B1 |
| C3-4 | Brief `:52` still says "**packaging does not change**" while the design edits `core/conanfile.py`. Defensible under the design's own reading, but it is the last place a reader can take a packaging claim away (B2). Narrow to "the package's **contents** do not change". | review §Cycle 2 B2 |
| C3-5 | §12 row 2a's `mechanical` label reaches further than its check. *Regressing* to a `shared_future` breaks the compile (mechanical, real); **adding one beside `SchemaArrival` compiles and reddens nothing** — only a human re-running the grep notices, and no lane runs it. That is the mutation the 2026-09-01 *One mechanism only* ruling forbids. One clause: either name the reddening edit ("re-add a `shared_future` member → the grep returns a non-comment hit") or say what row 5 says — forward protection is §3's place in the frozen list, not a machine. | review §Cycle 2 B3 |
| C3-6 | The durable sentence replacing §10's table must not carry the survival clause "every remaining occurrence is a comment or a gtest suite name" — a substring re-derivation returns 7 live hits for the helper `XrceConfigFor(...)`. Word it as *the retired types are **declared nowhere and constructed nowhere**; the compile is the check*. (Cycle 1's "all 38 occurrences are comments or gtest suite names" was imprecise the same way; the corrected count is 36, of which 7 are live and unrelated.) | review §Cycle 2 "undisturbed" |
| C3-7 | `core/README.md`'s new taxonomy section must **state no count** — no "the ten statuses". F1 binds *the spec*, so the newly published artifact sits outside the rule that protects it, and a stale "ten" in prose beside a machine-compared table is the drift this item exists to stop. The table is the enumeration. | review §Cycle 2 B4 |
| C3-8 | Say in §12 **which sections the two classes cover**. As written ("anything unlisted is `frozen`") the classification also freezes §10 (record) and §11 (scope), so correcting a stale record needs an owner stop-and-ask. While there, scope F1 to *counts that claim something about the current tree* — otherwise §10's kept past-tense records ("Five QoS tests … were retired", "Four tests … were retired") breach the design's own forbidden rule. | review §Cycle 2 B4 |
| C3-9 | Condition 3's `mechanical` label is witnessed on **MSVC only** at signing time: the GCC/Clang half of the promotion cannot run until the owner opens the PR (D7/P4). One clause in §12 or D7 — the `core` lane building `core_tests` on both platforms is what would expose a flag that fires on one and not the other. | review §Cycle 2 B1 / D7 |
| C3-10 | Attribute the blind-spot policy precisely: the 2026-09-01 *Ship the guard, hunt elsewhere* ruling's *Applies-to* is **item-scoped** ("PDA-DEC-1 … relieved by this ruling and by nothing else"). D6 carries it into §12 as "the standing policy" for two future rounds. The generalisation is in the safe direction and worth keeping, but write it as *PDA-DEC-1's ruling, carried forward as policy by PDA-DEC-9* — second time this round an item-scoped ruling could harden into round-wide precedent. | review §Cycle 2 B6 |

## PDA-DEC-A4 — §7 clause 6 at the caller tier (NEEDS-REWORK, 2 BLOCKERs, cycle 1 of 2)

Review: [PDA-DEC-A4-design-review.md](PDA-DEC-A4-design-review.md). The five items
below are DEBT and do **not** loop the design; the two BLOCKERs and the ruled
STOP-AND-ASK (idempotence needs its own owner authorisation) are in the review.

| Id | Owed | Where |
|----|------|-------|
| A4-DEBT-1 | `CallerTier.StaleSnapshotProbeIsDetected` is a control on nothing unless the instrument is shared. The design says a hand-built copy-then-release-then-call fan-out *"must be flagged by the same detector"*, but there is no detector object — the three primaries assert directly with latches. Factor the observation apparatus (latch protocol + the "entered after the unsubscribe returned" predicate) into one helper used by both the primary case and the probe, or drop the case and say in the README that the guard's falsification is the three primaries being red today. Fourth unfalsifiable-guard shape this round. | review §DEBT-1 |
| A4-DEBT-2 | The recursive gate makes premise **P2** unfalsifiable: a provider re-entering delivery for one subscription on one thread re-acquires the gate and proceeds **silently**, so P2's stop-and-ask can never fire. A non-recursive gate makes that violation deadlock loudly — detection rather than masking — and leaves the *typed* refusal to A3's `kReentrantCall` where the owner put it. Rides with BLOCKER 1's fix. | review §DEBT-2 |
| A4-DEBT-3 | **Carry with the stop-and-ask.** The brief does not name the carve-out in the memory-safety guarantee: an unsubscribe from inside its own callback *"does not wait for the frame it is in"*, so that is the one shape where a caller may **not** free or unpin its callback state on return. The brief's Forbidden list currently reads as if it were covered. One line in brief decision 2, added before the owner answers. | review §DEBT-3 |
| A4-DEBT-4 | `Files-to-delete` names `test_publisher_subscriber.cpp:434-436`, but `EXPECT_EQ(second_calls, 1)` recurs at `:438-440`, where the second publish's expectations also change (`first_calls == 2`, `second_calls == 0`). The wholesale rewrite covers it; the ledger is incomplete as written. | review §DEBT-4 |
| A4-DEBT-5 | *(cycle 1; see A4-DEBT-8 — the design now names the reversal but accepts it unmeasured, and review rules a probe is owed.)* The hot path's lock-free property is reversed without naming what it reverses: `subscriber.cpp:31-34` records the fan-out being deliberately made lock-free, and the budget it works to is `provider.hpp:109-113`'s measured **1.4 ns per call**. One uncontended `recursive_mutex` acquire per entry per sample is a real fraction of that (and `recursive_mutex` is dearer than `std::mutex` on MSVC), and `CopyAccounting` counts copies, not locks. Name the reversal in Risks and either measure it or state that it is accepted unmeasured. | review §DEBT-5 |

### PDA-DEC-A4 cycle 2 (`9b354cb`) — NEEDS-REWORK, 1 BLOCKER (it *is* the P6 stop-and-ask)

Cycle-1 BLOCKERs 1 and 2 are closed, as are A4-DEBT-1 (by deletion, which review
ruled the stronger answer), A4-DEBT-2 (folded into the non-recursive gate) and
A4-DEBT-4. A4-DEBT-3 was carried and produced the owner's 2026-09-04 rulings.
A4-DEBT-5 is superseded by A4-DEBT-8. The standing BLOCKER and the P6 ruling are in
the review's cycle-2 section.

| Id | Owed | Where |
|----|------|-------|
| A4-DEBT-6 | **Must land.** Edge B's named live check cannot fire: `ctest -R 'ProviderConformance\.'` constructs no `Subscriber` — the design's own premise, re-verified (the only `Subscriber` in the harness is `src/copy_accounting.cpp:247,266`, whose subjects are in-process and so have no provider that waits on its own in-flight delivery). Edge B has **no live check in the harness**, and the sentence that made its uncontrolled status acceptable is the one that is untrue. Delete the claim and say edge B rests on the scoping mandate with no live check, or name `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp:119,171,250,336` (via `SubscriberArrow`) and say plainly that it is opportunistic. | review §Cycle 2 A4-DEBT-6 |
| A4-DEBT-7 | The delivery-depth counter is per-**thread**, not per-logical-flow: a handler that hands the cancellation to a helper thread and waits for it deadlocks — the helper has depth 0 and takes the barrier on the gate the handler still holds. One line of handled residue plus one in the README; adjacent to, not covered by, the existing "a callback that never returns" entry. | review §Cycle 2 A4-DEBT-7 |
| A4-DEBT-8 | **A probe is owed before the fan-out loop is written.** The design does not merely accept an unmeasured cost — it defers a live choice to a number it declines to take (§1.2 rejects the atomic in-flight counter while calling it "cheaper per sample"; Risks says the alternative stands "if the number ever matters"). ~20 lines on this machine: uncontended `std::mutex` lock+unlock per entry per sample against the measured 1.4 ns call at `provider.hpp:109-113`, and the same for an `atomic` fetch_add. Supersedes A4-DEBT-5. | review §Cycle 2 A4-DEBT-8 |
| A4-DEBT-9 | §1.1's *"it is invisible across `Subscriber` instances"* is true of the storage and false of the effect. Correct it whichever way the owner rules on P6 — under the per-instance fix it becomes true; under the per-thread fix it must be replaced by a statement of the cross-instance effect. | review §Cycle 2 A4-DEBT-9 |

## PDA-DEC-A5 — topic name integrity (NEEDS-REWORK, 2 BLOCKERs, cycle 1 of 2)

Review: [PDA-DEC-A5-design-review.md](PDA-DEC-A5-design-review.md). The three items
below are DEBT and do **not** loop the design; the two BLOCKERs (both in the
evidence, not the mechanism) are in the review. Claims 2, 3 and 4 were ruled in the
design's favour and the PM carries nothing to the owner.

| Id | Owed | Where |
|----|------|-------|
| A5-DEBT-1 | **The `__schema` shadow — the one residue the "fix at the map" framing leaves open.** Both DDS providers derive a companion topic `name + "/__schema"` (`fast_dds_pubsub_provider.cpp:331,494`; `xrce_dds_pubsub_provider.cpp:720,881`). After A5 the accepted list `{"a","__schema"}` joins to `a/__schema`, which is the schema channel of `{"a"}` — a distinct Fletcher topic colliding with a provider-derived name, which is the class the design's own Summary names. The published invariant stays literally true (the collision is not between two *accepted lists*), so this is **not** a false spec sentence and not a blocker; and the consequence is loud inside one participant (Fast DDS `create_topic` returns null on a type mismatch → `kTransportFailure`) and a non-match across participants. XRCE uses `UXR_REPLACE` and I could not resolve its behaviour from the tree. **Proposed forbid, same rung, same door:** refuse a segment equal to `__schema` (or the reserved `__` prefix) — ~2 lines plus one table row. Note it makes brief decision 3 option (a), "reject exactly the three shapes that cause a collision", incomplete as framed; if the owner is to close it, it belongs in that decision. | review §Claim 5, §Claim 1 |
| A5-DEBT-2 | **The §3.5 replacement wording must not foreclose an injective driver-side name mapping.** Deleting "so the provider may join with any separator" is authorised and correct for the three in-tree providers, but PDA-ABI may not change the seam (locked decision 1; spec §1), and a future driver on a transport where `/` is not a legal topic character would be non-conforming under "every provider uses it" as written. One clause in the amendment — the seam-computed name is the identity, and a driver may map it into its own transport namespace only **injectively** — costs nothing now, is a narrowing rather than a widening, and cannot reopen the aliasing hole. | review §Claim 3 |
| A5-DEBT-3 | **The rung-1 argument is right for a weaker reason than the design gives.** §2 carries a carve-out the design does not cite — "this round *may* change the **types** in those signatures, and only where a type has **no C-expressible form**" (`docs/pubsub-interface-spec.md:104-106`) — which does not reach a sealed `TopicName`, because §3.5 already gives topic segments a C form. State that, and state the stronger point: across a boundary of pointer-and-length pairs a bad name is always constructible, so a sealed type relocates the check into a constructor rather than making anything unrepresentable. One line, so nobody later cites §2:104-106 as proof rung 1 was available. | review §Claim 4 |

### PDA-DEC-A5 cycle 2 (`64fc095`) — NEEDS-REWORK, 1 BLOCKER (an authorisation boundary, fixed by one line in the brief)

Cycle-1 BLOCKERs 1 and 2 are **closed and verified** (the new `TEST(TopicNames, …)`
home is real and cross-provider; six of seven mutations are live in-tree, not five).
A5-DEBT-1/2/3 are all folded into revision 2 and **closed**. Eight things the
implementer may rely on **without re-deriving them**, on top of cycle 1's list:
**no `__`-prefixed topic segment exists anywhere in the tree** (searched `*.cpp
*.hpp *.json *.js *.html` for `"__[A-Za-z0-9_]*"` — the `__rba.fletcher.rs` hits are
generated Rust filenames; this **discharges P5's `__` half**); the loopback has **no**
`__schema` companion, so that channel is a Fast DDS + XRCE construct only; M4 is
genuinely live in-tree (`pubsub/CMakeLists.txt:11-25` + `pubsub/tests/CMakeLists.txt:10-12`);
and the §3.5 replacement narrows on every clause, so it needs no authorisation
beyond 2026-09-03 except for the `__` rule itself.

| Id | Owed | Where |
|----|------|-------|
| A5-DEBT-4 | **The §3.5 amendment states injectivity but not the disjointness the `__` reservation actually implements.** A driver deriving `name + "/__schema"` is an injective map, so the A5-DEBT-2 clause alone does not stop a future driver deriving `name + ".meta"` and colliding with an accepted list all over again — and PDA-ABI may not change the seam to fix it (locked decision 1, §1). The reservation is only load-bearing if the amendment states the reciprocal obligation: **a driver's derived companion names must live in the reserved `__` namespace**, which is what makes every future companion name safe by construction rather than by inspection. One clause, written while the amendment is being written; it is the difference between "we refuse these four shapes" and a rule a third-party driver author can obey. | review §"`__` prefix width", §"§3.5 wording" |
| A5-DEBT-5 | The two new `TopicNames.AmbiguousSegmentsAreRefused` cases should name their **own** domain / session key rather than reusing the registry case's (`kRegistryDomain = 153`, `fastdds_main.cpp:42`; `kRegistrySessionBase`, `xrce_main.cpp`). The tree keeps a domain census and PDA-DEC-8 recorded **154–158 unused** (`design-debt.md`, PDA-DEC-8 cycle 2) — take one rather than re-survey. **PM correction: that census was wrong, 154 is taken; A5 took 155 and 153.** Cheap, and it keeps a refusal case (which creates a participant even though it never declares a topic) out of another case's discovery traffic. | review §"BLOCKER 1 closed" |

## PDA-DEC-A1 — a row composed *in* the delivered window (APPROVE-WITH-DEBT(5), cycle 1 of 2)

Review: [PDA-DEC-A1-design-review.md](PDA-DEC-A1-design-review.md). **No BLOCKERs**; the
five below are DEBT and do **not** loop the design. Things the implementer may rely on
without re-deriving them: **P1 is verified TRUE for all three in-tree subclasses** with the
arithmetic (`write_buffer.hpp:164-175`, `copy_accounting.cpp:83-95`, `write_buffer.hpp:198-200`
→ `status.hpp:138-144`) — do not stop-and-ask on it; **P2 is verified TRUE**
(`Blob(owner,data,size)` is public, used at `copy_accounting.cpp:151`); **P4 is RULED —
`kInvalidArgument` is correct and A1 must not append a status** (see the review for why
`kReentrantCall` is A3's to reconsider, for free, since nothing has shipped); the
**in-tree/packaged split is confirmed as the design states it** — `core_tests` links the
INTERFACE target `fletcher-core` so M1/M3/M4/M5 are live in-tree, the harness
`find_package`s the packaged core so the three forcing entries need a `core` package
rebuild, and M2 (`Judge`) is unaffected. `7 → 11` entries and the three subject labels are
verified.

| Id | Owed | Where |
|----|------|-------|
| A1-DEBT-1 | **The second handled residue: a writer that reports more than it wrote.** Committing `pos0 + used` publishes bytes nobody wrote. The design's "no new class" line is right for over-*writing* and wrong for over-*reporting*: no public member today advances `pos_` over a byte it did not itself write. Benign on both growable subclasses (the lent span is already zeroed by `resize`/value-init); **not** benign on `FixedWriteBuffer`, which in this tree wraps a transport payload (`sample_writer.hpp:119`, `fletcher_sample_pub_sub_type.hpp:86`) — recycled pool memory that may still hold the previous sample's bytes, i.e. the zero-copy DDS path. Owed: name it as the second residue, disclose it in the header's normative block, one line in the README claim limit. **Do not memset the lent span to "fix" it** — O(room) on exactly the path the item opens, and worth nothing on the two subclasses where the span is already zero. | review §Claim 2, §DEBT-1 |
| A1-DEBT-2 | **State the writer-throws behaviour and pin it.** Safe by construction (the commit is the last statement, so `pos_` is still `pos0`), but unstated. One sentence in the header contract — an exception from the writer commits nothing and propagates unchanged — plus one `WriteBufferInPlace.*` case. The C form cannot produce it, so it is a C++-caller contract that must be written down. | review §DEBT-2 |
| A1-DEBT-3 | **Two ladder claims are one word too strong; fix the words, not the mechanism.** (a) `PatchU32`/`PatchByte` from inside the writer move neither `data_` nor `pos_`, so check (a) does not catch them — they are harmless (bounded by `offset < pos0`, disjoint from the lent span) and should be stated as **permitted**, with the claim restated as "every re-entry that could invalidate the lend". (b) Stashing the lent pointer past return is **not** rung-1 unrepresentable (a lambda capture or a `ctx` field spells it in either language); it is a use-after-free of the same genus as the disclosed residue and belongs beside it. Also: `VectorWriteBuffer::Reserve` has **two** call sites (`write_buffer.hpp:153,172`) plus its definition (`:178`), not three. | review §Claim 2, §DEBT-3 |
| A1-DEBT-4 | **An unsampled ledger must fail as itself.** If `produced_in_window` defaults false, a leg where the sampler never ran scores `encode_copies == 1` and the staged control passes for the wrong reason — the "control that cannot fail" shape this round has logged five times. Assert `produced_at != 0` (and `produced_len == row_bytes`) before any verdict is read, in the same place and for the same reason `COPY_MUST_DELIVER_CLEANLY` asserts an attachment did not arrive MISSING. | review §Claim 4, §DEBT-4 |
| A1-DEBT-5 | **The §8 sentence must publish a permission, not an unconditional promise.** Under decision 1(a) the whole-path property is true only of a client that composes into the lent window; a staging client still pays one copy and the guard reports it. §8's replacement bullet must say so in the sentence itself, and §8.1's rewrite must move the interval's **start** to the producer's write site while keeping the window-base sample as an interior point — `row_copies` is preceded, not replaced. **PM-facing half:** one clause makes decision 1(a) unambiguous before it is presented — *"(a) whole path — the seam **permits** an uncopied row from the client's own write to the subscriber's read, for a client that uses the new call; the guard reports which kind of client it measured"*. Without it the owner rules on a promise wider than the mechanism, which is the A4 defect at the wording level. | review §Claim 5, §DEBT-5 |

## Round-level — found by PDA-DEC-7, owned by nobody yet (2026-09-02)

| Item | Detail | Source |
|---|---|---|
| ROUND-1 | **The harness proves its Agent is alive, not that its Agent is the one answering.** `SpawnedAgentAlive()` (`subjects/xrce_main.cpp:210`, PDA-DEC-1 `a963211`) checks the process *this binary spawned*, and `WaitUntilReachable()` asserts it — the guard is real and its comment explicitly anticipates a leftover Agent. **A PM note first recorded here claiming it "accepts an Agent it did not start" was wrong; this is the corrected statement.** The gap is that liveness is not port ownership: if the spawned Agent fails to bind because a leftover holds the port, but does not exit, both conditions hold and the foreign Agent certifies the run. The PDA-DEC-7 cycle-2 re-reviewer observed exactly that outcome — a full run at `conformance_xrce` 25/25 PASSED served by a foreign Agent — but the **mechanism is unconfirmed**, and establishing it is the first job of the fix, not an assumption to build on. Consequence: every XRCE green since PDA-DEC-1 is conditional on "no stray Agent was listening". Owner ruling 2026-09-02: **fix in-round, before PDA-DEC-9 signs the handoff**, since both ABI rounds inherit this harness as their oracle. Now tracked as **PDA-DEC-1H**. | PDA-DEC-7 compliance cycle 2, corrected by PM 2026-09-02 |

| A5-DEBT-6 | The gateway refusal case watches the two **text**-frame paths into `SplitTopic` but not the **binary `publish`** path (`gateway/src/ws_session.cpp:271`). Non-blocking: the seam refuses the name either way, so no wrong delivery is reachable — the gap is in what the *gateway* case observes, not in the product. Closing it means driving a binary publish frame with an empty part through `gateway-end-to-end`. Raised by code review's final check, 2026-09-04. | codereview §re-check 2 |
