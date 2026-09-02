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
  | `InProcessCarrying` | in-process loopback, `document = "schema_carriage=carried"` | this process | carried | drops |
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
  clause 1 sanctions the same transport being exercised in both modes, and the
  loopback now is — `InProcessLocal` and `InProcessCarrying` are one provider in
  its two modes, chosen (PDA-DEC-5) by the `schema_carriage` document key
  rather than a construction-time argument — there is no second constructor.
- **One binary per schema mode.** The two loopback subjects live in separate
  binaries (`subjects/inprocess_main.cpp`, `subjects/inprocess_carrying_main.cpp`)
  because clause 2's gate is the link line and `INSTANTIATE_TEST_SUITE_P`
  registers *every* clause in a binary against *every* subject in it. Two schema
  modes in one binary would run clause 2 against the schema-less one — present
  and failing, where the design says it should be absent. See "Clause 2 and the
  axis gate".
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
A **third** suite, `SeamVocabulary`, is described at the bottom of this section.

It decides, by **address provenance**, whether the payload bytes a subscriber
sees are the very bytes the publisher wrote. **A copy** is payload bytes coming
to exist at a second address, or moving to one, **by the provider** (and by the
thin `Publisher`/`Subscriber` layer, where a subject routes through it), between
the encoder's first write and the callback's return. Not a copy: the encode
itself; anything a transport does once the bytes leave the seam; a window
refill, below.

Four addresses per publish, sampled while their storage is live and compared as
integers. No counters, no allocator hooks, no sanitizer: counting catches only
allocation-shaped copies, misses copies into a pooled or reused buffer, and on
Windows does not interpose allocations inside a provider DLL carrying its own
CRT — blind exactly where a loaded driver will live. A copy into recycled
storage still lands at a different address than the *live* encode window.

| Entry | What it is |
|---|---|
| `PublishAndReceivePerformNoPayloadCopies` × 3 subjects | the forcing test: row at 64 B and 4 KiB, plus two 1 KiB attachments |
| `StagingIsCaught` | the **live negative control** — a deliberately-copying provider the same `Judge()` must score at `row_copies == 1`, `attachment_copies == 2` |
| `BorrowedAttachmentCostsNoCopies` | a provider's own borrowed memory, **pinned at zero** — it was `…CostsExactlyOneCopy`, pinned at one, until PDA-DEC-3 removed the copy |
| `RefillMovementIsCountedNotFailed` | the refill counter is live: non-zero on a growable window, zero on a fixed one |
| `JudgeArithmeticIsSound` | the pure verdict function, without a provider |

Subjects: `SeamProbe` (a fixed-arena provider in this harness — the positive
control, proving the seam *permits* zero-copy), `InProcessLoopback` (the real
`InProcessPubSubProvider` at the seam) and `InProcessViaPubSub` (the same
provider through `Publisher`/`Subscriber`, so the layers *above* the seam are
measured too). All same-process by construction: an address means nothing across
an address space, so a cross-process or off-thread subject cannot be built here
at all, and the ledger is unsynchronised to keep it that way.

**Refill is permitted and its cost is published, not failed** — §3.1 clause 1
allows bytes to move "inside a refill, which must preserve them verbatim", and
the owner's 2026-09-01 ruling permits it on condition the number is reported.
Every *other* byte movement fails the guard. The number is on stdout and in the
JUnit XML; today `InProcessLoopback` relocates 3 times / 5632 bytes writing a
4 KiB row in 64-byte appends, and `SeamProbe` nothing. The counter's own liveness
is pinned against a **harness-owned** growable window, never against a provider:
pre-sizing a provider's send buffer is an improvement and must not turn a test on
the instrument red.

### `BorrowedAttachmentCostsNoCopies` — what it does and does not pin

It is **not** a receive-side transport measurement; nothing here measures a
transport. It measures a *provider* that already holds payload bytes in memory it
owns — a stand-in for a transport-loaned sample — and must produce a `Blob` for
them inside `Publish`. A caller-owned blob rides the same publish and must cross
untouched, so the total is three-valued and moves with provider behaviour both
ways: **0** today, **1** the seam has lost the ability to carry borrowed memory,
**2** a provider copied bytes it was handed already shared. The suite proves that
standing, by running the identical leg against the deep-copying probe and
requiring 2.

**This pin was 1, and the change from 1 to 0 is the point.** The owner's
2026-09-01 ruling pinned the §3.2 copy at exactly one *so that removing it would
turn this test red* — silence being how such a fix gets forgotten or half-landed.
PDA-DEC-3 removed it: `Blob` stopped being a `shared_ptr` to a `vector` and became
an owner plus a span, so a provider hands over bytes it already holds where they
lie. The tripwire fired exactly as designed — the `static_assert` in
`copy_accounting.hpp` stopped the **build** before any test ran — and the
assertions here were updated deliberately, in the same change, with the rename
visible in `ctest -N`.

The residual the old text feared ("a PDA-DEC-3 that leaves `Blob` untouched and
adds a *parallel* borrowed-blob type trips neither") did not happen and now
cannot: the `static_assert`s in `copy_accounting.hpp` are the **inverse** of the
old one — they fail the build if `Blob` goes back to the retired shape, if it
loses its owner-plus-span constructor, or if a quiet conversion from the retired
`shared_ptr` alias is bolted on. That last one is what would have re-created the
coexistence window.

**Zero is still a claim about the SEAM's capability, never about a transport.**
It says the interface can carry memory it does not own without copying it. It
says nothing about any transport's receive path, and two copies the tree still
makes are in plain sight: `Envelope::row` is a `std::vector` copy on every XRCE
and gateway receive, and Fast DDS's *loanable* read path materialises one owning
copy per sample **that carries attachments** (down from one per attachment; an
attachment-free sample still crosses with no copy at all), because the loan is
returned when `Take` returns and a buffered pre-schema backlog can outlive it.
§8/§11 assign that last one to the zero-copy-receive stage by name.

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

### The premises a new subject must satisfy

PDA-ABI is expected to register a driver-backed subject into this same shape.
Two preconditions, and a subject that breaks either must not be registered:

- **P2 — synchronous delivery on the publishing thread.** The ledger has no lock
  because it needs none, and the delivery count is asserted `== 1` before any
  verdict is read, so "nothing arrived" can never read as "no copies".
- **P5 — encode-window liveness.** The bytes `encode_base` names must stay
  allocated and unfreed until the callback returns. That is what makes provenance
  immune to allocator reuse: a live allocation cannot be handed out twice, so a
  same-address copy needs free-then-allocate — and a freed encode window handed
  to a callback is a use-after-free, a worse bug than a copy. **Checked, not
  merely stated:** every leg asserts the window still held the row when the
  callback ran, so a recycled window that came back holding something else fails
  as *itself*. Not airtight — a window freed and immediately re-handed the same
  bytes at the same address is indistinguishable — which is why the precondition
  still binds.

## Clause 2 and the axis gate

`CallbackNeverSeesNullSchema` asserts a property of schema-*carrying*
transports. Its gate is the **link line**: `src/clauses_carried.cpp` is compiled
into the carrying subjects' binaries only. So on a schema-less subject the clause
is *absent from the ctest list* rather than present and skipped, and
`GTEST_SKIP` appears nowhere in this suite. Absence is visible; a skip is not.

This is why the two loopback subjects are in two binaries rather than two
`INSTANTIATE_TEST_SUITE_P` lines in one. A gtest instantiation registers every
`ProviderConformance` clause **in that binary** against its subject, so a single
binary holding both modes would put clause 2 on the schema-less one. One binary
per schema mode is what keeps "absent, not skipped" true.

The mirror property for a schema-less transport — null throughout, never mixed —
is clause 3, which runs on every subject.

Delivery clauses declare their topic with `DataSchema()`, which is no schema at
all on a schema-less subject: handing the loopback a schema and then asserting it
does not carry one would be a contradiction, not a test. Clauses 7 and 8 are the
exception — they observe only the *reply* to a declaration, so they declare the
real A and B on every subject.

## The `SeamVocabulary` suite — the crossing types themselves

Oracle: [docs/pubsub-interface-spec.md](../../docs/pubsub-interface-spec.md) §3.2,
§3.3, §3.4, §5.1, §7 clause 1. A **third** suite in this harness, in its own
binary (`conformance_seam_vocabulary`), seven entries, no provider SDK.

It asserts what the crossing *types* make representable, which no
provider-parameterised clause can reach:

| Entry | What it pins |
|---|---|
| `BorrowedTransportMemoryCrossesWithoutCopy` | §3.2: a provider hands over its own bytes where they lie, and a blob kept past the delivery — **after the provider itself has been destroyed** — still names *and reads back* those bytes, so the owner is real rather than a span with no keeper |
| `AbandonedSubscriptionReportsNoSchemaWillArrive` | §3.4: a subscription torn down before its schema arrives says `kSubscriptionEnded`, distinct from `kOk`+null ("this transport carries no schemas"), and the unbounded wait still returns |
| `BlobRefusesBytesNothingOwns` | §3.2 rung 1: no view-only `Blob` exists to build |
| `ErrorRefusesEveryNonFailureStatus` | §5.1: a failure can never carry `kOk`, `kPending` or `kSubscriptionEnded` |
| `ResolverRefusesNullAndWaitRefusesNegativeTimeout` | §3.4: only `Ready(nullptr)` can produce `kOk`+null; a negative timeout is refused, not silently a poll |
| `LaterDeclarationNeverReachesALiveSubscription` | §7 clause 1 **per subscription**: a declaration made after a subscription exists never reaches it |
| `EmptyTopicSegmentListIsRefusedAtEveryEntryPoint` | §3.5 rung 2: an empty topic names no topic, on all four methods — a new rule *and* a behaviour change (`JoinSegments({})` used to yield the legal topic key `""`) |

**How the ownership half is kept honest.** It was vacuous twice, and both shapes
are worth knowing because they recur:

1. The provider outlived the round trip, so the arena stayed alive whether or not
   the `Blob` owned it. Fixed by destroying the probe *inside* the round trip,
   before the retained blob is read, and asserting (`subject_released`, backed by
   a `weak_ptr`) that it really died — so a keep-alive added later fails loudly
   instead of quietly re-emptying the claim.
2. The content check compared the retained bytes against the *published address*
   — which, when provenance holds, is the same buffer: `memcmp(p, p, n)`, true for
   a dead owner too. Fixed by comparing against harness-owned storage
   (`CopyLedger::retain_expected`). `Arena` also fills its slots with `0xDD` on
   destruction, so "the bytes happened to survive the free" is not a way to pass.

Both were found by mutation, not by reading: giving the delivered blob an owner
unrelated to the arena leaves provenance intact and must fail the ownership half
alone. It now does, at the `retained_content_ok` assertion. Restoring the copy
instead fails the provenance half in both this suite and `CopyAccounting`.

The `LaterDeclaration…` clause is here rather than in `ProviderConformance` for a specific reason,
worth stating because it is the kind of gap that otherwise goes unnoticed: **no
conformance subject reaches that path.** `InProcessLocal` carries subject axis
`kAbsent`, so `CONF_MUST_DECLARE` never hands the loopback a real schema, and
`InProcessCarrying` is schema-carrying from the start. The only caller that
declares a schema on a schema-*less*-mode loopback is the gateway, which has no
subject. The forcing test borrows `CopyAccounting`'s instrument outright rather
than growing a second one, so this harness still has exactly one scoring path.

## The `Registry` suite — selection, not transport

Oracle: [docs/pubsub-interface-spec.md](../../docs/pubsub-interface-spec.md) §4,
§4.1, §4.2. A **fourth** suite in this harness, in its own binary
(`conformance_registry`), 19 entries, no provider SDK — and, for all but the
five PDA-DEC-5 entries below, **no provider at all**: every OTHER provider in
it is a probe defined in the test file, so those constructs nothing that opens
a socket, binds a port or joins a domain.

**What green here means, precisely.** A string read at run time decides which
provider a caller gets, the caller cannot tell which it got, and the same one
call serves a built-in name and a driver path. **It claims nothing whatsoever
about any transport** — not delivery, not ordering, not QoS, not zero-copy. The
probes deliver a row synchronously into a journal because that is the only way to
tell *which factory the registry reached*; the row is an instrument, not a
delivery claim.

| Entry | What it pins |
|---|---|
| `SelectsByNameWithoutCallerKnowingTheProvider` (forcing) | §4: two names, two providers, and the row surfaces under the tag the *name* maps to — **both directions**, so a `Create` that ignores the selector and returns its first factory is red |
| `PathSelectorResolvesThroughTheSameCall` | §4 clause 2, made executable: a stand-in resolver stands where PDA-ABI's loader will, and a path reaches it through the **identical** caller helper with only the config string differing |
| `PathSelectorWithoutResolverIsRefusedAsUnsupported` | the live negative control for the row above, plus the 2026-09-02 ruling's distinctness: a path with no resolver is `kNotSupported`, an unknown name is `kInvalidArgument`, and the message says which character made the string a path |
| `UnknownNameIsRefusedWithTheAvailableNames` | a refusal names what *is* registered |
| `DuplicateRegistrationIsRefused` / `SecondPathResolverIsRefusedAndTheFirstStillStands` | neither what a name means nor what every path resolves through may be silently swapped |
| `EachCreateReturnsAnIndependentInstance` / `ProvidersOutliveTheRegistryThatMadeThem` | §4 clause 3: no cache, no global, and a registry may be destroyed while its providers run |
| `SelectorShapeDecidesAndIsRefusedWhenItCannotMeanAnything` | the classification rule is total, disjoint and registry-independent; empty and embedded-NUL selectors are refused |
| `RegistrationAndSelectionShareOneVocabulary` | one predicate for both, so a name cannot be registrable but unselectable |
| `ConfigurationReachesTheProviderAndIsNeverRead` | §4.2: the document arrives byte-for-byte, NUL included, and Fletcher never parses it |
| `AFactoryThatFailsIsReportedAsATypedSeamFailure` | §5.1 at this entry point, including a factory that returns null |
| `AResolverThatFailsIsReportedAsATypedSeamFailure` | §5.1 on the **resolver** seat — the branch PDA-ABI fills. Both shapes: a resolver that throws, and one that returns null |
| `AModuleHeldOnlyByTheSeamOutlivesTheProvidersItMade` | the lifetime rule is **mechanical, not prose**: `Create` returns an aliasing handle owning `Anchor{seat, provider}`, so a factory's or resolver's module outlives every provider it made, whatever the author did. Pinned on **both** seats, and the probe records module-still-loaded *in its own destructor* — so the load-bearing member order is itself asserted |
| `InProcessResolvesAsABuiltIn` (forcing, PDA-DEC-5) | §4 clause 4: `RegisterInProcessProvider` makes `"inprocess"` selectable; a row published through the **base-typed** `shared_ptr<PubSubProvider>` handle arrives byte-identical, and a publish to a **second, never-declared** topic succeeds — pinning the default `as_declared` mode without an accessor for it |
| `InProcessCarriageComesFromTheDocument` | the live control for the row above: the identical registry call, `document = "schema_carriage=carried"`, and the opposite behaviour — publish-before-declare is refused `kTopicNotDeclared`, and a declared topic's delivery carries a non-null schema. Neither test alone proves the mode comes from the document |
| `InProcessRefusesAnUnrecognisedDocumentEntry` | rung-2 case 6: seven refusals — an unrecognised value, an unrecognised key, an entry with no `=`, a duplicate key, an empty value, and leading/trailing whitespace on key and value — all `kInvalidArgument` quoting the offending entry, never "threw something" and never a silently-defaulted typo |
| `InProcessDocumentToleratesCrlfAndBlankLines` | the two tolerances the reader *adds* are the ones that had no guard: a trailing `\r` is stripped (so a CRLF document means the same on every platform — the 2026-09-02 ruling) and a blank entry is skipped. Covers CRLF, a leading blank line, an interior blank line, a trailing newline, and all composed |
| `InProcessRefusesADocumentContainingANul` | a document carrying an embedded NUL is refused at the door with its offset, mirroring `ProviderSelector::Parse`. This is a **provider-format** rule, not a seam one — the seam's document stays length-authoritative and carries a NUL unchanged (§4.2). It is the suite's only guard against a NUL-truncating boundary |

**The link line is a machine check.** `conformance_registry` names
`fletcher-pubsub` and no transport SDK, so no DDS or XRCE vocabulary resolves
from it whatever the registry's implementation does. (Precisely "no transport
SDK", not "no provider header" — `in_process_provider.hpp` lives *inside*
`fletcher-pubsub`, and PDA-DEC-5 links this very binary against it.) The frozen
`Create` signature is a second machine check: the `static_assert` is in the
header, so this binary cannot build against a widened one.

**Mutation evidence** (every one applied singly against the whole suite, and
each re-derived independently by a reviewer from a separate tree).
*Selection:* swapping the two registrations, making `Create` return its first
factory, and making the probe stop recording each redden the forcing test — and
the first-factory mutation reddens it **only at the second direction**, which is
why both are asserted; one direction alone would have passed while the registry
ignored the name entirely. *Path:* classifying every selector as a name, and a
resolver that ignores its path argument, each redden
`PathSelectorResolvesThroughTheSameCall`; forwarding a default-constructed
config there reddens it on `max_payload_bytes`; handing back a provider instead
of refusing reddens the negative control **while the positive path stays green**,
which is what makes the control independent rather than a restatement.
*Vocabulary:* dropping `0-9` or `A-Z` from the name predicate reddens
`RegistrationAndSelectionShareOneVocabulary` at offset 4 and offset 0 of
`Fast2DDS-v1_x`. *Failure seats:* deleting the resolver's `TranslateSeamFailure`
wrapper, and deleting its null check, each redden
`AResolverThatFailsIsReportedAsATypedSeamFailure`. *Lifetime:* dropping either
keepalive reddens `AModuleHeldOnly…`, and so does **swapping `Anchor`'s two
members** — the member order is load-bearing and is pinned, not merely
commented. *Diagnostics:* reverting the backslash escaping reddens the refusal
test alone. A last-wins `Register` and a memoizing `Create` redden one entry each.
*The loopback built-in (PDA-DEC-5):* registering it under any other name reddens
`InProcessResolvesAsABuiltIn` (unknown name); making the registered factory
default to `carried` reddens it at the schema-arrival assertion (a forced
carrying instance answers `Subscribe` `kPending`, never the immediate `kOk`+null
the `as_declared` default gives when nothing was ever declared); dropping the
delivery inside `Publish` reddens it at the delivery assertion instead.
Parsing the `schema_carriage` key but hard-coding `as_declared` leaves the
forcing test green while `InProcessCarriageComesFromTheDocument` goes red — the
pair, not either alone, is what proves the mode comes from the document.
Ignoring an unrecognised document entry instead of refusing it reddens
`InProcessRefusesAnUnrecognisedDocumentEntry`; quoting the wrong entry for the
duplicate-key case (the document's first line rather than the repeated key)
would have passed with a message that only accidentally happened to contain the
right substring, which is why that test's helper takes the expected quote
explicitly rather than deriving it from the document. *(PDA-DEC-5)* Deleting the
document reader's `\r` strip, or its blank-entry skip, each redden
`InProcessDocumentToleratesCrlfAndBlankLines` alone — both were **fully green**
across all 17 entries before those cases existed. Deleting the NUL refusal reddens
`InProcessRefusesADocumentContainingANul` and reproduces the truncation it exists to
prevent: `what()` stops mid-string at the NUL. Memoising one built-in instance per
registry — which passed **19/19 Registry and 80/80 conformance** when found — now
reddens `InProcessCarriageComesFromTheDocument` at the distinct-instance assertion,
which is the property PDA-DEC-8 depends on.

Five of the mutations **in the list above** left the suite fully green when those
entries were first written, and were closed only after review; the four failure-seat and vocabulary
cases were all on the branch PDA-ABI fills, which is where coverage was thinnest
precisely because the code there already worked.

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

Isolation: fixed DDS domains 151/152/153 and Agent UDP ports 2019 (the suite's
own Agent) and 2119 (`ConformanceXrce.AForeignAgentDoesNotSatisfyTheHarness`
only, which deliberately puts two Agents on it), none of which
`fastdds-xrce-interop` (domain 145, port 2018) uses. `RESOURCE_LOCK` is one lock
per binary, not per domain, because ctest properties apply target-wide and
`conformance_fastdds` carries two subjects. The XRCE binary has a **single**
ctest entry (the interop precedent — one UDP port, and per-clause entries would
pay ~24 Agent start/stop cycles).

The Agent guard proves **ownership of the port**, not liveness of a process
(PDA-DEC-1H). Reaching the Agent is not enough and neither is the spawned
Agent still running: a second `MicroXRCEAgent` aimed at a port a leftover
already holds logs `bind error` and exits, but takes ~0.9 s to do it, while the
leftover answers the reachability probe in milliseconds — so a liveness check
was true at the instant it was asked and the suite would certify all 26 cases
against a foreign Agent. `SetUp` therefore requires the OS to record *this
binary's child* as the holder of UDP 2019 (`GetExtendedUdpTable` on Windows,
`/proc/net/udp` plus the child's `/proc/<pid>/fd` on Linux), and refuses with an
operator-actionable message otherwise.

There is **no fallback**, by design. A platform with neither query fails to
**compile** (`#error`), and a query that *fails at runtime* is a **refusal**
naming the OS error — not "unknown, carry on". An earlier revision tolerated
both cases with one stdout `INFO` line, a fall back to bare liveness, and a
**pass**, which re-admitted this guard's own defect through its error path.

**Scope: the proof is taken at bring-up only.** It is a point-in-time snapshot
in `SetUp` and is never re-taken, so a foreign Agent that appears *after* it is
not caught. What makes that acceptable is that a foreign Agent cannot take a
port our child already holds without our child dying first — not that the
window is shut.

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
