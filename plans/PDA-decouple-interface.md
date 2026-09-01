# PDA-decouple — Prepare the Pub/Sub Seam for Two ABIs — Execution Plan

Round plan + tracker for making `fletcher::PubSubProvider` a seam that a C ABI can
be built against **on either side, independently and in parallel**. Round token
**`PDA-DEC`**.

Spec (oracle): [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
— read it first; it wins on any conflict.
Locked decisions: [PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md).
This file is both the `plan_path` (tracker) and the `user_stories_path`.

## Goal

Two rounds are queued on opposite sides of one seam — **PDA-ABI** below it (a
protocol becomes a runtime-loadable driver) and **BIND-C#/BIND-Rust** above it (an
application in another language publishes and subscribes). Neither can be built
cleanly against the seam as it stands: the types crossing it are C++ types with no
stated C-expressible ownership model, failures cross as exceptions, the delivery
contract is prose, and which provider is in use is a compile-time decision baked
into every caller.

This round changes **only the seam**. It writes no C, defines no ABI, and loads
nothing at runtime. When it closes, the two ABI rounds can start together and
**meet only at the spec** — that is the deliverable, not a feature.

Concretely, three requirements (spec §0.1): a provider is selected and configured
**at runtime** by name or path; whether it is **built in or loaded is invisible**
above the seam; and every crossing type and failure has a **documented,
C-expressible** model.

## Branch strategy

- Base: `main` after the round's own PR merges — but this round *starts* from
  `feature/protocol-driver-abi` at `d77b9c4`, which already carries the Fast DDS
  modernization merge. That merge is a prerequisite, not an accident: it reshaped
  `WriteBuffer` into the window-plus-refill form the seam needs (spec §3.1).
- Branch: `feature/protocol-driver-abi` (continues; renaming it mid-flight buys
  nothing and breaks the pushed history).
- Rebased, not merged (repo convention).
- PR split, each independently reviewable: **PDA-DEC-1/PDA-DEC-2** (the guards) →
  **PDA-DEC-3** (the vocabulary, reviewed as a specification) → **PDA-DEC-4/PDA-DEC-5**
  (registry + first built-in) → **PDA-DEC-6/PDA-DEC-7** (provider config migration) →
  **PDA-DEC-8/PDA-DEC-9** (multi-instance, docs). No PR until green + reviewed; PR/merge
  is the user's step.

## Sequencing

Strictly linear. **The guards (PDA-DEC-1, PDA-DEC-2) MUST precede the vocabulary work** —
they are what make the delivery contract and the zero-copy property falsifiable,
and PDA-DEC-1 is expected to *change provider behaviour*, which must happen before the
seam is specified over it.

```
PDA-DEC-1  conformance suite (incl. cross-process)  →  PDA-DEC-2  copy-accounting oracle   →
PDA-DEC-3  the crossing vocabulary (spec review)    →  PDA-DEC-4  provider registry        →
PDA-DEC-5  InProcess promoted to a built-in         →  PDA-DEC-6  Fast DDS via document    →
PDA-DEC-7  XRCE via document                        →  PDA-DEC-8  multi-instance proof     →
PDA-DEC-9  seam spec, taxonomy, TD entry, handoff
```

Two notes on the shape:

- **PDA-DEC-3 is reviewed as a specification.** It is where the ownership rules land,
  and the wording is the most expensive thing in the round to get wrong — two
  later rounds derive their C types from it without being able to consult each
  other.
- **PDA-DEC-4 is the item that delivers requirement §0.1(2).** If it lands with a
  signature PDA-ABI has to widen to admit loading, the round has failed at its
  actual purpose even with every test green.

- **PDA-DEC-3 owns InProcess schema arrival, and gains a suite subject.** PDA-DEC-1
  exercises the loopback as a *schema-less* transport only (§7 permits that). Making
  it schema-carrying needs plumbing PDA-DEC-3 would replace, so PDA-DEC-3 lands that
  plumbing **and adds the sixth (schema-carrying loopback) subject** to the
  conformance suite. Recorded 2026-09-01 from the PDA-DEC-1 design review; do not
  lose it.
- **PDA-DEC-3 must update PDA-DEC-2's borrowed-attachment pin.** That pin asserts
  exactly one copy (owner ruling 2026-09-01) and goes red at 0 for any removal a
  *provider* can express — mutation B proves it. The one shape it cannot see is
  PDA-DEC-3 leaving `Blob` untouched and adding a **parallel** borrowed-blob type,
  which would land silently. PM ruling 2026-09-01: booked here as a PDA-DEC-3
  obligation rather than re-escalated; endorsed by the PDA-DEC-2 compliance
  re-check. Do not close PDA-DEC-3 without updating that pin.
- **PDA-DEC-5 is registration only.** PDA-DEC-1 lifts `InProcessProvider` out of
  `gateway/src/main.cpp` into `pubsub/`, because the class sits in an anonymous
  namespace inside an executable and is otherwise unlinkable by the suite — the
  alternative was a duplicate PDA-DEC-5 would delete. Endorsed by the PDA-DEC-1
  design review; PM ruling 2026-09-01.

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)
Kind: 🟩 test-guard · 🟪 spec · 🟦 seam impl · 🟧 provider migration · 🔬 proof · 📓 docs

| Item | Title | Kind | Forcing test | Status |
|------|-------|------|--------------|--------|
| PDA-DEC-1 | Conformance suite for the delivery contract, incl. a cross-process subject | 🟩 | `ProviderConformance.SchemaBeforeDataAcrossHandoff` + the §7 clause set, against all three providers | 🟢 |
| PDA-DEC-2 | Copy-accounting oracle (makes zero-copy falsifiable) | 🟩 | `CopyAccounting.PublishAndReceivePerformNoPayloadCopies` | 🟢 |
| PDA-DEC-3 | The crossing vocabulary: ownership, schema arrival, exception taxonomy — **plus the 6th (schema-carrying loopback) conformance subject** | 🟪 | `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` (+ PDA-DEC-2's attachment pin updated to 0 and green) | ⚪ |
| PDA-DEC-4 | Provider registry — name-or-path selector, typed core + opaque document | 🟦 | `Registry.SelectsByNameWithoutCallerKnowingTheProvider` | ⚪ |
| PDA-DEC-5 | `InProcessProvider` registered as a built-in (lifted into `pubsub/` by PDA-DEC-1) | 🟦 | `Registry.InProcessResolvesAsABuiltIn` + gateway `--provider inprocess` unchanged | ⚪ |
| PDA-DEC-6 | Fast DDS configured by document; retire `FastDDSProviderOptions` | 🟧 | `FastDdsConfig.ProfileDocumentConfiguresQos` + 4 external sites migrated | ⚪ |
| PDA-DEC-7 | XRCE configured by document (`key=value`) | 🟧 | `XrceConfig.DocumentConfiguresTransport` | ⚪ |
| PDA-DEC-8 | Multi-instance proof — two instances, two domains, through the registry | 🔬 | `Registry.TwoInstancesTwoDomainsStayIsolated` | ⚪ |
| PDA-DEC-9 | Seam spec, exception taxonomy, TD entry, and the parallelism handoff | 📓 | docs review; handoff checklist complete (§DoD) | ⚪ |

Suite shape: PDA-DEC-1/PDA-DEC-2 introduce a **provider-agnostic conformance harness**
parameterised over a provider factory, so one TU runs against InProcess, Fast DDS
and XRCE, and later against a driver-backed subject in PDA-ABI with no assertion
changes. **As landed (PDA-DEC-1):** `integration-tests/pubsub-conformance`, suite
`ProviderConformance`, five subjects — `InProcessLocal`, `FastDdsLocal`,
`FastDdsCrossProcess`, `XrceLocal`, `XrceCrossProcess` — with its own CI lane
`ci.integration-test.pubsub-conformance`. Wired into the runbook config's
`inner_loop_cmd`/`full_suite_cmd`.

---

## Items (user stories + acceptance)

> Design detail for each item lives in
> [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md); the design
> step expands per-item design docs from it.

### PDA-DEC-1 — Conformance suite for the delivery contract
**Story.** As a maintainer about to freeze a seam two teams will build against in
parallel, I have the delivery contract that is **prose** in `provider.hpp`
expressed as executable assertions, run against all three providers, so I know
they actually agree before anything is specified over them.
**Covers (spec §7):** schema-before-data; per-writer order **across the schema
handoff**; idempotent re-declaration; conflicting-schema rejection; one callback
per topic; late joiner; no delivery after `Unsubscribe`; and the no-schema
loopback case, which must not be mixed with the schema-carrying one.
**Forcing test.** `ProviderConformance.SchemaBeforeDataAcrossHandoff`, plus the
full clause set parameterised over the three providers.
**Acceptance.** Spec §7.1, §7.2. **A cross-process subject for the DDS providers
is mandatory** — a single-process suite cannot see the transport (Fast DDS serves
same-process endpoints over intra-process delivery), and the round has a shipped
example of a defect that a 70-test single-process suite could not observe.
**Divergences between the three are expected and fixed in-round**; one whose fix
would move **wire bytes** is a stop-and-ask. This item's size is not knowable up
front — that was accepted deliberately. Hard prerequisite for everything after.

### PDA-DEC-2 — Copy-accounting oracle
**Story.** As a maintainer whose seam must preserve zero-copy for two ABI rounds
that inherit it, I have an instrumented blob and write buffer that decide copying by
**address provenance**, so the property is a failing test rather than an aspiration.
**Forcing test.** `CopyAccounting.PublishAndReceivePerformNoPayloadCopies`.
**Acceptance.** Spec §8.1. Judges the encode path (row bytes into the
provider-supplied buffer) and the attachment path by comparing the encode-window
base against the delivered pointer, and records the **baseline**
— including the copy today's `Blob` forces on receive, which is the thing PDA-DEC-3
has to make avoidable. Must be green before PDA-DEC-3.

### PDA-DEC-3 — The crossing vocabulary (reviewed as a specification)
**Story.** As an ABI author on either side of the seam, I can read the seam's
headers and know, for every type that crosses, who owns the memory, for how long,
and what my C boundary must do — without inventing anything and without consulting
the other side.
**Content.** Normative ownership rules in the headers for `WriteBuffer` (§3.1),
`Blob`/`Attachments` (§3.2), `SharedSchema` (§3.3), topic segments (§3.5); a
**C-expressible form for schema arrival**, replacing the `shared_future` as the
contract (§3.4); and a **stable exception taxonomy** (§5.1) so both boundaries map
the same failures to the same statuses.
**Forcing test.** `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` —
red today, because `Blob` forces a copy into a `vector`, so a provider cannot hand
over borrowed transport memory at all.
**Acceptance.** Spec §3, §5. C++ changes only where a type has no C-expressible
form; the method set is untouched (§2). Adding, removing or reordering interface
methods is a stop-and-ask. PDA-DEC-1's suite and PDA-DEC-2's oracle stay green.

### PDA-DEC-4 — The provider registry
**Story.** As an application or gateway operator I name the protocol I want in
configuration, supply its settings as a document, and get a working provider —
without linking that protocol's SDK and without my code knowing whether the
protocol is built in or loaded.
**Content.** One creation function: selector (name **or** path) + typed core
(`{max_payload_bytes, domain_id}`) + opaque document → `shared_ptr<PubSubProvider>`.
A registry of built-ins, populated by registration rather than a hardcoded
`if`-chain. No global state; N instances per provider.
**Forcing test.** `Registry.SelectsByNameWithoutCallerKnowingTheProvider`.
**Acceptance.** Spec §4. **The signature is frozen here** — PDA-ABI must be able
to add a *resolver* for path selectors without changing it (§4 clause 2). Path
selectors need not resolve in this round; they must not be designed out. Fletcher
gains **no config parser** (§4.2) — a stop-and-ask.

### PDA-DEC-5 — `InProcessProvider` becomes a registered built-in
**Story.** As a driver-author-to-be I read one small, complete provider that is
selected the same way every other one is.
**Content.** Promote it out of [gateway/src/main.cpp:72](../gateway/src/main.cpp#L72)
into a real component and register it; the gateway's `--provider` becomes a
registry lookup.
**Forcing test.** `Registry.InProcessResolvesAsABuiltIn`, with the gateway's
existing `--provider inprocess` behaviour unchanged.
**Acceptance.** Spec §10. First proof the registry works. This component is later
the body of PDA-ABI's reference driver, so it should read as an example.

### PDA-DEC-6 — Fast DDS configured by document (the breaking change)
**Story.** As an operator I configure Fast DDS QoS at runtime through its native
XML profile, named in my Fletcher configuration, with nothing of mine compiling
against eProsima headers.
**Content.** Fast DDS reachable through the registry with a profile document;
per-topic overrides become per-topic **profile names** (Fast DDS's own idiom);
`FastDDSProviderOptions` **retired**, not deprecated; the 4 external sites / 19
occurrences migrated and the provider's QoS tests rewritten against documents.
**Forcing test.** `FastDdsConfig.ProfileDocumentConfiguresQos` — asserting the
**applied** QoS reflects the document, not merely that loading succeeded.
**Acceptance.** Spec §4.1, §10. This is what proves the seam carries no protocol
vocabulary. The `__schema` companion channel stays non-configurable
(Fletcher-internal, TD-005). Wire bytes unchanged.

### PDA-DEC-7 — XRCE configured by document
**Story.** The same, for the edge provider, without carrying a parser it cannot
afford.
**Content.** `key=value` document; `XrceConfig`'s POD fields become document keys.
**Forcing test.** `XrceConfig.DocumentConfiguresTransport`.
**Acceptance.** Spec §4.2. No JSON/YAML parser may be linked into the edge
provider. A link-size check is *not* required here — the footprint proxy belongs
to PDA-ABI, with the static-registration lane.

### PDA-DEC-8 — Multi-instance proof
**Story.** As an integrator I run two instances of the same provider, on two DDS
domains, in one process, through the registry, and they do not interfere.
**Forcing test.** `Registry.TwoInstancesTwoDomainsStayIsolated`.
**Acceptance.** Spec §4 clause 3. Proves no global state crept into PDA-DEC-4. This is
the primitive a future bridge composes and the property PDA-ABI's module/instance
handles depend on — cheap now, expensive to retrofit.

### PDA-DEC-9 — Seam spec, taxonomy, and the parallelism handoff
**Story.** As the maintainer starting two rounds at once, I have one document both
of them mirror, and a written statement of what is frozen.
**Content.** Finalise [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
against what actually landed; publish the exception taxonomy; restate the
"implementing one interface" claims in
[docs/architecture-overview.md](../docs/architecture-overview.md) and the root
[README.md](../README.md) in registry terms; add a **TD entry** for the seam and
the uniform-selection decision.
**Acceptance.** Spec §1, §9. The handoff checklist in the DoD below is the real
deliverable: after this item, PDA-ABI and BIND can be started **on the same day**
without either needing an answer from the other.

## Downstream (out of this round, and now parallel)

```
                    ┌─────────────── BIND-C# / BIND-Rust  (above the seam)
   PDA-decouple ────┤
     (this round)   └─────────────── PDA-ABI               (below the seam)
```

- **PDA-ABI** — [docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md),
  [PDA-ABI-protocol-driver-abi.md](PDA-ABI-protocol-driver-abi.md). Adds a path
  resolver to PDA-DEC-4's registry, the driver vtable and host callbacks, the loader,
  the driver ports, and zero-copy receive.
- **BIND-C# / BIND-Rust** — a C façade over `Publisher`/`Subscriber` plus PDA-DEC-4's
  selection. Note selection is **binding** surface, not driver surface (spec §9).
- Neither depends on the other. Neither may change the seam (spec §1).
- Still downstream of GIR: **RIR** (RBA↔IR reconciliation), unaffected by this
  split.

## Definition of done (round)

PDA-DEC-1..PDA-DEC-9 forcing tests 🟢; full component + integration suite green; **all
three providers agree** on the §7 contract with divergences fixed rather than
pinned; the conformance suite has a **cross-process subject** for the DDS
providers; PDA-DEC-2's oracle green **and** showing that borrowed transport memory can
now cross without a copy; `FastDDSProviderOptions` retired with all 4 external
sites migrated; `InProcessProvider` out of `gateway/src/main.cpp` and resolved as
a built-in; two instances on two domains isolated; **wire format byte-identical**;
Fletcher carrying **no config parser**.

**And the handoff, which is the point of the round:**

1. Every type crossing the seam has a normative, C-expressible ownership rule in
   the header (§3).
2. Schema arrival has a C-expressible form; the `shared_future` is a convenience
   over it, not the contract (§3.4).
3. The exception taxonomy is published and stable (§5.1).
4. The registry signature admits a path resolver **without change** (§4 clause 2).
5. Nothing above the seam branches on built-in versus loaded (§0.1(2)).
6. The spec states what is frozen, and that changing it is a stop-and-ask for
   either later round (§1).

When those six hold, PDA-ABI and BIND-C#/BIND-Rust are both unblocked, in
parallel.
