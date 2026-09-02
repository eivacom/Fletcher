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
