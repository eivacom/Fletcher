# PDA-DEC-6 — independent code review

**Item:** Fast DDS configured by document; retire `FastDDSProviderOptions`
**Diff:** `git diff f87248e..6a66a15` — 28 files, +2229 / −621
**Branch:** `feature/protocol-driver-abi`, working tree clean at `6a66a15`

**Verdict: no blocking findings.** 4 should-fix, 6 nits.

## What I ran

Built and ran the provider suite locally (not via `conan create`, which reported
"Already installed!" and is a false green):

```
conan install . -pr:a=<profile> -o "&:run_tests=True"
cmake --preset conan-default && cmake --build --preset conan-release
ctest -C Release
```

* **80/80, four consecutive runs**, zero failures, zero leaked shared-memory segments.
* `test_package` builds and passes with **no Fast DDS include directories** — the
  claimed machine check for "no eProsima type in the installed header" is real.
* Mutation, merge-seeding: seed both `internal::Resolve{Writer,Reader}Qos`'s output
  with `MakeFletcherDefault*Qos()` → **80/80 still green** (see RECORD).
* Mutation, M12: not-found branch returns `DataWriterQos()` / `DataReaderQos()` →
  **4 tests red**, including `AnAnchorOnlyDocumentResolvesToFletchersBuiltIn`. That
  guard is real and falsifiable.
* Concurrency probe: 4 threads × 400 iterations of `Resolve{Writer,Reader}Qos` over
  two different documents using the same profile names, on one publisher/subscriber
  pair — 0 wrong results across 3 runs. No process-global profile table, as claimed.
* Substrate probe: `pub->set_default_datawriter_qos(MakeFletcherDefaultWriterQos())`
  then resolve a minimal profile → `history=KEEP_LAST, max_samples=5000`, i.e. the
  publisher's default QoS does **not** seed `get_datawriter_qos_from_xml`. The
  substrate overwrites; the header's MEASURED note is correct from both directions.

**The no-merge form is correct.** A fresh QoS per call, Fletcher's built-in only on
the not-found branch, `__schema` on `MakeSchemaChannel*Qos()` with no profile name
consulted, and `create_publisher(PUBLISHER_QOS_DEFAULT)` /
`create_subscriber(SUBSCRIBER_QOS_DEFAULT)` so nothing Fletcher-valued sits under a
supplied profile even via the substrate's own seed. Every refusal fires before
`create_participant`. No leaked reference into a caller's string: the document is
copied by value into `Impl::document` and re-parsed per endpoint.

### Not a finding — environment, but tell the next agent

Mid-review, `FastDdsConfig.SchemaBoundComesFromTheDocument` and
`FastDdsConfig.AFailedSchemaAnnouncementCanBeRetried` began failing **0/5** with
`SEH exception 0xc0000005`. Bisected to `create_participant(99, PARTICIPANT_QOS_DEFAULT)`
faulting for *any* QoS and an empty document — nothing to do with this change. Cause:
**127 leaked Fast DDS shared-memory segments in `C:\ProgramData\eprosima\fastdds_interprocess`**
(dated Sep 1 and earlier), one of which poisoned domain 99. Moved them to
`C:\tmp\shm_backup`; domain 99 then fine and the suite 80/80 four times with no new
leaks. If provider tests start faulting on this box, clear that directory first.

---

## should-fix

### S1 — `--provider-config` has no test coverage at all (confidence: high)

The flag is the whole point of the item's second half ("DEBT-5 is CLOSED here…
charter requirement (b) is now reachable from gateway.exe"), and nothing exercises
it: not the happy path (a real profiles document reaching the provider), not the
unreadable-file exit 2, not a provider-rejected document exit 2. Grepping for
`provider-config` finds only `gateway/src/main.cpp`, the two READMEs and the spec.

A regression here is silent in exactly the way that matters: drop `std::ios::binary`,
or drop `config.document = args.document`, and the gateway still starts, still serves,
and quietly runs on Fletcher's defaults. Nothing goes red.

**Fix:** three cases in `integration-tests/gateway-end-to-end/test/end-to-end.test.ts`,
next to the existing CLI-refusal tests at line 183 (`an unregistered --provider name
exits 2 naming what IS registered`) — the pattern is already there:
`--provider-config <missing>` exits 2; `--provider fastdds --provider-config <a
document with a bad fletcher.* property>` exits 2 quoting the property; and one
happy-path run with a valid anchor-only document that still moves a row.

### S2 — an empty `--provider-config` file is accepted and silently means "unconfigured" (confidence: high)

`ReadProviderDocument` returns `buffer.str()`, and an empty file yields `""`, which is
the provider's documented "Fletcher's built-in profile everywhere". So
`gateway --provider-config qos.xml` against a zero-length, truncated or
wrong-but-empty file starts successfully, applies none of the operator's intent, and
prints nothing. The provider cannot refuse it — "empty means built-in" is its
contract, and rightly so, since every existing caller relies on it.

This is the forbidding direction: the refusal belongs at the door. An operator who
passed `--provider-config` asked to be configured *from that file*; an empty read is
as much a failure as an unreadable one, and the gateway is the only place that knows
the difference between "no flag" and "flag, empty file".

**Fix** (`gateway/src/main.cpp`, in `ReadProviderDocument`, after the `bad()` check):

```cpp
std::string document = buffer.str();
if (document.empty()) {
    std::fprintf(stderr,
                 "fletcher-gateway: --provider-config %s is empty; omit the flag to "
                 "run on the provider's own defaults\n", path);
    std::exit(2);
}
return document;
```

### S3 — the resolution ladder logs a Fast DDS **ERROR** on the happy path (confidence: high, measured)

`Resolve{Writer,Reader}Qos` use "look up, fail, fall through" as control flow against
an API that logs its misses at ERROR level. Measured on the tree as landed:

```
[XMLPARSER Error] Publisher profile not found -> Function fill_attributes_from_xml
```

* `FastDdsConfig.PerTopicProfileOverridesTheDefault` (a document with `fletcher_writer`
  plus one per-topic profile): **1** such line.
* `FastDdsConfig.ProfileDocumentConfiguresQos` and
  `FastDdsConfig.SchemaBoundComesFromTheDocument`: **4** each.

The anchor-only document is the shape the README calls "the most common document there
is" and "the shape of every document that exists only to carry a `fletcher.*`
property" — and it produces two ERROR lines per writer and two per reader, forever, on
a *correct* configuration. Two consequences: an operator reasonably reads
`[XMLPARSER Error]` as "my document is broken", and a genuine XML error in a document
becomes indistinguishable in the log from an expected fallback.

**Fix**, in order of preference: (a) narrow the ladder — try the topic-named profile
per endpoint, but resolve `fletcher_writer`/`fletcher_reader` once at construction
rather than re-probing per endpoint, which removes most of the misses; (b) suppress
the category around the two speculative lookups via `eprosima::fastdds::dds::Log`; or
at minimum (c) add a line to the README's "Known limits of the document" saying these
ERROR lines are expected and what they mean. (c) alone documents a defect instead of
removing it, but it beats leaving operators to guess.

### S4 — nine migrated call sites use positional aggregate init of `ProviderConfig` (confidence: high)

```cpp
auto pub_provider = std::make_shared<FastDDSPubSubProvider>(ProviderConfig{0, kTestDomain, ""});
```

appears 4× in `pubsub-arrow-fastdds/tests/test_roundtrip.cpp`, 3× in
`fastdds-xrce-interop/tests/test_interop.cpp`, once in
`gateway-fastdds-ts/src/fastdds_peer.cpp` and once in
`pubsub-conformance/subjects/fastdds_main.cpp`. Field order is
`{max_payload_bytes, domain_id, document}`, so today they are right — but
`ProviderConfig` is a public config struct in another component
(`pubsub/include/fletcher/pubsub/provider_registry.hpp`) whose own doc comment already
forward-notes a PDA-DEC-7 change around its `domain_id` field. Insert or reorder one
member and all nine silently re-bind. A domain id landing in `max_payload_bytes` fails
`IsPayloadBound` loudly; a bound landing in `domain_id` puts every endpoint on the
wrong DDS domain with **no error and no data**, and since the bound is baked into the
registered type name the symptom is "discovery never matches". This is the same class
of hazard §4.1 already warns about ("a truncated domain id … is a wrong answer rather
than a failure").

`fastdds_peer_main.cpp` shows the safe form in this very commit
(`fletcher::ProviderConfig config; config.domain_id = …`). **Fix:** named assignment,
or C++20 designated initialisers (`ProviderConfig{.domain_id = kTestDomain}`) — the
tree is `cxx_std_20`.

---

## nits

* `--provider-config` is read inside `ParseArgs`, so `gateway --provider-config missing.xml --help` exits 2 instead of printing help; defer the read until after the flag loop.
* `gateway --provider-config` with no value falls through to `unknown argument: --provider-config`, which misdescribes the mistake (pre-existing pattern for every flag).
* `Document` / `WriterProfile` / `ReaderProfile` / `AnchorProperty` / `kTenSlots` are duplicated between `tests/test_profile_document.cpp` and `tests/test_fast_dds_pubsub_provider.cpp`, with two copies of `kTenSlots` that must agree. One test-only header would remove a chunk of the 963 new lines and the drift risk with them.
* `BoundedDocument()` in `test_fast_dds_pubsub_provider.cpp` threads `DocumentParts::writer_topic` into the writer's `<topic>` but hard-codes `kTenSlots` for the reader's; harmless today, a trap for the next editor.
* `FastDDSPubSubProvider(const ProviderConfig& config = {})` — the default argument makes the provider default-constructible, mildly at odds with "`ProviderConfig` and nothing else"; every in-tree caller already passes one explicitly.
* `Resolve{Writer,Reader}Qos` re-parse the whole document per endpoint (once per topic per role, under `impl_->mu`). Fine, and deliberately un-memoised — worth one line saying so, since "cache the parsed document" is the obvious and wrong optimisation here.

---

## RECORD (paperwork for the PM — not blocking)

* `tests/test_profile_document.cpp`, above `MinimalProfileTakesFastDdsDefaultsNotFletchers`: "seed `internal::ResolveWriterQos`'s output with `MakeFletcherDefaultWriterQos()` instead of a freshly default-constructed QoS and this test is the only thing that goes red" — **measured false**: that mutation leaves 80/80 green. `src/internal/profile_document.hpp` lines 21–25 say exactly the opposite ("leaves the whole suite GREEN, because this version *overwrites* its output parameter") and are correct. The same test's claim to be "the ONLY thing that can tell the owner's answer from the one they rejected" also does not hold against fast-dds 3.4.0.
* `fastdds-pubsub-provider/README.md:199–201`: the published starting-point block "cannot drift from the code without a test going red". The test holds its **own copy** of that XML (`kPublishedStartingPoint`); the README block is a hand-mirror and can drift with every test green. Tree convention elsewhere (`integration-tests/protoc-arrow-bridge/tests/test_accessor_readme_example.cpp`) states plainly that it "MIRRORS … (it is not auto-extracted)"; this one claims enforcement it does not have.
* Commit message: "`AnAnchorOnlyDocumentResolvesToFletchersBuiltIn` closes that hole … M12 now reddens exactly it." M12 as landed reddens **four** tests: `AnAnchorOnlyDocumentResolvesToFletchersBuiltIn`, `WriterOnlyDocumentLeavesTheReaderOnFletchersDefault`, `FastDDSPubSubProviderTest.ResubscribeAfterUnsubscribeKeepsDelivering` and `FastDDSPubSubProviderTest.UnsubscribeUnknownTopicIsHarmless`.
* `test_profile_document.cpp` is 963 lines against a declared item budget of +780/−310 (~2.9× on adds), while production `src/` moved only +75/−52.
