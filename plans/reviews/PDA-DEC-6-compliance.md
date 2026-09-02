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

---

# Cycle 2 re-review (2026-09-02) — independent, adversarial

Fix pass reviewed: `git diff 6a66a15..7f6a310` (19 files, +1179/−96; **+661/−96**
excluding the two review documents). Working tree clean at `7f6a310`. Cycle 1 above is
the record of what was found and is not edited.

Verdict: **PASS-WITH-FINDINGS(2)** — **0 blocking**. Both cycle-1 blockers are closed;
F2/B1 is closed **by mutation, run here, in three directions**. Two non-blocking
conformance items and six `RECORD` lines follow.

## B1 / cycle-1 F2 — closed. Reproduced, not accepted.

The guard is real. I ran it; I did not read it.

| # | Mutation | Rebuild? | Result |
|---|---|---|---|
| 1 | `README.md:217` writer `<max_samples>100` → `101`, code untouched | **none** | **RED** — `test_profile_document.cpp(619)`, "no longer transcribes `MakeFletcherDefaultWriterQos()` exactly" |
| 1b | `README.md:232` **reader** `<max_samples>` → `99`, code untouched | **none** | **RED** at line 628, on the reader half — both roles are live, not just the writer |
| 2 | `qos_defaults.cpp:29` writer `max_samples = 100` → `103`, **README untouched** | yes (7 s incremental) | **RED** at line 619 |
| 3 | the **packaged** route: the same edit to the *exported* README inside the Conan build folder | none | **RED**, then green again on restore |
| 4 | README heading `#### The published starting point` renamed | none | **RED**, loudly: "found no fenced xml block under …" |

Both directions redden, and the degradation modes are loud rather than silent: a missing
README, an empty extraction, a renamed heading and a truncated block each hit an `ASSERT`
before any comparison happens.

**Exactly one copy of the XML in the repo.** `grep` for the transcribed `max_samples`
line over the tree returns two hits, both inside the single fenced block
(`README.md:217,232`); the test's `kPublishedStartingPoint` literal is gone, and
`grep -rln fletcher_writer` finds no second transcription.

**The packaged build reads the same file, not a stale copy.** The baked path in the
in-tree binary is `…/fastdds-pubsub-provider/tests/../README.md`; in the Conan cache
build it is `…/p/b/fletc68645fe78dff2/b/tests/../README.md` — the **exported** copy,
which `diff` reports byte-identical to the tree's. Mutation 3 reddens that binary, so the
cache build genuinely reads the exported file. Two further properties are worth naming as
*good*: putting `README.md` in `exports_sources` puts it in the recipe revision, so a
README edit invalidates the package rather than being papered over by a cached one; and
if the export were dropped the file would be absent at the baked path and the test would
fail on `ASSERT_FALSE(readme.empty())` — a loud failure, not a silent degradation. This is
the "works in-tree, degrades in the cache" hat the charge warned about, and it is not
being worn.

## The other findings — each checked against the fix, not the claim

Measured on the Conan cache build of `7f6a310`. (The in-tree
`fastdds-pubsub-provider/build/` tree is orphaned: its `fletcher-pubsub` package folder
was deleted by a concurrent `conan create`, so CMake cannot reconfigure there.
Environment, not a finding.)

- **Suite:** **85 ctest entries / 84 gtest cases, 84/84 green**, matching the PM's
  authoritative numbers exactly. `FastDdsConfig.*` 23/23.
  `SchemaBoundComesFromTheDocument` passes — cycle-1 F1 was environmental, as ruled.
- **S2 — closed, executed.** `gateway.exe --provider-config <whitespace-only>` prints
  `--provider-config <path> is empty; omit the flag to run on the provider's own defaults`
  and exits **2**. I ran all five CLI cases against the built gateway rather than trusting
  the TS assertions: missing file → `cannot read --provider-config`, exit 2; a
  **directory** → `cannot read`, exit 2; whitespace-only → `is empty`, exit 2; a typo'd
  `fletcher.max_schema_byte` document → the **provider's own** message quoting the
  property, exit 2 (so the bytes really cross the seam); and
  `--provider-config nope.xml --help` now prints help and exits **0** (the deferred read).
  A valid anchor-only document starts and prints `READY 19193` — with **no
  `[XMLPARSER Error]` line at all**, which is S3's fix where an operator would see it.
- **S1 — closed.** Five cases in `end-to-end.test.ts` (the three ordered, plus the
  directory and empty-file rows). Every stderr string they assert matches the real output
  I reproduced above; the ports (`+9`, `+10`) collide with nothing; the watchdog stops a
  refusal-that-stopped-refusing from burning the 30 s timeout. I did **not** run the TS
  suite (npm + gateway harness is the concurrent agent's scope).
- **F5 — closed, and bounded in both directions.** The refusal is real: for the two role
  profiles it fires at construction, for a per-topic profile at the first endpoint, and
  the three message assertions (`fletcher.loan_publish` + `data_writer` +
  `fletcher_writer`) cannot be satisfied by the malformed-document message, which quotes
  neither the property nor the endpoint element — so no most-vexing-parse-class vacuity
  this time. I checked the over-reach direction by mutation: widening the predicate from
  `fletcher.`-prefixed to *any* property reddens exactly
  `AForeignPropertyInAnEndpointProfileIsAccepted` and nothing else, and
  `ForeignPropertiesSurviveTheStrip` still passes. The `<qos>`-child placement really is
  refused by Fast DDS's own parser — its line is in the log
  (`Invalid element found into 'writerQosPoliciesType'. Name: propertiesPolicy`), so that
  row is measured, not assumed. The only in-tree document carrying a `fletcher.*` property
  (`benchmarks/exp_zero_copy.cpp`) puts it in the anchor, so nothing in the tree is
  collaterally refused.
- **S4 — the count of 13 is right and none was missed.** `git grep "ProviderConfig{0"` at
  `6a66a15` gives **13** sites in 4 files (test_roundtrip **8**, test_interop 3,
  fastdds_peer 1, fastdds_main 1) — cycle-1's counterpart missed four of the eight in
  `test_roundtrip.cpp`. All 13 are designated-initialiser form at `7f6a310`; the only
  surviving brace-inits are `ProviderConfig{}` and `ProviderConfig{.document = …}`, which
  carry no positional hazard.
- **S3 — the proof is sound; the numbers are slightly off (RECORD).** I judged the proof,
  not the count. XML element names cannot carry an entity or character reference, so
  `DocumentMayDefine{Writer,Reader}Profile` is a genuine superset test — `<data_writer>`
  and `<publisher>` are the only two spellings `get_datawriter_qos_from_xml` resolves and
  both contain the searched word, and a general entity expanding to a whole element would
  need DTD support the parser does not have. `DocumentMayName`'s `&` escape hatch is **not
  decorative**: I removed it and `FastDdsConfig.ALookupThatCannotSucceedIsNotAttempted`
  went red on the `a&amp;b/topic` row, so the one case where the substring test is wrong is
  guarded by a live test. Being wrong in the *other* direction costs a log line and never a
  policy, which is the right asymmetry. One residue the stated proof does not name: F9.
- **F4 — closed and falsifiable.** Disabling the reader ladder's topic-name lookup reddens
  `ReaderPerTopicProfileOverridesTheDefault` and nothing else; the negative half (the same
  document must **not** resolve that profile for a different topic) is asserted.
- **F7 — closed and machine-checked.** `payload_bound.hpp` is included by the public
  header, and `test_package/src/example.cpp` writes
  `ProviderConfig{.max_payload_bytes = kPayloadBytes<64 * 1024>}`. That TU compiled and
  linked at `7f6a310` (its `example.exe` is stamped 16:24, alongside the cache test
  binary) with no Fast DDS include directories, so dropping the include again is a compile
  error rather than prose. The bound still resolves to 65536, so the registered type name
  is unchanged.
- **F3 — the disclosure still reads true.** `src/internal/profile_document.hpp`'s MEASURED
  note says the seeding mutation leaves the whole suite green because fast-dds 3.4
  overwrites its output parameter, and that the form is mandated structurally rather than
  asserted; the fix pass corrected the test comment that claimed the opposite. Accepted
  debt, accurately described. Nothing more.

## Converse check — what survived that should not have? Nothing new, and one thing died.

- The test's own copy of the published XML block — the construct this fix pass exists to
  delete — **is gone**, and there is no second copy anywhere in the tree.
- **No second path to configure QoS from C++.** One constructor, one `ProviderConfig`; the
  fix pass added no setter and no eProsima type to the installed header.
- **No config parser or dependency in Fletcher.** `ExtractFencedXmlAfter` is a
  seventeen-line markdown scan **in the test TU**; nothing like it exists under `src/`.
- **No file access smuggled into shipped code.** `grep` for
  `fstream|ifstream|fopen|std::filesystem` over `fastdds-pubsub-provider/{src,include}` and
  `pubsub/{src,include}` returns **nothing**; the only occurrence is
  `tests/test_profile_document.cpp`. The gateway remains the single place that opens a
  file, and says so ("the ONLY file the gateway opens on a provider's behalf"). The
  2026-09-02 ruling is honoured.
- **No eProsima type or DDS vocabulary above the seam.** The Fast DDS XML in
  `end-to-end.test.ts` is a test fixture in the operator's own format, which is exactly
  where the ruling puts the convenience; no compiled code above the seam names an eProsima
  type.
- **No copy on the row or attachment path.** The new `DocumentMay*` scans and the new
  refusal all run inside `Resolve{Writer,Reader}Qos`, reached only from lazy writer
  creation (`fast_dds_pubsub_provider.cpp:401`) and `Subscribe` (`:449`) — once per topic
  per role, under the lock, never per row.
- **No Fletcher floor under a supplied profile.** The fresh-QoS-per-call form is intact for
  all three roles; `MinimalProfileTakesFastDdsDefaultsNotFletchers` and
  `AnAnchorOnlyDocumentResolvesToFletchersBuiltIn` both pass.
- **`ProviderConfig`'s typed core is untouched** — the fix pass contains no `pubsub/` file
  at all, so it is still exactly `{max_payload_bytes, domain_id}` plus the opaque document,
  and PDA-DEC-4's `static_assert` on `&ProviderRegistry::Create` is undisturbed.
- **No refusal became a recovery.** A per-topic refusal out of `Publish` leaves
  `ts.writer` null, so the next `Publish` refuses identically; nothing falls back to a
  default after refusing once.

## NON-BLOCKING

### F8 — the constructor's own stated invariant is now false, and the public surface does not disclose the new refusal

`src/fast_dds_pubsub_provider.cpp:194-197` still opens with

> "Everything the document decides is settled BEFORE the participant exists, so a
> misconfigured provider is never constructed at all (rung-2, spec §5.1)."

Fifty lines below it the fix pass calls
`RefuseMisplacedFletcherPropertiesInRoleProfiles` **after** `create_participant`,
`create_publisher` and `create_subscriber` — and for a profile named after a topic the
refusal fires from `Publish` (writer) or `Subscribe` (reader), i.e. from a data-plane
call, long after construction. The local comment on the new call is honest about this
("as late as the Publisher and Subscriber allow"); the sentence above it is not, and it is
the one a reader hits first. The public header's
"── Refused in the constructor, all `kInvalidArgument` ──" list, which is the item's
*documented surface*, omits the new refusal class altogether, and neither it nor spec
§4.1's Fast DDS paragraph tells an operator that a misplaced key in a per-topic profile
surfaces on the first publish rather than at start-up. Cycle 1 called a promise the code
does not keep worse than no promise; this is a smaller instance of the same class, created
by the fix pass.

Behaviour is *stricter*, not weaker, and the spec sets no per-method status set, so nothing
is violated — but the record is now wrong in the file that carries it.
**Acceptable fix:** narrow the ctor sentence to the refusals it still covers, add one
bullet to the header's refusal list naming the endpoint-profile case and where it fires,
and one clause in spec §4.1.

### F9 — the lookup-skip proof names one escape channel; there are two

`DocumentMayName`'s comment states the rule as a proof: a profile named `N` occurs
literally as `profile_name="N"` "UNLESS a character of it was spelled with an entity or
character reference, which needs an `&`". XML attribute-value handling has a second
channel the sentence does not name — whitespace normalisation (a tab/CR/LF inside an
attribute value becomes a space; document-wide CR/CRLF becomes LF). A profile name
Fletcher asks for as `"a b"` that the operator wrote as `profile_name="a<LF>b"` would
resolve in Fast DDS while failing the substring test, and the lookup would be skipped — a
silently different QoS, which is the failure the charge calls worse than log noise. It is
close to unreachable (it needs a topic name containing whitespace *and* an operator who
spelled that whitespace differently in the XML) and it is conditional on the parser
actually normalising, which I could not verify from the binary package. But the text
claims a proof, and a proof with an unnamed exit is a heuristic in better clothes.

**Acceptable fix:** one line — widen the hatch to `document.find_first_of("&\t\r\n")`
(it costs one extra lookup only on documents with a newline inside an attribute value,
i.e. almost none), or name the second channel beside the `&` one in the comment and in the
README residue.

## Budget — cycle 1's verdict still holds, with one honesty correction repeated

Fix pass, excluding the two review documents: **+661 / −96**. Split: tests + harness
**+451 / −70** (68% of the adds), production (public header + `src/`) **+149 / −15**, docs
**+34 / −9**. Every one of the findings closed here was ordered by a review, and four of
them ordered a test by name, so this is still **review-mandated test mass, not stored
complexity**, and the remedy is still a corrected number rather than scope reduction. Two
qualifications worth naming:

- The fix pass's production growth is **logic, not test mass**: `+132` lines in
  `src/internal/profile_document.hpp` are two new guard predicates, a new refusal function
  and its wiring. The resolution ladder the design describes as "a line-for-line
  replacement of `Impl::Resolve{Writer,Reader}Qos`'s map lookup" is now a **gated** ladder
  with a proof attached. It changes no answer (mutation-checked above) and it is disclosed
  in the header and the README — but it is design shape the design does not describe, and
  PDA-DEC-7 will read this file as the pattern for XRCE.
- Item total is now roughly **+2890 / −717** against a declared **+780 / −310** (~3.7× on
  adds). A fix pass that adds two thirds of a fresh item is worth naming once: it did so
  because cycle 1 and 4b between them ordered five guards and a file-reading test, not
  because scope drifted.

## RECORD (PM corrects in place; never blocking, never a fix cycle)

- **Cycle 1's production-accounting correction was repeated verbatim as an error.** The fix
  pass is reported as "production `src/` + public header only +30/−9"; measured, it is
  **+149 / −15**, because `src/internal/profile_document.hpp` (+132/−14) is again excluded
  from "production". Cycle 1 made this exact correction against the original implementer's
  "`src/` is only +75/−52".
- **S3's numbers, measured by me on the whole provider binary at `7f6a310`:** **10**
  `XMLPARSER Error` lines suite-wide, of which **1** contains "profile not found" — not
  "7 → 0". That one is the deliberate no-anchor row of
  `MalformedProfileDocumentIsRefused`, where the profile genuinely is absent and Fast DDS
  logs it before the provider throws; every other line belongs to a negative test with a
  genuinely malformed document. The substance holds and is the part that matters: **zero on
  any happy path**, and a real `--provider fastdds --provider-config <valid>` start-up
  prints nothing but `READY`.
- **Spec §10's corrected header still does not reconcile with its own table.** It now reads
  "8 files, 12 construction sites: 11 migrated … and the gateway's one replaced outright".
  The table's own `Sites` column sums to **13** (reading "4 pairs" as 4), and actual
  provider constructions in those eight files are **17** (test_roundtrip 8, test_interop 3,
  fastdds_peer 1, fastdds_main 1, fastdds_peer_main 1, example.cpp 2, exp_zero_copy 3,
  gateway 0) — the column mixes "pairs", helper functions and constructions. PDA-DEC-7 will
  cite this table, so the counting convention wants stating once.
- The other two in-place corrections **read true** against the tree: spec §10's "Docs:"
  paragraph (both `README.md` and `docs/architecture-overview.md` carry no retired
  vocabulary, and the only code they show is `make_shared<FastDDSPubSubProvider>()`, which
  still compiles because the ctor's `ProviderConfig` argument is defaulted), and
  `docs/architecture-overview.md` §7.4's include path.
- `plans/PDA-DEC-6-brief.md:58`'s "*As landed (`<date>`…)*" footer is still the placeholder
  cycle 1 recorded; unchanged by the fix pass.
- Counts confirmed against the tree: **85 ctest entries / 84 gtest cases, 84/84 green**.
- Environment, for whoever builds next: the in-tree `fastdds-pubsub-provider/build/` tree
  cannot reconfigure — its `fletcher-pubsub` package folder
  (`~/.conan2/p/b/fletcd3e11389427b8`) was removed by a concurrent `conan create`; use a
  cache build folder or re-run `conan install`.
  `C:\ProgramData\eprosima\fastdds_interprocess` was empty throughout and my runs leaked
  nothing into it.
