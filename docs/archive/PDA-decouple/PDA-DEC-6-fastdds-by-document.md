# PDA-DEC-6 — Fast DDS configured by document; `FastDDSProviderOptions` retired

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §4 (esp. clause
4), §4.1, §4.2 (§5.1, §7 constraining). Decisions 3, 8, 9, 13, 14. Rulings 2026-08-31
("XML profile config only — one way to do it"), 2026-09-02 (protocol settings move into
the document). Predecessors: [PDA-DEC-4](PDA-DEC-4-provider-registry.md) (frozen
`Create`), [PDA-DEC-5](PDA-DEC-5-inprocess-builtin.md) (the pattern repeated here).

## Summary

Fast DDS becomes selectable as the built-in name `fastdds` through PDA-DEC-4's registry,
and every QoS knob it has moves out of a typed C++ struct into its own native XML profiles
document, which only this provider reads. `FastDDSProviderOptions` is deleted, taking the
last eProsima type out of a Fletcher-facing header: the dependency drops PUBLIC→PRIVATE.

## Design

### 1. The registration, and the one construction API

```cpp
// fastdds-pubsub-provider/include/.../fast_dds_pubsub_provider.hpp
void RegisterFastDDSProvider(ProviderRegistry& registry);   // registers "fastdds"
class FastDDSPubSubProvider : public PubSubProvider {       // dtor, the four overrides
   public:                                                  // and PayloadBytes() as-is
    explicit FastDDSPubSubProvider(const ProviderConfig& config = {});
};
```

The header stops including `<fastdds/...>` entirely; `internal/qos_defaults.hpp` moves to
`src/internal/`, having been public only to default-initialise the retired struct.
Typed core (§4.1): `domain_id` is used as given (both `uint32_t`, no narrowing);
`max_payload_bytes == 0` means unset and resolves to **65536**, bit-for-bit the retired
struct's `kPayloadBytes<64*1024>`, because the bound is part of the registered DDS type
name and a different number silently stops endpoints discovering each other (decision
13). An unusable bound is still refused before the participant exists, now as
`PubSubError(kInvalidArgument)`: through a factory the old `std::invalid_argument`
reaches a caller as `kInternal`, which tells an operator nothing (§5.1).

### 2. The document is a Fast DDS XML profiles document, and Fast DDS parses it

Fletcher gains no parser and this provider gains no dependency (decision 8, §4.2): Fast
DDS 3.4 exposes `get_*_qos_from_xml(xml_string, qos, profile_name)` on the factory, the
`Publisher` and the `Subscriber`, parsing a **string** and returning a QoS **without
registering anything process-wide**. `load_XML_profiles_string` is deliberately *not*
used: it writes into the singleton `XMLProfileManager`, where two instances with
different documents under one profile name collide — the global state §4 clause 3
forbids. Reserved names, resolved at construction and at `CreateTopic`/`Subscribe`:

| Role | Profile looked up | Falls back to |
|---|---|---|
| participant | `fletcher_participant`, via `get_participant_**extended**_qos_from_xml` | `PARTICIPANT_QOS_DEFAULT`, name `FletcherParticipant` |
| data writer, topic `T` | `T` (the joined topic string), then `fletcher_writer` | `MakeFletcherDefaultWriterQos()` |
| data reader, topic `T` | `T`, then `fletcher_reader` | `MakeFletcherDefaultReaderQos()` |
| `__schema` channel | — nothing — | `MakeSchemaChannel{Writer,Reader}Qos()` |

Per-topic override = a profile named after the topic (the plan's own wording), a
line-for-line replacement of `Impl::Resolve{Writer,Reader}Qos`'s map lookup.

**The participant uses the *extended* call so `<domainId>` cannot be dropped in
silence.** A `<participant>` profile may carry a domain, and the likeliest first
document is one pasted out of the operator's own Fast DDS application. The typed core
still wins (ruling 2026-09-02), but a disagreement is **refused** `kInvalidArgument`
quoting both numbers rather than landing on domain 0 with no error — the "wrong answer
rather than a failure" `provider_registry.hpp:113-116` already rules unacceptable. The
extended call *replaces* the plain one, so this costs nothing. Residue:
`<domainId>0</domainId>` is indistinguishable from unset (README).

**A resolved profile is the WHOLE QoS for that endpoint.** No merge with Fletcher's
defaults: the XML API returns a filled QoS and cannot report which policies the document
mentioned, so any overlay rule would rest on a fact the substrate does not expose. One
rule instead — supply a profile for a role, own that role's QoS.

**That makes SILENCE load-bearing** — what a document omits decides an endpoint's QoS
outright — so four guards point at omission rather than at speech: the forcing test's
anchor-only row; `DefaultProfileTranscriptionIsExact` (**whole-struct**, in-process,
because the two policies whose loss is silent — `history` KEEP_ALL, `resource_limits` 100
— are `optional` in discovery data and unobservable there); P6; and the anchor itself.
**A non-empty document must define `<participant profile_name="fletcher_participant">`**
even if empty of policies: `get_*_from_xml` returns `BAD_PARAMETER` for both "malformed"
and "absent", so without one mandatory anchor a broken document — or an XRCE `key=value`
document pasted into the wrong field — resolves to "no profiles found" and runs happily
on the defaults. Failing that lookup is `kInvalidArgument`, in the constructor.

### 3. The two settings a QoS profile cannot express

`loan_publish` (which publish path) and `max_schema_bytes` (the `RawBytesPubSubType`
bound on the internal schema channel) are Fletcher's, not DDS's. Neither is a QoS policy,
and a second document format for them would break "one way to do it". They live as
**vendor properties inside the anchor's `<rtps><propertiesPolicy><properties>`** —
native Fast DDS XML, parsed by Fast DDS, surfaced as
`DomainParticipantQos::properties()`, so still no second reader:

```xml
<property><name>fletcher.loan_publish</name><value>true</value></property>
<property><name>fletcher.max_schema_bytes</name><value>131072</value></property>
```

Both are provider-wide switches, which is what a participant profile is for. A
`fletcher.`-prefixed property that is not one of these two, or whose value does not
parse, is refused `kInvalidArgument` quoting it; the two the provider consumes are
**stripped before `create_participant`** (consume what you own — which also means
`<propagate>true</propagate>` cannot put a Fletcher key into DDS discovery data, a
README residue), and every other property reaches Fast DDS untouched: security
plugins need that.

### 4. The callers

`gateway/src/main.cpp`: PDA-DEC-5's inline `fastdds` closure becomes
`fletcher::RegisterFastDDSProvider(registry)`, so the gateway names no concrete provider
type; new `--provider-config FILE` reads a file into `ProviderConfig::document`
(unreadable → stderr + exit 2, as for a bad selector) — the surface PDA-DEC-5 DEBT-5
recorded as missing. At the other **seven** external sites (Files-to-touch lists them;
spec §10's count is stale) each `FastDDSProviderOptions o; o.domain_id = D;` becomes
`ProviderConfig{0, D, ""}` — empty document, default bound, identical behaviour and type
name. `benchmarks/exp_zero_copy.cpp` also needs `fletcher.loan_publish` and a writer
profile, and is this item's own proof the document expresses what the struct did.

### 5. "Fletcher never learns DDS vocabulary" — how I would know it failed

Three machine checks, no prose. (1) `CMakeLists.txt` links fast-dds **PRIVATE** and
`conanfile.py` drops `transitive_headers=True` (keeping `transitive_libs`; the lib is
STATIC), so `test_package/src/example.cpp` compiles **without Fast DDS include
directories** and any surviving eProsima type in the public header is a compile error
there — the acceptance test, and it costs nothing. (2) `ProviderConfig` gains no field;
PDA-DEC-4's `static_assert` on `&ProviderRegistry::Create` fires if this item widens it.
(3) `pubsub/`, `pubsub-arrow/`, `gateway/` then name no eProsima type, proved by the
compiler. **PDA-DEC-7** does this for XRCE's `key=value` document and the `uint16_t`
narrowing refusal; this item shares no reader with PDA-DEC-5's loopback.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *Configuring Fast DDS QoS from C++ without the document.* One constructor, taking
   `ProviderConfig`; the eProsima types are unreachable from the installed headers, so
   "caller holds a `DataWriterQos` for this provider" cannot be written.
2. *Two instances fighting over one profile table.* No process-wide registration; each
   parses its own string, so "instance B sees instance A's profiles" does not exist.
3. *A partially-applied QoS.* A role's QoS is the resolved profile or the built-in
   default, never a merge — no precedence question, no half-configured endpoint.
4. *A configurable `__schema` channel* (no profile name is consulted for it; §4.1 /
   TD-005), and *the gateway branching on which provider it selected* — PDA-DEC-5's
   property, completed here by removing the last concrete provider type from that file.

**Rung 2 — refused typed at the door, in the constructor**

5. A non-empty document that is not a Fast DDS profiles document, or omits
   `fletcher_participant` → `kInvalidArgument`. Closes §2's silent-fallback hole.
6. An unknown or unparseable `fletcher.*` property → `kInvalidArgument`, quoting it; a
   typo'd `fletcher.loanpublish` must not be inert.
7. A non-zero `<domainId>` in the anchor disagreeing with the typed core →
   `kInvalidArgument`, quoting both; silently preferring either is a wrong answer.
8. `max_payload_bytes` that `IsPayloadBound` rejects → `kInvalidArgument` (was
   `std::invalid_argument`; §1). A duplicate `fastdds` registration or an unknown
   selector → PDA-DEC-4's refusals, no new code.

**Handled residue** — each with why it could not be forbidden

- **H1 — a per-topic profile whose name matches no topic is inert.** *Why not forbidden:*
  the XML API resolves a profile **by name** and cannot enumerate what a document defines,
  so the provider cannot see a profile it never asks for — rungs 1 and 2 both need
  knowledge the substrate does not expose. README.
- **H2 — a supplied reader profile can re-enable receive-side data-sharing,** which the
  built-in default turns off. *Why not forbidden:* a Fletcher floor would mean the document
  does not really configure QoS (2026-08-31 ruling), and PDA-ABI-7 needs it on. README.
- **H3 — a profile's `resource_limits` can oversize the data-sharing segment; H4 — every
  non-empty document loses the `FletcherParticipant` name** unless its anchor sets one,
  so the anchor makes H4 **universal, not exotic**. *Why not forbidden:* ordinary policies
  in the profile the operator now owns (rung-1 case 3), and Fletcher knows neither the
  memory budget nor the naming need; H4 is diagnostic-only, nothing keys on it. README.

## Premises and stop conditions

P1 and P6 are unprovable from a binary-only Conan package, so **neither is left as a bet:
each has a test row that measures it in this item.**
- **P1 — the `*_from_xml` family exists and registers nothing process-wide.** The API
  half is **verified** in `fast-dds/3.4.0` (`DomainParticipantFactory.hpp:251`,
  `Publisher.hpp:390`, the `Subscriber` counterpart, B2's extended variant), as is the
  one-code-for-both return contract. The global-state half is measured by
  `TwoInstancesResolveTheirOwnDocuments`, because a stale singleton entry hands instance
  B instance A's QoS — a wrong answer, not a refusal. **STOP-AND-ASK** if it goes red:
  the answer is *not* `load_XML_profiles_string` plus name mangling, it is whether §4
  clause 3 survives for this provider.
- **P6 — a document containing any malformed profile fails *every* `get_*_from_xml` call
  on it,** so the anchor catches partial malformation too. Forbidding is unavailable (the
  substrate cannot enumerate a document's profiles); measured by the
  valid-anchor-plus-broken-`fletcher_writer` row of `MalformedProfileDocumentIsRefused`.
  **STOP-AND-ASK** if a document can partially parse: the anchor is then insufficient and
  the not-found rung — reading `BAD_PARAMETER` as "absent" — needs a different shape.
- **P2 — discovery carries `durability`, `reliability`, `data_sharing`** (verified,
  `rtps/builtin/data/PublicationBuiltinTopicData.hpp`) — and, equally load-bearing,
  **`history` and `resource_limits` are `optional` there and NOT observable**, hence the
  in-process transcription guard. **STOP-AND-ASK** if the discovery callbacks prove
  unusable; a delivery-timing inference is no substitute.
- **P3 — a factory's `PubSubError` reaches the caller intact** (PDA-DEC-5's P1, proved);
  **STOP-AND-ASK** if a bad document arrives as `kInternal`. **P4 — `Create`/`Register`
  suffice as frozen**; **STOP-AND-ASK** if a registry change looks necessary (§4 clause
  2). **P5 — no in-tree caller sets `max_schema_bytes` in production** (two tests, both
  probing the rejection path); **STOP-AND-ASK** if one turns up.

## Forcing-test mapping

New TU `fastdds-pubsub-provider/tests/test_profile_document.cpp`. To prove a QoS reached
a live **endpoint**, a test reads the *discovered* value from a bare observer participant
(`DomainParticipantListener`), with a **per-row latch and hard timeout — nothing is
compared until that row's callback has fired**, else a row expecting a value equal to a
default-constructed policy proves nothing while staying green. Where the claim is about
**resolution**, structs are compared whole in-process: no discovery, every policy.

| Test | Green by | Red for the right reason / mutation |
|---|---|---|
| **`FastDdsConfig.ProfileDocumentConfiguresQos`** (forcing) | §2. One TEST (not `TEST_P`, so the ctest name is exact) looping a **4**-row table: empty document → discovered `TRANSIENT_LOCAL`+`RELIABLE`; **anchor-only, non-empty document → the same, i.e. *Fletcher's* built-in, not Fast DDS's**; a `fletcher_writer` profile with `VOLATILE` → `VOLATILE`; one with `BEST_EFFORT` → `BEST_EFFORT` | Does not compile before the change (`ProviderConfig` ctor absent). **M1: hard-code any single value** → ≥1 row red, the point of two rows differing from the default. **M2: parse then ignore** → rows 3–4 red. **M3: apply row 3's profile to the reader** → red. **M12: fall back to `DATAWRITER_QOS_DEFAULT` whenever the document is non-empty** → row 2 red, and row 2 is the only row that catches it — it is also the shape every `fletcher.*`-property document has |
| `FastDdsConfig.PerTopicProfileOverridesTheDefault` | §2's lookup order. Two topics on one instance; only one has a topic-named profile; discovered durability differs between them | M4: drop the topic-name lookup → both show the default → red. M5: apply the topic profile to every topic → red |
| `FastDdsConfig.ReaderProfileConfiguresTheReader` | Same, via `on_data_reader_discovery` and `SubscriptionBuiltinTopicData` | Live control for M3: writer-only application reddens this while the forcing test stays green — neither alone is sufficient |
| `FastDdsConfig.SchemaChannelIgnoresTheDocument` | Rung-1 case 4. A document whose `fletcher_writer` says `VOLATILE`; the `__schema` topic's discovered writer must still be `TRANSIENT_LOCAL` | M6: apply the data profile to the schema channel → red. The negative control for "the document reached Fast DDS at all" |
| `FastDdsConfig.DefaultProfileTranscriptionIsExact` (was `PublishedDefault…`) | **In-process, whole-struct.** Feed the README's starting-point block to `get_datawriter_qos_from_xml(doc, qos, "fletcher_writer")` and assert `qos == MakeFletcherDefaultWriterQos()`; same for the reader. `operator==` exists on both. Covers all six policies, including `history` and `resource_limits`, which discovery cannot see | M7 (repaired): edit the README block or `qos_defaults.cpp` apart → red **unconditionally** — the old discovery form compared two instances and stayed green if the provider ignored the document entirely. M13: transcribe `KEEP_LAST(1)` or `max_samples 5000` → red, and those are the two whose loss is silent row loss and the 32-bit segment overflow. A policy that provably cannot be transcribed is named in the README by this assert failing |
| `FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` | P1's global-state half, ~15 lines: two providers alive at once, **same** profile names, different values, one topic each; each writer's discovered durability must be its own document's | M14: route resolution through `load_XML_profiles_string` → the second load collides or the first wins → red. Without this row P1's bet ships untested until PDA-DEC-8 |
| `FastDdsConfig.MalformedProfileDocumentIsRefused` | Rung-2 cases 6–8. Asserts `kInvalidArgument` **and** the quoted text for: truncated XML; a `key=value` document; no `fletcher_participant`; **a valid anchor plus a syntactically broken `fletcher_writer`** (P6); `fletcher.loanpublish=true`; `fletcher.loan_publish=yes`; **`<domainId>3</domainId>` against `config.domain_id = 7`** | M8: fall back to defaults on a parse failure → red. The guard that stops a provider which never reads the document from greening the suite. **If the P6 row cannot be made red, P6 is false — stop, do not weaken the row** |
| `FastDdsConfig.LoanPublishComesFromTheDocument` | §3. With the property `true`, a row past the bound **throws** out of `Publish`; with it absent, the sample is dropped internally — the documented difference between the two publish paths | M9: hard-code either path → one of the two rows red |
| `FastDdsConfig.SchemaBoundComesFromTheDocument` | §3. Replaces the two retired `max_schema_bytes = 8` tests: property `8` → the oversized schema is rejected on the channel; absent → it is delivered | M10: ignore the property → red |
| `Registry.FastDdsResolvesAsABuiltIn` — in **`conformance_fastdds`**, not `conformance_registry` | §1, mirroring PDA-DEC-5. Publishes a row through a base-typed handle from `MakeProvider(registry, "fastdds", cfg)` and asserts it arrives byte-identical. `conformance_registry`'s link line is deliberately narrow (PDA-DEC-4 DEBT-7's "no transport SDK is reachable" guard) and linking the DDS SDK into it would weaken that check | M11: register under `"dds"` → unknown name → red |

Inner loop: `ctest -R '^FastDdsConfig\.'` in the provider suite, then `-R '^Registry\.'`
in `pubsub-conformance`. Forcing test alone:
`ctest -R '^FastDdsConfig\.ProfileDocumentConfiguresQos$'`. **This item mandates a
full-suite run** (provider's 69 + conformance with `-DFLETCHER_CONFORMANCE_XRCE=ON`
passed explicitly + both gateway harnesses).

**`gateway-fastdds-ts` is in the blast radius and its binaries are stale (2026-09-01
22:05).** Its CMake project builds *both* `gateway.exe` and `fastdds_peer`; `npm test`
builds neither. Reconfigure and rebuild first, then report 4/4 with **no intermittent row
loss across repeated runs**, and that peer and gateway agree on the type name — they do
only if both resolve the bound to 65536, and a mismatch shows as zero deliveries, not a
warning. Only reproducer of the data-sharing defect: green, assertions unweakened.

## Risks / Unknowns

- **False greens.** (1) `-o run_tests=True` is a no-op on a cached Conan package —
  "Already installed!" is not a pass; `fastdds-pubsub-provider` and `pubsub` must be
  re-created. (2) `cmake --preset conan-default` does not reset a cached
  `FLETCHER_CONFORMANCE_XRCE`; pass it explicitly. (3) The harness resolves providers
  **and core headers** from the Conan cache, so a stale one greens the new tests against
  *old* behaviour. (4) Both gateway harnesses run whatever binary is in `build/`.
- **Public surface: +2 / −2, net 0** — adds `RegisterFastDDSProvider` and the
  `ProviderConfig`-**taking** ctor (`ProviderConfig` itself stays a frozen aggregate);
  retires `FastDDSProviderOptions` (7 fields, 2 eProsima types) and its ctor.
  Structurally negative: `qos_defaults.hpp` leaves the installed tree, fast-dds stops
  being transitive. **No coexistence window is created**, and PDA-DEC-5's `fastdds`
  closure body — the only construct scheduled for deletion — goes here, on schedule.
- **Wire bytes unchanged** (decision 13): encoder, sample layout and registered type name
  untouched, bound resolves to the same 65536. No divergence fix, so no stop-and-ask.
- **Spec touch, editorial:** §4 clause 4 records `fastdds`; §4.1 gains the Fast DDS
  document beside the loopback's; §10's blast-radius table is **corrected, not merely
  marked done** — "4 files / 19 occurrences" is wrong in both halves and PDA-DEC-7 cites
  it. **Assumed:** no out-of-tree caller constructs the provider with a typed QoS.

## Files-to-touch

**Changed**, under `fastdds-pubsub-provider/` unless noted:
`include/fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp` ·
`src/fast_dds_pubsub_provider.cpp` · `{CMakeLists.txt,conanfile.py}` · `README.md`
(document format, reserved names, starting-point profile, H1–H4, the `<propertiesPolicy>`
residues) · `test_package/src/example.cpp` ·
`tests/{test_fast_dds_pubsub_provider.cpp,CMakeLists.txt}` ·
`benchmarks/{exp_zero_copy.cpp,conanfile.py,CMakeLists.txt}` — the build files because
dropping `transitive_headers` cuts Fast DDS include dirs off all four benchmark TUs
(outside CI, so this rots silently); they gain an explicit require and link. Elsewhere:
`gateway/src/main.cpp` ·
`integration-tests/{pubsub-arrow-fastdds/tests/test_roundtrip.cpp,fastdds-xrce-interop/tests/test_interop.cpp,gateway-fastdds-ts/src/fastdds_peer.cpp}`
· `integration-tests/pubsub-conformance/{subjects/fastdds_main.cpp,subjects/fastdds_peer_main.cpp,src/registry.cpp,CMakeLists.txt}`
· `docs/{pubsub-interface-spec,architecture-overview}.md` · root `README.md` · stale
comments naming the retired type in `pubsub/include/fletcher/pubsub/provider.hpp:72-77`
(booked here by PDA-DEC-4 DEBT-10b),
`src/internal/{data_writer_listener,data_reader_listener,sample_writer}.hpp` and both
integration-test READMEs. **New:**
`fastdds-pubsub-provider/{tests/test_profile_document.cpp,src/internal/qos_defaults.hpp}`.

## Files-to-delete

- `include/fletcher/fastdds_pubsub_provider/internal/qos_defaults.hpp` → `src/internal/`.
- `struct FastDDSProviderOptions` and `FastDDSPubSubProvider(FastDDSProviderOptions)` →
  `ProviderConfig` + the profiles document. Field by field: `domain_id`,
  `max_payload_bytes` → typed core (0 = unset → 65536); `default_writer_qos`,
  `default_reader_qos` → profiles `fletcher_writer` / `fletcher_reader`;
  `topic_writer_qos`, `topic_reader_qos` → profiles named after the topic;
  `loan_publish`, `max_schema_bytes` → properties `fletcher.loan_publish` /
  `fletcher.max_schema_bytes`. Plus the gateway's inline `fastdds` closure →
  `RegisterFastDDSProvider`.
- **Tests retired, each replaced:** `CustomDefault{Writer,Reader}Qos`,
  `PerTopic{Writer,Reader}QosOverridesDefault`, `AutonomyStyleProfileViaOptions` →
  `ProfileDocumentConfiguresQos`, `PerTopicProfileOverridesTheDefault`,
  `ReaderProfileConfiguresTheReader`, asserting the *applied* QoS rather than "a sample
  arrived"; the two `max_schema_bytes = 8` tests → `SchemaBoundComesFromTheDocument`.
  *Nothing retired without replacement.*

## Numbers

Declared net lines **+780 / −310** (+60 vs cycle 1: B1/B2/B3, DEBT-1's row, the
two-instance measurement, the benchmark build files). New public surface **net 0**
(+2 / −2; the struct's 7 fields and 2 eProsima types not in its favour). Cycles 2/2.
