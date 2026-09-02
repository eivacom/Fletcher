# PDA-DEC-8 — architecture review, cycle 1 of 2

Reviewed at `5f886c4`: `plans/PDA-DEC-8-multi-instance.md` (292 lines) and
`plans/PDA-DEC-8-brief.md` (60 lines), against `docs/pubsub-interface-spec.md` §4,
the PDA-DEC rulings ledger, `plans/PDA-decouple-locked-decisions.md` and
`plans/PDA-decouple-interface.md` §PDA-DEC-8.

**Verdict: NEEDS-REWORK — 3 BLOCKERs, 6 DEBT.** No ruling or locked-decision
deviation; no STOP-AND-ASK. All three blockers are the same class: **the design
assumes provider behaviour it did not check against the code**, and in two places
the tree behaves the other way, which would ship a guard that cannot fail (B1) and
a forcing test that is red on the unmutated tree (B2).

The oracle citation checks out. §4's third normative list item reads verbatim
"**No global state.** The registry takes and returns explicit objects; multiple
instances of the same provider with different configs must be ordinary."
(`docs/pubsub-interface-spec.md:359-362`). The literal string "clause 3" is absent
from the list itself because §4 uses a bare numbered list — but §4 clause 2's own
prose says "**Clause 3** sanctions destroying a registry while its providers run"
(`:335`), so the spec itself numbers these items as clauses. The plan's and the
design's "§4 clause 3" citation is accurate.

---

## BLOCKER 1 — The two instances cannot discover each other on *any* domain, so M1 cannot redden and the positive control cannot cross

`pubsub/include/fletcher/pubsub/payload_bound.hpp:60-62`, the tree's own words:

> The registered DDS type name for a bound, and **the only thing keeping two
> bounds apart: DDS matches by type name, so endpoints on different bounds fail to
> discover each other** rather than exchanging samples one of them cannot hold.

The name is `fletcher_<bound>` (`FletcherTypeName`, set in
`fastdds-pubsub-provider/src/internal/fletcher_sample_pub_sub_type.hpp:47`,
registered at `fast_dds_pubsub_provider.cpp:232`, pinned by
`FastDDSPubSubProviderTest.TheTypeNameCarriesTheBoundForEveryProvider`). Design §2
gives instance A `max_payload_bytes=4096` and B `65536`, so their data endpoints
are `fletcher_4096` and `fletcher_65536` and **never match, whatever the domain**.

Two consequences, both fatal to the gate:

1. **M1 — the table's headline row — cannot produce its named failure.** With
   `create_participant(0, …)` both instances sit on domain 0, but the data
   endpoints still do not match, so "B's markers appear in A's journal" is
   unreachable. The only channel that *does* match cross-instance on a shared
   domain is the `__schema` companion topic, whose type name is a constant
   (`internal/raw_bytes_pub_sub_type.hpp:28`) and carries no bound — so M1's only
   route to red is a race over which announcement each subscription latches
   first (schemas are latched at first delivery, 2026-09-01 ruling). That is a
   flaky red, or no red at all when each subscription happens to see its own
   announcement first. M2 is the same story.
2. **The standing positive control is broken either way.**
   `Registry.TwoInstancesOneDomainDoInterfere` "runs the same helper … the only
   difference being that both instances sit on domain 156" (§3). If it really
   shares the isolation case's bounds it can never observe a crossing and is red
   on the unmutated tree. If the implementer notices and quietly equalises the
   bounds in the control only, then the control **no longer shares the
   arrangement**, and rung-1 forbidden case 2 — "the control shares the
   arrangement helper, so 'the streams could never have crossed' reddens the
   control in the same binary" — becomes false: the isolation case would have an
   extra, wire-visible reason it can never interfere that the control cannot
   detect. The forcing test then passes for exactly the absence-of-opportunity
   reason this item exists to rule out.

The design states P1 (matching is domain-scoped) but not the premise it actually
rests on: that **every other discovery key — topic name *and* registered type
name — is equal**, so the domain is the only wire-visible difference. That is the
unstated premise.

**Acceptable fix (cheapest, and forbidding beats handling here):** hold
`max_payload_bytes` **equal in both instances everywhere a crossing is asserted or
denied** — the isolation case, the control, the concurrent case — so the domain is
the only discovery-key difference; move the per-instance-bound witness into its
own instance pair on its own two domains, where no crossing is claimed in either
direction. Add the premise ("the bound is part of the registered type name, locked
decision 13, so unequal bounds are an independent reason for non-matching") with
its stop condition. Do not try to *handle* the confound inside the assertions —
remove it from the arrangement. Spec coverage is unharmed: `domain_id` is half the
typed core, so two instances differing only in domain are already "different
configs", and the bound axis keeps its own case.

## BLOCKER 2 — An oversized row is **not** refused on the publish flow this design configures; §2's assertion 3 is red on the unmutated tree

§2 asserts "An 8 KB row is **refused `kPayloadTooLarge` by A** and delivered by B
on the same call — a zero-timing, deterministic witness". With an **empty
document** `fletcher.loan_publish` is false, so the provider installs
`internal::SampleWriter` (`fast_dds_pubsub_provider.cpp:220-224`), and on that
flow the overflow is caught inside `serialize()` and **deliberately not recorded**:

- `internal/fletcher_sample_pub_sub_type.hpp:106-116` — "on this path it is
  dropped and logged rather than surfaced … **Deliberately NOT recorded, so
  Publish does not throw for it.**"
- `internal/sample_writer.hpp:59, 80-84` — "An oversized row fails under
  `write()` rather than in front of it, so it cannot throw here … an OVERSIZED row
  throws `kPayloadTooLarge` on the **loaned** flow … and is dropped-and-logged
  here."
- Pinned both ways by `FastDDSPubSubProviderTest.DataSharingOversizedRowDoesNotThrow`
  (`EXPECT_NO_THROW`, `test_fast_dds_pubsub_provider.cpp:590-600`) and
  `LoanedOversizedRowThrowsWithoutLeakingLoans` (`:637-660`).

So A's 8 KB publish returns normally and drops the row. The case is red on the
unmutated tree, and M6's named must-redden ("B's 8 KB row is refused too") rides
on an exception that never arrives. The obvious field repair — weaken to "A's row
never arrives" — silently converts the design's one zero-timing witness into a
bounded absence. `PayloadBytes()` is not an escape either: it is a Fast DDS
extension, not on `PubSubProvider` (`pubsub/include/fletcher/pubsub/provider.hpp`),
so a base-typed handle cannot ask for the bound.

**Acceptable fix (cheapest):** re-anchor the witness on **delivery**, not refusal —
a row between the two bounds is *delivered on the high-bound instance* (the
positive assertion, and the one that reddens under M6) and absent on the low-bound
instance within `kSettle`; say plainly in §2 and in the README that the low-bound
instance **drops silently**, which is the pre-existing serialising-flow behaviour.
If a typed refusal is wanted instead, both instances must carry
`fletcher.loan_publish=true` in their (still identical) participant profile
document — that is the only configuration in which `Publish` throws
`kPayloadTooLarge` — and the design must say so.

## BLOCKER 3 — The mutation gate has no clean-environment precondition, and two of its six rows plausibly kill the process

The item's entire deliverable is six recorded reds. M2 (a shared function-local
`static` participant) and M3 (a file-scope `static` topics map) make the second
`~Impl` delete endpoints through a publisher/subscriber whose participant the
first `~Impl` already destroyed (`fast_dds_pubsub_provider.cpp:151-170`) — a
use-after-free, i.e. a killed process. A killed Fast DDS process orphans an shm
segment in `C:\ProgramData\eprosima\fastdds_interprocess`, and a stale segment
makes the *next* run's `create_participant` fail with a **false `0xC0000005`** —
the failure that has already cost this round a review cycle. The next row is then
recorded red without its guard having run at all: a red that proves nothing, which
is the same defect class as a green that cannot fail. §7's "this design adds **no
new path** by which a killed process orphans a segment" is true of the shipped
tree and false of the gate the evidence comes from.

**Acceptable fix (cheapest, three lines in §6):** state the per-row procedure —
the three cases are observed **green on the unmutated build immediately before
each mutation** (that is the environment check; no separate probe is needed), the
mutated run's failure text is recorded verbatim, and after any run that *crashes*
rather than fails, `C:\ProgramData\eprosima\fastdds_interprocess` is cleared
before the next row.

---

## Row-by-row on the mutation table (the question I was asked to answer)

| Row | Would it redden? | Through the stated mechanism? |
|---|---|---|
| M1 domain→0 | **No** on the row assertions (B1); at best a flaky schema-channel race | No |
| M2 static participant | Yes — but by `register_type`/`create_topic` refusal or a teardown crash | No ("as M1" is unreachable, B1) |
| M3 static topic map | Yes, deterministically — B's `CreateTopic` hits A's `schema_writer` with different IPC bytes → `kSchemaConflict`; or B's `Subscribe` hits A's reader → "already subscribed" | Not as stated ("isolation on the shared topic") |
| M4 memoising `Create` | Yes, deterministically — one instance, so `kSchemaConflict` on the second declaration and "already subscribed" on the second subscribe | Broadly yes |
| M5 appending `JoinSegmentsInto` | Yes — but at **A's own second publish** (`pdadec8/sharedpdadec8/only-a` → `kTopicNotDeclared`), not at B's lookup; and already caught elsewhere in the tree (DEBT-1) | No |
| M6 static payload bound | Yes — but only via a missing delivered marker, not the refusal §2 names (B2) | No |

After B1 and B2's fixes, M1 reddens deterministically by markers crossing (topic
name and type name then both match on domain 0), and M6 reddens by the high-bound
instance's marker going missing. That is what makes the table worth having.

## The rest of what I was asked to judge

- **Is the contention real?** Yes, and better than it reads: `Publish` holds the
  mutex *shared*, and spec §6 clause 2 explicitly sanctions concurrent delivery
  "including across provider instances", so the concurrent case is testing a
  sanctioned path rather than an invented one. The `static thread_local` at
  `fast_dds_pubsub_provider.cpp:382` really is the provider's only `thread_local`
  and `JoinSegmentsInto`'s only caller — but the claim that the alternating
  sequence is "the only arrangement that reaches" it is false (DEBT-1).
- **The positive control can fail, and cannot rot into flakiness.** Default writer
  and reader QoS is RELIABLE + KEEP_ALL + TRANSIENT_LOCAL
  (`fastdds-pubsub-provider/src/qos_defaults.cpp:23-42`), so a late-matching
  reader still gets the backlog: the control fails for the reasons §3 names
  (names drift, no subscription, window collapse) and not for ordinary discovery
  jitter. And because one `kSettle` serves both cases, the only pressure on that
  number is toward *widening*, which strengthens the isolation claim rather than
  weakening it. This part of the design is right.
- **Placement.** Confirmed and correct. `conformance_registry`'s link line is
  `fletcher-pubsub::fletcher-pubsub` + gtest and nothing else
  (`integration-tests/pubsub-conformance/CMakeLists.txt:209-213`), which is the
  guard; `conformance_fastdds` already links the provider (`:221-229`), already
  carries `RESOURCE_LOCK conformance_fastdds` and `TIMEOUT 180` (`:296-297`) and
  is discovered per case. Spec §4 clause 4 already records this precedent for both
  `ResolvesAsABuiltIn` cases. No SDK is smuggled anywhere.
- **Domains.** Census verified: the tree uses 0, 7, 43, 91–99, 137, 145, 151–153
  and nothing in 154–158. P3 verified — `gtest_discover_tests … PROPERTIES
  RESOURCE_LOCK` applies the lock per discovered entry, so no two of this binary's
  cases run at once. The design does not repeat `pubsub-arrow-fastdds`'s mistake:
  it shares topic names deliberately and therefore owns its domains outright.
- **P2's evidence** is accurate: `test_profile_document.cpp:551-552` stands up two
  providers in one process.
- **Deletion / end-state.** `Files-to-delete: None` is real and justified — this is
  a proof item, there is no bridge, shim, dual path or superseded test, and the two
  retirement candidates are correctly kept. No coexistence window, nothing
  scheduled for later deletion. Nothing to say.
- **Budget.** Design 292 ≤ 300; brief 60 ≤ 60 with nothing essential squeezed out.
  **New public surface 0, verified**: no new header, no new type or method, no new
  CMake target, and the arrangement helper is file-local to one test TU. Suite
  arithmetic verified and *not* conflated: 81 discovered entries across the six
  `build/*_tests-Release.cmake` files plus one `add_test(conformance_xrce)` = 82
  entries today; the three cases land in a `gtest_discover_tests` target, so +3
  cases = +3 entries = 85. Declared +460/−0 is optimistic once B1 and B2 land
  (a fourth arrangement, or a participant-profile document): expect **~+540**.
  I am not asking for guards beyond those two fixes.

## The Brief's three "decisions for you"

1. **Same-domain overlap sanctioned vs refused — STRIKE, already decided.** The
   acceptance oracle's own first sentence is "No global state. The registry takes
   and returns explicit objects". Option (b) cannot be implemented without a
   process-wide list of domains in use — precisely the state §4 clause 3 forbids
   and this item exists to disprove. There is no live alternative to put to the
   owner. Record as decided by spec §4 clause 3, with the brief's own reasoning as
   the rationale.
2. **Which protocols carry the claim — STRIKE, but not on the "All three"
   ruling.** The PM's doubt is reasonable but that ruling does not reach this: its
   own *Applies-to* is scoped to **migration** ("all three providers are
   migrated — in PDA-DEC to the registry and document config"), and carrying an
   isolation *proof* is not migration. What settles it is oracle 4:
   `plans/PDA-decouple-interface.md:226-229` states the story as "two instances of
   the same provider, **on two DDS domains**, in one process, through the
   registry" and names this forcing test. The other options are empty anyway —
   `pubsub/src/in_process_provider.cpp` holds no `static` and no `thread_local`, so
   two loopback instances have no cross-instance path to measure (and
   `Registry.EachCreateReturnsAnIndependentInstance` already pins independence
   there), and a second XRCE instance measures the Agent. Record as decided by the
   plan, one line, with the loopback's absence of cross-instance state as evidence.
3. **How wide the published claim is — KEEP.** This is the only one that is
   genuinely the owner's: it spends money (a cross-host harness) and it fixes what
   we publish. Two caveats for the PM. Option (b) is close to a straw man by the
   brief's own argument, and the owner has already chosen this shape twice
   (2026-09-01 "Scope to the interface, say so plainly"; 2026-09-01 "Ship the
   guard, hunt elsewhere" — blind spot into the README), so the honest framing is
   "confirm the standing pattern applies here" rather than an open choice. If it is
   asked, it must carry DEBT-4's third exclusion, because that changes what (a)
   actually claims.

## DEBT (6) — appended to `plans/reviews/design-debt.md`, handed to the implementer

1. §2's claim that the alternating sequence "is the only arrangement that reaches
   `Publish`'s `static thread_local`" is false, and M5's stated mechanism is wrong.
2. M2/M3/M4's "Must redden" cells predict crossings; they will actually redden by
   typed refusal at declaration/subscription time.
3. M5 mutates a header every provider uses; the whole tree goes red, and the
   recorded evidence must name *these* cases' failure.
4. §8's published claim should name intra-process delivery (locked decision 12) as
   a third exclusion.
5. The concurrent case must join both threads before either provider is destroyed
   (§6 clause 5).
6. `+460` is optimistic once B1/B2 land; ~`+540`.

## Nothing found against

Rulings ledger: no contradiction. The design builds no ABI, no loader and no
bridge; it changes no `PubSubProvider` method, adds no copy on either path, and
keeps `ProviderConfig`'s typed core at two fields. Locked decisions 3 and 14 are
cited correctly and honoured; 12 is honoured in substance (the suite's
cross-process DDS subject already exists) and needs only DEBT-4's disclosure.
Rung-1 forbidden case 5 verified: `ProviderRegistry` has no static member, no free
function with storage, and `Create` is `const`
(`pubsub/include/fletcher/pubsub/provider_registry.hpp:277-287`). No
hand-composed post-change ledger: the domain census is a pre-change tree fact and
it is accurate; the suite counts are arithmetic and they are right.
