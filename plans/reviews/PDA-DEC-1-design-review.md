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
