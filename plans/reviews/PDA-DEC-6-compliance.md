# PDA-DEC-6 — architecture-conformance review (step 4a, independent)

Item: **Fast DDS configured by document; `FastDDSProviderOptions` retired.**
Reviewed: `git diff f87248e..6a66a15` (28 files, +2229/−621). Design:
[PDA-DEC-6-fastdds-by-document.md](../PDA-DEC-6-fastdds-by-document.md) ·
[design review](PDA-DEC-6-design-review.md) (APPROVE-WITH-DEBT(5)) ·
[design-debt.md](design-debt.md) PDA-DEC-6 DEBT-1…11 + C2-1…C2-5 ·
locked decisions 8, 9, 13 · rulings 2026-08-31 ("XML profile config only"),
2026-09-02 (typed core; the XML text, not a filename; whole-QoS).

Verdict: **PASS-WITH-FINDINGS(7)** — 2 blocking, 5 non-blocking.

The retirement itself is clean and the converse check is mostly satisfied: nothing
survives that the design ordered deleted, no second C++ QoS path exists, no parser or
config dependency arrived in Fletcher, no eProsima type is reachable above the seam, no
copy landed on the row or attachment path, and the whole-QoS ruling is honoured with no
Fletcher floor underneath a supplied profile (measured, not taken — see "What I verified
by execution"). The two blocking items are a **red design-mandated test** and a
**guard that pins a copy of the artefact it claims to pin**.

---

## What I verified by execution, not by reading

Built the provider from the commit (`conan create` with `run_tests=True`, forced rebuild;
fresh binary at `.conan2/p/b/fletcf1371fa29b519/b/build/tests/Release`) and ran it.

- **`test_package` PASSED** — §5's acceptance check is real and works: `example.cpp`
  compiled and linked with no Fast DDS include directories and printed
  `PASS: round-trip publish/subscribe OK`. Claim 7 is substantiated; the installed
  include tree holds exactly one header and `internal/qos_defaults.hpp` is not packaged
  (`conanfile.py:package()` copies only from `include/`).
- **79 tests in the provider suite; 78 pass, 1 fails** (F1 below). The counting claim
  47−7+18+21+1 = 80 is arithmetically right (base was 47+21 gtest cases plus the
  MSVC-only `NodiscardTest.FastDDSConcreteSubscribeCompileFailsOnDiscard` = 69); the
  **green** claim is not.
- **The whole-QoS ruling is measured green.** `MinimalProfileTakesFastDdsDefaultsNotFletchers`
  passes: a writer profile mentioning only `durability` resolves to Fast DDS's
  `KEEP_LAST(1)` and `max_samples 5000`, and a reader profile mentioning only `durability`
  resolves to `data_sharing` AUTO. No Fletcher floor survives underneath a supplied profile.
- `DefaultProfileTranscriptionIsExact`, `AnAnchorOnlyDocumentResolvesToFletchersBuiltIn`,
  `TwoInstancesResolveTheirOwnDocuments`, `MalformedProfileDocumentIsRefused` (all 8 rows,
  including the P6 row), `ForeignPropertiesSurviveTheStrip`,
  `WriterOnlyDocumentLeavesTheReaderOnFletchersDefault`, `SchemaChannelIgnoresTheDocument`,
  `PerTopicProfileOverridesTheDefault`, `ReaderProfileConfiguresTheReader` and the
  4-row forcing test all pass. Claims 2, 3 and 4 hold, with the caveat in F3.
- **I did not build `conformance_fastdds`**, so `Registry.FastDdsResolvesAsABuiltIn`
  (the 80th) is read but not executed here.
- Note for the PM: the in-tree `fastdds-pubsub-provider/build/` was **stale** when I
  started (it still listed `CustomDefaultWriterQos` and
  `ASchemaTooLargeForItsChannelIsReported`), and the repo has had roughly a dozen
  concurrent provider builds today. Any "green" read out of that directory is a false green.

---

## BLOCKING

### F1 — `FastDdsConfig.SchemaBoundComesFromTheDocument` crashes: access violation, deterministic

The design's forcing-test table lists this test as the replacement for **both** retired
`max_schema_bytes = 8` tests — "property `8` → the oversized schema is rejected on the
channel; absent → it is delivered", with M10 ("ignore the property") as its mutation. It
does not run to completion:

```
[ RUN      ] FastDdsConfig.SchemaBoundComesFromTheDocument
unknown file: error: SEH exception with code 0xc0000005 thrown in the test body.
[  FAILED  ] FastDdsConfig.SchemaBoundComesFromTheDocument (12-85 ms)
```

Evidence that this is neither flake nor an artefact of my build:

- **6/6 failures**: five consecutive isolated runs, one filtered-pair run, and the
  full-suite run (78 passed / 1 failed).
- **Reproduced on a second, independently produced binary** — the cache build stamped
  15:06 (not mine; mine is 15:20) fails identically.
- Not DDS-domain interference: every discovery-latched test in the same processes passes,
  and the failure time (~70 ms) is far below any latch budget.

Localisation evidence, for whoever fixes it (root-causing is 4b's job, not mine):

- The process emits **no** `[FLETCHER_SCHEMA Error] schema serialize failed` line during
  this test, while the sibling `AFailedSchemaAnnouncementCanBeRetried` — whose provider
  construction and first `CreateTopic` are character-for-character the same, same document,
  same domain — emits it twice and **passes**. So the oversized-schema rejection this
  test's first block asserts never reaches the write.
- gtest attributes the AV to "the test body", i.e. the test's own thread, not a DDS
  listener thread.
- The second block is the **only** end-to-end publish→subscribe→deliver round trip under a
  **non-empty** document anywhere in the suite: `ReaderProfileConfiguresTheReader` and
  `WriterOnlyDocumentLeavesTheReaderOnFletchersDefault` subscribe but never wait or deliver;
  the forcing test publishes but never subscribes; `Registry.FastDdsResolvesAsABuiltIn`
  delivers but with an **empty** document. If the fault is in the provider rather than in
  the test, this is the item's headline capability and nothing else would have caught it.
- Latent and separate from the crash: in that block `sub` is declared *before* the
  `std::atomic<int32_t> received` its callback captures by reference, so the provider
  outlives the captured state — the inverse of the "Capture state must outlive `sub`"
  ordering the sibling TU adopts deliberately.

**Conformance impact.** This is a retirement whose replacement does not execute:
`ASchemaTooLargeForItsChannelIsReported` is deleted and the assertions meant to replace it
never run. The design's "*Nothing retired without replacement*" is not satisfied as landed,
and the "all 80 green" claim is contradicted by the tree.

**Acceptable fix:** root-cause the AV, fix it, and show the test green both in isolation and
in the full suite. Do **not** weaken, split or delete the row — M10 needs both halves
(property present → rejected; property absent → delivered).

*(Noted, not a finding: another agent is already bisecting this in the working tree — an
uncommitted `TmpProbe.Bisect` test is present. I did not touch it; this review is of
`6a66a15`.)*

### F2 — the transcription guard pins a *copy* of the published block, so the README can drift silently

`fastdds-pubsub-provider/README.md` states, of the starting-point block: "**so this block
cannot drift from the code without a test going red**". `DefaultProfileTranscriptionIsExact`
carries its own `kPublishedStartingPoint` string literal, marked "keep the two in step".
I extracted both and they are byte-identical **today** — so nothing is wrong yet, and the
guard is genuinely total over the writer and reader QoS (22/22 policies, per the design
review's own verification).

But the design's M7 is "edit the README block **or** `qos_defaults.cpp` apart → red
**unconditionally**", and the design review closed B3 on exactly that sentence. Only the
`qos_defaults.cpp` half is live: editing the README's fenced block reddens nothing. Every
migrating caller of the retired struct is routed through that block, and it is the artefact
the ruling itself names ("We publish Fletcher's exact profile as a starting point, kept true
setting-for-setting by a test"). A promise the code does not keep is worse than no promise.

**Acceptable fix:** inject the README path with `target_compile_definitions` and have the
test read the fenced block off disk; or keep the literal and add one assert that the README
file's block equals it. Either makes the README sentence true and costs about ten lines.

---

## NON-BLOCKING

### F3 — C2-1's assert cannot detect the form it exists to hold, and claim 1 overstates it

The mandated form **is** in the code, for all three roles: `DomainParticipantExtendedQos
extended;`, `DataWriterQos per_topic; / DataWriterQos role;`, and the reader counterpart —
each `get_*_from_xml` call takes a freshly default-constructed QoS, and Fletcher's built-in
is returned only on the not-found branch. Structurally clean, and the reason is written
where someone would otherwise "simplify" it away.

What is not true is the claim that "the assertion could fail". The file itself records the
measurement: reintroducing the seeding leaves the **whole suite green** under fast-dds 3.4,
because this version overwrites its output parameter. So
`MinimalProfileTakesFastDdsDefaultsNotFletchers` asserts the *substrate's* behaviour, not
the *code's* form; it would redden only if a future Fast DDS overlaid **and** the seeding
were reintroduced. The 2026-09-02 ruling's note ("must mandate the form … and assert it, or
this ruling is unfalsifiable") is therefore half-satisfied — mandate yes, assert no.

I looked for a cheaper guard and found none available in-tree, and the implementer disclosed
the gap in the header rather than claiming the guard works, which is the right call.
**Acceptable fix:** none required; keep the disclosure and correct the claim. If a guard is
wanted later it has to be a lint or compile-time check that no `get_*_from_xml` call site
passes a non-default-constructed argument.

### F4 — the reader's per-topic profile branch is asserted by nothing

`internal::ResolveReaderQos`'s first lookup — the profile named after the topic — is
exercised by no test. `PerTopicProfileOverridesTheDefault` is writer-only;
`ReaderProfileConfiguresTheReader` supplies only `fletcher_reader`. Delete the reader's
topic-name lookup and nothing reddens, although the README documents per-topic reader
overrides and shows them ("`<data_reader>` profiles work the same way on the subscriber
side"). The design's row for `ReaderProfileConfiguresTheReader` reads "**Same**, via
`on_data_reader_discovery`", where the row above it is the per-topic lookup-order row — so
the reading that the reader's lookup order was to be covered is available, and was not taken.

Not a coverage loss: the retired `PerTopicReaderQosOverridesDefault` was equally blind (it
asserted only that a row arrived, so dropping the map lookup would have left it green). This
is a blind spot carried forward, not created. **Acceptable fix:** one in-process row on the
existing `XmlProbe` — a document defining `<data_reader profile_name="x/y">` must resolve for
topic `x/y` and must not for a different topic.

### F5 — a correctly-spelled `fletcher.*` property in the wrong profile is silently inert, and undisclosed

`ConsumeFletcherProperties` runs only over the participant anchor's `PropertyPolicyQos`.
`fletcher.loan_publish` placed inside a `<data_writer>` (or `<data_reader>`) profile's
`<propertiesPolicy>` is parsed by Fast DDS, ignored by the provider and never reported. That
is the same failure rung-2 case 6 forbids for a *misspelled* name ("a typo'd
`fletcher.loanpublish` must not be inert"), and unlike H1 it is **not** a substrate limit:
`DataWriterQos::properties()` / `DataReaderQos::properties()` make it observable at the moment
the profile is resolved. It is reachable — an operator who already writes a per-topic writer
profile is the likeliest person to put a Fletcher key in it — and it appears at no rung and
in no README residue.

**Acceptable fix:** refuse `kInvalidArgument` on any `fletcher.`-prefixed property found in a
resolved writer/reader profile, quoting it; or, if that is judged too costly, one line under
"Known limits of the document" saying the two properties are read **only** from the anchor.

### F6 — two ordered `Files-to-touch` entries skipped, and spec §10's record of them left unmarked

`docs/architecture-overview.md` and the root `README.md` are named in `Files-to-touch` (the
"implementing one interface" claims) and are **not** in the diff. Both still say adding a
transport "requires implementing one interface — no changes to generated code or the codec",
which after PDA-DEC-4/5/6 is incomplete: a transport is also *registered* by name and
configured by *its own document*. Spec §10's "**Docs:**" paragraph still lists both files
with no status, while every other line in §10 was updated to "migrated" / "still owed" — so
the one record that would have carried the omission forward does not.

Two other `Files-to-touch` entries are unmatched and I judge both **correct**:
`pubsub-conformance/{src/registry.cpp,CMakeLists.txt}` became unnecessary once DEBT-6 moved
`Registry.FastDdsResolvesAsABuiltIn` into `conformance_fastdds` (and `conformance_registry`'s
deliberately narrow link line is intact — I checked it is unchanged); and
`integration-tests/{pubsub-arrow-fastdds,fastdds-xrce-interop}/README.md` contain nothing
naming the retired type or the typed QoS path, so DEBT-7's claim about them was over-inclusive.

**Acceptable fix:** one sentence in each of the two docs, or mark them in §10 with an owner.

### F7 — the public header dropped `payload_bound.hpp`, so its own documented idiom no longer compiles

The retired struct's doc told callers to "Write it as `kPayloadBytes<N>` to be told at compile
time instead". The new header keeps the substance of that advice — "A value `IsPayloadBound`
rejects is refused with `PubSubError(kInvalidArgument)`" — but drops
`#include <fletcher/pubsub/payload_bound.hpp>`, and neither `provider.hpp` nor
`provider_registry.hpp` supplies it. In-tree TUs still compile because they reach it through
provider internals (`internal/fletcher_sample_pub_sub_type.hpp`); an out-of-tree consumer
writing `ProviderConfig{kPayloadBytes<128*1024>, 7, doc}` gets a compile error. Not reflected
in the declared "+2 / −2, net 0" surface accounting.

**Acceptable fix:** re-add the include (it is Fletcher's own header — no eProsima, so §5's
machine check is unaffected), or name it in the README's `ProviderConfig` table.

---

## Converse check — what survived that should not have? Nothing did.

Grepped, and executed wherever a binary could answer instead of a grep:

| Ordered deleted | State at `6a66a15` |
|---|---|
| `struct FastDDSProviderOptions` + its ctor | Gone. `grep` over the tree finds it only in historical comments (`exp_zero_copy.cpp:36`, three comment blocks in the old test TU) and one spec sentence about the past. No declaration anywhere. |
| `include/.../internal/qos_defaults.hpp` | Gone from `include/`; `include/fletcher/fastdds_pubsub_provider/` holds **exactly one** header. `conanfile.py:package()` copies only `include/`, so it has left the installed tree. |
| PDA-DEC-5's inline `fastdds` closure in the gateway | Gone, together with the DEBT-5 comment that recorded the missing surface. `gateway/src/main.cpp` names no concrete provider type and does not branch on the selector. |
| `CustomDefault{Writer,Reader}Qos`, `PerTopic{Writer,Reader}QosOverridesDefault`, `AutonomyStyleProfileViaOptions` | All five gone — confirmed by `--gtest_list_tests` on the fresh binary, not by grep. |
| `ASchemaTooLargeForItsChannelIsReported` | Gone. Its replacement is F1. |
| `AFailedSchemaAnnouncementCanBeRetried` (re-anchored) | Moved to the new TU with the `max_schema_bytes = 8` route replaced by the document property; assertions unchanged (the same two `"failed to announce the schema"` checks). Passes. |
| the `bound == 0` row of `AnUnusablePayloadBoundIsRefused` | Removed, and 0 now means unset — pinned by `AnUnsetPayloadBoundResolvesToSixtyFourKiB` (passes, 65536). The remaining rows moved from `std::invalid_argument` to `PubSubError`, and the typed status is asserted in the new TU. |
| fast-dds `PUBLIC` / `transitive_headers=True` | Both gone; `transitive_libs` kept. Verified by the passing `test_package`. |

And the specific things I was charged to hunt:

- **A second C++ QoS path:** none. One constructor,
  `explicit FastDDSPubSubProvider(const ProviderConfig& = {})`. No setter, no eProsima type
  in the installed header, and `MakeFletcherDefault*Qos()` now lives under `src/`. Rung-1
  case 1 holds structurally, not by convention.
- **A parser or config dependency in Fletcher:** none. Everything is a call into Fast DDS's
  own `get_*_qos_from_xml`; `load_XML_profiles_string` is not used, and
  `TwoInstancesResolveTheirOwnDocuments` passes, so P1's global-state half is measured rather
  than assumed. No new require anywhere. Fletcher opens no file: `--provider-config` is read
  in the **gateway**, in binary mode, contents into `ProviderConfig::document`, unreadable →
  stderr + exit 2 — exactly what the 2026-09-02 ruling places there.
- **eProsima or DDS vocabulary above the seam:** none in code. `grep` finds no direct
  `<fastdds/…>` or `<fastcdr/…>` include outside the two provider components. The gateway's
  `--help` and README name the *formats* in prose, which is unavoidable operator
  documentation and is where the ruling puts the convenience.
- **Branching on built-in vs loaded:** none. Both providers are registered unconditionally,
  then one `Create`; the document is forwarded to whichever was selected with no branch.
- **A copy on the row or attachment path:** none. `ResolveWriterQos` changed from returning a
  reference to returning a value, but it is called once per topic, under the exclusive lock,
  at first writer creation — never per row. The document is copied once, at construction,
  which §4.1 explicitly sanctions ("a provider that keeps it copies it").
- **`ProviderConfig`'s typed core:** untouched by this diff and still exactly
  `{max_payload_bytes, domain_id}` plus `document`; PDA-DEC-4's `static_assert` on
  `&ProviderRegistry::Create` is intact.
- **Refusals that became recoveries:** none. All five rung-2 cases refuse `kInvalidArgument`
  in the constructor, before the participant exists, and `MalformedProfileDocumentIsRefused`
  asserts the status **and** the quoted text on all eight rows — including the P6 row, which
  passes, so P6 holds and the anchor is sufficient. C2-4's positive domain rule is in the
  README and in the refusal message, and the accepted `<domainId>0</domainId>` residue is
  pinned by its own test.

`AnAnchorOnlyDocumentResolvesToFletchersBuiltIn` (the declared deviation, claim 2) is
**genuinely additive and genuinely closes M12**: forcing row 2 stays, the new test compares
both roles whole-struct on the production ladder, and it carries its own non-vacuity asserts
(`MakeFletcherDefault*Qos() != DataWriter/ReaderQos()`). Mutating the not-found branch to
return Fast DDS's default reddens it on `history` and `resource_limits` — the two policies
discovery cannot carry. Right call, right place.

The spec amendment says what the code does: the three `get_*_from_xml` names, the reserved
profile names and the mandatory anchor, the whole-QoS rule, the `<propertiesPolicy>` route
for the two Fletcher settings, `domain_id` winning with a non-zero disagreement refused, the
file-reading convenience living in the gateway, and the `PRIVATE` / no-`transitive_headers`
machine check all match the tree. §4 clause 4 records `fastdds` and cites the right test in
the right binary. §10 is corrected rather than marked done. The only spec sentence the tree
now contradicts is the unmarked "**Docs:**" line — F6.

---

## Budget — review-mandated mass, not stored complexity; the remedy is not scope reduction

Declared **+780 / −310**; actual **+2229 / −621** (~2.9× on adds). Measured split:

| Bucket | Lines |
|---|---|
| new test TU `test_profile_document.cpp` | +963 |
| provider README rewrite | +286 / −60 |
| `src/internal/profile_document.hpp` (new) | +244 |
| existing test TU rewrite | +234 / −281 |
| `Registry.FastDdsResolvesAsABuiltIn` in `fastdds_main.cpp` | +73 |
| spec + gateway README | +79 / −26 |
| public header | +69 / −102 |
| provider `.cpp` plus moved/edited internals | +79 / −54 |
| benchmarks + build files | +100 / −29 |

Judgement: **honest overrun, not scope creep.** Fifty-seven per cent of the adds are test
mass the two design-review cycles ordered by name (DEBT-1/2/3, C2-1, C2-2, C2-5, P6, plus
the discovery observer and `XmlProbe` that the design's own "read the discovered value /
compare whole in-process" wording requires); the README rewrite is an explicit
`Files-to-touch` line with six named subjects. No unordered construct appears in the diff,
and public surface really is +2 / −2. Reducing scope would mean giving back guards the
reviews demanded, which is the wrong trade. The remedy is a corrected number, not a smaller
item.

Two honesty corrections to the implementer's framing:

- "`src/` is only +75/−52" excludes the item's **actual** new production code — the 244-line
  `src/internal/profile_document.hpp`. Production code (public header plus `src/`) is
  **+393 / −157**.
- The 18 tests in the new TU are 13 named or ordered plus 5 small additions
  (`TheDomainRefusalQuotesBothNumbers`, `AnExplicitZeroDomainIdIsReadAsAbsent`,
  `AnUnsetPayloadBoundResolvesToSixtyFourKiB`,
  `AnUnusablePayloadBoundIsRefusedAsInvalidArgument`, and the strip test). Each pins a rung
  or an accepted residue; the domain-refusal one partly duplicates a row of
  `MalformedProfileDocumentIsRefused`. All defensible.

---

## RECORD (PM corrects in place; never blocking, never a fix cycle)

- The commit message and the step-3 report say **all 80 tests green**; the tree gives
  **78 / 79** in the provider suite, plus one conformance test I did not execute. Recorded
  here for the register; the underlying red test is F1, which *is* blocking.
- `plans/PDA-DEC-6-brief.md`'s "*As landed*" footer is still the `<date>` placeholder.
- Spec §10's header reads "**8 files, 12 construction sites**, all migrated to
  `ProviderConfig`"; the table's rows sum to 12 migrations **plus** the gateway's one site,
  which was *replaced* rather than migrated. Correct on a careful reading, easy to misquote —
  and PDA-DEC-7 will cite this table.
- The design's declared line budget (+780 / −310) understates the actual adds by ~2.9×.
