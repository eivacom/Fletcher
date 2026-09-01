# pubsub-conformance — the delivery contract, executable

Oracle: [docs/pubsub-interface-spec.md](../../docs/pubsub-interface-spec.md) §7,
§7.1, §7.2, plus §6 clause 1. This harness encodes those clauses **once** and
runs them against every provider. It is what makes the seam a contract rather
than a description, and what a later ABI round checks itself against without
re-deriving the rules.

## Shape

- **Clauses** live in `src/clauses.cpp` (plus `src/clauses_carried.cpp`, below),
  one value-parameterised gtest suite named literally `ProviderConformance`. A
  full test name reads `<Subject>/ProviderConformance.<Clause>/<Subject>`, so
  `ctest -R 'ProviderConformance\.SchemaBeforeDataAcrossHandoff'` scopes to one
  clause across every subject.
- **Subjects** are one provider exercised one way. Each is ~40 lines of
  registration in `subjects/`:

  | Subject | Provider | Publisher lives | Schema mode | Retention |
  |---|---|---|---|---|
  | `InProcessLocal` | in-process loopback | this process | absent | drops |
  | `FastDdsLocal` | Fast DDS (domain 151) | this process | carried | retains |
  | `FastDdsCrossProcess` | Fast DDS (domain 152) | **a child process** | carried | retains |
  | `XrceLocal` | XRCE-DDS (domain 153, Agent :2019) | this process | carried | retains |
  | `XrceCrossProcess` | XRCE-DDS (domain 153, Agent :2019) | **a child process** | carried | retains |

- **A clause body sees only a `ProviderSubject`** — no `PubSubProvider&`, no
  `Publish`, no `CreateTopic`. A clause publishing locally on a cross-process
  subject is therefore unrepresentable, rather than something review has to
  catch.
- **Retention is keyed by provider**, not by subject
  (`RetentionForProvider`), so a provider's two subjects cannot disagree and
  clause 6 has no per-subject escape hatch. `schema_mode` *is* per subject: §7
  clause 1 sanctions the same transport being exercised in both modes.
- **Rows are 8 opaque bytes** (magic + seq) written straight into the
  provider-supplied `WriteBuffer`. No codec, no Arrow C++, no generated type —
  so the suite cannot see payload layout and no divergence it forces can be a
  wire-format change (locked decision 13).

## Divergences are countable without a ledger

Clause names are identical across subjects, so a divergence *is* a clause
failing on one subject and passing on another. Nothing is ever recorded as
"known divergent": locked decision 11 says they are fixed.

The first full run (2026-09-01) found **three**, all fixed in the same change:

1. `InProcessLocal` / `ConflictingRedeclarationIsRejected` — the loopback
   silently overwrote the declared schema. Now refused.
2. `Xrce*` / `IdenticalRedeclarationIsIdempotent` — `CreateTopic` threw on any
   existing topic state, so an identical re-declaration was refused.
3. `XrceLocal` / `SchemaBeforeDataAcrossHandoff`, `CallbackNeverSeesNullSchema`,
   `BacklogNeverInterleavesWithLiveSamples`,
   `SubscribeNeverBlocksSchemaArrivesLater` — same root cause as (2): because
   `Subscribe` creates topic state lazily, *every* subscriber-first declaration
   was refused, so a subscriber on an XRCE instance could never be joined by a
   publisher on it.

A fourth thing the harness disclosed was a packaging defect rather than a
behaviour divergence: `fletcher-xrcedds-pubsub-provider` never declared
`ws2_32`, which resolved by accident because every consumer also linked the Fast
DDS provider. One provider per binary is a deliberate property here, so it
surfaced immediately.

## What these clauses do NOT prove — read before trusting them

- **Clause 1 cannot *force* the pre-schema window.** Fast DDS's `Publish` throws
  on an undeclared topic, so no ordering of verbs puts data ahead of the schema;
  the window is only a race between the `__schema` and data channels. The clause
  asserts the observable contract and may pass on a run that never exercised the
  provider's pre-schema buffer. It is not proof the buffer ran.
- **Clause 12 observes serialization; on a cross-process subject it cannot force
  it.** The peer protocol is one request/reply at a time, so two concurrent
  publishes to a peer subject are impossible — there, the clause is an
  observation. On the two `*Local` subjects `PublishRow` is a direct call, the
  clause publishes from two threads, and the assertion is real (it is a genuine
  check on the loopback, whose `Publish` holds its mutex across the callback).
- **Clause 6 is not proven able to catch the shipped data-sharing defect.** The
  falsification the design mandated was run: reader-side `data_sharing().off()`
  in `fastdds-pubsub-provider/src/qos_defaults.cpp` was temporarily reverted and
  `FastDdsCrossProcess/ProviderConformance.LateJoinerBacklogIsAllOrNothing`
  **passed 12/12** (and 8/8 with a 2 s late-joiner gap added), while
  `integration-tests/gateway-fastdds-ts`, rebuilt against that same falsified
  provider, failed with the documented signature (4/4, 4/4, then 2/4). So the
  defect is live and reproducible, and this harness's single-writer /
  single-reader cross-process shape does **not** reproduce it. Clause 6 will
  fail on any partial replay it observes — it is all-or-nothing, so partial
  fails under both trait values — but do not read it as covering that defect
  class. Open finding, handed to the PM.

## Clause 2 and the axis gate

`CallbackNeverSeesNullSchema` asserts a property of schema-*carrying*
transports. Its gate is the **link line**: `src/clauses_carried.cpp` is compiled
into the carrying subjects' binaries only. So on a schema-less subject the clause
is *absent from the ctest list* rather than present and skipped, and
`GTEST_SKIP` appears nowhere in this suite. Absence is visible; a skip is not.

The mirror property for a schema-less transport — null throughout, never mixed —
is clause 3, which runs on every subject.

Delivery clauses declare their topic with `DataSchema()`, which is no schema at
all on a schema-less subject: handing the loopback a schema and then asserting it
does not carry one would be a contradiction, not a test. Clauses 7 and 8 are the
exception — they observe only the *reply* to a declaration, so they declare the
real A and B on every subject.

## Handoff owed to PDA-DEC-3

The loopback is exercised **only** as a schema-less transport. Making it
schema-carrying needs a real promise, a pre-schema queue and an ordered flush
inside the exact class PDA-DEC-3 replaces (spec §3.4 retires the `shared_future`
as the contract), so it is not built twice. **When PDA-DEC-3 lands, a sixth
subject — a schema-carrying loopback — joins this suite: one
`INSTANTIATE_TEST_SUITE_P` line in `subjects/inprocess_main.cpp` and one
`schema_mode` value. No new clause.** Also recorded in
`plans/PDA-decouple-interface.md`.

## Building and running

```sh
PROFILE=<abs path>/.conan-profiles/Windows-msvc194-x86_64-Release
conan install . --build=missing -pr:a=$PROFILE
cmake --preset conan-default            # MSVC is multi-config: CONFIGURE here
cmake --build --preset conan-release    # BUILD and TEST with conan-release
ctest --preset conan-release --output-on-failure
```

`-DFLETCHER_CONFORMANCE_XRCE=OFF` drops the two XRCE subjects **and** the
MicroXRCEAgent `ExternalProject` from the graph — the inner loop uses it so the
forcing test iterates without a ~10–15 min superbuild in the way. CMake says so
with a `message(STATUS)`; the subjects then do not exist, rather than existing
and skipping. With the option **ON** and the Agent missing, the XRCE binary
**fails** naming the path it looked at.

The Agent install directory is shared with
`integration-tests/fastdds-xrce-interop` (`C:/fl-uxa-install` on Windows) so the
superbuild happens once per machine and CI caches one unit; the
`ExternalProject` *prefix* is this harness's own, because a shared source+build
tree collides when two harnesses configure concurrently.

Isolation: fixed DDS domains 151/152/153 and Agent UDP port 2019, none of which
`fastdds-xrce-interop` (domain 145, port 2018) uses. `RESOURCE_LOCK` is one lock
per binary, not per domain, because ctest properties apply target-wide and
`conformance_fastdds` carries two subjects. The XRCE binary has a **single**
ctest entry (the interop precedent — one UDP port, and per-clause entries would
pay ~24 Agent start/stop cycles).

Every wait is a bounded predicate wait on one deadline per clause. There are no
sleeps and no retries; the two waits that are *expected* to time out (clauses 9
and 11 — nothing may arrive) pay `kSettleBudget` in full and say so.
