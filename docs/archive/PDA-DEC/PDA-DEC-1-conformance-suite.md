# PDA-DEC-1 — Conformance suite for the delivery contract (design)

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §7/§7.1/
§7.2 (§0.1, §6, §11 constraining); locked decisions 11, 12, 13. Cited, not restated.

## Summary

A new `integration-tests/pubsub-conformance/` harness encodes §7's clauses **once**, in a
provider-agnostic static library, and runs them against five **subjects** that *register*
rather than copy tests — the in-process loopback (schema-less, per §7 clause 1's plain
reading) plus Fast DDS and XRCE, each in-process and cross-process. The cross-process
subjects put the *publisher* in a child process driven over a request/reply pipe, which is
what makes transport behaviour — and the shipped receive-side data-sharing defect class —
observable at all.

## Design

### The subject abstraction (the load-bearing decision)

```cpp
// include/fletcher/conformance/subject.hpp — Topic = the seam's segment vector
class ProviderSubject {   // clause bodies see ONLY this — no Publish/CreateTopic, ever
  public:
    virtual const ProviderTraits& Traits() const = 0;   // ONE row per PROVIDER
    // Publisher side — MAY be another process. nullopt = ok; string = type + what().
    virtual std::optional<std::string> DeclareTopic(const Topic&, SchemaId) = 0;
    virtual std::optional<std::string> PublishRow(const Topic&, uint32_t seq) = 0;
    // Subscriber side — always this process, always this instance.
    virtual SubscriptionResult Subscribe(const Topic&, SubscribeCallback) = 0;
    virtual void Unsubscribe(const Topic&) = 0;
};
```

`ProviderTraits` is `{provider name; SchemaMode kCarried|kAbsent; Retention
kRetainsPreSubscribe|kDropsPreSubscribe}` — two sealed two-valued enums, no third value.
The subject **never hands out a `PubSubProvider&`**, so a clause body cannot publish
locally: an in-process shortcut on a cross-process subject is unrepresentable rather than
a review risk. Traits keyed by **provider** mean a provider's two subjects cannot declare
different values — the in-process one pins the cross-process one. Every clause runs on its
own freshly-named topic. **Local** calls `CreateTopic`/`Publish` on the same
instance the subscriber side uses, catching into the reply; **Peer** spawns a child and
exchanges one line per request over stdin/stdout — `create <topic> <A|B|none>`,
`publish <topic> <seq>`, `quit`, replying `ok` or `err <type>: <what>`, after printing
`READY` (the `fastdds_peer` convention), its provider supplied by a factory linked into
it so the loop and protocol exist once. There is **no `subscribe` verb**: the peer
publishes and cannot observe, and no clause needs it to.

### The clause set

`src/clauses.cpp` — one TU, one value-parameterised gtest suite named literally
`ProviderConformance`, instantiated per subject, so full names read
`<Subject>/ProviderConformance.<Clause>/0` and
`ctest -R 'ProviderConformance\.SchemaBeforeDataAcrossHandoff'` scopes to that clause
across every subject — exactly the item's forcing-test id.

The clauses, each with its authority: **1** `SchemaBeforeDataAcrossHandoff` *(forcing)*
§7.1+§7.2 · **2** `CallbackNeverSeesNullSchema` §7.1 · **3** `SchemaModeIsUniformNeverMixed`
§7.1 last sentence · **4** `PerWriterOrderIsMonotonic` §7.2 · **5**
`BacklogNeverInterleavesWithLiveSamples` §7.2 · **6** `LateJoinerBacklogIsAllOrNothing`
decision 12 + §7.1 "buffered and delivered" · **7** `IdenticalRedeclarationIsIdempotent`
§7.3 · **8** `ConflictingRedeclarationIsRejected` §7.3 **as amended below** · **9**
`OneCallbackPerTopicPerInstance` §7.4 · **10** `SubscribeNeverBlocksSchemaArrivesLater`
§7.5 · **11** `NoDeliveryAfterUnsubscribeReturns` §7.6 · **12**
`DeliveryIsSerializedPerSubscription` §6 clause 1 (the one delivery promise no other item
carries).

Clauses 7 and 8 observe only the reply to a declaration, so they run on **every** subject
regardless of `schema_mode`: whether a transport later carries a schema is independent of
whether a declaration is accepted. Clause 2 is the only axis-gated clause — `kCarried`
only, with `kAbsent`'s mirror (null throughout, never mixed) belonging to clause 3 — and
the gate is applied at *instantiation*, so an absent clause shows in the ctest list and
`GTEST_SKIP` appears nowhere. Clauses 7, 9 and 12 assert **cardinality and idempotence
only** (9 counts exactly one delivery across two registrations without asserting which
wins; 7 asserts no observable change rather than a return value; 12 counts callback
overlap): §7.3/§7.4 state cardinality, not a winner, and encoding a winner would repeat
clause 8's original mistake.

### §7 clause 3 is amended in this PR, and the loopback is schema-less

**Owner ruling 2026-09-01 ("Refused, every protocol"):** §7 clause 3 goes from "may be
rejected" to "**must** be rejected" in **this** item's PR, so oracle and suite agree the
moment the suite lands and clause 8 asserts uniform rejection *against* the oracle rather
than ahead of it. Two decision-11 fixes follow, **owned by this item** — expect them: the
loopback silently overwrites a conflicting schema (`gateway/src/main.cpp:78-80`, no
comparison) and must refuse; XRCE has no conflict handling and gains it; Fast DDS already
refuses. **The loopback is exercised as a schema-less transport only** (PM ruling): §7
clause 1's last sentence names it as the null-throughout transport, and making it carrying
today would mean a real promise, a pre-schema queue and an ordered flush **inside the exact
class PDA-DEC-3 replaces** (§3.4 retires the `shared_future` as the contract) — a construct
whose deletion is already scheduled, which the lean default forbids. **Handoff to
PDA-DEC-3, not to be lost:** it owns InProcess schema arrival, and a sixth
**schema-carrying loopback subject** joins this suite when that lands — one
`INSTANTIATE_TEST_SUITE_P` line and one trait row, no new clause. Also in the plan.

### Clause 6 is the defect clause, and needs no honesty from the implementer

The naive shape — "a late joiner receives the retained backlog" — is not uniform (Fast
DDS's defaults replay it; the loopback retains nothing), and a per-subject expected
count invites a subject to declare its way to green: decision 11's forbidden pinned
divergence wearing a trait. So clause 6 asserts **all or nothing**: publish N rows,
subscribe, count arrivals from before the subscribe — `kRetains` ⇒ exactly N, `kDrops`
⇒ exactly 0. **Partial delivery fails under both values**, and per-provider traits stop
the cross-process subject picking the other one. The shipped defect delivered "often
just the newest sample": it fails either way. **Falsification the implementer runs
once** (verification, not a shipped test): temporarily revert the single
`qos.data_sharing().off()` at `fastdds-pubsub-provider/src/qos_defaults.cpp:68`
(default **reader** QoS), confirm
`FastDdsCrossProcess/ProviderConformance.LateJoinerBacklogIsAllOrNothing` goes red
while `FastDdsLocal/...` stays green, restore. If it does not go red, the cross-process
subject is wrong and the item is not done.

Rows are 8 opaque bytes (magic u32 + seq u32) written by the harness's `RowEncoder`
straight into the provider-supplied `WriteBuffer` — no copy, no codec, no generated type —
and schemas come from nanoarrow directly (`A = struct<seq:int32>`,
`B = struct<seq:int32,extra:float64>` for the conflict case). Decision 13 consequence: the
suite cannot see payload layout, so no divergence it forces can be a wire-format change.

**Type-check broadly, link narrowly.** `conformance_clauses` (clause TU + fixtures + pipe
helper) compiles once as a static library against the seam only, linking `fletcher-core` +
`fletcher-pubsub` + gtest and **no provider, no Arrow C++**. Each subject binary is a
~40-line registration TU linking it plus one provider —
`conformance_{inprocess,fastdds,xrce}` and peers `conformance_{fastdds,xrce}_peer` — so
editing a clause recompiles one TU and relinks three small binaries, and no binary links
two providers. `FLETCHER_CONFORMANCE_XRCE` (CMake option, default `ON`) gates the XRCE
subjects *and* the MicroXRCEAgent `ExternalProject`; the inner loop sets it `OFF`, so the
forcing test iterates without the Agent in the graph.

**Isolation and flake.** Fixed, distinct DDS domains (Fast DDS 151/152, XRCE 153) and Agent
UDP port 2019, so the harness cannot collide with `fastdds-xrce-interop` (145, 2018). The
two Agent-free binaries use `gtest_discover_tests(... PRE_TEST)` for per-clause ctest names;
the **XRCE binary gets a single ctest entry** (the interop precedent — one UDP port, and
per-clause entries would pay ~24 Agent start/stop cycles). `RESOURCE_LOCK` is **one lock per
binary**, not per domain, since properties are applied target-wide and `conformance_fastdds`
carries two. Every wait is a bounded predicate wait on one shared deadline; no sleeps, no
retries. The harness spawns its own Agent (`ExternalProject` + `::testing::Environment`)
with its **own `AGENT_PREFIX`** (a shared source+build tree collides when two harnesses
configure concurrently) but a **shared `AGENT_INSTALL_DIR`** — the part the CI cache buys,
so the ~10–15 min superbuild happens once. Option `ON` with the Agent missing ⇒ the XRCE
subject **fails** naming the path, never skips; `OFF` ⇒ the subject does not exist and CMake
says so via `message(STATUS)`. The absence is loud either way.

**Divergences are countable without a ledger:** clause names are identical across subjects,
so a divergence *is* a clause failing on one subject and passing on another — group failures
by clause suffix from `--gtest_output=xml` (works for the single-entry XRCE binary too).
Decision 11 fixes them; the first run's count is a log disclosure.

### Runbook and CI wiring (exact instructions for the implementer)

MSVC multi-config on this box: CONFIGURE `conan-default`, BUILD/TEST `conan-release`. In
`inner_loop_cmd`, extend the component list to `for C in core pubsub
fastdds-pubsub-provider` (plus `xrcedds-pubsub-provider` when XRCE is `ON`) — the harness
resolves providers from the Conan cache, so without this an implementer iterating on a Fast
DDS fix would see no change — then replace the "NOT YET WIRED" comment block with, all
inside `(cd integration-tests/pubsub-conformance && …)`: `conan install . --build=missing
-pr:a=$PROFILE` → `cmake --preset conan-default -DFLETCHER_CONFORMANCE_XRCE=OFF` → `cmake
--build --preset conan-release` → `ctest --preset conan-release --output-on-failure -R
'ProviderConformance\.SchemaBeforeDataAcrossHandoff'`. In `full_suite_cmd`, add
`pubsub-conformance` to the existing `for H in ...` list (its configure/build/test shape is
already identical) and delete the "Plus PDA-DEC-1's conformance harness once it lands"
comment. In `known_accepted_failures`, change the pinned component baseline `…/70/11` to
`…/69/11` (the deleted Fast DDS test) in the **same** commit, or the next full-suite run
reads as an introduced regression. Nothing is *added* to the accepted set — the harness has
no skips, and the existing "XRCE tests … skip without one" line does **not** extend to it.

**CI, and coverage that is not net-reduced.** Every harness has
`.github/workflows/ci.integration-test.<name>.yml` plus a path-filter/job entry in
`ci.pr.yml`; this item adds both, mirroring `ci.integration-test.fastdds-xrce-interop.yml`
**including its Agent cache step**. Without it the round's first guard never runs on the
shared lane while the same PR deletes a test that runs there on both platforms — green
with no signal. Three measured repo gotchas, so they are not rediscovered red:
(1) **every `ci.integration-test.*.yml` job uses `sparse-checkout`**, so the new lane's own
list must name `pubsub`, both providers and `integration-tests/pubsub-conformance` or the
build fails on a missing target; the gateway/pubsub jobs already check out `pubsub`
(`ci.gateway.yml:42`), so the loopback move needs no edit there. (2) **Licence-header
and format gates scan the whole tree, not the diff**: every new tracked file needs
`SPDX-License-Identifier: LGPL-3.0-or-later` + `Copyright (C) 2026 The Fletcher Authors`
in its first 10 lines (`//` for C/C++/TS, `#` for CMake/YAML); no JSON fixture is added,
so no denylist entry is owed. (3) **clang-format is exactly 18.1.3 on CI** — other
18.1.x patches disagree on `<<`-chain wrapping and pass locally while failing CI.

## Corner cases forbidden

Rung 1 — **unrepresentable**: (1) *a clause publishing locally on a cross-process
subject* — `ProviderSubject` exposes no `PubSubProvider&`, `Publish` or `CreateTopic`;
(2) *partial backlog delivery* — clause 6 is all-or-nothing, so no trait value admits
"some rows lost"; (3) *two subjects of one provider disagreeing on traits*, and *a third
enum value* — the table is keyed by provider and both enums are two-valued, so widening
one is a §7 change, not a test-local escape hatch; (4) *a silently skipped clause* — no
`GTEST_SKIP`, and the one gate is applied at instantiation so absence shows in the ctest
list; (5) *remote observation* — no `subscribe` verb; (6) *wire-format assertions* — no
codec, no Arrow C++, opaque rows; (7) *any ABI surface* — no `extern "C"`, C header,
`dlopen`, vtable or negotiation.

Rung 2 — **refused typed at the door**: (8) *missing MicroXRCEAgent* ⇒ the subject fails
naming the path; no skip, no degraded subject. (9) *peer child crash, hang or EOF* ⇒ the
request's deadline expires and returns a typed failure that fails the clause; no retry, no
reconnect, no partial mode. (10) *assertions on a specific exception type* — the taxonomy
does not exist until PDA-DEC-3/9, so clause 8 asserts *that* the call failed, not which.

Handled residue, with why it could not be forbidden: **asynchronous arrival** via one
bounded predicate wait — §7 clause 5 makes asynchrony the contract, so waiting is the
behaviour under test; and **schema-less delivery** on the loopback — §7 clause 1
explicitly sanctions it, so it is contract, not an edge.

## Premises and stop conditions

Premise "the loopback lift is mechanical" is **discharged**: its only dependencies are
`JoinSegments`, `MakeSharedSchema`, `OwnedSchema::DeepCopy`, `VectorWriteBuffer`,
`MakeReadySchemaFuture` — no gateway type.

1. Fast DDS's shipped defaults replay the whole retained backlog to a cross-process late
   joiner (pinned in-process by `DefaultQosReplaysEveryRetainedRowToALateJoiner`).
   Partial cross-process delivery is the defect resurfacing: fix under decision 11 —
   **STOP-AND-ASK if the fix would move wire bytes** (`ParityOracle.EncodeEquals...` red
   is the trigger).
2. Two XRCE clients in separate processes can share one Agent with distinct session keys
   (`fastdds-xrce-interop` proves it in one process). **STOP-AND-ASK if they cannot** —
   ask whether the XRCE cross-process subject is dropped; never weaken it into an
   in-process subject wearing the name.
3. Every divergence clause 6 finds has a **Fletcher-side** fix. XRCE is the likely
   counter-example: if Agent retention makes the count neither 0 nor N, clause 6 is
   unsatisfiable under both values, pinning is forbidden and the item cannot close.
   **STOP-AND-ASK then** — the fix is outside our tree and the owner chooses; never add a
   third trait value.
4. No exception taxonomy and no pipe helper exist yet. **STOP-AND-ASK if a clause seems to
   need a specific exception type** (PDA-DEC-3/9 invents it, not this item), or **if the
   pipe helper exceeds ~250 lines or is unstable on MSVC** — then propose splitting the
   cross-process subject into its own item rather than shipping it degraded.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `ProviderConformance.SchemaBeforeDataAcrossHandoff` (all 5 subjects) | clause 1 in `src/clauses.cpp` + the five instantiations | Today it cannot even be named: no provider-agnostic harness and no cross-process subject exist. After the harness lands but before divergence fixes it fails on whichever subject violates the handoff, and the ctest name says which. |
| §7 clause set + §6.1 (clauses 2–12, all subjects) | one clause each, listed above | Each is red on any provider that diverges; failure names clause **and** subject, which is the divergence list. Clause 8 is red on the loopback (silent overwrite) and XRCE (no conflict handling) until this item's two fixes land. Clause 6 on `FastDdsCrossProcess` is proven red-capable by the falsification procedure while `FastDdsLocal` stays green — precisely §7.2's point. |

Machine checks proving the rest for free: the clause library's link line names no provider,
so a provider header cannot resolve; `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips`
proves no fix moved wire bytes; the ctest list proves no clause vanished.

## Risks / Unknowns

- **Item size is not knowable** (decision 11, by ruling). Sanctioned split shape: an
  individually large divergence fix becomes PDA-DEC-1b…n after this item; **PDA-DEC-1 does
  not close until they are green**, and no clause is ever disabled, skipped or
  trait-declared to close it. ~1750 added lines is large for a guard and justified
(actual at close: **+3429 / −194** — see the Stage Brief's "As landed"):
  greenfield twice over, clause set written once not three times.
- **Plan-shape finding for the PM.** Lifting the loopback into `pubsub/` is scheduled in
  PDA-DEC-5 but needed *now* (anonymous namespace in a TU defining `main`). Moving it here
  and leaving registration in PDA-DEC-5 means **no coexistence window and no duplicate
  class**; a harness-local copy would be a bridge PDA-DEC-5 deletes. Recommend reducing
  PDA-DEC-5 to "register it + `--provider` becomes a lookup".
- **Clause 1 cannot *force* the pre-schema window.** Fast DDS's `Publish` throws on an
  undeclared topic, so no verb ordering puts data ahead of the schema; the window is only
  a race between the `__schema` and data channels. Clause 1 asserts the observable contract
  and may pass on a run that never exercised the buffer — recorded in the harness README so
  nobody reads it as proof the buffer ran.
- **Cross-process flake would destroy a guard's value.** Mitigations above; per the round
  config, intermittent loss in clause 6 is the defect signature, never flake.
- No coexistence window, bridge, shim or re-export is introduced. The sixth
  (schema-carrying loopback) subject is a **later addition** owned by PDA-DEC-3.

## Files-to-touch

New, under `integration-tests/pubsub-conformance/`: `conanfile.py`, `CMakeLists.txt`,
`README.md`, `include/fletcher/conformance/{subject,fixtures}.hpp` (the latter: nanoarrow
schemas A/B, row encode/decode, the deadline wait), `src/clauses.cpp`,
`src/{local,peer}_subject.cpp`, `src/child_process.{hpp,cpp}`, `src/peer_main.cpp`,
`subjects/{inprocess,fastdds,xrce}_main.cpp`, `subjects/{fastdds,xrce}_peer_main.cpp`.
New elsewhere: `pubsub/{include/fletcher/pubsub/in_process_provider.hpp,src/in_process_provider.cpp}`
and `.github/workflows/ci.integration-test.pubsub-conformance.yml`. Changed:
**`docs/pubsub-interface-spec.md`** (§7 clause 3, "may" → "**must** be rejected" — the
owner's ruling, one line, in this PR), `.github/workflows/ci.pr.yml` (path filter + job
entry; the new lane carries its own `sparse-checkout` list),
`pubsub/CMakeLists.txt`, `gateway/src/main.cpp` (drop the local class, include the new
header), `.claude/runbook.PDA-DEC.config.md` (commands + baseline). Plus the two named
decision-11 fixes — conflict rejection in the loopback and in
`xrcedds-pubsub-provider/src/**` — and whatever else the first full run discloses.

## Files-to-delete

- `InProcessProvider` in `gateway/src/main.cpp` (57 lines + a 10-line rationale comment)
  — replaced by `fletcher::InProcessPubSubProvider` in `pubsub/`.
- `DefaultQosReplaysEveryRetainedRowToALateJoiner` in `fastdds-pubsub-provider/tests/`
  (~46 lines with its rationale block) — replaced by clause 6 on `FastDdsLocal` (same
  property) **and** `FastDdsCrossProcess` (the direction it could not see); its CI
  coverage is replaced in the same PR by the new lane, so coverage is not net-reduced.
- The word "may" in §7 clause 3 — replaced by "must", per the owner's ruling. No config
  keys, shims or re-exports are retired; there are none in this area.

## Numbers

Declared net lines: **+1750 / −115** — *actual at close **+3429 / −194*** (+50 for the CI lane and the two conflict-rejection
fixes; −115 corrects the earlier −70, which omitted both rationale blocks); excludes
further decision-11 fixes, not knowable up front by ruling. **New public surface: 1** —
`fletcher::InProcessPubSubProvider`, paid for by retiring the gateway-local
`InProcessProvider`. One reading throughout: only *product*-visible additions count, so the
harness's own header types and CMake option — scaffolding shipping in no package — do not.
