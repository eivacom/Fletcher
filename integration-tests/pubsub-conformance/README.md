# pubsub-conformance — the delivery contract, executable

Oracle: [docs/pubsub-interface-spec.md](../../docs/pubsub-interface-spec.md) §7,
§7.1, §7.2, plus §6 clause 1. This harness encodes those clauses **once** and
runs them against every provider. It is what makes the seam a contract rather
than a description, and what a later ABI round checks itself against without
re-deriving the rules.

## Shape

- **Clauses** live in `src/clauses.cpp` (plus `src/clauses_carried.cpp`, below),
  one value-parameterised gtest suite named literally `ProviderConformance`.
  There are two names for each clause and they differ, which is worth knowing
  before writing a filter:

  - **gtest** (`--gtest_filter`, `--gtest_list_tests`):
    `<Subject>/ProviderConformance.<Clause>/0` — the trailing `0` is the
    parameter index, since the suite has one parameter per instantiation.
  - **ctest** (`ctest -R`, `ctest -N`):
    `<Subject>/ProviderConformance.<Clause>/<Subject>` — CMake's
    `gtest_discover_tests` substitutes the *printed parameter* for the index, and
    the parameter prints as the subject label (see `SubjectFactory::label`,
    which exists so that name is stable rather than a dump of uninitialised
    bytes).

  Either way `ctest -R 'ProviderConformance\.SchemaBeforeDataAcrossHandoff'`
  scopes to one clause across every subject, because the clause name sits
  between the two varying parts.
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
- **Clause 6 is not proven able to catch the shipped data-sharing defect, and
  nobody yet knows why.** This is an OPEN question, not a closed one. Do not
  read the paragraph below as a reason to distrust cross-process conformance
  subjects in general — that reading has been measured and refuted.

  **Owning stage: PDA-ABI-7** (zero-copy receive), which is the feature the
  defect actually blocks. Its plan entry —
  [plans/PDA-ABI-protocol-driver-abi.md](../../plans/PDA-ABI-protocol-driver-abi.md),
  §PDA-ABI-7 — carries the handoff and says to start from the evidence below
  rather than from scratch (owner ruling 2026-09-01, "Ship the guard, hunt
  elsewhere"). Read that entry for what is owed; this section is the evidence,
  and the two are deliberately not duplicates.

  **Measured on:** Windows 11 (26200), MSVC 19.44 / VS 2022, Release,
  Fast DDS **3.4.0** (the Conan pin), single machine, 2026-09-01. Nothing here
  has been reproduced on Linux, and the ratio of red runs is load-sensitive, so
  treat the counts as existence proofs rather than rates.

  The falsification the design mandates was run twice, and the control was run
  beside it each time. Procedure: temporarily revert the reader-side
  `qos.data_sharing().off()` in `fastdds-pubsub-provider/src/qos_defaults.cpp`,
  confirm the harness links that package (check the source in the resolved Conan
  package folder, not just that a build ran), run, restore.

  | Against the falsified provider | Result |
  |---|---|
  | `FastDdsCrossProcess/…LateJoinerBacklogIsAllOrNothing`, with a live sentinel row published after `Subscribe` | green 12/12 (and 8/8 with a 2 s late-joiner gap) |
  | the same clause with the sentinel **removed** — the sequence as it ships now | green 12/12 |
  | `integration-tests/gateway-fastdds-ts`, rebuilt against that same package | **red**, documented signature: 2/4 on runs 1 and 3, 4/4 on runs 2 and 4 |

  Two hypotheses have been offered and both are now refuted by measurement:

  1. *"A single-writer / single-reader cross-process shape cannot see this
     class."* Refuted by the control: `gateway-fastdds-ts` has exactly that
     shape and does reproduce.
  2. *"The clause's own live sentinel row masked it"* — the sentinel came from
     the same RELIABLE + KEEP_ALL writer, so waiting on it would force in-order
     NACK/repair of the very gap the clause observes. Mechanically sound, and it
     is why the sentinel was removed (it was also not in the approved design),
     but refuted as *the* cause: the clause is still green 12/12 without it.

  What still differs, and is untested here — the remaining candidates, in the
  order worth trying: `gateway-fastdds-ts`'s peer participant holds a **reader**
  (TsToCpp) beside its writer and the gateway holds both too, so several
  data-sharing endpoints coexist per participant; the gateway's reader sits under
  a `Subscriber` fan-out layer rather than directly on the provider; and two
  topics plus two `__schema` channels are live rather than one of each. The
  *schema*-propagation test fails in the same runs as the row test, so whatever
  it is also loses a retained `KEEP_LAST(1)` sample — which points at the number
  and mix of data-sharing endpoints rather than at the row channel.

  Note the first candidate cannot be tested from this harness as designed: it
  would need the peer child to subscribe, and the design deliberately gives the
  peer protocol **no `subscribe` verb** (a peer publishes and cannot observe).
  Changing that is a design decision for PDA-ABI-7, not an implementation one.

  `integration-tests/gateway-fastdds-ts` is the only harness that reproduces the
  defect. **Do not weaken or delete it.**

  Meanwhile: clause 6 is all-or-nothing, so it fails on any partial replay it
  *does* observe, under either trait value — but do not read it as covering that
  defect class.

## The `CopyAccounting` suite — zero-copy, made falsifiable

Oracle: [docs/pubsub-interface-spec.md](../../docs/pubsub-interface-spec.md) §8,
§8.1, §3.1, §3.2. A **second** suite in this harness, in its own binary
(`conformance_copy_accounting`), linking no provider SDK and running in
milliseconds. `ctest -R 'CopyAccounting\.'` runs the whole oracle: seven entries.

It decides, by **address provenance**, whether the payload bytes a subscriber
sees are the very bytes the publisher wrote — for rows and for attachments. Four
pointers per publish, compared. No counters, no allocator hooks, no sanitizer:
counting catches only allocation-shaped copies, misses copies into a pooled or
reused buffer, and on Windows does not interpose allocations made inside a
provider DLL carrying its own CRT — blind exactly where a loaded driver will
live. A copy into recycled storage still lands at a different address than the
*live* encode window.

| Entry | What it is |
|---|---|
| `PublishAndReceivePerformNoPayloadCopies` × 3 subjects | the forcing test: row at 64 B and 4 KiB, plus two 1 KiB attachments |
| `StagingIsCaught` | the **live negative control** — a deliberately-copying provider the same `Judge()` must score at `row_copies == 1`, `attachment_copies == 2` |
| `BorrowedAttachmentCostsExactlyOneCopy` | the one §3.2 receive-side copy, **pinned at exactly one** |
| `RefillMovementIsCountedNotFailed` | the refill counter is live: non-zero on a growable buffer, zero on a fixed one |
| `JudgeArithmeticIsSound` | the pure verdict function, without a provider |

Subjects: `SeamProbe` (a fixed-arena provider in this harness — the positive
control, proving the seam *permits* zero-copy), `InProcessLoopback` (the real
`InProcessPubSubProvider`, called directly at the seam) and `InProcessViaPubSub`
(the same provider reached through `Publisher` and `Subscriber`, so the layers
*above* the seam are inside a measured path too). All three are same-process by
construction: provenance is an address, and an address means nothing across an
address space, so a cross-process or off-thread subject cannot be built here at
all — and the ledger is deliberately unsynchronised to keep it that way.

**Refill is permitted and its cost is published, not failed.** Spec §3.1 clause 1
says bytes already written "must not move or be flushed **except inside a
refill**, which must preserve them verbatim", and the owner's 2026-09-01 ruling
permits it on condition the number is reported. Every *other* byte movement fails
the guard. The number is on stdout and in the JUnit XML; today
`InProcessLoopback` relocates 3 times / 5632 bytes writing a 4 KiB row in 64-byte
appends, and `SeamProbe` relocates nothing.

### What green does NOT prove — read before trusting it

- **It proves nothing about any transport's internals.** Green means *the
  interface* performs no copies. It is not evidence about Fast DDS or XRCE:
  not about data-sharing, not about loaned samples, not about receive-side
  zero-copy — none of which exist or are enabled today. Measuring the DDS
  receive leg now would measure the serialize-and-copy path and report the
  number as evidence, so it is **not measured** rather than measured wrongly
  (owner ruling 2026-09-01, "Scope to the interface, say so plainly"; the
  receive-side data-sharing defect is owned by PDA-ABI-7).
- **The DDS/XRCE publish-side loan path is unmeasured.** The `LoanedRoundTrip`,
  `LoanedDeliversAttachments` and `DataSharing*` tests in the Fast DDS provider's
  own suite assert *delivery over* the loan path; none of them asserts the
  *absence of copies*. Do not read them as covering this ground.
- **`BorrowedAttachmentCostsExactlyOneCopy` is a red-on-fix tripwire, not an
  accepted divergence.** Today's `Blob = shared_ptr<const vector<uint8_t>>`
  cannot alias foreign memory, so a provider handed borrowed transport memory
  must copy it once. **PDA-DEC-3 removes that limitation, and when it does this
  test goes red on purpose** — update the number there, do not delete the test.
  Silence is how such a fix gets forgotten or half-lands.

### The premises a new subject must satisfy

PDA-ABI is expected to register a driver-backed subject into this same shape.
Two preconditions, and a subject that breaks either must not be registered:

- **Synchronous delivery on the publishing thread.** The ledger has no lock
  because it needs none, and the delivery count is asserted `== 1` before any
  verdict is read — so "nothing arrived" can never read as "no copies".
- **Encode-window liveness.** The bytes `encode_base` names must remain
  allocated and unfreed until the subscriber callback returns. That is what makes
  provenance immune to allocator reuse: a live allocation cannot be handed out
  twice, so a same-address copy would require free-then-allocate — and a freed
  encode window handed to a callback is a use-after-free, a worse bug than a
  copy. A subject that frees, recycles or pools its encode window before delivery
  makes the measurement unsound.

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

Every wait is a bounded predicate wait on one deadline per clause. That deadline
is anchored at the clause's **first wait**, not at `SetUp`: on a cross-process
subject the synchronous pipe round-trips before it (`PerWriterOrderIsMonotonic`
makes 21) would otherwise spend the budget, and the wait would fail on harness
throughput rather than on delivery behaviour. It is still one deadline per
clause, so no clause can pass by spending several.

There are no sleeps and no retries; the waits that are *expected* to time out
(clauses 6-on-a-dropping-transport, 9 and 11 — nothing may arrive) pay
`kSettleBudget` in full and say so.

Clause 6 publishes **nothing** after `Subscribe`, deliberately: a live row from
the same RELIABLE + KEEP_ALL writer would force in-order repair of the very gap
the clause exists to observe. See the clause comment.

A clause's `Collector` lives on the test-body stack and the provider outlives the
test body, so every clause declares its `Collector` first and a
`ScopedSubscription` second — destruction is reverse declaration order, so the
subscription always ends before the storage its callback writes into. A trailing
`Unsubscribe` cannot do that job: no `ASSERT_` failure path reaches it, so the
bug would fire exactly when a clause fails.

The peer protocol is **tagged**: each request carries an id and each reply echoes
it. Untagged, one stray line on the child's stdout (a library log line, or a late
reply to a request that already timed out) shifts every later reply by one
forever, and a `create` that actually failed reports the previous request's
`ok` — a silent false pass. And `DeclareTopic`/`PublishRow` return a three-valued
`Reply`, so a negative clause asserts `refused()` and a dead peer or an expired
deadline (`kHarnessFailure`) cannot satisfy it.
