# PDA-DEC-1 — Conformance suite for the delivery contract (design)

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §7/§7.1/
§7.2 (§0.1, §6, §11 constraining); locked decisions 11, 12, 13. Cited, not restated.

## Summary

A new `integration-tests/pubsub-conformance/` harness encodes §7's clauses **once**,
in a provider-agnostic static library, and runs them against six **subjects** —
InProcess (schema-carrying and schema-less), Fast DDS and XRCE (each in-process and
cross-process). A subject registers; it does not copy tests. The cross-process
subjects put the *publisher* in a child process driven over a request/reply pipe,
which is what makes transport behaviour — and the shipped receive-side data-sharing
defect class — observable at all.

## Design

### The subject abstraction (the load-bearing decision)

One type, five methods, in `include/fletcher/conformance/subject.hpp`:

```cpp
struct SubjectTraits {
    std::string name;
    enum class SchemaMode { kCarried, kAbsent } schema_mode;
    enum class Retention { kRetainsPreSubscribe, kDropsPreSubscribe } retention;
};

class ProviderSubject {                       // clause bodies see ONLY this
  public:
    virtual ~ProviderSubject() = default;
    virtual SubjectTraits Traits() const = 0;
    // Publisher side — MAY execute in another process.
    virtual std::optional<std::string> DeclareTopic(const Topic&, SchemaId) = 0;
    virtual std::optional<std::string> PublishRow(const Topic&, uint32_t seq) = 0;
    // Subscriber side — always this process, always this instance.
    virtual SubscriptionResult Subscribe(const Topic&, SubscribeCallback) = 0;
    virtual void Unsubscribe(const Topic&) = 0;
};
```

`nullopt` = the publisher-side call succeeded; a string = it failed, carrying the
exception's type name and `what()`. The subject **never hands out a
`PubSubProvider&`**, so a clause body cannot publish locally: an in-process shortcut
on a cross-process subject is unrepresentable rather than a review risk.

Two publisher-side implementations. **Local** calls `CreateTopic`/`Publish` on the
same instance the subscriber side uses, catching into the reply. **Peer** spawns a
child executable and exchanges one line per request over stdin/stdout —
`create <topic> <A|B|none>`, `publish <topic> <seq>`, `quit`, replying `ok` or
`err <type>: <what>`, after printing `READY` (the `fastdds_peer` lifecycle
convention). The child's provider comes from a factory linked into it, so the loop
and protocol exist once. There is **no `subscribe` verb**: the peer publishes and
cannot observe, and no §7 clause needs it to.

### The clause set

`src/clauses.cpp` — one TU, one value-parameterised gtest suite named literally
`ProviderConformance`, instantiated per subject via
`INSTANTIATE_TEST_SUITE_P(<Subject>, ProviderConformance, ...)`. Full names read
`<Subject>/ProviderConformance.<Clause>/0`, so
`ctest -R 'ProviderConformance\.SchemaBeforeDataAcrossHandoff'` scopes to that one
clause across every subject — which is exactly the item's forcing-test id.

| # | Clause (gtest name) | §7 |
|---|---|---|
| 1 | `SchemaBeforeDataAcrossHandoff` **(forcing)** | 1 + 2 |
| 2 | `CallbackNeverSeesNullSchema` | 1 |
| 3 | `SchemaModeIsUniformNeverMixed` | 1 (last sentence) |
| 4 | `PerWriterOrderIsMonotonic` | 2 |
| 5 | `BacklogNeverInterleavesWithLiveSamples` | 2 |
| 6 | `LateJoinerBacklogIsAllOrNothing` | 2 + decision 12 |
| 7 | `IdenticalRedeclarationIsIdempotent` | 3 |
| 8 | `ConflictingRedeclarationIsRejected` | 3 |
| 9 | `OneCallbackPerTopicPerInstance` | 4 |
| 10 | `SubscribeNeverBlocksSchemaArrivesLater` | 5 |
| 11 | `NoDeliveryAfterUnsubscribeReturns` | 6 |

Applicability has **exactly one gate**: clause 2 is instantiated only for
`kCarried` subjects, and its mirror assertion (schema is null throughout) is
clause 3's job for `kAbsent`. Everything else runs on every subject. The gate is
applied at *instantiation*, so an absent clause is visible in the ctest list;
`GTEST_SKIP` appears nowhere in the harness.

### Clause 6 is the defect clause, and why it needs no honesty from the implementer

The naive shape — "a late joiner receives the retained backlog" — is not uniform:
Fast DDS's shipped defaults replay it, the in-process loopback retains nothing. A
per-subject expected count invites a subject to declare its way to green, which is
decision 11's forbidden pinned divergence wearing a trait.

So clause 6 asserts **all or nothing**: publish N rows, subscribe, count what
arrives from before the subscribe. `kRetainsPreSubscribe` ⇒ exactly N;
`kDropsPreSubscribe` ⇒ exactly 0. **Partial delivery fails under both values**, so
the partial-serve state is not declarable at all. The shipped defect delivered
"often just the newest sample" — it fails whichever value Fast DDS carries.

**Falsification procedure the implementer runs once** (verification, not a shipped
test): temporarily revert `data_sharing().off()` on the Fast DDS default *reader*
QoS and confirm `FastDdsCrossProcess/ProviderConformance.LateJoinerBacklogIsAllOrNothing`
goes red while `FastDdsLocal/...` stays green, then restore. If it does not go red,
the cross-process subject is wrong and the item is not done.

### Rows, schemas, and staying away from the wire

Rows are 8 opaque bytes (magic u32 + seq u32) written by the harness's `RowEncoder`
straight into the provider-supplied `WriteBuffer` — no copy, no codec, no generated
type. Schemas are built with nanoarrow directly (`A = struct<seq:int32>`,
`B = struct<seq:int32,extra:float64>` for the conflict case), so the harness links
`fletcher-core` + `fletcher-pubsub` + one provider + gtest and **no Arrow C++**.
Decision 13 consequence: the suite cannot see payload layout, so no divergence it
forces can be a wire-format change by construction.

### Cost: type-check broadly, link narrowly

`conformance_clauses` (the clause TU + fixtures + pipe helper) compiles once as a
static library against the seam only. Each subject binary is a ~40-line registration
TU linking that library plus one provider: `conformance_inprocess`,
`conformance_fastdds`, `conformance_xrce`, plus peers `conformance_fastdds_peer`,
`conformance_xrce_peer`. Editing a clause recompiles one TU and relinks three small
binaries; no binary links two providers. `FLETCHER_CONFORMANCE_XRCE` (CMake option,
default `ON`) gates the XRCE subjects *and* the MicroXRCEAgent `ExternalProject`;
the inner loop configures it `OFF`, so the forcing test iterates without the Agent
build in the graph.

### Isolation and flake

Fixed, distinct DDS domains (Fast DDS local 151, cross-process 152, XRCE 153) and
Agent UDP port 2019, so the harness cannot collide with `fastdds-xrce-interop`
(domain 145, port 2018). `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)` gives
per-clause ctest names, so each binary's tests carry
`RESOURCE_LOCK "fletcher-dds-<domain>"` to stay safe under `ctest -j`. Every wait
is a bounded predicate wait on one shared deadline constant; no sleeps, no retries.

### XRCE does not inherit the skip

The harness builds and spawns its own Agent (`ExternalProject` +
`::testing::Environment`, the proven `fastdds-xrce-interop` pattern), reusing that
suite's `AGENT_PREFIX`/`AGENT_INSTALL_DIR` defaults verbatim so the existing local
build and the CI cache serve both and the ~10–15 min superbuild happens once. With
the option `ON` and the Agent missing at runtime the XRCE subject **fails** with
"MicroXRCEAgent not found at <path>" — never skips. With it `OFF` the subject does
not exist and CMake says so via `message(STATUS)`. Either way the absence is loud.

### Divergences as a countable list

Clause names are identical across subjects, so a divergence *is* a clause failing
on one subject and passing on another: `ctest --output-junit conformance.xml`,
grouped by clause suffix, is the whole mechanism. No divergence ledger lands in the
repo — decision 11 fixes them. The count at first full run is a log disclosure.

### Runbook wiring (exact lines for the implementer)

MSVC multi-config on this box: CONFIGURE `conan-default`, BUILD/TEST `conan-release`.
In `inner_loop_cmd`, replace the "NOT YET WIRED" comment block with:

```
    (cd integration-tests/pubsub-conformance \
      && conan install . --build=missing -pr:a=$PROFILE \
      && cmake --preset conan-default -DFLETCHER_CONFORMANCE_XRCE=OFF \
      && cmake --build --preset conan-release \
      && ctest --preset conan-release --output-on-failure \
           -R 'ProviderConformance\.SchemaBeforeDataAcrossHandoff')
```

In `full_suite_cmd`, add `pubsub-conformance` to the existing `for H in ...` list —
its configure/build/test shape is already identical — and delete the "Plus
PDA-DEC-1's conformance harness once it lands" comment. Nothing is added to
`known_accepted_failures`: the harness has no accepted skips, and the existing
"XRCE tests … skip without one" line does **not** extend to it.

## Corner cases forbidden

Rung 1 — **unrepresentable**:

1. **A clause publishing locally on a cross-process subject** — `ProviderSubject`
   exposes no `PubSubProvider&`, no `Publish`, no `CreateTopic`; publisher-side ops
   are the two reply-returning methods that may be remote.
2. **Partial backlog delivery** — clause 6 is all-or-nothing, so no trait value
   admits "some rows lost". The design's central move: every alternative shape had
   to invent a per-provider expected count, which is a pinned divergence.
3. **A third retention or schema-mode value** — both enums are two-valued; widening
   one is a §7 change, not a test-local escape hatch.
4. **A silently skipped clause** — no `GTEST_SKIP` in the harness; the one gate is
   applied at instantiation, so absence shows in the ctest list.
5. **Remote observation** — no `subscribe` verb in the peer protocol.
6. **Wire-format assertions** — no codec and no Arrow C++ is linked; rows are opaque
   bytes, so decision 13 cannot be violated from inside the suite.
7. **Any ABI surface** — the existing C++ seam only: no `extern "C"`, no C header,
   no `dlopen`, no vtable, no version negotiation.

Rung 2 — **refused typed at the door**:

8. **Missing MicroXRCEAgent** ⇒ subject fails with the path in the message; no skip,
   no degraded subject.
9. **Peer child crash, hang, or EOF** ⇒ the request's deadline expires and returns
   a typed failure that fails the clause. No retry, no reconnect, no partial mode.
10. **Assertions on a specific exception type** — the seam's taxonomy does not exist
    until PDA-DEC-3/9; clause 8 asserts *that* the call failed, never which type.

Handled residue, each with why it could not be forbidden:

- **Asynchronous arrival** (DDS discovery, late schema). Handled by one bounded
  predicate wait. *Why not forbidden:* §7 clause 5 makes asynchrony the contract, so
  waiting is the behaviour under test. One deadline constant, no sleeps.
- **Schema-less delivery** (null schema throughout). Handled as a second
  instantiation. *Why not forbidden:* §7 clause 1 explicitly sanctions it ("passes
  null throughout instead"), so it is contract, not an edge state.

## Premises and stop conditions

1. `InProcessProvider` can be lifted out of `gateway/src/main.cpp` mechanically into
   `pubsub/`. **STOP-AND-ASK if it depends on gateway-internal types** — do not copy
   it into the harness; ask whether PDA-DEC-5 should run first.
2. Fast DDS's shipped defaults replay the whole retained backlog to a cross-process
   late joiner (the merge's fix, pinned in-process by
   `DefaultQosReplaysEveryRetainedRowToALateJoiner`). If cross-process shows partial
   delivery, that is the defect resurfacing: fix under decision 11 —
   **STOP-AND-ASK if the fix would move wire bytes** (`ParityOracle.EncodeEquals...`
   going red is the trigger).
3. Two XRCE clients in separate processes can share one Agent with distinct session
   keys (`fastdds-xrce-interop` proves it in one process). **STOP-AND-ASK if they
   cannot** — ask whether the XRCE cross-process subject is dropped; do not weaken it
   into an in-process subject wearing the name.
4. No exception taxonomy exists yet. **STOP-AND-ASK if a clause seems to need a
   specific exception type** — do not invent taxonomy here; that is PDA-DEC-3/9.
5. No child-process-with-pipes helper exists in the tree (the interop suite spawns
   fire-and-forget; `gateway-fastdds-ts` pipes from Node). **STOP-AND-ASK if the
   helper exceeds ~250 lines or is unstable on MSVC** — propose splitting the
   cross-process subject into its own item rather than shipping it degraded.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `ProviderConformance.SchemaBeforeDataAcrossHandoff` (all 6 subjects) | clause 1 in `src/clauses.cpp` + the six instantiations | Today it cannot even be named: no provider-agnostic harness and no cross-process subject exist. After the harness lands but before divergence fixes, it fails on whichever subject violates the handoff, and the ctest name says which. |
| §7 clause set (clauses 2–11, all subjects) | one clause each, table above | Each is red on any provider that diverges from §7; failure names clause **and** subject, which is the divergence list. |
| `LateJoinerBacklogIsAllOrNothing` on `FastDdsCrossProcess` | clause 6 + the peer publisher | Proven red-capable by the falsification procedure above (revert the reader-side `data_sharing().off()`); the in-process subject stays green, which is precisely §7.2's point. |

Machine checks that prove the rest for free: the compiler proves the clause library
links against no provider; `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` proves
no divergence fix moved wire bytes; the ctest list proves no clause vanished.

## Risks / Unknowns

- **Item size is not knowable** (decision 11, accepted by ruling). Sanctioned split
  shape: a single divergence fix that is individually large becomes PDA-DEC-1b…n,
  inserted after this item; **PDA-DEC-1 does not close until they are green**, and
  no clause is ever disabled, skipped or trait-declared to close it.
- **Plan-shape finding for the PM.** Lifting `InProcessProvider` into `pubsub/` is
  scheduled in PDA-DEC-5, but this item needs it *now* (it is unlinkable in an
  anonymous namespace in `main.cpp`). Doing the mechanical move here and leaving
  registration in PDA-DEC-5 means **no coexistence window and no duplicate class**;
  the alternative — a harness-local copy — is a bridge PDA-DEC-5 would delete.
  Recommend reducing PDA-DEC-5 to "register it + gateway `--provider` is a lookup".
- **§7 clause 3 says rejection "may" happen**, so the three providers could
  legitimately diverge there while decision 11 forbids pinning divergence. Clause 8
  asserts uniform rejection — permitted for each provider individually by "may" —
  and PDA-DEC-9 owes §7 clause 3 a tightening to "must". Escalated as brief decision
  1; not a contradiction of the spec, so not a STOP-AND-ASK.
- **Agent superbuild cost** (~10–15 min, first configure only) now sits in
  `full_suite_cmd`; mitigated by sharing `fastdds-xrce-interop`'s install dir.
- **Cross-process flake would destroy a guard's value.** Mitigations above; per the
  round config, intermittent loss in clause 6 is the defect signature, never flake.
- **Size:** ~1700 added lines is large for a guard, and justified: greenfield twice
  over, clause set written once rather than three times. No split proposed for the
  harness; the split axis, if needed, is the divergence fixes.
- No coexistence window, bridge, shim or re-export is introduced by this item.

## Files-to-touch

New, under `integration-tests/pubsub-conformance/`: `conanfile.py`,
`CMakeLists.txt`, `README.md`, `include/fletcher/conformance/subject.hpp`,
`include/fletcher/conformance/fixtures.hpp` (nanoarrow schemas A/B, row
encode/decode, the deadline wait), `src/clauses.cpp`, `src/local_subject.cpp`,
`src/peer_subject.cpp`, `src/child_process.{hpp,cpp}`, `src/peer_main.cpp`,
`subjects/{inprocess,fastdds,xrce}_main.cpp`, `subjects/{fastdds,xrce}_peer_main.cpp`.

New elsewhere: `pubsub/include/fletcher/pubsub/in_process_provider.hpp`,
`pubsub/src/in_process_provider.cpp`. Changed: `pubsub/CMakeLists.txt`,
`gateway/src/main.cpp` (drop the local class, include the new header),
`.claude/runbook.PDA-DEC.config.md` (the two commands above), plus whichever
provider sources decision 11's fixes land in (disclosed at first full run).

## Files-to-delete

- `InProcessProvider` in `gateway/src/main.cpp` (~57 lines, anonymous namespace) —
  replaced by `fletcher::InProcessPubSubProvider` in `pubsub/`.
- `DefaultQosReplaysEveryRetainedRowToALateJoiner` in `fastdds-pubsub-provider/tests/`
  — replaced by clause 6 on `FastDdsLocal` (same property) **and**
  `FastDdsCrossProcess` (the direction it could not see).
- No config keys, shims or re-exports retired; there are none in this area.

## Numbers

Declared net lines: **+1700 / −70**, excluding decision-11 divergence fixes (not
knowable up front, by ruling). New public surface: **3** — `ProviderSubject`,
`SubjectTraits`, `InProcessPubSubProvider`; the last is paid for by retiring the
gateway-local `InProcessProvider`. `FLETCHER_CONFORMANCE_XRCE` is harness build
configuration, not seam surface.
