# PDA-DEC-9 — Seam spec, exception taxonomy, TD entry, and the parallelism handoff

Item: `PDA-DEC-9` · Kind 📓 docs · Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §1, §9 · Rulings: [PDA-DEC-rulings.md](PDA-DEC-rulings.md) · Locked: [PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md)

## Summary

Close the round by making the seam's contract *signable*: reconcile the spec with what
landed, publish the exception taxonomy in a form a machine compares to the enum, restate the
two top-level docs' "implementing one interface" claims in registry terms, add TD-008, and
add spec **§12 — the handoff**: what is frozen **and who may act on each class**, how each
DoD condition is verified (`mechanical` / `by-reading` / `by-construction`, honestly per
condition), and what the evidence does **not** cover. One new test whose guard fails the
**build** when a status is appended; no new public C++ surface.

## Design

### D1 — Four artifacts (paths in *Files-to-touch*)

1. **The spec** — status `proposed` → **frozen**; §5.1 *cites* the published table instead of
   restating it; §9 gains one row (what each round inherits as its oracle); §10's stale count
   and per-site ledger are removed (D5); **new §12**, the handoff.
2. **`core/README.md`** — one *Error taxonomy (published)* table, **name · number · meaning**,
   for all ten `PubSubStatus` values, beside the header that defines them (D4). Only name and
   number are machine-compared and the section says so; *meaning* stays normatively owned by
   `status.hpp`'s doc comments, and there is no "who raises it" column — it rots the moment
   PDA-ABI ships a driver raising `kNotSupported` (C1-2).
3. **TD-008** (D8), and **4. the round's own record** — the plan's DoD checklist gains a
   verification column, ticked against §12; the tracker row goes 🟢; the progress log gains
   the item entry.

### D2 — The six conditions, and how each is VERIFIED

§12 carries this table. A row says `mechanical` **only where a named machine reddens on a
named mutation**, so the honest count is **two of six end-to-end** (3, 4): condition 2 splits
into one mechanical clause and one read one, 1 and 6 are `by-reading` with reader and date, 5
is `by-construction` with no machine check and says so. Labels came down, not guards up (B3).

| DoD | Artifact | Check | How |
|---|---|---|---|
| 1. Ownership rules in the header (§3) | `write_buffer.hpp`, `types.hpp`, `owned_schema.hpp`, `schema_arrival.hpp`, `provider.hpp` | Correctness of the *wording* is read. What the types make **representable** is pinned by the `SeamVocabulary` suite — no view-only `Blob`, zero-size normalisation, empty-segment refusal | **by-reading** (reader + date) |
| 2a. No second schema-wait mechanism exists (§3.4) | `schema_arrival.hpp`, and the absence of `shared_future` | The tree compiles with no `shared_future` member on any seam type. Derive the survivors, do not trust a tally here: `grep -rn "shared_future" core pubsub pubsub-arrow gateway *-provider --include=*.hpp --include=*.cpp`. **The DoD's "a convenience over it, not the contract" clause is superseded** by owner ruling 2026-09-01 (*"One mechanism only"*): the three `shared_future` members are **retired**, so there is no convenience wrapper to look for | **mechanical** |
| 2b. `SchemaArrival` has a coherent C-expressible form (§3.4) | `schema_arrival.hpp` + §3.4's outcome table | Read. This is the clause both later rounds derive `fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)` from, weeks apart, without talking — an absence grep passes whether or not the form is coherent, so it is not labelled as if it settled this | **by-reading** (reader + date) |
| 3. Taxonomy published and stable (§5.1) | `status.hpp` + `core/README.md` table | Per-enumerator `static_assert`s pin the numbering (`status.hpp:67-83`); the new `Taxonomy.PublishedNumbersMatchTheEnum` reads the table **off disk** and compares it to the enum; the exhaustive-`switch` name mapping with the promoted diagnostic makes **appending an enumerator a compile failure**, and its one-past-the-last assertion holds the suite red until the README carries the new row (D4) | **mechanical** |
| 4. Registry signature admits a resolver without change (§4 cl. 2) | `provider_registry.hpp` | `static_assert` on the **member-pointer type** of `Create` (`provider_registry.hpp:292-297`) — sees a defaulted extra parameter or a dropped `const`, which a return-type check cannot; plus `Registry.PathSelectorResolvesThroughTheSameCall` (stand-in resolver) and `…WithoutResolverIsRefusedAsUnsupported` as its live negative control | **mechanical** |
| 5. Nothing above the seam branches on built-in vs loaded (§0.1(2)) | `provider_registry.hpp`, `gateway/src/main.cpp` | Today the distinction is unrepresentable above the seam: the public registry exposes no accessor that reports it, and the gateway names no concrete provider type in selection. **No machine notices a later round adding one** — the frozen-signature assert pins `Create`'s type alone. What protects it forward is §12's own frozen list: §4's registry surface is `frozen`, so an accessor is a **stop-and-ask**, and this is the condition PDA-ABI will pressure hardest | **by-construction (no machine check)** |
| 6. The spec states what is frozen, and changing it is a stop-and-ask (§1) | spec §1 + §12's frozen list | A wording condition: read. §12 makes it checkable *in kind* — every normative element sits in exactly one of two named classes and the default is `frozen` | **by-reading** (reader + date) |

### D3 — The frozen list: two classes, no third, default `frozen`

§12 lists every normative element in exactly one class, **by section number** so no
normative sentence needs a paraphrase to be caught (C1-3), and each class carries a *who may
act* sentence — without that the two differ in no observable way and the classification is
decorative (B5):

- **`frozen`** — §2; §3 entire (§3.1–§3.5, so §3.5's empty-segment refusal and §3.4's outcome
  table and `timeout_ms` rules are named, not left to the default); §4 and §4.1's *rules*;
  §4.2; §5.1's mapping and refusal rules; §5.3's callback rule; §6; §7's clauses; §8's
  zero-copy property. Anything unlisted is `frozen`; there is no "negotiable" class. **Nobody
  acts alone** — §1: *"A later round finding the seam insufficient is a **stop-and-ask**
  against this spec — not a local workaround, and not a change landed inside an ABI round."*
- **`append-only`** — the `PubSubStatus` values; `ProviderConfig`'s typed core fields;
  registered provider names. The class constrains only the **shape**: an append, nothing
  renumbered, reordered, reused or removed. **Making an append is still a stop-and-ask, and
  the owner allocates the number or field** — so two parallel rounds cannot both take
  `PubSubStatus = 10`, and no allocation protocol or reserved range is needed. A
  `PubSubStatus` append carries its `core/README.md` row in the same change, which D4's guard
  makes mechanical rather than a request. For the typed core §4.1 is carried **verbatim**:
  *"It is **exactly those two fields** and it is append-only; a later field never changes
  `Create`. Widening it because one protocol wants a setting typed is a stop-and-ask (owner
  ruling 2026-09-02: 'Fletcher keeps exactly payload size and domain')."*

**Contract text is not the test set** (C1-4): `frozen` binds the *wording* of §7's clauses;
the suite stays extendable and both rounds are expected to add cases — what the 2026-08-31
ruling means by the suite pressure-testing the ABI.

### D4 — Publishing the taxonomy so it cannot drift

Owner ruling 2026-09-01: *"One error, numbered cause."* Two independent bindings need the
**numbers in prose**, beside the enum that defines them — one copy, in `core/README.md`,
which the spec **cites** (§5.1) and does not restate. §5.1's two in-prose numbers (`kOk = 0`,
"taxonomy entry 4") stay because the rules there are *about* those numbers, and §5.1 says in
one clause that they are cited exceptions and the table is the only enumeration (C1-7).

**The guard holds no count and no copy of the numbers.** `core/tests/test_status_taxonomy.cpp`
reads `FLETCHER_CORE_README_PATH` at **run** time (`#error` if the define is missing; empty
string on an unreadable file, which the caller asserts on — precedent
`xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`; a `configure_file` bake-in would
re-create the very held-copy defect the disk read kills). Three parts:

1. **`StatusName(PubSubStatus)` — one `switch`, every enumerator, no `default:` label**;
   fallthrough past the switch returns `""` (a statement *after* the switch, not a `default:`
   label, so the diagnostic still fires and MSVC C4715 stays quiet). The diagnostic is
   promoted to an error on this one source file — `/we4062` (MSVC; fires only when the switch
   has no `default:`) and `-Werror=switch` (GCC/Clang) via `set_source_files_properties(...
   COMPILE_OPTIONS ...)`: narrowest scope, no new target, and `core/tests` has no other
   `switch` today. Precedent `pubsub-arrow/tests/CMakeLists.txt:57-59`; the tree has **no**
   global `/WX` or `-Werror`, so the flag must be explicit. **The mutation that reddens it:
   appending an enumerator** — a *compile* failure of `core_tests`, not a test failure. (A
   `kLastStatus` sentinel is the other route and is **+1 public surface against a declared
   0**, so it is not taken; see P3b.)
2. **Row for row against the file:** `StatusName(static_cast<PubSubStatus>(number)) == name`.
   Editing a name or number on either side goes red. The test asserts the read was non-empty
   and at least one row parsed, so the loop cannot be vacuously green (C1-6).
3. **One past the last row is not a status:** `StatusName(static_cast<PubSubStatus>(
   rows.size()))` must be `""` — this closes the append: part 1 forces the appender into this
   file, part 3 holds the suite red until the README gains the row. No count is needed because
   §5.1 pins the values *appended only, never reordered or reused*, so the set is the
   contiguous prefix `0 … rows.size()-1`, which the test also asserts.

`core/conanfile.py`'s `exports_sources` gains `"README.md"` — **verified absent today**
(`conanfile.py:24-29`) — so `core_tests` can read it when the lane builds in the Conan cache.
That is the *test's* input, exactly as the precedent's comment says ("exported for the TESTS,
not for documentation"); `package()` is unchanged and nothing claims the README reaches a
consumer's package folder (B2).

### D5 — The reconciliation set (what is actually false today — checked this step)

| Where | What I verified | Fix |
|---|---|---|
| spec §10, migration header **and its per-site table** | *"8 files, 18 `ProviderConfig` constructions … re-derivable with `grep -E 'ProviderConfig[[:space:]]*\{ …'`"*. Ran that pattern over the tree excluding `plans/`, `build/`, `node_modules/`: **96 occurrences in 21 files**. The quantity stopped existing when the migration finished — post-migration, `ProviderConfig` is how *everything* is configured. The table below it does not reproduce either, and its rows do not sum to its own header | Delete the total, the recipe **and the eleven-row table** (F1: it is a hand-composed post-change ledger with no derivable command). Replace with the one durable sentence: the retired config types name nowhere in code — every remaining occurrence is a comment or a gtest suite name — and **the compile is the check** |
| spec §10, docs paragraph | *"… carry no retired vocabulary — their 'implementing one interface' claims stand"*. True as far as it goes (`make_shared<FastDDSPubSubProvider>()` still compiles, `fast_dds_pubsub_provider.hpp:104`), but the inspection covered **vocabulary**, not accuracy | Narrow it to what was checked; fix the two inaccuracies below |
| `architecture-overview.md:164` (and `:86`) | *"the provider returns a `SubscriptionResult` containing the publisher's schema as an `OwnedSchema`"* — the seam has carried a `SchemaArrival` since PDA-DEC-3, and carried a `shared_future` before that. Never an `OwnedSchema` | Restate as: `Subscribe` returns immediately with a **waitable schema arrival** whose outcome is typed (§3.4) |
| `architecture-overview.md:15`, `README.md:38` | *"Adding a new transport … requires implementing one interface"* — now incomplete, not wrong: a transport is also **registered under a name** and reached through the registry (§4) | Restate in registry terms; note that after PDA-ABI a driver arrives the same way, by path, with no caller change |
| `README.md:315` (roadmap) | *"Dynamically loadable transport plugins … so an application can pick a transport without compile-time coupling to its library"*. Two promises, and only one moved: runtime **selection among linked-in providers** shipped (PDA-DEC-4/5/6/7); **loading** did not, and neither did freedom from linking the provider library | **Keep the coupling clause — it is still a true unshipped promise** (debt C1-1). Narrow the bullet to loading, and note that selection by name already ships. Deleting the coupling half would understate PDA-ABI |
| design-debt C2-6 | `pubsub-conformance/CMakeLists.txt:306-308` already reads **27**; C2-6 is discharged | Record the check; change nothing |

### D6 — What the round's lessons inherit as, and with what force

§9's table gains one row — **"Inherits as its oracle"** — pointing both rounds at the
conformance suite, `CopyAccounting`, `SeamVocabulary` and the registry cases. §8.1's
sentence is **restated unchanged, not strengthened**: *"A live negative control ships with
it: a guard nobody has made go red is a guard nobody has measured."* It is **not** turned
into a close gate for the later rounds — the owner ruled on exactly that gate in this round
and relieved it (2026-09-01, *"Ship the guard, hunt elsewhere"*). §12 carries that ruling as
the standing policy and does not re-pose it (B6).

§12 records the concrete inheritance in ~12 lines. The inert guards this round found, **as a
list, not a total** (F1): DEC-6's discovery-based QoS guard · DEC-7's four unwitnessed document
keys · DEC-7's socket-leak probe · DEC-1H's unreached refusal arm · DEC-8's
unreachable-by-construction pair · DEC-8's `kOk`-on-null-schema wait — each read as protection
and could not fail. Then the **false-green** (a foreign Agent could serve a whole XRCE run;
fixed in PDA-DEC-1H), the **false-red** (leaked shm segments fault `create_participant` with
`0xC0000005`), and the two blind spots that keep their owners by ruling: the receive-side
data-sharing defect (**PDA-ABI-7**) and §3.4's unbounded wait.

### D7 — The evidence statement (what §12 says about platforms)

Written positively, so no sentence can be read as a portability claim: the round's greens are
**local Windows runs plus one WSL compile**; every component and integration lane in
`ci.pr.yml` is a `workflow_call` from a `pull_request`-triggered workflow (no count written —
the lanes are that file's `uses:` entries), so **opening the PR is the owner's step** and no
CI has run on `feature/protocol-driver-abi`. Consistent with the three rulings where the owner
chose a narrow claim stated honestly (2026-09-01 ×2, 2026-09-03; Brief decision 1). It also
**instructs** rather than only recording (C1-5): both rounds treat Linux as unverified for
seam behaviour; the first PR of either runs the lanes; a Linux-only failure *of seam
behaviour* is a **stop-and-ask against this spec**, not a local fix — a local fix in one round
is exactly the silent divergence §1 forbids.

### D8 — TD-008

`## TD-008: The Pub/Sub Seam as a Frozen Contract with Uniform Provider Selection`, in the
log's own shape (Status / Context / Decision / Rationale / Alternatives / Risks). Decision:
the seam is the single meeting point for two parallel rounds; selection is by name-or-path
through one registry with built-in-vs-loaded invisible above it; config is a typed core plus
an opaque document; failures are one type with a numbered cause. Risks: the freeze buys
parallelism at the price of a stop-and-ask whenever either round finds the seam short.

## Corner cases forbidden

- **F1 — A free-floating count, or any hand-composed per-site ledger, is unrepresentable in
  the spec.** A number carries the command that derives it, inline, or it is not written, and a
  table of counts a machine cannot re-derive is **deleted, not dated**: §10's total, recipe and
  eleven-row table all go, as do this design's own bare counts (**no suite totals**).
- **F2 — A handoff row with no verification method, or a `mechanical` label with no named
  mutation.** Every §12 row is {artifact, check, mechanical | by-reading | by-construction},
  and `mechanical` must name the edit that reddens it. Two of six survived that test (B3).
- **F3 — A guard that cannot fail on the mutation it claims to catch.** A row-count equality
  against a number the test itself holds *is* the held-copy defect this item exists to close:
  forbidden. The append is a **compile** failure, the missing row a **test** failure, and no
  count lives in the test (D4).
- Also unrepresentable, each by construction of §12's text: **F4** a second copy of the
  taxonomy numbers (the spec cites, never restates) · **F5** a doc-presence grep dressed as a
  check (it cannot fail once the heading exists; conditions 1, 2b, 6 name a reader instead) ·
  **F6** any sentence readable as a platform claim ("portable", "both platforms", "CI-green"
  appear nowhere in §12) · **F7** a third, "negotiable" freeze class · **F8** a separate
  handoff addendum — a second contract, and the one that rots.

**Handled residue**

- **H1 — §3.4's unbounded-wait clause stays pinned by no test.** *Why not forbidden:* the
  Windows standard library clamps the deadline inside `wait_for`, so conforming and
  non-conforming implementations are indistinguishable here and a test would pass for the
  wrong reason. Disclosed, and listed in §12 as inherited-unproven.
- **H2 — The receive-side data-sharing defect and the conformance blind spot survive the
  round.** *Why not forbidden:* ruling 2026-09-01 assigned them to PDA-ABI-7; forbidding
  them here would reopen work a ruling closed.
- **H3 — Linux-side correctness is unproven.** *Why not forbidden:* opening the PR is the
  owner's step, so the round cannot run the lanes from inside itself; handled by D7.
- **H4 — Condition 5 has no machine check.** *Why not forbidden:* the only rung-1 move is to
  make a built-in-vs-loaded accessor unrepresentable, and any mechanism for that is new public
  surface in PDA-ABI, the round that adds loading. Handled by §12's frozen list making such an
  accessor a stop-and-ask, and by the `by-construction` label.

## Premises and stop conditions

- **P1 — `core/README.md` can be exported without disturbing the package.** `core` clears its
  `package_id`, so adding a doc to `exports_sources` should be inert. **STOP-AND-ASK** if it
  changes core's package ID or breaks `conan create core`: do not move the table into a test
  fixture — ask, because the disk read is the whole guard.
- **P2 — A second disk-read doc test is welcome here.** Verified precedent:
  `xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`. **If review rejects it**, fall back to
  the header as the published artifact and re-label condition 3 `by-reading` — do **not**
  substitute a grep for the table's presence (F5).
- **P3 — `PubSubStatus` is ten values `0..9` (`status.hpp:67-83`) and §5.1 keeps the published
  set a contiguous prefix.** Verified. D4 part 3 relies on the contiguity, not the ten; a
  status appended before implementation fails the compile — the mechanism working.
- **P3b — the promoted diagnostic exists and fires on the compilers that build `core_tests`**
  (MSVC `/we4062`, GCC/Clang `-Werror=switch`; §12 names those two, and the `core` lane builds
  `core_tests` in the cache on both platforms, `ci.core.yml:55,107`). **STOP-AND-ASK if a
  toolchain there rejects or does not fire the flag** — the fallback is the owner's call
  between a `kLastStatus` sentinel (+1 surface) and relabelling condition 3. Do **not**
  substitute a hand-maintained list and keep the `mechanical` label.
- **P4 — No CI has run on this branch.** Given by the PM. **If a run exists at implementation
  time**, rewrite D7 from that run's result — do not soften or widen the sentence by hand.
- **P5 — Both docs' `make_shared<FastDDSPubSubProvider>()` examples still compile.** Verified
  (`:104`). If that ctor loses its default argument, they change in *that* PR, not here.

## Forcing-test mapping

- **`docs review; handoff checklist complete (§DoD)`** → §12 (D2's table, D3's classes and
  who-may-act sentences, D6, D7) plus the plan's ticked checklist. **Red for the right reason
  today:** §12 does not exist; the six conditions have no recorded verification method; §10's
  own recipe yields 96/21 against its stated 18/8 and its per-site table does not reproduce;
  and `architecture-overview.md:164` describes a `SubscriptionResult` carrying an
  `OwnedSchema`, which the seam has not done since PDA-DEC-3.
- **`Taxonomy.PublishedNumbersMatchTheEnum`** (new, `core_tests`) → D4. **Red for the right
  reason today:** there is no published table and `core/README.md` is not exported, so the
  test fails on the empty read — not vacuously on zero rows. After the change three mutations
  each break it, which is the claim B1 found missing: a **name or number** edited on either
  side → red (part 2); **an enumerator appended** → **fails to compile** (part 1); the `case`
  added but **not the row** → red (part 3).

## Risks / Unknowns

- **Reconciliation scope.** Audited: the *spec*, the two top-level docs, the DoD's own claims.
  Component READMEs' counts stay with their items (C2-5 is PDA-DEC-8's PR, C2-6 is discharged);
  a whole-tree number audit would not fit the budget.
- **Freezing a spec whose Linux behaviour is unrun** — mitigated by D7 stating *and*
  instructing; the residue is Brief decision 1.
- **No coexistence window is created.** Nothing is bridged, delegated or re-exported; the
  retirements landed in PDA-DEC-3/6/7 with no shim — the compile is the check.
- **Two of six mechanical is the honest count** (H4 covers condition 5). A handoff claiming
  "a machine watches this" where none does is worse for two blind teams than one naming the
  reader — which is why the fix was relabelling, not three more guards.
- **No STOP-AND-ASK on the spec.** Both tensions resolve by carrying existing wording rather
  than designing around it: §4.1's *"Widening it … is a stop-and-ask"* goes into §12
  **verbatim** (the earlier paraphrase would have loosened §4.1 and the 2026-09-02 ruling);
  §8.1's falsification sentence is restated **unchanged**, not promoted into a close gate,
  because 2026-09-01 settled that a guard may ship with a recorded blind spot. §10's "their
  claims stand" is about *retired vocabulary* and PDA-DEC-9 owns §10, so narrowing it is this
  item's job. Two **conditional** stop-and-asks stay live: P1, P3b.

## Files-to-touch

- `docs/pubsub-interface-spec.md` — status → frozen; §5.1 cites the published table; §9 gains
  the oracle row; §10 pruned (D5); **new §12**.
- `docs/architecture-overview.md` — §1 principle (registry terms), `:86` and `:163-167`
  (schema arrival), §7.4 gains the registry-selected variant · `README.md` — `:38` principle,
  `:315` narrowed to *loading* with the coupling clause kept (C1-1); `:155` untouched ·
  `docs/technology-decisions.md` — TD-008 · `core/README.md` — the taxonomy section.
- `core/tests/test_status_taxonomy.cpp` — **new** (holds the exhaustive `switch`) ·
  `core/tests/CMakeLists.txt` — source, `FLETCHER_CORE_README_PATH`, and
  `set_source_files_properties(test_status_taxonomy.cpp … COMPILE_OPTIONS /we4062|-Werror=switch)`
  · `core/conanfile.py` — `exports_sources` gains `"README.md"`.
- `plans/PDA-decouple-interface.md` — DoD checklist gains the verification column, ticked
  against §12; tracker row 🟢 · `plans/PDA-decouple-progress-log.md` — the item entry.

## Files-to-delete

- **spec §10's migration total and `grep -E` recipe** — replaced by the retirement claim
  (the compile is the check) — **and its eleven-row per-site count table**: *no replacement —
  a hand-composed post-change ledger, deleted rather than dated (F1, B4).*
- **spec §10's correction archaeology** (two paragraphs recording that "4 files / 19
  occurrences" and "12 construction sites" were each wrong twice) — one sentence replaces
  them; that history belongs in the progress log and the reviews.
- **spec §10's "their claims stand" sentence** — replaced by the narrower statement of what
  PDA-DEC-6 actually inspected.
- **`README.md:315`'s implication that runtime selection is still future** — replaced by the
  narrowed bullet; loading and freedom from linking the provider library stay promised, only
  selection-by-name is struck as shipped (C1-1).
- No tests and no code are retired. Justified: the round's retirements landed in PDA-DEC-3/6/7
  with no coexistence window; this item's product-code footprint is one test, superseding
  nothing.

## Numbers

Declared net lines: **+330 / −100** (re-declared from +360/−70: the review found the adds
figure had slack, and B4 moves ~30 lines of §10's table and header from *kept* to *deleted*;
B1's guard is size-neutral) · new public surface: **0** (one test, one README section over an
existing enum, one packaging line, three CMake lines) · design cycles: 2/2.
