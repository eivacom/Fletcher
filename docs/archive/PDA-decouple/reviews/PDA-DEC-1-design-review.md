# PDA-DEC-1 — architecture review, cycle 1/2

Reviewed cold at `720d96a`. Oracles in precedence order: `docs/pubsub-interface-spec.md`
§7/§7.1/§7.2 (+§0.1, §6, §11) → the PDA-DEC rulings ledger → locked decisions 11/12/13 →
`plans/PDA-decouple-interface.md` → `.claude/runbook.PDA-DEC.config.md`.

**Verdict: NEEDS-REWORK — 3 BLOCKERs, 10 DEBT.** One BLOCKER is a **STOP-AND-ASK**
(oracle-wins, §7 clause 3). The design's core shape — one clause TU, subject registration,
publisher-in-a-child-process, all-or-nothing backlog — is right and survives every tree check
I ran. The three BLOCKERs are an over-strong clause, one unstated premise, and one omission
from `Files-to-touch`; none of them asks for a redesign.

---

## What I verified in the tree (claim → result)

| Design claim | Result |
|---|---|
| §7 is 11 clauses, complete | **Complete for all six §7 clauses** — mapping below. Over-encoded at clause 3 (B1). |
| A clause body cannot reach a provider | **Holds.** `ProviderSubject` exposes no provider; `local_subject`/`peer_subject` take an injected `shared_ptr<PubSubProvider>`, the concrete provider is constructed only in `subjects/*_main.cpp`, and a `conformance_clauses` target that links only `fletcher-pubsub`/`fletcher-core` cannot resolve a provider header at all. |
| Clause 6 + cross-process would catch the shipped defect | **Yes.** The defect is *"a cross-process subscriber that joins after the rows were published received only part of the `TRANSIENT_LOCAL` backlog, often just the newest sample, with no error anywhere"* (progress log, merge entry). Clause 6's sequence — peer declares, peer publishes N, parent then subscribes, count pre-subscribe arrivals — is that scenario exactly, and it is also the shape `gateway-fastdds-ts`'s `fastdds_peer` already exercises (publishes 3 rows, *then* prints `READY`). Under `kRetains` a partial count ≠ N fails; under `kDrops` a partial count ≠ 0 fails. |
| The falsification procedure targets something real | **Yes.** `fastdds-pubsub-provider/src/qos_defaults.cpp:68` is a single `qos.data_sharing().off()` on the default **reader** QoS; README §"the one place this provider overrides the policy" corroborates. Reverting it is a one-line, reversible experiment. |
| Partial backlog is not declarable under either trait value | **True as stated.** No trait value accepts a partial count. One residual escape survives — see DEBT-1. |
| `InProcessProvider` is unlinkable where it lives | **True.** `gateway/src/main.cpp:60` opens `namespace {`, the class is lines 72–128, and it is instantiated only at line 189. |
| The lift is mechanical (premise 1) | **True — premise discharged.** Its only dependencies are `fletcher::internal::JoinSegments`, `MakeSharedSchema`, `OwnedSchema::DeepCopy`, `VectorWriteBuffer`, `MakeReadySchemaFuture`. No gateway type. |
| `-R 'ProviderConformance\.SchemaBeforeDataAcrossHandoff'` scopes correctly | **Correct.** `ctest -R` is a substring regex over the full gtest name `<Prefix>/ProviderConformance.<Clause>/0`. |
| MSVC preset split | **Correct** — configure `conan-default`, build/test `conan-release`, matching `full_suite_cmd`'s existing `for H in` shape. |
| The inner loop does not link the whole corpus | **Substantially correct.** `FLETCHER_CONFORMANCE_XRCE=OFF` drops the Agent and the XRCE subjects; what remains is one clause TU plus three small binaries, no binary linking two providers. One real gap: DEBT-2. |
| Agent reuse (`AGENT_PREFIX`/`AGENT_INSTALL_DIR`, hard-fail not skip) | **Workable.** `integration-tests/fastdds-xrce-interop/CMakeLists.txt:40-66` sets `C:/fl-uxa` / `C:/fl-uxa-install` as cache vars, skips the superbuild when a *complete* install is present, and `test_interop.cpp:233` already `FAIL()`s rather than skips when the Agent is unreachable — so "does not inherit the skip" follows an existing precedent rather than inventing one. Ports/domains 2018/145 confirmed, 2019 unused anywhere in the tree. See DEBT-5, DEBT-6. |
| No copy introduced on the row path | **True**, and `VectorWriteBuffer::Finish()` moves (`write_buffer.hpp:106-113`), so lifting the loopback does not carry a hidden copy into `pubsub/` either. |
| Boundaries (no `extern "C"`, no C header, no `dlopen`, no vtable, no version negotiation, method set untouched, `Publish` still inverted, no config parser, wire format untouched) | **No violation found.** |

### §7 → clause mapping, checked sentence by sentence

| §7 sentence | Clause |
|---|---|
| 1 — "never invoked with a null schema" | 2 (gated to `kCarried`) |
| 1 — "may subscribe before any publisher exists" | 1, 10 |
| 1 — "buffered and delivered once the schema is known" | 1 |
| 1 — "passes null throughout instead, and must never mix the two" | 3 |
| 2 — per-writer order | 4 |
| 2 — "delivered before, and never interleaved with" | 5 |
| 3 — idempotent identical re-declaration | 7 |
| 3 — "conflicting re-declaration **may** be rejected" | 8 — **over-encoded, see B1** |
| 4 — one callback per topic per instance | 9 |
| 5 — late joiners, `Subscribe` never blocks | 10 |
| 6 — no callback after `Unsubscribe` returns | 11 |

No §7 obligation is dropped. Attribution note (not a finding): clause 6's authority is
**locked decision 12** and §7 clause 1's "buffered and delivered", not §7 clause 2 — §7 says
nothing about retained-sample replay. That is fine: clause 6 adds a property with explicit
locked-decision authority and, because it is all-or-nothing, never demands retention where
none is promised. Clause 8 is the opposite case, which is why it is a BLOCKER and clause 6 is not.

---

## BLOCKERs

### B1 — Clause 8 tests "must" where §7 clause 3 says "may". **STOP-AND-ASK (oracle-wins).**

The design states its own position plainly (Risks, l.256-260) and escalates it as brief
decision 1 — the right instinct — but then lands `ConflictingRedeclarationIsRejected` as a
uniform assertion on all six subjects *now*, with the spec tightening deferred to PDA-DEC-9.
For eight items the suite would therefore be red against a provider the oracle calls
conformant, and, under decision 11, would drive provider behaviour changes justified by a test
rather than by the spec. §7 wins over the design; this cannot be a PDA-DEC-9 to-do.

The tree makes the answer nearly forced, and also exposes a defect in how the brief poses it:

- Fast DDS **already** implements the strong reading — `CreateTopicRejectsConflictingSchema`
  (`fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp:191-197`) asserts
  `EXPECT_THROW(..., std::runtime_error)` with the comment *"a genuine conflict and must not be
  silently dropped"*.
- The loopback **silently overwrites**: `gateway/src/main.cpp:78-80` replaces `slot.schema`
  with no comparison.
- XRCE has **no conflict handling at all** (no match for `conflict` anywhere under
  `xrcedds-pubsub-provider/`).

So brief decision 1's option (b), *"each keeps today's behaviour"*, is **not an available
option**: today's behaviours differ three ways, and keeping them is precisely locked decision
11's forbidden pinned divergence. The owner is being offered a choice one of whose branches a
locked decision forbids.

**Acceptable fix (forbidding is cheaper than handling):** amend §7 clause 3 from "may be
rejected" to "**must** be rejected" **in this item's PR**, once brief decision 1 is answered,
so the oracle and the suite agree the moment the suite lands — one line, and it removes the
divergent state rather than encoding a permissive OR. Restate brief option (b) as what it
actually is ("all three silently accept the conflicting shape — which means deleting the Fast
DDS check that ships today"). Do not land clause 8 ahead of the answer; if the owner
nonetheless chooses (b), clause 8 is **deleted**, never weakened into a per-provider expected
outcome.

### B2 — Unstated premise: that the in-process loopback is a *schema-carrying* subject.

The Summary declares six subjects including "InProcess (schema-carrying …)" and nothing in the
design says why, what it costs, or what stops it. §7 clause 1's last sentence names the
gateway's in-process loopback as the null-throughout transport; the design silently promotes it
to both modes. Today's loopback cannot satisfy §7 clause 1 in the carrying mode:

- `Subscribe` returns `MakeReadySchemaFuture(slot.schema)` where `slot.schema` may be **null**
  (`main.cpp:104-110`), i.e. an *already-resolved* future holding null — the header promises the
  future "resolves with a non-null `SharedSchema`" (`provider.hpp:22-28`).
- `Publish` dispatches immediately with whatever is cached (`main.cpp:97-101`), so a row
  published before `CreateTopic` reaches the callback with a null schema — and a later row on the
  same topic carries one, which is the "must never mix the two" violation.
- There is no pre-schema buffer anywhere in the class.

Clauses 1, 2, 3 and 10 are therefore red for the carrying InProcess subject on day one, and
decision 11 requires fixing them here. The fix is new schema-arrival plumbing (a real promise,
a pre-schema queue, an ordered flush) inside the exact class PDA-DEC-3 is about to re-vocabulary
— §3.4 replaces the `shared_future` as the contract. That is plausibly the largest single piece
of work in an item whose size is already declared unknowable, it is invisible in the design, and
the brief tells the owner the move is "MOVED (behaviour identical)".

**Acceptable fix — the cheaper of the two is forbidding:** drop the schema-carrying InProcess
subject (five subjects, §7 clause 1's plain reading: the loopback is the null-throughout
transport) and let the carrying mode arrive when the loopback becomes a general built-in
(PDA-DEC-5), after PDA-DEC-3 has settled schema arrival. **Or**, if it is kept: add a premise
with a stop condition — *"STOP-AND-ASK if making the loopback schema-carrying requires building
schema-arrival machinery PDA-DEC-3 will replace; ask whether that subject waits for
PDA-DEC-3"* — and correct the brief's "behaviour identical" to say the loopback's delivery
behaviour changes in this item.

### B3 — `Files-to-touch` omits the CI wiring for a new integration harness, in the same PR that deletes a CI-covered test.

Every existing integration harness has a `.github/workflows/ci.integration-test.<name>.yml`
plus a path-filter entry in `ci.pr.yml` (the block at `ci.pr.yml:165-272`). The design touches
`.claude/runbook.PDA-DEC.config.md` and nothing under `.github/`. Meanwhile it deletes
`DefaultQosReplaysEveryRetainedRowToALateJoiner`, which runs on CI today on **both** platforms
(`ci.fastdds-pubsub-provider.yml:63` and `:121`, `-o "&:run_tests=True"`). Net effect: the
round's first guard never runs on the shared lane, the shipped defect class loses the CI pin it
has now, and CI reports green with no signal that either happened. Reachable on every PR;
consequence silent.

**Acceptable fix:** add `.github/workflows/ci.integration-test.pubsub-conformance.yml` and the
`ci.pr.yml` path-filter/job entry to `Files-to-touch` (mirror
`ci.integration-test.fastdds-xrce-interop.yml`, including its Agent cache step — or run that
lane with `FLETCHER_CONFORMANCE_XRCE=OFF`). Cheaper alternative if CI work is out of appetite:
**keep** the provider-local Fast DDS test, so CI coverage is not net-reduced.

---

## DEBT — for the implementer, does not loop the design

**DEBT-1 — traits are declared per *subject*, which is the one residual way to declare around
clause 6.** `FastDdsLocal` and `FastDdsCrossProcess` may carry different `Retention` values, so
`kDropsPreSubscribe` on the cross-process subject alone would go green on any run where the
defect drops *everything*. *Proposed forbid:* key the trait table by **provider**, not by
subject, so local and cross-process cannot disagree — `FastDdsLocal`'s exactly-N assertion then
pins the value the cross-process subject must use. The design's narrower claim (a *partial*
count is not declarable under either value) is true as written.

**DEBT-2 — the inner loop can validate against a stale provider package.** The added block runs
`conan install` for the harness, but the loop's component list is still `for C in core pubsub`.
Decision 11's fixes land in `fastdds-pubsub-provider/src/**`, which the harness resolves from
the Conan cache — so an implementer iterating on a Fast DDS divergence fix would see no change
in behaviour. Add `fastdds-pubsub-provider` (and `xrcedds-pubsub-provider` when XRCE is ON) to
the inner loop's `for C in` list in the same edit. Contained by the mandated full-suite run for
this item, which does re-create the providers.

**DEBT-3 — update the baseline in `known_accepted_failures`.** It pins "components
28/61/19/16/**70**/11". Deleting the Fast DDS late-joiner test makes it 69; edit the line in the
same commit or the next full-suite run reads as an introduced regression.

**DEBT-4 — `−70` understates the deletions.** The loopback class is 57 lines
(`main.cpp:72-128`) plus its 10-line rationale comment, and the deleted Fast DDS test is ~46
lines with its rationale block (`:345-391`) — nearer `−110`. Correct it in the brief's "As
landed" line.

**DEBT-5 — share the Agent *install* dir, not the ExternalProject *prefix*.** `C:/fl-uxa` is
the source+build tree; two harnesses configuring it concurrently (two jobs on one runner)
collide. `C:/fl-uxa-install` is what buys the cache. Give the conformance harness its own
`AGENT_PREFIX`, keep `AGENT_INSTALL_DIR` shared.

**DEBT-6 — per-clause ctest entries cost one Agent lifecycle per clause.** `fastdds-xrce-interop`
uses a *single* ctest entry for a stated reason (`CMakeLists.txt:133-137`: the Agent binds one
UDP port, so two ctest-spawned processes race). `RESOURCE_LOCK` removes the race but not the
~22 Agent start/stop cycles for the XRCE binary. If it bites, collapse that binary to one ctest
entry (or make the Agent a ctest `FIXTURES_SETUP`) and take per-clause divergence grouping from
gtest's own `--gtest_output=xml` instead of `ctest --output-junit`. Related: `RESOURCE_LOCK
"fletcher-dds-<domain>"` cannot be per-domain as written — `gtest_discover_tests` applies
properties to every test in a target and `conformance_fastdds` carries two domains (151+152).
One lock per binary is what is implementable, and it is strictly safer.

**DEBT-7 — clause 1 cannot force the pre-schema window; say so.** Fast DDS's `Publish` throws
on an undeclared topic (`fast_dds_pubsub_provider.cpp:337`), so no verb ordering in the peer
protocol can put data ahead of the schema; the window exists only as a race between the
`__schema` channel and the data channel. Clause 1 asserts the observable contract (all N rows,
ordered, never a null schema) and may pass on a run that never exercised the buffer. Record that
in the harness README so nobody later reads clause 1 as proof the buffering path ran.

**DEBT-8 — do not let clauses 7 and 9 repeat clause 8's mistake.** §7 clause 4 states a
*cardinality*; it does not say whether a second `Subscribe` on one topic replaces the first or
is refused. Clause 9 must assert "exactly one delivery total across both registrations", not
which one wins. Same discipline for clause 7: assert idempotence, not a specific return.

**DEBT-9 — §6 clause 1 is the one prose delivery promise left unfalsifiable.** "Delivery is
serialized per subscription" is the third bullet of the header's own delivery contract
(`provider.hpp:106-108`) and no tracker item carries a forcing test for it. Either add it as
clause 12 (an overlap/re-entrancy counter in the callback is cheap) or name PDA-DEC-3 as its
home, so it does not fall between items.

**DEBT-10 — name the exit for a divergence whose fix is outside Fletcher's tree.** Premise 2's
stop condition covers only wire bytes. XRCE is the likely instance: if the Agent's retention
makes the late-joiner count neither 0 nor N and no Fletcher-side fix exists, clause 6 is
unsatisfiable under both trait values, pinning is forbidden, and the item cannot close. The
round's standing stop-and-ask list catches it, but the design should name it.

**DEBT-11 (accounting) — pick one reading of "new public surface".** If the harness's own header
types count (the design counts two), then `SchemaId` and `Topic` count too; if they do not, the
only product-visible addition is `fletcher::InProcessPubSubProvider` — one. Either reading is
defensible and neither is over budget; the mixed one is not defensible.

---

## Advice to the PM (not findings)

**On the early `InProcessProvider` lift — endorsed on the merits, not scope creep.** The class
is in an anonymous namespace inside a TU that defines `main`, so it is genuinely unlinkable from
a test; the InProcess subject cannot be dropped (ruling *"All three"*, and the tracker's forcing
test says all three providers); and the only other shape is a harness-local copy, i.e. a
duplicate that PDA-DEC-5 is already scheduled to delete. Building a coexistence bridge whose
deletion is already scheduled is the more expensive shape — the design chose delete-first, which
is right. Reducing PDA-DEC-5 to "register it + gateway `--provider` becomes a lookup" is
coherent; its forcing test `Registry.InProcessResolvesAsABuiltIn` still means what it means.
Nothing in the spec, the rulings, or decisions 1-14 constrains the ordering (§10 states the move
with no sequencing), so this is a granularity call and it is yours. Note the interaction with
B2: the lift is mechanical, but what the item then *does* to that class is not.

**On budget.** Design doc 300/300, brief 60/60 — at budget, not over. `+1700` is large for a
guard and is honestly declared and justified (greenfield twice over, clause set written once
rather than three times); the split axis named in Risks (divergence fixes become PDA-DEC-1b…n)
is the right one, and "PDA-DEC-1 does not close until they are green" is the correct guard on it.

**On the brief.** Principle-level and behaviour-visible throughout; code identifiers appear only
in the indented background footnotes. The one substantive problem is decision 1's option (b) —
see B1. Decisions 2 and 3 are well posed, and decision 2's recommendation is the one the tree
supports.

**No hand-composed post-change ledger** appears in the design — the machine checks it names
(`ctest` list, `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips`, the clause library's link
line) are real and are the right substitutes.

Two NITs were fixed silently in the design doc, line-neutral.

---

# Cycle 2 (final) — reviewed at `154a1a2`

**Verdict: APPROVE-WITH-DEBT(6).** All three cycle-1 BLOCKERs are closed, and all eleven
cycle-1 DEBT items were folded in. Nothing in the revision contradicts the spec, the rulings
ledger (now 19 entries, including the 2026-09-01 "Refused, every protocol" selection), or
locked decisions 1-14. No BLOCKER stands, so there is no framed question owed to the owner.
The register below is for the implementer; **DEBT-C2-1 must land in this PR** because it is
the other half of the owner's ruling.

## BLOCKER 1 — §7 clause 3 "may" → "must": **closed**

- The amendment lands **here**, not in PDA-DEC-9: design l.78-93 states the ruling, l.272-273
  puts `docs/pubsub-interface-spec.md` in Files-to-touch as a one-line change, and
  Files-to-delete l.288 records "the word *may* in §7 clause 3". Clause 8's authority now reads
  "§7.3 **as amended below**" (l.61) — it asserts *against* the oracle rather than ahead of it.
- Both consequent divergence fixes are named as **owned by this item**, with the evidence:
  loopback silently overwrites (`gateway/src/main.cpp:78-80`, l.84-85), XRCE gains conflict
  handling, Fast DDS already refuses. They appear in Files-to-touch (l.277-278) and in the
  forcing-test mapping as "red until this item's two fixes land" (l.235), which is the right
  place for them.
- Brief: the change is now its own `CHANGED` interface row (l.15) and decision 1 is recorded as
  answered (l.32-33). The misleading option (b) is gone.

**The "does anything else depend on the old reading?" sweep.** I searched the whole tree
(excluding `docs/archive/**`) for `may reject` / `may be rejected` / `conflict`:

- **Inside the spec: nothing else.** §7 clause 3 (l.330-331) is the only occurrence; §5.1, §7.1,
  §9, §10 and §11 carry no optionality about re-declaration.
- **`docs/protocol-driver-abi-spec.md:221`** already has `FLETCHER_ERR_SCHEMA_CONFLICT /*
  re-declaration with a different schema */` — consistent with "must", nothing owed.
- **`fastdds-pubsub-provider/README.md:255`** documents the check as unconditional — consistent.
- **One residual, and it is the seam's own header:**
  `pubsub/include/fletcher/pubsub/provider.hpp:68` says *"providers **may** reject a
  re-declaration with a conflicting schema"*, and it is **not** in Files-to-touch. §7's own
  preamble says this contract is "prose in `provider.hpp`", locked decision 5 requires the
  normative rule to live in the header, and PDA-ABI driver authors read the header, not the plan.
  Left alone, the PR ships a header contradicting the spec it amended in the same commit.
  This is **DEBT-C2-1**, not a BLOCKER: it is one word plus one Files-to-touch line, needs no
  redesign, and the failure mode is a loud clause-8 red plus a doc contradiction rather than a
  silent wrong answer. It would be a poor use of the owner's one framed question.

## BLOCKER 2 — five subjects, loopback schema-less: **closed**

Design l.8-14 and l.86-93. The justification is the right one (§7 clause 1's last sentence names
the loopback as the null-throughout transport) and the reason for not doing it now is stated
correctly (a promise + pre-schema queue + ordered flush inside the exact class PDA-DEC-3's §3.4
replaces — a construct scheduled for deletion). The handoff is written, and names what returns:
one `INSTANTIATE_TEST_SUITE_P` line plus one trait value, no new clause. The brief tells the
owner plainly (l.16, l.46-47), and the cycle-1 "MOVED (behaviour identical)" claim — which was
false once decision 11's fixes are counted — is gone, replaced by an explicit `CHANGED` row.

**Clause-set completeness over the five subjects, checked:** InProcess `{kAbsent, kDrops}`;
FastDds Local + CrossProcess `{kCarried, kRetains}`; Xrce Local + CrossProcess `{kCarried, …}`.

- Clause 2 (`kCarried`-gated) runs on 4 subjects / 2 providers — that is §7 clause 1's own
  carve-out, not a gap.
- Clause 3 carries the mirror for the one `kAbsent` subject, so §7 clause 1's last sentence
  keeps its only possible coverage.
- Clauses 1, 4-12 run on all five / all three providers.
- **No clause became unexercised by any provider**, and no §7 sentence lost its clause. The
  mapping I verified in cycle 1 still holds, with §7.3 now satisfied *by* the oracle rather than
  in tension with it.

## BLOCKER 3 — CI: **closed**

`.github/workflows/ci.integration-test.pubsub-conformance.yml` and the `ci.pr.yml` path-filter/
job entry are both in Files-to-touch (l.271-274), mirroring
`ci.integration-test.fastdds-xrce-interop.yml` **including its Agent cache step** — which is the
right model: that file's restore/save split exists precisely so a partial Agent install from a
failed run is never cached under the exact key (`:103-148`). Files-to-delete l.286-287 now states
that the deleted Fast DDS test's CI coverage is replaced in the same PR, so coverage is not
net-reduced. Two of the three "measured gotchas" check out exactly (whole-tree licence-header and
format scans; clang-format pinned at 18.1.3); the third was mis-targeted and I corrected it as a
NIT — see the NIT note at the end.

## Re-checked as design changes

**Traits keyed by provider (design l.24, l.38-39, rung-1 item 3) — the claim holds for clause 6.**
One `ProviderTraits` row per provider makes `FastDdsLocal` and `FastDdsCrossProcess` unable to
disagree, and `FastDdsLocal`'s exactly-N assertion (the property the deleted test pins today)
forces the row to `kRetainsPreSubscribe` — so the cross-process subject *must* expect N and the
defect's "often just the newest sample" fails. Setting the row to `kDrops` instead fails the
in-process subject. **The cycle-1 hole is genuinely closed**, and the XRCE case where no single
value works is correctly routed to premise 3's stop-and-ask rather than to a third enum value.
One consequence the revision did not notice — **DEBT-C2-2**.

**Clause 12 `DeliveryIsSerializedPerSubscription` — §6-faithful, weakly exercisable.**
Fidelity is right: §6 clause 1 says "delivery is **serialized per subscription**; the thread may
differ between samples", and the header's third delivery bullet (`provider.hpp:106-108`) says
"never two deliveries in flight for the same subscription". Counting callback **overlap** (l.74)
asserts exactly that, and asserting nothing about thread identity is the correct non-assertion —
a clause that pinned the thread would contradict §6 clause 1's own second half. It is also
correctly grouped with clauses 7 and 9 as cardinality-only. What the design does not say is that
clause 12 can only *observe* overlap, never provoke it, and on the cross-process subjects it
cannot provoke it at all because the peer pipe is one request/reply at a time — see
**DEBT-C2-3**. That is the same honest limitation the design already records for clause 1, and it
is worth the same sentence. Not a fidelity defect: the property clause 12 protects is a future
driver that dispatches per-sample onto a pool, and it will catch that.

## Numbers

- **`−115`: honest.** 57 (`main.cpp:72-128`) + 10 (rationale comment) + ~46 (the Fast DDS test
  with its block, `:345-391`) + the amended word ≈ 115. Matches what I measured independently.
- **`+1750`: understated by roughly 150.** The `+50` attributed to "the CI lane and the two
  conflict-rejection fixes" cannot cover a workflow that mirrors a **156-line** file including a
  two-job matrix, sparse-checkout lists and a split cache restore/save, plus a `ci.pr.yml` job
  entry, plus clause 12, plus the trait table. See **DEBT-C2-5**. No net-lines budget exists in
  the round config, and the ruling makes the item's size unknowable, so this is accounting, not
  a breach.
- **New public surface 1: honest, and now one consistent reading.** `pubsub/` ships in a Conan
  package, so `fletcher::InProcessPubSubProvider` is genuinely the only product-visible addition;
  `integration-tests/**` ships in none, so the harness's `ProviderSubject`, `ProviderTraits`,
  `SchemaId`, `Topic` and `FLETCHER_CONFORMANCE_XRCE` are correctly excluded. The stated rule
  (l.297-298) is applied uniformly, which is what cycle-1 DEBT-11 asked for.
- Budgets: design 298/300, brief 56/60 — inside, after two line-neutral NIT fixes below.

## Cycle-1 DEBT: all 11 folded in

Traits by provider (D1) l.24/38-39 · inner-loop provider components (D2) l.150-153 · baseline
`70`→`69` in the same commit (D3) l.160-162 · `−115` correction (D4) l.293-294 · own
`AGENT_PREFIX`, shared `AGENT_INSTALL_DIR` (D5) l.135-138 · single ctest entry for the XRCE
binary + `--gtest_output=xml` grouping + one `RESOURCE_LOCK` per binary (D6) l.128-144 · clause 1
cannot force the window (D7) l.253-257 · clauses 7/9 cardinality-only (D8) l.72-76 · clause 12
added (D9) l.63-65 · out-of-tree divergence exit (D10) premise 3, l.220-224 · one surface reading
(D11) l.297-298.

## DEBT register — cycle 2 (for the implementer)

**DEBT-C2-1 — `provider.hpp:68`'s "may reject" must change in this PR.** Add
`pubsub/include/fletcher/pubsub/provider.hpp` to Files-to-touch and edit lines 67-69 —
*"providers **may** reject a re-declaration with a conflicting schema"* → *must*. It is the same
sentence the owner ruled on, in the file §7 itself calls the contract's current home, and locked
decision 5 puts the normative rule in the header. Shipping the amended spec beside an unamended
header is the one way this ruling can still be half-landed.

**DEBT-C2-2 — key *retention* by provider, but leave *schema_mode* per subject.** "ONE row per
PROVIDER" (l.24) over-rotates: the design's own PDA-DEC-3 handoff (l.91-93) adds a
schema-carrying loopback subject with "one trait row", which under a strictly per-provider table
gives the InProcess provider two rows differing in `schema_mode` — exactly what rung-1 item 3
declares unrepresentable. The escape hatch that keying closes is on the **retention** axis only
(clause 6); `schema_mode` is a *usage* axis and §7 clause 1 explicitly sanctions one transport
being exercised in both modes. So: retention keyed by provider, `schema_mode` chosen per subject,
and narrow rung-1 item 3 to "a provider's subjects disagreeing on **retention**". Clause 6's hole
stays closed either way, and the promised handoff stops colliding with a forbidden-cases entry.

**DEBT-C2-3 — say that clause 12 observes overlap and cannot force it.** The peer protocol is one
request/reply at a time, so a clause cannot issue two concurrent publishes to a cross-process
subject at all; on those subjects clause 12 is an observation, not a proof. Give it clause 1's
honesty note in the harness README. Cheap teeth on the in-process subjects: have the **local**
subjects publish from two threads (representable — `PublishRow` is a direct call there), which is
a real assertion against the loopback, whose `Publish` holds `mu_` across the callback
(`main.cpp:90-101`).

**DEBT-C2-4 — reuse the conflict comparison that already ships above the seam, and note the
gateway path is unaffected.** `Publisher::CreateTopic` (`pubsub/src/publisher.cpp:46-78`) already
implements this exact check: it compares Arrow-IPC bytes, treats "no schema at all" as empty
bytes that still conflict with a schema-bearing declaration, and treats a schema that cannot be
IPC-encoded as unprovable ("such topics accept any re-declaration", `:26-29`). The loopback's new
provider-level check should not invent a different comparison. Two consequences worth knowing
before starting: (a) clause 8's A-vs-B pair is plainly encodable, so the unprovable case never
reaches it and "must be rejected" is not weakened by it; (b) the gateway cannot regress —
`Publisher` throws on a conflict and *returns early* on an identical re-declaration (`:68-76`),
so the provider never sees either through the gateway, and
`integration-tests/gateway-end-to-end`'s `createTopic schema conflict` test (which runs in both
provider contexts, `end-to-end.test.ts:373-388`) is untouched by the loopback fix.

**DEBT-C2-5 — two Files-to-touch entries owed, both one line.** `plans/PDA-decouple-interface.md`:
the design says the PDA-DEC-3 handoff is "Also in the plan" (l.93) and the brief says it is
"recorded there so it is not forgotten" (l.47), but PDA-DEC-3's tracker entry says nothing about a
schema-carrying loopback subject and the plan is not in the change list — the PM's ruled handoff
would otherwise live only in this design doc. And re-declare nearer **+1900 / −115** once the
156-line CI lane is counted.

**DEBT-C2-6 (informational, nothing owed) — the sparse-checkout gotcha was mis-targeted.** Every
`ci.integration-test.*.yml` job does use `sparse-checkout`, but the gateway/pubsub jobs already
check out `pubsub` (`ci.gateway.yml:42-48`, `:110-113`), so moving the loopback from
`gateway/src/main.cpp` into `pubsub/` needs **no** edit to any existing job's list. What does need
one is the new lane's own list (`pubsub`, both providers, `integration-tests/pubsub-conformance`,
`.conan-profiles`, `.github`). I corrected the paragraph and the Files-to-touch phrase in place so
nobody hunts for a change that is not needed.

## NITs fixed silently in cycle 2 (all line-neutral)

1. The sparse-checkout gotcha (l.170-174) and the matching Files-to-touch phrase — a verifiable
   tree claim that was wrong; see DEBT-C2-6.
2. Forcing-test mapping row 2 was labelled "§7 clause set (clauses 2-12)" while clause 12's
   authority is §6 clause 1 — now reads "§7 clause set + §6.1".

No hand-composed post-change ledger appears in the revision; the machine checks it names are
still the real ones.
