# PDA-DEC-9 — Seam spec, exception taxonomy, TD entry, and the parallelism handoff

Item: `PDA-DEC-9` · Kind 📓 docs · Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
§1, §9 · Rulings: [PDA-DEC-rulings.md](PDA-DEC-rulings.md) · Locked: [PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md)

## Summary

Close the round by making the seam's contract *signable*: reconcile the spec with what
landed, publish the exception taxonomy in a form a machine compares to the enum, restate
the two top-level docs' "implementing one interface" claims in registry terms, add TD-008,
and add spec **§12 — the handoff**, which lists what is frozen, how each of the DoD's six
conditions is verified (`mechanical` or `by-reading`, per condition), and what the round's
evidence does **not** cover. One new test; no new public C++ surface.

## Design

### D1 — Four artifacts

1. **`docs/pubsub-interface-spec.md`** — status `proposed` → **frozen**; §5.1 cites the
   published taxonomy table instead of restating it; §9 gains one row (what each round
   inherits as its oracle); §10's stale live count is removed; **new §12** (the handoff).
2. **`core/README.md`** — a new *Error taxonomy (published)* section: one table, name ·
   number · meaning · who raises it, for all ten `PubSubStatus` values. This is the
   **single** prose copy of the numbers, and it lives in the component that owns
   `status.hpp`, so it ships with the package and a test can read it off disk.
3. **`docs/technology-decisions.md`** — **TD-008**, one entry covering the seam-as-contract
   *and* uniform selection: the registry is not independently reversible from the freeze,
   and TD entries in this log are one-per-architectural-call (TD-001…007 precedent).
4. **The round's own record** — the plan's DoD checklist gains a verification column and is
   ticked against §12; the tracker row goes 🟢; the progress log gains the item entry.

### D2 — The six conditions, and how each is VERIFIED

§12 carries this table. Each row names the artifact **and** the check; the last column is
the honest one. "By-reading" rows name the reader and the date — they are not softened
into a grep, because a doc-presence grep is an inert guard (see *Corner cases*, F3).

| DoD | Artifact | Check | How |
|---|---|---|---|
| 1. Ownership rules in the header (§3) | `write_buffer.hpp`, `types.hpp`, `owned_schema.hpp`, `schema_arrival.hpp`, `provider.hpp` | Correctness of the *wording* is read. What the types make **representable** is pinned by the `SeamVocabulary` suite (7 entries) — no view-only `Blob`, zero-size normalisation, empty-segment refusal | **by-reading** |
| 2. Schema arrival C-expressible; `shared_future` a convenience, not the contract (§3.4) | `schema_arrival.hpp` | `shared_future` occurs **nowhere** in `core/ pubsub/ pubsub-arrow/ gateway/ *-provider/` except two historical comments — verified this step; the tree compiles with no such member, so there is no second mechanism to drift | **mechanical** |
| 3. Taxonomy published and stable (§5.1) | `core/include/fletcher/core/status.hpp` + `core/README.md` table | Ten `static_assert`s pin the numbers (`status.hpp:67-83`); the new `Taxonomy.PublishedNumbersMatchTheEnum` reads the README table **off disk** and compares it to the enum, and a totality `static_assert` on the last enumerator fails the build if a status is appended without a table row | **mechanical** |
| 4. Registry signature admits a resolver without change (§4 cl. 2) | `provider_registry.hpp` | `static_assert` on the **member-pointer type** of `Create` (`provider_registry.hpp:292`) — sees a defaulted extra parameter or a dropped `const`, which a return-type check cannot; plus `Registry.PathSelectorResolvesThroughTheSameCall` (stand-in resolver) and `…WithoutResolverIsRefusedAsUnsupported` as its live negative control | **mechanical** |
| 5. Nothing above the seam branches on built-in vs loaded (§0.1(2)) | `provider_registry.hpp`, `gateway/src/main.cpp` | The distinction is **unrepresentable above the seam**: the public registry has no accessor that reports it, so there is nothing to branch on; and the gateway names no concrete provider type in selection (`main.cpp:210` registers, `Create` selects). Adding such an accessor would be new public surface | **mechanical** |
| 6. The spec states what is frozen, and changing it is a stop-and-ask (§1) | spec §1 + §12's frozen list | A wording condition: read. §12 makes it checkable *in kind* — every normative element sits in exactly one of two named classes and the default is `frozen` | **by-reading** |

### D3 — The frozen list: two classes, no third, default `frozen`

§12 lists every normative element in exactly one class:

- **`frozen`** — §2's method set and `Publish`'s inversion; §3's ownership rules; §3.4's
  outcome table and its `timeout_ms` rules; §4's registry surface (`Create`, `Register`,
  `SetPathResolver`) and `ProviderSelector::Parse`'s classification; §5.1's mapping rules;
  §6; §7's clauses.
- **`append-only`** — `PubSubStatus` values; `ProviderConfig`'s typed core fields (a later
  field never changes `Create`, per §4.1); registered provider names.

Anything not listed is `frozen`. There is no "negotiable" class, so no later round has to
judge which sentence binds it: *"A later round finding the seam insufficient is a
**stop-and-ask** against this spec"* (§1) applies to everything above.

### D4 — Publishing the taxonomy so it cannot drift

Owner ruling 2026-09-01: *"One error, numbered cause — A single error type carrying a
stable numbered cause, identical across protocols."* Two independent bindings need the
**numbers**, in prose, in the package they consume.

- The table lives in `core/README.md` — one copy, in the component that owns the enum. The
  spec **cites** it (§5.1) and does not restate it; a second copy is exactly the drift the
  ruling exists to stop.
- `core/tests/test_status_taxonomy.cpp` reads `FLETCHER_CORE_README_PATH` at **run** time
  and asserts, row for row, that the published name/number pairs equal
  `static_cast<int32_t>(PubSubStatus::…)`. Precedent and mechanism: `XrceConfig.Published`
  `DefaultsAreExact` (`xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`) — a
  `file(READ)`/`configure_file` bake-in would re-create the held-copy defect the disk read
  exists to kill.
- Totality: `static_assert(static_cast<int32_t>(PubSubStatus::kSubscriptionEnded) == 9)`
  plus a row-count equality in the test. Appending a status without a table row fails.
- `core/conanfile.py`'s `exports_sources` gains `"README.md"` — **verified absent today**
  (`conanfile.py:24-29`), so the cache build would otherwise read a file that is not there.

### D5 — The reconciliation set (what is actually false today — checked this step)

| Where | What I verified | Fix |
|---|---|---|
| spec §10, migration header | *"8 files, 18 `ProviderConfig` constructions … re-derivable with `grep -E 'ProviderConfig[[:space:]]*\{ …'`"*. Ran that pattern over the tree excluding `plans/`: **96 occurrences in 21 files**. The quantity stopped existing when the migration finished — post-migration, `ProviderConfig` is how *everything* is configured | Delete the live total and the recipe. Keep the per-site table marked **as of PDA-DEC-7, historical**; state the durable fact instead: the retired types name nowhere in code, and the compile is the check |
| spec §10, docs paragraph | *"architecture-overview.md and the root README.md were inspected in PDA-DEC-6 and carry no retired vocabulary — their 'implementing one interface' claims stand"*. True as far as it goes: `make_shared<FastDDSPubSubProvider>()` still compiles (`fast_dds_pubsub_provider.hpp:104`, ctor takes `const ProviderConfig& = {}`). But the inspection covered **vocabulary**, not accuracy | Narrow the sentence to what was checked, and fix the two inaccuracies below |
| `architecture-overview.md:164` (and `:86`) | *"the provider returns a `SubscriptionResult` containing the publisher's schema as an `OwnedSchema`"* — the seam has carried a `SchemaArrival` since PDA-DEC-3, and carried a `shared_future` before that. Never an `OwnedSchema` | Restate as: `Subscribe` returns immediately with a **waitable schema arrival** whose outcome is typed (§3.4) |
| `architecture-overview.md:15`, `README.md:38` | *"Adding a new transport … requires implementing one interface"* — now incomplete, not wrong: a transport is also **registered under a name** and reached through the registry (§4) | Restate in registry terms; note that after PDA-ABI a driver arrives the same way, by path, with no caller change |
| `README.md:315` (roadmap) | *"Dynamically loadable transport plugins … so an application can pick a transport without compile-time coupling"* — the **selection** half shipped (PDA-DEC-4/5/6/7); only loading remains | Reword so the roadmap does not offer what already landed |
| design-debt C2-6 | `pubsub-conformance/CMakeLists.txt:306-308` already reads **27** (24 clauses + 1 `Registry` + 2 `ConformanceXrce`). C2-6 is discharged | Record the check; change nothing |

**No suite totals are written by this item.** `ctest -N` and `--gtest_list_tests` cannot be
run in the design step, which is the reason the design *forbids* the figure rather than
quoting one (F1 below). Where §12 must point at guards it names them and gives the command
to run them; entries and cases are never mixed in one sentence.

### D6 — What the round's lessons inherit as, and with what force

§9's table gains one row — **"Inherits as its oracle"** — pointing both rounds at the
conformance suite, `CopyAccounting`, `SeamVocabulary` and the registry cases, plus the
obligation §8.1 already states for this round: *"a guard nobody has made go red is a guard
nobody has measured."* Under Brief decision 2 this becomes **normative for both later
rounds**: a guard ships with a recorded falsification or it is not evidence.

§12 records the concrete inheritance in ~12 lines, no more: the **seven inert guards** this
round found (DEC-6's discovery-based QoS guard, DEC-7's four unwitnessed keys and its
socket-leak probe, DEC-1H's unreached refusal arm, DEC-8's unreachable-by-construction pair
and its `kOk`-on-null-schema wait) — each read as protection and could not fail; the
**false-green** (a foreign Agent could serve a whole XRCE run — fixed in PDA-DEC-1H, and
every XRCE green before it was conditional on no stray Agent listening) and the
**false-red** (leaked shm segments fault `create_participant` with `0xC0000005`); and the
two blind spots that keep their owners by ruling — the receive-side data-sharing defect
(**PDA-ABI-7**) and §3.4's unbounded-wait clause, pinned by no test on purpose.

### D7 — The evidence statement (what §12 says about platforms)

Written positively, so there is no sentence a reader can take as a portability claim: the
round's greens are **local Windows runs plus one WSL compile**; the `workflow_call` lanes in
`ci.pr.yml` (20 component + integration lanes, including
`ci.integration-test.pubsub-conformance`) are PR-triggered and **opening the PR is the
owner's step**, so no CI has run on `feature/protocol-driver-abi`. Consistent with the three
rulings in which the owner chose a narrow claim stated honestly — *"Scope to the interface,
say so plainly"* (2026-09-01), *"Ship the guard, hunt elsewhere"* (2026-09-01), *"One
application on one machine, with three exclusions in the docs"* (2026-09-03). See Brief
decision 1.

### D8 — TD-008

`## TD-008: The Pub/Sub Seam as a Frozen Contract with Uniform Provider Selection`, in the
log's own shape (Status / Context / Decision / Rationale / Alternatives considered / Risks).
Decision records: the seam is the single meeting point for two parallel rounds; selection is
by name-or-path through one registry with built-in-vs-loaded invisible above it; config is a
typed core plus an opaque provider document; failures are one type with a numbered cause.
Risks records: the freeze buys parallelism at the price of a stop-and-ask whenever either
round finds the seam short.

## Corner cases forbidden

- **F1 — A free-floating count in the spec is unrepresentable.** Either a number carries the
  command that derives it, inline, or it is not written. §10's live total goes; the
  per-site table becomes dated history. (Eight miscounted figures this round, one introduced
  while correcting another; four of them were entries-vs-cases conflations.)
- **F2 — A second copy of the taxonomy numbers.** One prose table, in the component that
  owns the enum, machine-compared to it. The spec cites; it does not restate.
- **F3 — A doc-presence grep dressed as a handoff check.** It would be the eighth inert
  guard: it cannot fail once the heading exists. Conditions 1 and 6 are labelled
  `by-reading`, with the reader named, and are not laundered into a mechanical claim.
- **F4 — A handoff row without a verification method.** The §12 table's shape has no cell
  for a promise: every row is {artifact, check, mechanical|by-reading}.
- **F5 — An implied platform claim.** §12 states what ran and where; it contains no
  "portable", "both platforms" or "CI-green" sentence to be misread.
- **F6 — A third freeze class.** Two named classes, default `frozen` (D3), so no later
  round decides for itself whether a sentence binds it.
- **F7 — A "handoff addendum" document.** The handoff lives **in the spec**, the one
  document both rounds already mirror (split ruling: *"only 'meet' and the interface defined
  in PDA-decouple"*). A separate file would be a second contract, and it would be the one
  that rots.

**Handled residue**

- **H1 — §3.4's unbounded-wait clause stays pinned by no test.** *Why not forbidden:* the
  Windows standard library clamps the deadline inside `wait_for`, so a conforming and a
  non-conforming implementation are indistinguishable here; a test would pass for a reason
  other than the one it states. Handled by disclosure, and listed in §12 as inherited-unproven.
- **H2 — The receive-side data-sharing defect and the conformance blind spot survive the
  round.** *Why not forbidden:* owner ruling 2026-09-01 assigned them to PDA-ABI-7;
  forbidding them here would reopen work a ruling closed.
- **H3 — Linux-side correctness is unproven.** *Why not forbidden:* the lanes are
  `workflow_call` from a PR-triggered workflow and opening the PR is the owner's step, so the
  round cannot run them from inside itself. Handled by stating the evidence base (D7).

## Premises and stop conditions

- **P1 — `core/README.md` can be exported without disturbing the package.** `core` clears
  its `package_id`, so adding a doc to `exports_sources` should be inert.
  **STOP-AND-ASK** if adding it changes core's package ID or breaks `conan create core`:
  do not move the table into a test fixture — ask, because the table's whole value is that
  consumers get it.
- **P2 — A second disk-read doc test is welcome in this tree.** Verified precedent:
  `xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`. **If review rejects it**, fall back
  to the header as the published artifact and re-label condition 3 `by-reading` in §12 — do
  **not** substitute a grep for the table's presence (that is F3).
- **P3 — `PubSubStatus` is the ten values `0..9` pinned at `status.hpp:67-83`.** Verified.
  If a status is appended before implementation, the totality assert forces the row; that is
  the mechanism working, not a premise failure.
- **P4 — No CI has run on this branch.** Given by the PM and consistent with the workflow
  shape. **If a CI run exists at implementation time**, rewrite D7's paragraph from that
  run's actual result — do not soften or widen the sentence by hand.
- **P5 — Both docs' `make_shared<FastDDSPubSubProvider>()` examples still compile.**
  Verified (`fast_dds_pubsub_provider.hpp:104`). If that ctor ever loses its default
  argument, the examples change in *that* PR, not retroactively here.

## Forcing-test mapping

- **`docs review; handoff checklist complete (§DoD)`** → §12 (D2's table, D3's frozen list,
  D6, D7) plus the plan's ticked checklist. **Red for the right reason today:** §12 does not
  exist; the six conditions have no recorded verification method; §10's own grep recipe
  yields 96/21 against its stated 18/8; and `architecture-overview.md:164` describes a
  `SubscriptionResult` carrying an `OwnedSchema`, which the seam has not done since
  PDA-DEC-3.
- **`Taxonomy.PublishedNumbersMatchTheEnum`** (new, `core_tests`) → D4. **Red for the right
  reason today:** there is no published table to read, and `core/README.md` is not exported,
  so the test cannot find its input. After the change, editing either the table or an
  enumerator alone turns it red; appending a status without a row fails the build.

## Risks / Unknowns

- **Reconciliation scope.** I audited the *spec*, the two top-level docs and the DoD's own
  claims. Component READMEs' counts stay owned by their items (design-debt C2-5 belongs to
  PDA-DEC-8's PR, C2-6 is discharged). A whole-tree number audit would not fit the budget and
  would duplicate work the items already own.
- **Freezing a spec that describes unrun Linux builds.** Mitigated by D7 stating it rather
  than implying otherwise; the residue is Brief decision 1.
- **No coexistence window is created by this item.** Nothing is bridged, delegated or
  re-exported; the round's retirements (`FastDDSProviderOptions`, `XrceConfig`,
  `shared_future`) already landed with no shim — verified this step: every remaining
  occurrence in code is a historical comment or a gtest suite name.
- **No STOP-AND-ASK.** The one apparent tension — §10 saying the top-level docs' claims
  "stand" while this item is told to restate them — is not a contradiction: §10's sentence
  is about *retired vocabulary* (nothing there fails to compile), and PDA-DEC-9 owns §10, so
  it narrows that sentence to what was actually checked.

## Files-to-touch

- `docs/pubsub-interface-spec.md` — status → frozen; §5.1 cites the published table; §9 gains
  the oracle row; §10 pruned (D5); **new §12**.
- `docs/architecture-overview.md` — §1 principle (registry terms); `:86` and `:163-167`
  (schema arrival); §7.4 gains the registry-selected variant beside the direct one.
- `README.md` — `:38` principle; `:315` roadmap; `:155` verified already correct, untouched.
- `docs/technology-decisions.md` — TD-008.
- `core/README.md` — *Error taxonomy (published)* section.
- `core/tests/test_status_taxonomy.cpp` — **new**.
- `core/tests/CMakeLists.txt` — source + `FLETCHER_CORE_README_PATH` definition.
- `core/conanfile.py` — `exports_sources` gains `"README.md"`.
- `plans/PDA-decouple-interface.md` — DoD checklist gains the verification column and is
  ticked against §12; tracker row 🟢.
- `plans/PDA-decouple-progress-log.md` — the item entry (round convention).

## Files-to-delete

- **spec §10's live migration total and its `grep -E` recipe** — replaced by the retirement
  claim plus the dated per-site table; the compile is the check.
- **spec §10's correction archaeology** (the two paragraphs recording that "4 files / 19
  occurrences" and "12 construction sites" were each wrong twice) — replaced by one sentence;
  the corrections are in the progress log and the reviews, which is where they belong.
- **spec §10's "their claims stand" sentence** — replaced by the narrower statement of what
  PDA-DEC-6 actually inspected.
- **`README.md:315`'s "without compile-time coupling" promise** — no replacement for the
  selection half: it shipped, so the roadmap stops offering it.
- No tests and no code are retired. Justified: the round's retirements landed in PDA-DEC-3/6/7
  with no coexistence window (verified above); this item's product-code footprint is one new
  test, and there is nothing it supersedes.

## Numbers

Declared net lines: **+360 / −70** · new public surface: **0** (one new test, one README
section documenting an existing enum, one packaging line) · design cycles used: 1/2
