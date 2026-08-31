# PDA — Protocol Driver ABI — Execution Plan

Round plan + tracker for putting the pub/sub protocols behind a **pure C ABI**, so
a protocol becomes a *driver* selected and configured at **runtime** instead of a
compile-time link decision.

Spec (oracle): [docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md)
— read it first; it wins on any conflict.
Locked decisions: [PDA-locked-decisions.md](PDA-locked-decisions.md).
This file is both the `plan_path` (tracker) and the `user_stories_path`.

## Goal

Today `fletcher::PubSubProvider` is an honest four-method abstraction — but it is a
**C++** interface, so the protocol is chosen at compile time
([gateway/src/main.cpp:183](../gateway/src/main.cpp#L183)), configuring Fast DDS
requires compiling against eProsima headers (`FastDDSProviderOptions` embeds
`DataWriterQos`/`DataReaderQos` plus per-topic maps of them), and a third party
cannot ship a protocol at all.

Define a pure C ABI **below** `PubSubProvider` (a `DriverProvider` adapter wraps a
loaded driver, so generated code, `Publisher`/`Subscriber` and the codec are
untouched), port all three providers to it, and deliver the two end-user
requirements: **(a)** pick the driver at runtime, **(b)** configure it at runtime
with protocol-specific detail. Zero-copy is preserved for rows *and* attachments,
and the delivery contract that is prose today becomes an executable conformance
suite a third-party driver author can run.

## Branch strategy

- Base: `main`, after the GIR round's PR #125 merges. **Confirm the base at
  kickoff** — PDA touches `pubsub/`, `core/`, both providers, the gateway and the
  integration tests, i.e. almost the exact complement of GIR's `protoc/` footprint,
  so overlap should be near zero, but verify rather than assume.
- Rebased, not merged (repo convention).
- Branch: `feature/protocol-driver-abi`.
- PR split, each independently reviewable:
  **PDA-1/PDA-2** (the guards) → **PDA-3** (the header, reviewed as a spec) →
  **PDA-4/PDA-5** (host + adapter, equivalence proof) → **PDA-6** (InProcess
  reference driver) → **PDA-7** (Fast DDS + the breaking config change) →
  **PDA-8** (XRCE + static registration) → **PDA-9/PDA-10** (multi-instance,
  loaned samples) → **PDA-11** (docs/TDs).
  No PR until green + reviewed; PR/merge is the user's step.

## Sequencing

Strictly linear. **The guards (PDA-1, PDA-2) MUST precede any ABI work** — they
are what make the delivery contract and the zero-copy claim falsifiable, and
PDA-1 is expected to change provider behaviour (decision 10), which must happen
*before* the ABI is defined over it.

```
PDA-1  conformance suite vs C++ iface   →  PDA-2  copy-accounting oracle        →
PDA-3  the C header (spec review)       →  PDA-4  host: load + adapter          →
PDA-5  retarget suite through the ABI   →  PDA-6  InProcess reference driver    →
PDA-7  Fast DDS driver + XML config     →  PDA-8  XRCE driver + static reg      →
PDA-9  multi-instance proof             →  PDA-10 loaned-sample zero-copy       →
PDA-11 docs, TD entries, licensing intent
```

Two notes on the shape:

- **PDA-3 is reviewed as a specification, not code.** It is the header plus the
  version/compat policy, with no implementation. Getting the ownership wording
  wrong here is the most expensive mistake available in this round.
- **PDA-5 is the pivot.** Same assertions as PDA-1, new subject. If PDA-1 passes
  against the C++ interface and PDA-5 passes through the ABI, the ABI is
  behaviourally equivalent to the interface it replaces — which is the whole
  claim of the round.

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)
Kind: 🟩 test-guard · 🟪 spec · 🟦 host/adapter impl · 🟧 driver port · 🔬 equivalence proof · ⚡ perf · 📓 docs

| Item | Title | Kind | Forcing test | Status |
|------|-------|------|--------------|--------|
| PDA-1 | Behavioural conformance suite vs the C++ `PubSubProvider` | 🟩 | `ProviderConformance.SchemaBeforeDataAcrossHandoff` (+ the §8 clause set) run against all three providers | ⚪ |
| PDA-2 | Copy-accounting oracle (makes L4 falsifiable) | 🟩 | `CopyAccounting.PublishAndReceivePerformNoPayloadCopies` | ⚪ |
| PDA-3 | The C header + version/compat policy | 🟪 | `AbiHeader.CompilesAsC99AndIsSelfContained` + review-as-spec (no impl) | ⚪ |
| PDA-4 | Host: explicit-path load, static registry, handles, `DriverProvider` adapter | 🟦 | `DriverHost.LoadsByPathAndAdaptsToPubSubProvider` | ⚪ |
| PDA-5 | Retarget PDA-1's suite **through** the ABI | 🔬 | the PDA-1 clause set, green with the driver-backed subject | ⚪ |
| PDA-6 | InProcess reference driver (promoted out of `gateway/src/main.cpp`) | 🟧 | PDA-5 green against the InProcess **driver** | ⚪ |
| PDA-7 | Fast DDS driver + XML-profile config; retire `FastDDSProviderOptions` | 🟧 | `FastDdsDriver.XmlProfileConfiguresQos` + PDA-5 green + 4 external sites migrated | ⚪ |
| PDA-8 | XRCE driver + static-registration lane + footprint budget | 🟧 | `XrceDriver.StaticRegistrationNeedsNoLoader` + `Footprint.EdgeDriverWithinLinkBudget` | ⚪ |
| PDA-9 | Multi-instance proof (two instances, two DDS domains) | 🔬 | `MultiInstance.TwoInstancesTwoDomainsStayIsolated` | ⚪ |
| PDA-10 | Zero-copy receive via DDS loaned samples | ⚡ | `LoanedSamples.ReceivePathPerformsNoCopy` (via PDA-2's oracle) | ⚪ |
| PDA-11 | ABI spec, driver-author guide, TD entries, licensing intent | 📓 | docs review; TD reconciliation recorded (spec §11) | ⚪ |

Suite shape: PDA-1/PDA-2 introduce a **provider-agnostic conformance harness**
parameterised over a provider factory, so the same TU runs against InProcess,
Fast DDS, XRCE (PDA-1) and then against driver-backed subjects (PDA-5) with no
assertion changes. Its home and exact target names are PDA-1's design call; wire
them into the runbook config's `inner_loop_cmd`/`full_suite_cmd` when it lands.

---

## Items (user stories + acceptance)

> Design detail for each item lives in
> [docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md); the
> design step expands per-item design docs from it.

### PDA-1 — Behavioural conformance suite against the existing C++ interface
**Story.** As a maintainer about to publish an ABI, I have the delivery contract
that is **prose** in `provider.hpp` today expressed as executable assertions, run
against all three existing providers, so I know they actually agree before I
define a public contract over them.
**Covers (spec §8):** schema-before-data; per-writer order **across the schema
handoff** (buffered backlog delivered before, and never interleaved with, live
samples); idempotent re-declaration; conflicting-schema rejection; one callback
per topic; late-joining subscriber; no delivery after `Unsubscribe`.
**Forcing test.** `ProviderConformance.SchemaBeforeDataAcrossHandoff` — plus the
full clause set, parameterised over the three providers.
**Acceptance.** **Divergences are expected and are fixed in-round** (decision 10)
so all three agree before the ABI wraps them. A divergence whose fix would change
**wire bytes** is a stop-and-ask. This item's size is not fully knowable up front;
that was accepted deliberately. Hard prerequisite for everything after.

### PDA-2 — Copy-accounting oracle
**Story.** As a maintainer whose central constraint is zero-copy (L4), I have an
instrumented blob and write buffer that **count copies**, so "zero-copy" is a
failing test rather than an aspiration that regresses silently.
**Forcing test.** `CopyAccounting.PublishAndReceivePerformNoPayloadCopies`.
**Acceptance.** Spec §8.2. Counts on both the encode path (row bytes into the
provider-supplied buffer) and the attachment path. Establishes the **baseline for
today's C++ interface**, including the copy that `Blob = shared_ptr<const
vector<uint8_t>>` currently forces on receive — that baseline is what PDA-10 later
improves on. Must be green before PDA-3.

### PDA-3 — The C header + version/compat policy (reviewed as a specification)
**Story.** As a third-party driver author I have a single, self-contained C header
that tells me exactly what to implement, what I may call, who owns what memory,
and for how long.
**Content.** `fletcher_blob`, `fletcher_schema`, `fletcher_write_buffer`,
`fletcher_provider_config`, `fletcher_status`, the driver vtable (role 1), host
callbacks (role 2), the management API (role 3), `fletcher_driver_entry`, and the
version-negotiation + append-only-struct rules.
**Forcing test.** `AbiHeader.CompilesAsC99AndIsSelfContained` (compiled as C, not
C++, with no Fletcher C++ header reachable).
**Acceptance.** **No implementation in this item.** Spec §3, §4, §5, §6, §9.1.
Must state the pre-1.0 no-compatibility-promise and the deprecation policy in the
header itself. The three roles must be structurally separated in the header, not
merely documented — bindings consume only role 3.

### PDA-4 — Host side: loading, handles, and the `DriverProvider` adapter
**Story.** As an application author I load a driver from an explicit path in my
config, create one or more provider instances from it, and hand the result to any
code that expects a `PubSubProvider`.
**Content.** Explicit-path load (spec §2.1); the static-registration table; the
module/instance handle split with **no global state** (spec §7); and
`DriverProvider : PubSubProvider` with **zero-copy** `shared_ptr` ↔ fat-handle
bridging in both directions (spec §3.2), plus status → exception translation
(spec §6) so the existing throwing contract survives.
**Forcing test.** `DriverHost.LoadsByPathAndAdaptsToPubSubProvider`.
**Acceptance.** PDA-2's oracle stays green across the adapter — the bridge must
not introduce a copy, or the adapter approach is not viable and that is a
stop-and-ask.

### PDA-5 — Retarget the conformance suite through the ABI (the equivalence proof)
**Story.** As a maintainer I run the *same* contract assertions against a
driver-backed provider and see them pass, so I know the ABI is behaviourally
equivalent to the C++ interface it wraps.
**Forcing test.** PDA-1's clause set, unmodified, with a driver-backed subject.
**Acceptance.** Spec §8.1. **The assertions may not be weakened to make them
pass.** Anything the suite cannot express through the ABI is evidence the ABI is
underspecified — that is a PDA-3 defect, not a suite defect, and a stop-and-ask.

### PDA-6 — InProcess reference driver
**Story.** As a driver author I read one small, complete, real driver to learn the
ABI.
**Content.** Promote `InProcessProvider` out of
[gateway/src/main.cpp:72](../gateway/src/main.cpp#L72) into a real component and
implement it as a driver. It becomes the conformance vehicle and the
driver-author reference.
**Forcing test.** PDA-5 green against the InProcess **driver**.
**Acceptance.** Spec §10.1. The gateway keeps working with no user-visible change
to its `--provider inprocess` behaviour.

### PDA-7 — Fast DDS driver + XML-profile config (the breaking change)
**Story.** As an operator I configure Fast DDS QoS at runtime through a native XML
profile named in my Fletcher config — without anything of mine compiling against
eProsima headers.
**Content.** Fast DDS as a driver; QoS via its native XML profile loader;
per-topic overrides become per-topic **profile names** (Fast DDS's own idiom);
`FastDDSProviderOptions` **retired**, not deprecated (decision 9); migrate the 4
external sites / 19 occurrences and rewrite the provider's QoS tests against
profiles.
**Forcing test.** `FastDdsDriver.XmlProfileConfiguresQos` — asserting the applied
QoS actually reflects the profile, not merely that loading succeeded.
**Acceptance.** Spec §5.3, §10. This item delivers end-user requirement (b) on the
path the gateway actually uses. The `__schema` companion channel stays
non-configurable (Fletcher-internal, per TD-005). Wire bytes unchanged.

### PDA-8 — XRCE driver + static registration + footprint budget
**Story.** As an MCU integrator I get the same driver contract with **no loader**,
and I find out in CI if someone makes the edge driver fat.
**Content.** XRCE as a driver; the static-registration lane end-to-end (proving
L1's second path); `key=value` config document (spec §5.1); and a **desktop
link-size budget test** as a footprint proxy.
**Forcing test.** `XrceDriver.StaticRegistrationNeedsNoLoader` +
`Footprint.EdgeDriverWithinLinkBudget`.
**Acceptance.** Spec §11.2. The budget test is explicitly a **proxy, not an MCU
build** — CI has no MCU lane, and this item must not pretend otherwise; its value
is failing when a heavyweight dependency is added. No JSON/YAML parser may be
linked into the edge driver (spec §5.2).

### PDA-9 — Multi-instance proof
**Story.** As an integrator I run two instances of the same driver, on two DDS
domains, in one process, and they do not interfere.
**Forcing test.** `MultiInstance.TwoInstancesTwoDomainsStayIsolated`.
**Acceptance.** Spec §7. Locks L6 **without building a bridge**. This is the
primitive a future bridge composes; it is needed even with a single protocol, and
it is the item that proves no global state crept into PDA-4.

### PDA-10 — Zero-copy receive via DDS loaned samples
**Story.** As a high-frequency consumer I receive transport-owned memory with no
copy, returning the loan when I am done.
**Content.** Use `release` as the `return_loan` hook (spec §3.2) to eliminate the
copy into `std::vector` that today's `Blob` forces on every receive.
**Forcing test.** `LoanedSamples.ReceivePathPerformsNoCopy`, measured by PDA-2's
oracle against the baseline it recorded.
**Acceptance.** Kept **in-round** by maintainer decision. This is the item where
the ABI is demonstrably *better* than the interface it wraps rather than merely
equivalent — it turns the migration from a cost into a performance win. It is
also the only item that touches Fast DDS internals, so it is the natural
descope candidate if the round needs to shed weight; descoping is the user's call.

### PDA-11 — Docs, TD entries, and recorded intent
**Story.** As a third-party driver author I have a guide; as a maintainer I have
the architectural record straight.
**Content.** Driver-author guide (including §4.1's random-access-not-a-stream
requirement); update the "implementing one interface" claims in
[docs/architecture-overview.md](../docs/architecture-overview.md) and
[README.md](../README.md) to driver terms; a **new TD entry** reconciling
TD-007's recorded dynamic-linking skepticism (spec §11.1 — answering *fragile*,
*hard to test*, *platform-dependent* on the record); and the **LGPL-3.0 /
third-party-driver intent** recorded explicitly (spec §11.3).
**Acceptance.** Spec §11. The licensing intent must be stated **before the ABI is
published**, not left to silence. Do not restate the spec — link it.

## Downstream (out of this round)

- **A protocol bridge component** (DDS↔MQTT routing). PDA-9 makes it trivial to
  build; building it is its own round.
- **BIND-C# / BIND-Rust** — consume the *shared vocabulary* fixed here (spec §9.2)
  for the opposite direction. **No roadmap reordering needed** (L5). The concrete
  inheritance: the `fletcher_write_buffer` struct verbatim, the blob
  retain/release protocol, `ArrowSchema` as-is, and the error/version conventions.
- **A search-path / manifest discovery mechanism** — additive on top of L7's
  explicit path, with its own security design.
- **A real MCU CI lane** — would replace PDA-8's desktop proxy with a genuine
  footprint gate.
- **Retiring the C++ `PubSubProvider` interface** — made *optional* by decision 8;
  a separate decision if ever wanted.

## Definition of done (round)

PDA-1..PDA-11 forcing tests 🟢; the full component + integration suite green;
**all three providers agree** on the §8 contract (divergences fixed, not pinned —
decision 10); the conformance suite passes **both** against the C++ interface and
through the ABI with identical assertions (PDA-5); PDA-2's oracle green, showing
no copies on the row or attachment path and an *improvement* on receive (PDA-10);
`FastDDSProviderOptions` retired with all 4 external sites migrated;
`InProcessProvider` out of `gateway/src/main.cpp` and serving as the reference
driver; static registration proven with no loader (PDA-8); two instances on two
domains isolated (PDA-9); **wire format byte-identical** (decision 12); Fletcher
carrying **no config parser** (spec §5.2); and the TD reconciliation plus
licensing intent recorded (PDA-11).

On completion, end-user requirements (a) and (b) are delivered — the protocol is
chosen and configured at runtime — and a third party can ship a driver without
building against the Fletcher tree.
