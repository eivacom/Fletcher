# PDA-DEC-8 — Multi-instance proof

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) **§4 clause 3**
("No global state" — *"with different configs"*, not only different domains), then
[plans/PDA-DEC-rulings.md](PDA-DEC-rulings.md), then
[locked decisions](PDA-decouple-locked-decisions.md) 3, 12, 13, 14.

## Summary

Two instances of the Fast DDS provider, created through **one registry** in **one
process** with **different configurations**, are shown not to interfere: no row
crosses, no topic declaration is shared, each honours its own domain and its own
payload bound. **No new public surface, no product code change** — the tree already
has the property, so the deliverable is a guard against its loss plus the **recorded
evidence that the guard can fail**: four gtest cases, six mutations.

**Stated plainly, because this round has shipped five guards that could not fail:**
the forcing test is **green on the unmutated tree**, so its red-for-the-right-reason
condition is §6's mutation gate under §6's clean-environment procedure.

## Design

### 1. Where it lives, and why not in `conformance_registry`

`integration-tests/pubsub-conformance/subjects/fastdds_main.cpp`, beside
`Registry.FastDdsResolvesAsABuiltIn` (spec §4 clause 4's precedent).
`conformance_registry` links **no transport SDK** and that narrowness *is* a guard.
`conformance_fastdds` already links the provider, holds `RESOURCE_LOCK` and a 180 s
per-entry timeout, and discovers cases singly, so `ctest -R 'Registry\.'` reaches them.

### 2. The arrangement — genuine contention, then separation

One helper, `MakeInstance(registry, domain_id, payload_bound, SchemaId)`, builds an
instance through `ProviderRegistry::Create` and returns the base-typed handle, a
**shared** topic name identical in every instance (`pdadec8/shared`), an
**instance-private** one (`pdadec8/only-a` / `…/only-b`), and one journal per
subscription of delivered markers. Every case builds both instances from one
registry, one `RegisterFastDDSProvider`, an **empty document**.

**The discovery key is this design's load-bearing fact.** Two Fletcher endpoints
match only if topic name *and* registered type name agree, and the type name is
`fletcher_<max_payload_bytes>` (`payload_bound.hpp:60-65`, locked decision 13) — so
unequal bounds are an *independent* reason for non-matching, on any domain. Hence
every case asserting **or denying** a crossing gives both instances the same bound,
one `constexpr uint32_t kBound` (rung 1: unequal means deleting the constant),
leaving `domain_id` the only wire-visible difference; the bound claim moves to §2a.

Contention is arranged, not hoped for:

- **Overlapping topic names at an equal bound.** Both instances declare and
  subscribe `pdadec8/shared`, so the endpoints are genuinely matchable and only the
  domain separates them (A `domain_id=154`, B `155`).
- **Different shapes.** A declares `SchemaId::kA`, B `kB`, on that same name; each
  subscriber must receive **its own** shape.
- **One thread, alternating.** shared-A, private-A, shared-B, private-B, so
  consecutive publishes through different instances use *different* topic names,
  reaching `Publish`'s `static thread_local std::string` scratch
  (`fast_dds_pubsub_provider.cpp:382`) — the provider's one piece of cross-instance
  shared mutable state. Not the *only* arrangement reaching it
  (`test_profile_document.cpp:556-557` does too); the one that checks delivery too.
- **Concurrent traffic**, in its own case: two threads, one per instance, 32 rows
  each onto the identical shared topic name.

Separation is asserted two ways, both delivered-row claims: each journal compared
**whole** against the exact vector of markers that instance published (never
"contains", never a tolerance), and each subscriber's delivered `SharedSchema`
matching its **own** shape. B publishes **before** A, so anything that was going to
cross had a head start.

### 2a. The per-instance bound, on a pair that claims no crossing

`Registry.TwoInstancesKeepTheirOwnPayloadBounds`: domains **159/160**, bounds
**4096** and **65536**, each instance publishing on its **own private** topic to its
own subscription, so unequal bounds confound nothing and clause 3's "different
configs" keeps its second axis. Each publishes small, a row sized between the two
bounds, then small again: the high-bound journal must equal all three markers, the
low-bound one exactly the two small ones.

The middle row is **dropped silently** on the low-bound instance; it does **not**
throw — the serialising flow's pinned pre-existing behaviour
(`fletcher_sample_pub_sub_type.hpp:106-116` "Deliberately NOT recorded, so Publish
does not throw for it"; `sample_writer.hpp:59,80-84`;
`DataSharingOversizedRowDoesNotThrow`), said plainly here and in the README. A typed
`kPayloadTooLarge` lives only on the loaned flow, which both instances would need a
`fletcher.loan_publish=true` document to select; delivery is anchored instead so
every case keeps the empty document and one publish path (and `PayloadBytes()` is a
Fast DDS extension a base-typed handle cannot call). No new timing number: the third
row goes *after* the oversized one and must arrive, so nothing dead can pose as it.

### 3. What makes the negative assertion honest — the standing positive control

`Registry.TwoInstancesOneDomainDoInterfere` runs the **same helper**, topic name and
`kBound`, differing in that both instances sit on domain 156 — and in using **one**
shape rather than two (forbidden case 4) — and asserts the row **does** cross. Whenever the arrangement loses its teeth — names drift, a bound
changes on one side, a subscription is not live before the publish, the window
collapses — the control reddens while the forcing test stays green: that is what
distinguishes "isolated" from "never had a chance". One `constexpr kSettle` serves
both cases (rung 1: no build can widen the control's budget and narrow the isolation
test's), and the control measures that a real crossing fits inside it.

### 4. What this adds beyond PDA-DEC-6, and the domains it owns

`FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` owns the "own documents, no
process-wide profile table" claim and is not re-litigated (every document here is
empty). New: creation **through the registry**, differing domains (§2a: differing
bounds), **overlapping** topic names, and a claim about *delivered rows*.

`integration-tests/pubsub-arrow-fastdds` fails 1–2 of 4 under `jobs: 28` because
four tests share domain 137 **and** topic names. This design shares topic names
deliberately, so it owns its domains outright: **154, 155** (isolation), **156**
(control, both instances), **157, 158** (concurrent), **159, 160** (§2a) — none used
in the tree (census 0, 7, 43, 91–99, 137, 145, 151–153; no `domain_id` names 159 or
160). `RESOURCE_LOCK conformance_fastdds` serialises this binary's cases, and a
case-private pair keeps a lingering reader out of the next case.

### 6. Mutations — the gate, and the core of this design

Each row is a minimal edit (one to three lines) to **product** code, applied alone
and reverted, its failure recorded in the suite README (PDA-DEC-1's precedent).
Predicted mechanisms are typed refusals where that is what the tree does; P5 asks
only that a *named* assertion redden.

| # | Mutation | Class of global state it stands for | Must redden |
|---|---|---|---|
| M1 | `create_participant(config.domain_id, …)` → `create_participant(0, …)` (`fast_dds_pubsub_provider.cpp:227`) | the typed core's domain never reaching the wire | isolation: with `kBound` equal both instances now match, so B's markers appear in A's journal — or B's `CreateTopic` of the same name with a second shape is refused `kSchemaConflict`. Either fails the case |
| M2 | make `Impl::participant` a function-local `static` shared by all instances | one process-wide participant | isolation: `register_type` / `create_topic` refusal → `kTransportFailure`; and/or a teardown crash (see the procedure) |
| M3 | make `Impl::topics` a file-scope `static` map | a process-wide topic/writer table keyed by name | isolation: B's `CreateTopic` meets A's `schema_writer` with different IPC bytes → `kSchemaConflict`, or B's `Subscribe` → "already subscribed to"; and/or a teardown crash |
| M4 | `ProviderRegistry::Create` memoises one provider per name | registry-level global state | isolation: one instance, so `kSchemaConflict` on the second declaration and "already subscribed to" on the second subscribe |
| M5 | `internal::JoinSegmentsInto` appends instead of assigning (`pubsub/include/fletcher/pubsub/internal/segments.hpp` — used by **every** provider, so most pub/sub suites redden at once) | thread-local scratch shared across instances | isolation, at **A's own second publish** (`pdadec8/shared` + `pdadec8/only-a` → `kTopicNotDeclared`), not at B's lookup. Already caught by `test_profile_document.cpp:556-557`; the row costs nothing and stays |
| M6 | resolve the payload bound once into a file-scope `static` | a process-wide config cache | §2a: the high-bound instance inherits 4096, so its middle-row marker never arrives and its whole-journal comparison fails |

**Procedure, per row — the gate is only as good as this.** The four cases are
observed **green on the unmutated build immediately before** that row's mutation
(that is the environment check; no separate probe is needed); the mutated run's
failure text is recorded **verbatim** and must name *these* cases, not "the tree
went red" (M5 produces that too); and after any run that **crashes** rather than
fails — M2 and M3 delete endpoints through an already-destroyed participant, so they
may — `C:\ProgramData\eprosima\fastdds_interprocess` is cleared before the next row,
since a stale segment makes the next `create_participant` fail with a false
`0xC0000005`. A red indistinguishable from an environment fault is not evidence; a
row whose green precondition was not observed is not recorded. The concurrent case
is additionally the only one that can catch an unguarded shared map or a
non-thread-local scratch by mis-delivery or crash. No sanitizer.

### 7. Teardown

Both providers are destroyed inside each case's scope before it returns (spec §6
clause 5 quiescence: subscriptions dropped, no callback in flight). The concurrent
case **joins both publisher threads before either provider is destroyed** — a call
in flight through a dying provider is the one path by which this item's own code
could kill the process — and asserts on the main thread after the join, since a
gtest `ASSERT_*` in a spawned thread records a failure without stopping the case.
No case spawns a child process; §6's mutated builds can crash by construction,
which is why §6, not this section, owns the segment-clearing step.

### 8. The claim, as it will be published

Written into `integration-tests/pubsub-conformance/README.md` and mirrored as an
"**As landed** (PDA-DEC-8)" paragraph under spec §4 clause 3 (clauses 1/2/4's style):

> Two instances of one provider, created through one registry in **one application
> on one machine** with **different domains**, exchange no rows and share no topic
> declaration, **within a window in which a same-domain control measured a real
> crossing.** *Separately*, two instances with **different payload bounds** each
> honour their own: a row over one bound is dropped there and delivered on the other.
> That pair claims **no crossing either way** — the bound is in the registered type
> name (P1b), so it could not cross regardless. **Three exclusions, stated rather
> than implied** (ruling 2026-09-03): nothing about isolation between machines,
> nothing about vendor process-wide state both instances would set identically, and
> nothing about the shared memory two *separate* processes on one machine use — Fast
> DDS serves same-process endpoints over intra-process delivery (locked decision 12),
> so what is shown isolated is the matching and routing layer.

## Corner cases forbidden

**Rung 1 — unrepresentable:**

1. **A drifting wait budget, or a second reason the streams cannot meet** — one
   `kSettle`, one `kBound`, one helper; the isolation case and its control differ in
   `domain_id` and in nothing that *can* keep two endpoints apart (P1b).
2. **An arrangement with no teeth** — the control shares the helper *and* the bound,
   so "the streams could never have crossed" reddens the control in the same
   binary; the forcing test cannot pass for absence of opportunity alone.
3. **A "partially isolated" state** — journals compared whole against exact expected
   vectors: no leakage threshold, no tolerance, no partial mode, so "mostly
   isolated" is not a result this suite can report. *(Storage precedent: gaps are
   impossible, not handled.)*
4. **A wire-level schema conflict in the control** — it uses **one** shape in both
   instances; differing shapes appear only across domains, so the §7 clause 3
   question this item does not own cannot arise on the unmutated tree.
5. **Registry global state as an option** — `ProviderRegistry` has no static member,
   no free function with storage, `Create` is `const`; global state must be *added*,
   and M3/M4 prove adding it is visible.

**Rung 2 — refused typed at the door:**

6. **"It threw" or "non-null" as a pass** — `src/registry.cpp`'s vacuity rule
   verbatim: every assertion is a delivered marker byte in an instance-distinct
   journal, an exact schema shape, or a specific `PubSubStatus`.
7. **A runtime skip** — no `GTEST_SKIP`: `conformance_fastdds` links the provider
   unconditionally, so unavailability is a build failure and a skip would be a
   green nobody measured (PDA-DEC-1H).
8. **Isolation decided by pointer identity or RTTI** —
   `Registry.EachCreateReturnsAnIndependentInstance` pins object identity, and
   address inequality says nothing about what crosses a transport.

**Handled residue**, each with its *why not forbidden?*:

- **A bounded negative assertion** (a row that must not cross; §2a's dropped row).
  *Why not forbidden:* absence over an unbounded future is not testable. Bounded by
  the foreign publish going first, by a later row of the same instance completing a
  round trip afterwards, and by `kSettle` being the window in which the control
  observed a real crossing — published as exactly that.
- **Fast DDS's `DomainParticipantFactory` process-wide singleton.** *Why not
  forbidden:* vendor state Fletcher cannot remove and must route through (`:227`);
  the response is to scope the claim (§8), not to imply an absolute.
- **Orphaned shm segments after a killed binary.** *Why not forbidden:* the SDK
  makes them and the OS orphans them, with no code of ours running; handled where
  they occur — §6's mutated runs — by clearing between rows.

## Premises and stop conditions

- **P1 — `domain_id` reaches the wire and Fast DDS matching is domain-scoped**, so
  two instances on different domains cannot match. **STOP-AND-ASK if false:** rows
  crossing domains on the unmutated tree is a provider defect and a PDA-DEC-6
  regression — report it; do **not** weaken an assertion or widen `kSettle`.
- **P1b — the payload bound is part of the registered DDS type name, hence of the
  discovery key** (`payload_bound.hpp:60-65`: "endpoints on different bounds fail to
  discover each other"; locked decision 13) — why one `kBound` serves every case
  that asserts or denies a crossing, and why §2a claims none. **STOP-AND-ASK if the
  bound ever leaves the type name:** §2a's pair becomes mutually discoverable and
  must be re-argued; do not equalise its bounds to keep it green. The **schema shape
  is not** part of that key and cannot suppress a delivery (a reader runs no
  row-against-schema validation; a schema only releases `OrderedDelivery`'s
  pre-schema backlog), so shapes are an outcome, never a separator — which is what
  makes the control's single shape free.
- **P2 — two participants on *different* domains in one process are supported on
  both CI platforms.** Evidence: `test_profile_document.cpp:551-552` stands up two
  in one process (same domain). **STOP-AND-ASK if false:** do **not** fall back to
  two processes — they cannot share the in-memory state this item disproves, so that
  arrangement cannot serve the item at all.
- **P3 — `RESOURCE_LOCK conformance_fastdds` serialises this binary's cases**, so
  the seven domains are this binary's alone while a case runs. If the lock is ever
  removed the concurrent case flakes first; restore the lock, never widen the window.
- **P4 — `MakeConformanceSchema(kA)` and `(kB)` are distinguishable through a
  delivered `SharedSchema`.** If not, drop that assertion and name it uncovered in
  the README — never substitute a weaker "a schema arrived" check.
- **P4b — an oversized row on the serialising publish flow is dropped and logged,
  not thrown** (`fletcher_sample_pub_sub_type.hpp:106-116`, pinned by
  `DataSharingOversizedRowDoesNotThrow`), so §2a asserts delivery and absence.
  **If it throws instead**, the round changed that behaviour: report it and
  re-anchor §2a on the typed refusal — never catch and ignore.
- **P5 — every mutation in §6 reddens at least one named assertion.**
  **STOP-AND-ASK if any does not:** a surviving mutation means the guard does not
  measure that class — record it as a named blind spot in the README (PDA-DEC-1
  precedent) and bring it to the owner; do **not** delete the row.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason |
|---|---|---|
| **`Registry.TwoInstancesTwoDomainsStayIsolated`** (forcing) | §2's arrangement: one registry, one `kBound`, overlapping topic name, differing domains, whole-journal comparison, per-instance shape | **Not the pre-change tree** — it does not exist there, and the property already holds. Red under **M1–M5 individually** (M6 reddens §2a), reverted, each under §6's procedure with the failure recorded. That table is this item's real gate. |
| `Registry.TwoInstancesOneDomainDoInterfere` | the same helper and the same bound, both instances on domain 156, one shape | Red whenever the arrangement loses its teeth: names drift, a bound is changed on one side, subscription not live before publish, `kSettle` collapses. This is the guard **on** the forcing test. |
| `Registry.TwoInstancesStayIsolatedUnderConcurrentTraffic` | two threads, 32 rows each, identical shared topic name and bound, journals compared whole | M3 and M5; plus any unguarded shared map, by mis-delivery or crash. |
| `Registry.TwoInstancesKeepTheirOwnPayloadBounds` | §2a: unequal bounds, private topics, whole-journal comparison on both sides | M6 — the high-bound instance inherits the low bound and its middle row never arrives. Also red if the bound is ignored altogether. |

## Risks / Unknowns

- **The forcing test is green before the change**, disclosed rather than dressed up:
  §6's gate is the falsification, and a proof item over a property the tree already
  has has no other honest red.
- **The isolation claim is bounded, not absolute** — three exclusions in §8,
  published not implied. Concurrency detection power is mis-delivery and crash only.
- **The §6 gate is manual evidence**, worth only as much as the recorded text
  matching the predicted mechanism — hence the typed-refusal column and the void.
- **No coexistence window**, nothing scheduled for later deletion, and **no
  STOP-AND-ASK**: no `PubSubProvider` change, no ABI, no loader, no copy.
- **Suite counts:** `pubsub-conformance` **82 → 86 ctest entries**;
  `conformance_fastdds` **+4 gtest cases** (the review verified the 82 base and one
  entry per discovered case). `conformance_xrce` unchanged at **1 / 27**, provider
  suites unchanged (fastdds 85/84, xrce 16/15). No CMake change: the 180 s
  per-entry timeout is an order of magnitude above these cases' cost.
- **Declared net lines: +570 / −0** (superseding revision 0's `+460` and the review's
  `~+540`). **Landed: +705 / −25** — 473 `fastdds_main.cpp`, 108/−2 README, 22 spec,
  2/−2 the CMakeLists count fix, 100/−21 these plans and the log. **New public surface: 0** — a
  proof of absent global state that needed a new type would be proving the opposite.

## Files-to-touch

- `integration-tests/pubsub-conformance/subjects/fastdds_main.cpp` — the four cases,
  the helper, seven domain constants, `kSettle`, `kBound`.
- `integration-tests/pubsub-conformance/README.md` — the scope statement (§8), the
  silent-drop disclosure (§2a), and the evidence table with §6's procedure, under
  `## The Registry suite`.
- `docs/pubsub-interface-spec.md` — an "**As landed** (PDA-DEC-8)" paragraph on §4
  clause 3. `plans/PDA-decouple-progress-log.md`,
  `plans/PDA-decouple-interface.md` — the item's entry and tracker row.

## Files-to-delete

**None**, justified: a proof item over a property the code already has retires no
shim, dual path, config key or test. Two candidates were considered and kept —
`Registry.EachCreateReturnsAnIndependentInstance` (deterministic, and in the SDK-free
binary these cases cannot live in) and
`FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` (owns §4's document claim).
