# PDA-DEC-8 — Multi-instance proof

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) **§4 clause 3**
("No global state"), then [plans/PDA-DEC-rulings.md](PDA-DEC-rulings.md), then
[plans/PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md) (3, 14).
The plan's `§4 clause 3` citation is **accurate**: §4's normative list item 3 is
"**No global state.** The registry takes and returns explicit objects; multiple
instances of the same provider **with different configs** must be ordinary."
Note *configs*, not only domains — the design covers both.

## Summary

Two instances of the Fast DDS provider, created through **one registry** in **one
process** with **different configurations**, are shown not to interfere: no row
crosses, no topic declaration is shared, and each honours its own domain and its
own payload bound. **No new public surface, no product code change** — the tree
already has the property, so the deliverable is a guard that would catch its loss
plus the **recorded evidence that each guard can fail**. Three gtest cases and a
six-row mutation table.

**Stated plainly, because this round has shipped four guards that could not fail:**
the forcing test is **green on the unmutated tree**. Its red-for-the-right-reason
condition is therefore not "before the change" — it is the **mutation gate** in
§Mutations. The item is not done until every row of that table has been observed
red and the observation recorded.

## Design

### 1. Where it lives, and why not in `conformance_registry`

`integration-tests/pubsub-conformance/subjects/fastdds_main.cpp`, beside
`Registry.FastDdsResolvesAsABuiltIn` — the precedent that spec §4 clause 4
already records: `conformance_registry`'s link line names `fletcher-pubsub` and
**no transport SDK**, and that narrowness *is* a guard, so registering a real
transport there would destroy it. `conformance_fastdds` already links the
provider, already holds `RESOURCE_LOCK conformance_fastdds` and already has a
180 s per-entry timeout. Its cases are discovered individually, so the three new
cases are reachable by `ctest -R 'Registry\.'` as the item requires.

### 2. The arrangement — genuine contention, then separation

One helper, `MakeInstance(registry, domain_id, payload_bound, SchemaId)`, builds
an instance through `ProviderRegistry::Create` and returns a value holding: the
base-typed provider handle, a **shared** topic name identical in every instance
(`pdadec8/shared`), an **instance-private** topic name (`pdadec8/only-a` /
`…/only-b`), and one journal per subscription recording delivered marker bytes.
Two instances are built from **one registry** with **one** `RegisterFastDDSProvider`.

Contention is arranged, not hoped for:

- **Overlapping topic names.** Both instances declare and subscribe
  `pdadec8/shared`. Any provider-internal keying by topic name alone, rather than
  per instance, breaks here.
- **Different configs on every axis Fletcher owns.** A: `domain_id=154`,
  `max_payload_bytes=4096`. B: `domain_id=155`, `max_payload_bytes=65536`. So the
  claim covers clause 3's actual words, not just the story's domains.
- **Different shapes.** A declares `SchemaId::kA`, B `kB`, on the same topic name.
  Each subscriber must receive **its own** shape.
- **One thread, alternating.** The publish sequence is shared-A, private-A,
  shared-B, private-B **on the test thread**, so consecutive publishes through
  different instances use *different* topic names. This is the only arrangement
  that reaches `Publish`'s `static thread_local std::string name` scratch buffer
  (`fast_dds_pubsub_provider.cpp:382`) — the one piece of cross-instance shared
  mutable state in the provider today.
- **Concurrent traffic**, in its own case: two threads, one per instance, 32 rows
  each onto the identical shared topic name.
- **The foreign row goes first.** In the isolation case B publishes before A, so
  anything that was going to cross had a head start over A's own row.

Separation is then asserted three ways, all of them delivered-row claims:

1. Each journal is compared **whole** against the exact vector of markers that
   instance published. Never "contains", never a tolerance.
2. Each subscriber's delivered `SharedSchema` matches its **own** declared shape.
3. An 8 KB row is **refused `kPayloadTooLarge` by A and delivered by B** on the
   same call — a zero-timing, deterministic witness that the typed core is
   per-instance.

### 3. What makes the negative assertion honest — the standing positive control

`Registry.TwoInstancesOneDomainDoInterfere` runs the **same helper** with the
**same topic name** and the only difference being that both instances sit on
domain 156, and asserts the row **does** cross. It is the live control: whenever
the arrangement loses its ability to interfere — topic names drift apart, a
subscription is not established before the publish, the settle window collapses —
this case goes red while the forcing test stays green. That pairing is what
distinguishes "isolated" from "never had a chance to interfere", which is the
specific trap this item exists to avoid.

The settle window is **one** `constexpr kSettle` used by both cases (rung 1: a
build cannot widen the control's budget and narrow the isolation test's, because
there is one number). The control also measures that a crossing fits inside it.

### 4. What this adds beyond PDA-DEC-6

`FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` already proves two Fast DDS
instances resolve **their own documents** with no process-wide profile table, and
this design does **not** re-litigate it: the two documents here are identical
(both empty). What is new: the instances are made **through the registry** rather
than by direct construction; they sit on **different domains** with **different
payload bounds**; the topic names **overlap**; and the claim is about *delivered
rows* rather than about announced QoS. Different documents are deliberately out —
that fact is already owned.

### 5. Domains, and the flake this must not reproduce

`integration-tests/pubsub-arrow-fastdds` fails 1–2 of 4 under `jobs: 28` because
four tests share domain 137 **and** topic names. This design deliberately shares
topic names, so it must own its domains outright: **154, 155** (isolation),
**156** (control, both instances), **157, 158** (concurrent). None is used
anywhere in the tree today (census: 0, 7, 43, 91–99, 137, 145, 151–153). Within
the binary, `RESOURCE_LOCK conformance_fastdds` already serialises every case, so
no two of these run at once; separate domains for the concurrent case rather than
reusing 154/155 keeps a lingering reader from one case out of the next. That is
not fixing the `pubsub-arrow-fastdds` flake — it is another item's file.

### 6. Mutations — the gate, and the core of this design

Each row is a one-line edit to **product** code, applied alone and reverted. The
implementer records the observed failure for each in the suite README (the
PDA-DEC-1 evidence-table precedent).

| # | Mutation | Class of global state it stands for | Must redden |
|---|---|---|---|
| M1 | `create_participant(config.domain_id, …)` → `create_participant(0, …)` (`fast_dds_pubsub_provider.cpp:227`) | the typed core's domain never reaching the wire | isolation: B's markers appear in A's journal |
| M2 | make `Impl::participant` a function-local `static` shared by all instances | one process-wide participant | isolation (as M1) |
| M3 | make `Impl::topics` a file-scope `static` map | a process-wide topic/writer table keyed by name | isolation on the shared topic; also the per-instance shape assertion |
| M4 | `ProviderRegistry::Create` memoises one provider per name | registry-level global state | isolation; both handles become one |
| M5 | `internal::JoinSegmentsInto` appends instead of assigning | thread-local scratch shared across instances | the alternating single-thread sequence: B's private-topic lookup carries A's name → `kTopicNotDeclared` |
| M6 | resolve the payload bound once into a file-scope `static` | a process-wide config cache | the payload split: B's 8 KB row is refused too |

The concurrent case is additionally the only one that can catch an unguarded
shared map (M3 without the deterministic path) or a non-thread-local scratch
buffer, by mis-delivery or by crash. It relies on no sanitizer.

### 7. Teardown

Both providers are destroyed inside each case's scope before the case returns
(§6 clause 5 quiescence: subscriptions dropped, no callback in flight). No case
spawns a child process, so this design adds **no new path** by which a killed
process orphans a Fast DDS shm segment in `C:\ProgramData\eprosima\fastdds_interprocess`.

### 8. The claim, as it will be published

Written into `integration-tests/pubsub-conformance/README.md` and mirrored as an
"**As landed** (PDA-DEC-8)" paragraph under spec §4 clause 3, in the style clauses
1/2/4 already use:

> Two instances of one provider, created through one registry in one process with
> different domains and different payload bounds, exchange no rows, share no topic
> declaration and share no configuration — **over the delivery path an in-process
> arrangement exercises, within a settle window a same-domain control measured a
> real crossing inside of.** It says nothing about isolation between hosts, and
> nothing about process-wide state inside the transport SDK that both instances
> would set to the same value.

## Corner cases forbidden

**Rung 1 — unrepresentable:**

1. **A drifting wait budget between the isolation test and its control** — one
   `kSettle`, one helper; the two cases differ only in their `domain_id` values.
2. **An arrangement with no teeth** — the control shares the arrangement helper,
   so "the streams could never have crossed" reddens the control in the same
   binary. The forcing test cannot pass for the absence-of-opportunity reason
   alone.
3. **A "partially isolated" state** — journals are compared whole against exact
   expected vectors. There is no leakage threshold, no tolerance count and no
   partial mode to configure, so "mostly isolated" is not a result this suite can
   report. *(The storage precedent: gaps are impossible, not handled.)*
4. **A wire-level schema conflict in the control** — the control uses **one**
   shape in both instances. Two instances declaring one topic name with different
   shapes on **one** domain would raise a §7 clause 3 question this item does not
   own; differing shapes are used only in the two-domain case, where no wire
   conflict can arise.
5. **Registry global state as an option** — `ProviderRegistry` has no static
   member and no free function with storage, and `Create` is `const`. Global state
   must be *added*, and M3/M4 are the proof that adding it is visible.

**Rung 2 — refused typed at the door:**

6. **"It threw" or "non-null" as a pass** — `src/registry.cpp`'s vacuity rule is
   adopted verbatim: every assertion is a delivered marker byte under an
   instance-distinct journal, an exact schema shape, or a specific `PubSubStatus`.
7. **A runtime skip** — no `GTEST_SKIP` in these cases. `conformance_fastdds`
   links the provider unconditionally, so unavailability is a build failure. A
   skip is a green nobody measured (PDA-DEC-1H).
8. **Deciding isolation by pointer identity or RTTI** — forbidden here.
   `Registry.EachCreateReturnsAnIndependentInstance` already pins object identity,
   and address inequality is not evidence about what crosses a transport.

**Handled residue**, each with its *why not forbidden?*:

- **A bounded negative assertion.** *Why not forbidden:* absence over an unbounded
  future is not a testable proposition. Bounded by three things together — the
  foreign publish goes first, the instance's own row completes a full transport
  round trip afterwards, and `kSettle` is the window in which the control observed
  a real crossing — and the published claim is scoped to exactly that (§8).
- **Fast DDS's `DomainParticipantFactory` process-wide singleton.** *Why not
  forbidden:* it is vendor state Fletcher cannot remove and must route through
  (`:227`). The response is to scope the claim, not to imply an absolute.
- **Orphaned shm segments after a killed test binary.** *Why not forbidden:* the
  segment is created by the SDK and orphaned by the OS killing the process; no
  code this item owns runs then. Reduced to *no added path* (§7); the pre-existing
  tree-wide exposure is unchanged in kind.

## Premises and stop conditions

- **P1 — `domain_id` reaches the wire and Fast DDS matching is domain-scoped**, so
  two instances on different domains cannot match. **STOP-AND-ASK if false:** if
  rows cross between domains on the unmutated tree, that is a provider defect and
  a PDA-DEC-6 regression — report it; do **not** weaken the assertion or widen
  `kSettle` to make the case pass.
- **P2 — two participants on *different* domains in one process are supported on
  both CI platforms.** Evidence: `test_profile_document.cpp:551-552` already
  stands up two in one process (same domain). **STOP-AND-ASK if false:** do **not**
  fall back to a two-process arrangement — two processes cannot share the
  in-memory state this item exists to disprove, so that arrangement cannot serve
  the item at all.
- **P3 — `RESOURCE_LOCK conformance_fastdds` serialises this binary's cases**, so
  the five domains are exclusively this binary's while a case runs. If the lock is
  ever removed, the concurrent case flakes first; the remedy is restoring the
  lock, never widening the window.
- **P4 — `MakeConformanceSchema(kA)` and `(kB)` are distinguishable through a
  delivered `SharedSchema`.** If not, drop that one assertion and name it in the
  README as uncovered — do **not** substitute a weaker "a schema arrived" check.
- **P5 — every mutation in §6 reddens at least one named assertion.**
  **STOP-AND-ASK if any does not:** a mutation the set survives means the guard
  does not measure that class. Record it as a named blind spot in the README
  (PDA-DEC-1 precedent) and bring it to the owner; do **not** delete the row from
  the table.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason |
|---|---|---|
| **`Registry.TwoInstancesTwoDomainsStayIsolated`** (forcing) | §2's arrangement: one registry, two configs, overlapping topic name, whole-journal comparison, per-instance shape, per-instance payload bound | **Not the pre-change tree** — it does not exist there, and the property already holds. Red under **each of M1–M6 individually**, reverted, with the observed failure recorded. That table is this item's real gate. |
| `Registry.TwoInstancesOneDomainDoInterfere` | the same helper, both instances on domain 156, one shape | Red whenever the arrangement loses its teeth: names drift, subscription not live before publish, `kSettle` collapses. This is the guard **on** the forcing test. |
| `Registry.TwoInstancesStayIsolatedUnderConcurrentTraffic` | two threads, 32 rows each, identical shared topic name, journals compared whole | M3 and M5; plus any unguarded shared map, by mis-delivery or crash. |

## Risks / Unknowns

- **The forcing test is green before the change.** Disclosed above rather than
  dressed up. If the owner or reviewer judges a proof item without a pre-change
  red unacceptable, the alternative is not a bigger design — it is to accept the
  mutation gate as the falsification, which is what PDA-DEC-6's review note
  ("the implementation must mandate the form and assert it, or this ruling is
  unfalsifiable") already demands of this round.
- **The isolation claim is bounded, not absolute.** Scoped in §8. In-process tests
  cannot see process-wide SDK state that both instances would set identically, and
  see nothing about isolation between hosts. Stated, not implied.
- **Concurrency detection power is mis-delivery and crash only** — no sanitizer
  lane is assumed.
- **No coexistence window.** Nothing bridges, delegates or re-exports; nothing is
  scheduled for deletion in a later stage.
- **No STOP-AND-ASK against spec or rulings.** No `PubSubProvider` method changes,
  no ABI, no loader, no copy on the row or attachment path.
- **Suite counts:** `pubsub-conformance` **82 → 85 ctest entries**;
  `conformance_fastdds` **+3 gtest cases**. `conformance_xrce` unchanged at
  **1 ctest entry / 27 gtest cases**. Provider suites unchanged (fastdds 85/84,
  xrce 16/15). No CMake change: the three cases are discovered into the existing
  target, whose 180 s per-entry timeout is an order of magnitude above their cost.
- **Declared net lines: +460 / −0.** Costed up front rather than after review:
  ~350 in `fastdds_main.cpp` (helper ~70, forcing case ~90, control ~50,
  concurrent ~70, constants and waits ~40, the mutation-gate header comment ~30),
  ~60 README (claim + six-row evidence table), ~20 spec, ~20 plan and progress
  log, ~10 margin.
- **New public surface: 0.** For a proof item that is the expected answer: if the
  design needed a new type or method to show the absence of global state, the
  registry would not have the property.

## Files-to-touch

- `integration-tests/pubsub-conformance/subjects/fastdds_main.cpp` — the three
  cases, the arrangement helper, five domain constants, `kSettle`.
- `integration-tests/pubsub-conformance/README.md` — the scope statement (§8) and
  the mutation evidence table, under `## The Registry suite`.
- `docs/pubsub-interface-spec.md` — an "**As landed** (PDA-DEC-8)" paragraph on §4
  clause 3, in the style clauses 1/2/4 use.
- `plans/PDA-decouple-progress-log.md`, `plans/PDA-decouple-interface.md` — the
  item's entry and tracker row.

## Files-to-delete

**None**, and the justification: this is a proof item over a property the code
already has, so there is no shim, dual path, config key or superseded test to
retire. Two candidates were considered and rejected —
`Registry.EachCreateReturnsAnIndependentInstance` (kept: it is deterministic and
lives in the SDK-free binary, which the new cases cannot) and
`FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` (kept: it owns the document
claim, which §4 deliberately does not re-litigate). The round's no-back-compat
rule has nothing to bite on here.
