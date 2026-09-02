# PDA-DEC-6 — architecture review (cycle 1 of 2)

Design: [PDA-DEC-6-fastdds-by-document.md](../PDA-DEC-6-fastdds-by-document.md) (`738c634`, 300 lines).
Brief: [PDA-DEC-6-brief.md](../PDA-DEC-6-brief.md).
Verdict: **NEEDS-REWORK — 3 BLOCKERs, 11 DEBT.** No stop-and-ask: nothing here
deviates from a ruling or a locked decision.

Findings only. What is not listed below was checked and is sound — in particular
the retirement itself, the frozen `Create`, the no-ABI scope, the wire-byte
invariant, the aggregate field order (`ProviderConfig{0, D, ""}` is
`{max_payload, domain, document}` — correct), the `65536` bound continuity, the
call-site inventory, and the `<propertiesPolicy>` choice (ruled on in §R2).

---

## The one sentence behind all three BLOCKERs

The no-merge rule ("a resolved profile is the WHOLE QoS for its role") makes
**silence load-bearing**: what a document *fails* to say now decides an endpoint's
QoS outright. The design guards **speech** — every named test feeds a document that
says something and checks that it arrived — and leaves **silence** unguarded and, in
two places, unstated. B1, B2, B3 and DEBT-1 are four faces of that one gap.

---

## BLOCKERs

### B1 — unstated premise: is a Fast DDS profiles document parsed all-or-nothing?

§2 closes the silent-fallback hole with one anchor: a non-empty document must define
`fletcher_participant`, because `get_*_from_xml` returns `RETCODE_BAD_PARAMETER` for
both "malformed XML" and "no such profile". That closure is only total **if a
malformed profile anywhere in the document makes the participant lookup fail too**.
The design never states this, and the header cannot answer it — the Conan package
(`C:\Users\CTM\.conan2\p\fast-6b6862e624296\p\include\...`) ships headers and libs,
no source.

If Fast DDS's parse is instead per-profile tolerant, the exact hazard §2 exists to
kill survives by another route:

```xml
<dds><profiles>
  <participant profile_name="fletcher_participant"/>          <!-- parses -->
  <data_writer profile_name="fletcher_writer"><qos>
     <durabilty><kind>TRANSIENT_LOCAL</kind></durabilty>      <!-- typo -->
  </qos></data_writer>
</profiles></dds>
```

Construction succeeds (the anchor resolved). At `CreateTopic` the writer lookup
returns `BAD_PARAMETER`, the ladder reads that as "profile absent", and the writer
silently runs on Fletcher's built-in QoS while the operator believes their profile
is in force. Reachable (a typo in a policy name is the ordinary operator error) and
silent (an eProsima log line is not a typed signal — the design's own standard
everywhere else is refusal at the door).

**Acceptable fix (cheapest I would approve):** forbidding is not available (the
substrate cannot enumerate what a document defines), so state it as a premise and
measure it. Add **P6 — "a document containing any malformed profile fails *every*
`get_*_from_xml` call on it, so the `fletcher_participant` anchor catches partial
malformation too. STOP-AND-ASK if a document can partially parse: the anchor is then
not sufficient and the ladder's not-found rung needs a different shape."** Plus one
row in `MalformedProfileDocumentIsRefused`: valid anchor + syntactically broken
`fletcher_writer` → refused. Two lines of design, one test row; and if the premise is
false the row tells the implementer immediately instead of in production.

### B2 — a `<domainId>` in the operator's participant profile is silently ignored

`<domainId>` is part of a Fast DDS `<participant>` profile — that is precisely why the
`get_participant_**extended**_qos_from_xml` family exists
(`DomainParticipantExtendedQos::domainId()`, `uint32_t`, default 0 —
`DomainParticipantExtendedQos.hpp:75`). The design resolves the participant with the
**non-extended** call, so a document carrying `<domainId>3</domainId>` has it dropped
on the floor: the typed core wins, silently.

Ruling 2026-09-02 makes the typed core authoritative, so the *precedence* is right.
The *silence* is not. An operator pasting a participant profile out of their own Fast
DDS application — the single most likely source of a first document — lands on domain
0 with no error, no data and nothing to grep for. This is the same failure the seam
header already calls out as unacceptable for PDA-DEC-7: "a truncated domain id puts
the client on the wrong domain with no error, which is a **wrong answer** rather than
a failure" (`provider_registry.hpp:113-116`). The corner-case ladder does not list
this case at any rung.

**Acceptable fix: forbid, and it is cheaper than handling.** Resolve the participant
with `get_participant_extended_qos_from_xml(doc, extended, "fletcher_participant")` —
it returns the QoS you already wanted *plus* the domain, so it replaces a call rather
than adding one — and refuse with `kInvalidArgument`, quoting both numbers, when
`extended.domainId() != 0 && extended.domainId() != config.domain_id`. `<domainId>0</domainId>`
stays indistinguishable from "unset"; say so in the README as residue rather than
chasing it.

### B3 — the published starting point's guard cannot see the policies that lose rows

§2 promises "the README ships the exact XML transcription of Fletcher's built-in
profiles as the copy-paste starting point, kept honest by
`PublishedDefaultProfileMatchesTheBuiltIn`". That test compares **three** policies —
durability, reliability, data-sharing — via discovery. The built-in profile
(`src/qos_defaults.cpp:23-43`) is six:

| policy | built-in | Fast DDS default if the README block omits it |
|---|---|---|
| durability | TRANSIENT_LOCAL | VOLATILE — *checked* |
| reliability | RELIABLE | RELIABLE — *checked* |
| data_sharing (reader) | off | auto — *checked* |
| **history** | **KEEP_ALL** | KEEP_LAST(1) — **unchecked** |
| **resource_limits** | **100 / 1 / 100** | 5000 — **unchecked** |
| (reader) memory policy | PREALLOCATED (gates `CanLoanSamples`) | unchecked |

The two unchecked ones are the two the tree already documents as dangerous: KEEP_ALL
is what stops a RELIABLE writer overwriting unacked samples (silent row loss), and
`max_samples = 100` exists because "at 5000 that is gigabytes, which overflows the
segment's 32-bit size and drops the endpoint back to the transport"
(`qos_defaults.cpp:18-22`) — silently. Every migrating caller of the retired struct is
routed through this README block, so an inexact block is a silent regression for all
of them.

Worse, the guard **cannot** be fixed by adding rows to the discovery read-back:
`history` and `resource_limits` are `fastcdr::optional` in `PublicationBuiltinTopicData`
(lines 141-145) and are absent unless optional-QoS propagation is switched on.

**Acceptable fix, and it is cheaper than what is designed:** make this test in-process
and total. Feed the README block to `get_datawriter_qos_from_xml(doc, qos,
"fletcher_writer")` and assert `qos == internal::MakeFletcherDefaultWriterQos()` —
whole-struct `operator==` exists on `DataWriterQos` (`DataWriterQos.hpp:57`),
`DataReaderQos` and `DomainParticipantQos`. No participant, no discovery, no timing,
and it catches drift in every policy. If some policy provably cannot be transcribed
into XML, the assert says so and the README names it as a known non-transcribable
difference. (This also repairs M7, which is not a valid mutation as written — see
DEBT-10.)

---

## R1 — ruling on P1 (the load-bearing premise)

**Half verified in the tree, half correctly stop-conditioned; the design's word
"verified" overstates the second half.**

Verified, in the `fast-dds/3.4.0` package the provider requires
(`conanfile.py:48` → `C:\Users\CTM\.conan2\p\fast-6b6862e624296`):

- `DomainParticipantFactory::get_participant_qos_from_xml(const std::string& xml,
  DomainParticipantQos&, const std::string& profile_name) const` — `DomainParticipantFactory.hpp:251`;
- `Publisher::get_datawriter_qos_from_xml(xml, DataWriterQos&, profile_name)` — `Publisher.hpp:390`;
- the `Subscriber` counterpart; and the extended/participant variants B2 wants;
- the documented return contract is exactly what §2 claims: `RETCODE_OK` on success,
  `RETCODE_BAD_PARAMETER` otherwise — one code for "malformed" and "no such profile";
- `load_XML_profiles_string` exists as the alternative the design rejects
  (`DomainParticipantFactory.hpp:359`).

**Not** verifiable from the cache: "without registering anything process-wide". The
package is binary-only. The best header-level evidence is circumstantial and does
point the design's way — the sibling `get_default_participant_qos_from_xml` carries
"@note This method **does not update** the default participant qos", i.e. the whole
`*_from_xml` family is deliberately non-mutating. So P1 is a reasonable bet with the
right stop-and-ask attached, and I am **not** blocking on it. But see DEBT-3: the bet
is convertible into a measurement in this item for ~15 lines, and the failure mode if
it is wrong is not always loud (a stale singleton entry yields the *other* instance's
QoS — a wrong answer, not a refusal). P2 is verified as stated
(`PublicationBuiltinTopicData` carries `durability` / `reliability` / `data_sharing`
at lines 74/89/139; `on_data_writer_discovery` and `on_data_reader_discovery` exist
in `DomainParticipantListener.hpp`).

## R2 — ruling on `<propertiesPolicy>` for `loan_publish` / `max_schema_bytes`

**Honest, forced, and correct — keep both settings (Brief decision 3(a)).**

- Ruling 2026-09-02 forbids typing them at the seam; ruling 2026-08-31 forbids a
  second document format; decision 8 forbids a parser in Fletcher. The document must
  therefore be parseable **in its entirety** by Fast DDS, and `<propertiesPolicy>` is
  the only construct in that grammar that carries arbitrary operator key/value pairs
  into something the provider can read (`DomainParticipantQos::properties()`,
  verified). A `<fletcher>` sibling element would fail Fast DDS's parse and force the
  second reader decision 8 bans. The choice is forced, not smuggled.
- Decision 8 is **not** blurred. The key is the *Fast DDS provider's*, not Fletcher's:
  a provider owns its document format completely, and the reading code sits in
  `fast_dds_pubsub_provider.cpp` exactly where PDA-DEC-5 put the loopback's reader.
  Fletcher-the-seam still only copies and forwards. Had these two gone into
  `ProviderConfig` instead, *that* would have been the decision-8 violation.
- Keep, not drop: `loan_publish` is the switch PDA-ABI-7's defect hunt needs and
  `exp_zero_copy` is its in-tree caller; dropping `max_schema_bytes` would delete the
  only bound on the schema channel in a round already breaking the config API. Note
  the schema channel's type name is a constant (`raw_bytes_pub_sub_type.hpp:28`), not
  parameterised by the bound, so a per-side `max_schema_bytes` mismatch does not break
  discovery the way the payload bound does — the existing rejection behaviour is
  unchanged by this item, neither improved nor made worse.
- Two honesty residues, both DEBT-8: say whether `fletcher.*` is stripped before
  `create_participant`, and warn that `<propagate>true</propagate>` would put a
  Fletcher key into DDS discovery data.

## R3 — the no-merge rule and the `fletcher_participant` requirement

Both sound. The no-merge rule rests on a real substrate limit (the XML API returns a
filled QoS and cannot report which policies were mentioned), and the mandatory anchor
is the only self-identification a document can carry given one return code for
"malformed" and "absent". A document defining only `fletcher_writer` is refused at
construction — correct, and cheap for the operator to fix.

Two things the design should say out loud, neither blocking:

- The anchor makes **H4 universal**, not exotic: every non-empty document must carry a
  participant profile, and an empty one drops the `FletcherParticipant` name. Verified
  harmless — nothing keys on that name anywhere in the tree (only
  `fast_dds_pubsub_provider.cpp:200` sets it) — so one README line, DEBT-9.
- The no-merge rule reaches further than QoS: a document-supplied reader profile also
  decides Fletcher's internal backlog bound (`fast_dds_pubsub_provider.cpp:423-430`)
  and whether the loanable read path is used at all (`CanLoanSamples`, line 431). Both
  are already handled correctly (non-positive → "unbounded outright"; non-PREALLOCATED
  → logged downgrade), and neither is new — but it is the strongest argument for B3's
  whole-struct README check.

## R4 — falsifiability, test by test

The forcing test's shape is right and defeats the two cheats it targets: with rows 2
and 3 differing from the defaults, a provider that ignores the document reddens both
(M2), and one that hard-codes any single value reddens at least one (M1).
`MalformedProfileDocumentIsRefused` is the strongest guard in the set — it is the one
that stops a provider that never reads the document from greening the suite — and
`SchemaChannelIgnoresTheDocument` is a genuine negative control. `LoanPublishComesFromTheDocument`
is grounded in real behaviour: the throw-vs-internal-drop difference is documented in
the header being retired (`fast_dds_pubsub_provider.hpp:70-71`), so M9 is a live
mutation, not a hoped-for one.

Three weaknesses, all DEBT:

1. **The most common document shape is untested** (DEBT-1): a non-empty document with
   an anchor and *no* writer/reader profile must fall back to **Fletcher's** built-ins,
   not Fast DDS's. Every `fletcher.*`-property document has exactly this shape. An
   implementation that fell back to `DATAWRITER_QOS_DEFAULT` when the document is
   non-empty passes every row in the table as written, and loses rows in production.
2. **Partial vacuity under a discovery race** (DEBT-2): row 2 expects VOLATILE, which
   is what a default-constructed `DurabilityQosPolicy` already holds. If that row's
   discovery callback never fires and the harness reads its default-initialised
   capture, the row proves nothing while the test stays green. Rows 1 and 3 are
   immune, so a total discovery failure is caught — it is the per-row flake that hides.
3. **M7 is not a standalone mutation** (DEBT-10): if the provider ignored the document,
   editing the README block apart would still leave both instances equal and green.
   B3's fix makes it valid unconditionally.

## R5 — decision 8, the structural acceptance test, scope, budget

- **No parser in Fletcher: confirmed.** Nothing in the design parses XML; no
  dependency arrives (`tinyxml2` is already inside fast-dds, not a Fletcher require);
  and the design correctly does **not** reuse PDA-DEC-5's `key=value` reader — that
  would have been a fresh stop-and-ask, and it says so.
- **The §5 acceptance test is real.** `CMakeLists.txt:22-24` links fast-dds `PUBLIC`
  today and `conanfile.py:48` sets `transitive_headers=True`; dropping both stops
  Fast DDS include dirs reaching `test_package`, so any surviving eProsima include in
  the public header is a compile error there. The library is `STATIC` and
  `transitive_libs=True` must stay — the design says only `transitive_headers` drops,
  which is right, and no external consumer includes `<fastdds/...>` directly (verified:
  the only in-tree direct includers are the provider's own `src/`, `tests/`,
  `benchmarks/` and the public header). It is a genuine machine check, not prose.
  The one thing it will *also* break is the benchmarks — DEBT-4.
- **Scope.** No ABI, no `extern "C"`, no loader; `Create` untouched and its
  `static_assert` cited as the guard; nothing above the seam branches on built-in vs
  loaded; no `PubSubProvider` method changes; no wire-byte movement (encoder, sample
  layout and type name untouched, bound resolves to the same 65536). PDA-DEC-7's XRCE
  work, including the `uint16_t` narrowing refusal, is explicitly left alone. Clean.
- **Budget.** 300 lines, at the cap, not over. New public surface 2 (≤3). Net lines
  declared. The design is not longer than the item needs — the length is the corner-case
  ladder and the mutation table, which is where it should be.
- **Deletion and end-state.** `Files-to-delete` is real and specific, and it deletes
  PDA-DEC-5's `fastdds` closure on schedule. **No coexistence bridge is created** —
  the retirement is immediate and the tests are replaced in the same item. The design
  argues against splitting into "document + migration" then "test rewrite"; I agree,
  and for the stated reason: the split would land a configuration path whose only guard
  is that it compiles.
- **Call sites: the list is complete.** Verified by grep — external construction sites
  are exactly the eight named (`test_roundtrip.cpp` 8 occurrences, `test_interop.cpp`
  3, `gateway/src/main.cpp` 1, `fastdds_peer.cpp` 1, both conformance subjects,
  `test_package/src/example.cpp`, `benchmarks/exp_zero_copy.cpp`), plus the provider's
  own test TU (45). `tests/discard_probe.cpp` takes a reference and constructs nothing,
  so it is unaffected. **Yes, §10 should be corrected, not just marked done** —
  DEBT-11.

---

## DEBT register (11) — appended to design-debt.md

DEBT-1 (must land in this PR), DEBT-2 … DEBT-11 as recorded in
[design-debt.md](design-debt.md) under PDA-DEC-6.
