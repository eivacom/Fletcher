# PDA-ABI — The Protocol Driver ABI — Execution Plan

Round plan + tracker for the pure-C protocol driver ABI **below** the pub/sub seam.
Round token **`ABI`**.

Spec (oracle): [docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md).
**Seam spec, which wins over it on anything about the interface:**
[docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md).
Locked decisions: [PDA-ABI-locked-decisions.md](PDA-ABI-locked-decisions.md).
This file is both the `plan_path` (tracker) and the `user_stories_path`.

## Gate

**This round is blocked until PDA-decouple closes**, and its Definition of Done is
this round's entry condition — specifically: every crossing type has a normative
C-expressible ownership rule; schema arrival has a C-expressible form; the
exception taxonomy is published; the registry signature admits a path resolver
without change; nothing above the seam branches on built-in versus loaded.

**It is not blocked on BIND-C#/BIND-Rust, and BIND is not blocked on it.** The two
run in parallel and meet only at the seam spec. Neither may change the seam; a
seam that proves insufficient is a stop-and-ask against *that* spec.

## Goal

Turn a protocol into a **driver**: a separate binary, implementable by anyone in
any language, loaded and configured at runtime with no Fletcher rebuild — and
indistinguishable, to everything above the seam, from a built-in provider.

The seam already carries runtime selection and configuration (PDA-decouple), so
this round is narrower than it looks: a C header for what a driver *implements*
and what the host *calls back*, a loader, an adapter that registers as the
resolver for path selectors, the three provider ports, and the zero-copy receive
path the ABI makes expressible.

## Branch strategy

- Base: `main` after PDA-decouple merges. **Confirm at kickoff.**
- Branch: `feature/protocol-driver-abi-c` (PDA-decouple keeps
  `feature/protocol-driver-abi`; a fresh branch keeps the two rounds' PRs
  separable, and BIND's branch is independent of both).
- Rebased, not merged (repo convention).
- PR split: **ABI-1** (the header, reviewed as a spec) → **ABI-2/ABI-3** (host,
  adapter, equivalence proof) → **ABI-4** (reference driver) → **ABI-5** (Fast DDS)
  → **ABI-6** (XRCE + static lane) → **ABI-7** (zero-copy receive) → **ABI-8**
  (docs/TDs). No PR until green + reviewed; PR/merge is the user's step.

## Sequencing

```
ABI-1  the C header (spec review, no impl)  →  ABI-2  loader + handles + adapter  →
ABI-3  seam suite retargeted through the ABI →  ABI-4  InProcess reference driver  →
ABI-5  Fast DDS driver                       →  ABI-6  XRCE driver + static lane   →
ABI-7  zero-copy receive (loaned samples)    →  ABI-8  guide, TD entries, licensing
```

- **ABI-1 is reviewed as a specification, not code** — header plus policy, no
  implementation. Its types are views of the seam's; getting the ownership wording
  wrong is the most expensive mistake available here, and a third-party driver
  author has nothing else to read.
- **ABI-3 is the pivot.** Same assertions as the seam's suite, new subject. Pass
  both and the ABI is behaviourally equivalent to the seam it implements.

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)
Kind: 🟪 spec · 🟦 host/adapter impl · 🟧 driver port · 🔬 equivalence proof · ⚡ perf · 📓 docs

| Item | Title | Kind | Forcing test | Status |
|------|-------|------|--------------|--------|
| ABI-1 | The C header: driver vtable, host callbacks, version/compat policy | 🟪 | `AbiHeader.CompilesAsC99AndIsSelfContained` + review-as-spec (no impl) | ⚪ |
| ABI-2 | Loader (explicit path) + static registry + module/instance handles + `DriverProvider` adapter | 🟦 | `DriverHost.PathSelectorResolvesThroughTheSeamRegistry` | ⚪ |
| ABI-3 | The seam's conformance suite retargeted **through** the ABI | 🔬 | the seam clause set, unchanged, with a driver-backed subject (incl. cross-process) | ⚪ |
| ABI-4 | InProcess reference driver | 🟧 | ABI-3 green against the InProcess **driver** | ⚪ |
| ABI-5 | Fast DDS driver | 🟧 | ABI-3 green against the Fast DDS **driver**, config document unchanged from DEC-6 | ⚪ |
| ABI-6 | XRCE driver + static-registration lane + footprint budget | 🟧 | `XrceDriver.StaticRegistrationNeedsNoLoader` + `Footprint.EdgeDriverWithinLinkBudget` | ⚪ |
| ABI-7 | Zero-copy receive via loaned samples | ⚡ | `LoanedSamples.ReceivePathPerformsNoCopy` via the seam's copy oracle | ⚪ |
| ABI-8 | Driver-author guide, TD entries, licensing intent | 📓 | docs review; TD reconciliation + licensing intent recorded | ⚪ |

Suite shape: this round adds **no new conformance assertions**. It reuses the
harness PDA-decouple built, parameterised over a provider factory, with
driver-backed subjects added. If an assertion has to change, something is
underspecified — see ABI-3.

---

## Items (user stories + acceptance)

> Design detail lives in [docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md)
> and, for anything crossing the seam, in
> [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md).

### ABI-1 — The C header (reviewed as a specification)
**Story.** As a third-party driver author I read one self-contained C header and
know exactly what to implement, what I may call, who owns which memory, and for
how long.
**Content.** `fletcher_blob`, `fletcher_schema`, `fletcher_write_buffer`,
`fletcher_status`, the driver vtable (role 1), the host callbacks (role 2),
`fletcher_driver_entry`, and the version-negotiation + append-only-struct rules.
The types are **views of the seam's vocabulary** — derived from the seam spec, not
invented, and not derived from the language-binding ABI.
**Forcing test.** `AbiHeader.CompilesAsC99AndIsSelfContained` — compiled as C, not
C++, with no Fletcher C++ header reachable.
**Acceptance.** Spec §3, §4, §0.2. **No implementation in this item.** Roles 1 and
2 only — **selection is not in this header** (it is seam surface, spec §0.2). The
pre-1.0 no-compatibility-promise and the deprecation policy must be stated in the
header itself. The exception→status mapping must be exhaustive against the seam's
published taxonomy.

### ABI-2 — Host side: loading, handles, adapter
**Story.** As an operator I put a driver's path in my configuration and get a
working provider, selected exactly like a built-in one.
**Content.** Explicit-path loading (spec §2.1); the static-registration table;
module/instance handles with **no global state** (spec §5); and
`DriverProvider : PubSubProvider` registered as the **resolver for path
selectors** in the seam's registry (spec §2.2), bridging the seam's
shared-ownership types to the ABI's handles with **no copy** in either direction,
and translating status back into the seam's exceptions.
**Forcing test.** `DriverHost.PathSelectorResolvesThroughTheSeamRegistry`.
**Acceptance.** The seam's registry signature must be **unchanged** — needing to
widen it means the seam was underspecified, which is a stop-and-ask against the
seam spec, not a change made here. The copy oracle stays green across the adapter;
if the bridge introduces a copy, the adapter approach is not viable and that too
is a stop-and-ask.

### ABI-3 — The equivalence proof
**Story.** As a maintainer I run the *same* contract assertions against a
driver-backed provider and see them pass, so I know the ABI is behaviourally
equivalent to the seam it implements.
**Forcing test.** The seam's clause set, unmodified, with a driver-backed subject,
including the **cross-process** subject (seam §7.2).
**Acceptance.** Spec §7. **Assertions may not be weakened to pass.** Anything the
suite cannot express through the ABI is a defect in the header or in the seam — a
stop-and-ask either way, not a relaxed test.

### ABI-4 — InProcess reference driver
**Story.** As a driver author I read one small, complete, real driver to learn the
ABI.
**Content.** Implement the component PDA-decouple promoted out of the gateway
(DEC-5) as a driver. It becomes the conformance vehicle and the driver-author
reference, so it should read as an example rather than as production plumbing.
**Forcing test.** ABI-3 green against the InProcess **driver**.
**Acceptance.** The gateway's `--provider inprocess` behaviour is unchanged, and
resolves through the registry either as the built-in or as the driver.

### ABI-5 — Fast DDS driver
**Story.** As an operator I run Fast DDS as a loaded driver, configured by the same
profile document I already use, with the gateway none the wiser.
**Content.** Fast DDS behind the ABI. **The config document is unchanged from
DEC-6** — the protocol-vocabulary problem was solved at the seam, so this item is
a port, not a redesign. Per-topic QoS remains per-topic profile names.
**Forcing test.** ABI-3 green against the Fast DDS **driver**.
**Acceptance.** Wire bytes unchanged. The `__schema` companion channel stays
non-configurable (Fletcher-internal, TD-005). This is the port that exercises the
ABI on the path the gateway actually uses.

### ABI-6 — XRCE driver, static registration, footprint
**Story.** As an MCU integrator I get the same driver contract with **no loader**,
and CI tells me if someone makes the edge driver fat.
**Content.** XRCE behind the ABI; the static-registration lane end-to-end (proving
spec §2's second path); a **desktop link-size budget test** as a footprint proxy.
**Forcing test.** `XrceDriver.StaticRegistrationNeedsNoLoader` +
`Footprint.EdgeDriverWithinLinkBudget`.
**Acceptance.** Spec §8. The budget test is explicitly a **proxy, not an MCU
build** — CI has no MCU lane and this item must not imply otherwise; its value is
failing when a heavyweight dependency appears. No JSON/YAML parser in the edge
driver.

### ABI-7 — Zero-copy receive via loaned samples
**Story.** As a high-frequency consumer I receive transport-owned memory with no
copy, and the loan is returned when I am done.
**Content.** Use `release` as the `return_loan` hook (spec §3, §6) to eliminate the
copy the seam's `Blob` used to force on receive — which PDA-decouple made
avoidable (seam §3.2) but did not deliver.
**Forcing test.** `LoanedSamples.ReceivePathPerformsNoCopy`, measured by the seam's
copy oracle against the baseline DEC-2 recorded.
**Acceptance.** Spec §6, §6.1. **The receive-side data-sharing defect must be
answered, not stepped around**: re-enabling data-sharing without resolving the
silent `TRANSIENT_LOCAL` sample loss would ship a known defect as a feature. It is
legitimate for this item to conclude the answer is upstream and defer — provided
it says so, and provided the default stays safe. This is the item where the ABI is
demonstrably *better* than the interface rather than merely equivalent, and it is
also the only one touching Fast DDS internals, so it is the natural descope
candidate; descoping is the user's call.

### ABI-8 — Guide, TD entries, licensing intent
**Story.** As a third-party driver author I have a guide; as a maintainer I have
the architectural record straight.
**Content.** Driver-author guide (including the random-access-not-a-stream
requirement, seam §3.1); a **new TD entry** reconciling TD-007's recorded
dynamic-linking skepticism (spec §9.1, answering *fragile* / *hard to test* /
*platform-dependent* on the record); and the **LGPL-3.0 / third-party-driver
intent** recorded explicitly (spec §9.2).
**Acceptance.** Licensing intent stated **before the ABI is published**, not left
to silence — the round's one genuinely open decision. Do not restate the specs;
link them.

## Definition of done (round)

ABI-1..ABI-8 forcing tests 🟢; full component + integration suite green; the seam's
conformance suite passing **through the ABI with identical assertions** and a
cross-process subject; the copy oracle green and showing an *improvement* on
receive (ABI-7) or an explicit, defaulted-safe deferral; all three providers
running as drivers; static registration proven with no loader; the seam's registry
signature **unchanged**; **wire format byte-identical**; and the TD reconciliation
plus licensing intent recorded.

On completion: a protocol is chosen and configured at runtime, a third party can
ship a driver without building against the Fletcher tree, and nothing above the
seam can tell the difference.
