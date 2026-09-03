# PDA-DEC-9 — architecture review (cycle 1 of 2)

Reviewer: independent architecture review, cold context. Design:
`plans/PDA-DEC-9-seam-spec-handoff.md`. Brief: `plans/PDA-DEC-9-brief.md`.
Oracles read: `docs/pubsub-interface-spec.md` (§1, §4.1, §5.1, §8.1, §9, §10),
`plans/PDA-DEC-rulings.md` (all), `plans/PDA-decouple-locked-decisions.md` (all),
`plans/PDA-decouple-interface.md` §PDA-DEC-9 + Definition of done.

**Verdict: NEEDS-REWORK — 6 BLOCKERs, 8 DEBT.** Budget is clean (design 259/300, brief
52/60, new public surface 0, and I could find nothing creeping in). Every BLOCKER's
acceptable fix is a deletion, a relabel, one sentence, or three lines of CMake — none is a
redesign, and none asks for a new guard except the one the item already promises and does
not deliver (B1).

The item is reviewed as a specification: the test applied throughout is "what would a
hostile-but-fair implementer of PDA-ABI or BIND-C#/BIND-Rust, who cannot ask anyone,
derive from this wording?"

---

# BLOCKERs

## B1 — The totality mechanism for the published taxonomy does not exist (D2 row 3, D4, forcing test 2)

The design claims twice, in the two places that matter:

- D2 row 3: "a totality `static_assert` on the last enumerator fails the build if a status is
  appended without a table row";
- Forcing-test mapping: "appending a status without a row fails the build."

D4 names the assert: `static_assert(static_cast<int32_t>(PubSubStatus::kSubscriptionEnded) == 9)`.
That assert **already exists** (`core/include/fletcher/core/status.hpp:82-83`, verified) and it
**cannot fail when a status is appended**: adding `kFoo = 10` after `kSubscriptionEnded` leaves
`kSubscriptionEnded == 9` true. It pins numbering, not totality. There is no `kLast`/`kCount`
sentinel (verified: `status.hpp:32-63` is ten values `kOk = 0` … `kSubscriptionEnded = 9`, no
sentinel), so nothing in C++ counts the enumerators.

The second half — "plus a row-count equality in the test" — cannot close it either. A row-count
equality compares the README's rows against a number the **test** holds. Appending an enumerator
without touching the README **or** the test leaves the count equal and the suite green. That is
the held-copy defect one altitude up: the drift the item exists to stop, inside the guard built
to stop it.

Consequence at the altitude this item is reviewed at: DoD condition 3 is labelled `mechanical` in
the **frozen** spec largely on the strength of this assert, so both later rounds are told a
machine watches the published numbers when the only real machine check is the per-enumerator
numbering asserts PDA-DEC-3 already shipped. A status appended in PDA-ABI drifts from the table
two independent bindings mirror, silently.

**Acceptable fix (cheapest I would approve):** give the test's status→name mapping an exhaustive
`switch` over `PubSubStatus` with **no `default:`**, and turn the unhandled-enumerator diagnostic
into an error on that target only — the tree already does exactly this per-target for the
`[[nodiscard]]` probes (`pubsub-arrow/tests/CMakeLists.txt:57-59` `/we4834` +
`-Werror=unused-result`; `xrcedds-pubsub-provider/tests/CMakeLists.txt:66`), and there is **no**
global `/W4 /WX` or `-Wall -Werror` in this tree (verified), so the flag must be explicit:
`/we4062` on MSVC (fires only when the switch has no `default:`) and `-Werror=switch` on
gcc/clang. Then the sentence becomes true. §12 must also name **which compilers enforce it**,
because this branch has only ever been built locally on Windows (B-context: D7).
If that is judged too clever, the honest alternative is a `kLastStatus` sentinel — but that is
+1 public surface against a declared 0, so it is a Brief question, not a silent change.
Forbidding is **not** cheaper here: this is the item's only new guard, and without totality
condition 3's `mechanical` label is false. What is **not** acceptable is keeping the sentence
"appending a status without a table row fails the build" backed by a hand-maintained list.

## B2 — "It ships with the package" is false as designed (D1.2, D4, and the Brief's only NEW interface)

D1.2: the table "lives in the component that owns `status.hpp`, so **it ships with the package**
and a test can read it off disk." D4: the bindings need the numbers "in prose, **in the package
they consume**." Brief interfaces table: "Published error-number table, **shipped in the core
package** — NEW."

Verified: the design's only packaging change is `exports_sources` gaining `"README.md"`
(Files-to-touch; and `core/conanfile.py:24-29` indeed lacks it today, as D4 claims).
`exports_sources` puts the file in the **source** folder of the cache build — which is what the
*test* needs, and that half is right and load-bearing (the `core` lane really does build in the
cache: `.github/workflows/ci.core.yml:55,107` run `conan create . -o "&:run_tests=True"` on both
platforms). It does **not** put the file in the package: `core/conanfile.py:58-66` `package()`
copies `*.hpp` from `include/` and `*.cmake` from `cmake/`, and nothing else. A consumer of the
`fletcher-core` package will not find `README.md` in the package folder.

The cited precedent does not do it either, and says so in as many words:
`xrcedds-pubsub-provider/conanfile.py:21-26` exports the README with the comment "README.md is
exported **for the TESTS, not for documentation**", and that package's `package()` does not copy
it. So the design imported the precedent's mechanism and attached to it a distribution claim the
precedent explicitly disclaims.

**Acceptable fix:** one of two, both one line. Either add
`copy(self, "README.md", src=self.source_folder, dst=self.package_folder)` to `core`'s
`package()` and keep the claim; **or — cheaper, and preferred — delete the packaging claim** from
D1.2, D4 and the Brief's interface row, and say what is true: the numbers are published in
`core/README.md` in the repository, beside the header that defines them, and a test reads that
file. **Forbidding the claim is cheaper than delivering it**, and a frozen spec must not cite an
artifact by a distribution route that does not carry it.

## B3 — Two of the four `mechanical` labels name no machine check (D2 rows 2 and 5)

The §12 table's last column *is* the item's product, and F3/F4 correctly forbid a doc-presence
grep and a row with no method. But two rows carry `mechanical` over a check that is either
partial or is reading in mechanical clothing — the "merely adjacent" failure.

**Row 2** — condition (DoD 2): "Schema arrival has a C-expressible form; the `shared_future` is a
convenience over it, not the contract." The stated check is that `shared_future` occurs nowhere
plus "the tree compiles with no such member". That mechanically settles the *second* clause — and
in fact over-satisfies it: per owner ruling 2026-09-01 ("One mechanism only") the `shared_future`
is **retired**, so it is not a convenience over anything. Say that, because a PDA-ABI reader
taking the DoD's wording literally will go looking for a convenience wrapper the tree does not
have (verified: the only survivors in code are historical comments —
`pubsub/include/fletcher/pubsub/schema_arrival.hpp:7`,
`pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp:51`, plus a third the design's
stated scope excludes, `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp:120`).
It does **nothing** for the first clause, which is the load-bearing one: whether `SchemaArrival`
has a C-expressible form is exactly what PDA-ABI and BIND each derive
`fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)` from, weeks apart, without talking.
A grep for an absence passes unchanged whether or not that form is coherent.

**Row 5** — condition (DoD 5): "Nothing above the seam branches on built-in versus loaded." The
stated check is that "the public registry has no accessor that reports it" and that "the gateway
names no concrete provider type in selection". Both are **readings of today's tree** (both
accurate — `gateway/src/main.cpp:209-227` registers both built-ins unconditionally and selects
via `registry.Create(ProviderSelector::Parse(...))`, naming no concrete type). No machine notices
a later round adding `bool IsLoaded()`: the frozen-signature `static_assert`
(`pubsub/include/fletcher/pubsub/provider_registry.hpp:292-297`) pins the **member-pointer type
of `Create` only** — verified — and the "and nothing else" restriction on the class's surface
lives in a comment (`:252-254`). Row 5 is the condition PDA-ABI pressures hardest, because
PDA-ABI is the round that adds loading and will feel the pull to expose which kind it got.

**Acceptable fix: relabel, do not build.** Split row 2 into its two clauses — `mechanical` for
"no second mechanism exists" (with the command, per B4), `by-reading` for "the C form is
coherent", reader and date named — and record that the DoD's "convenience over it" clause was
superseded by the 2026-09-01 one-mechanism ruling. Relabel row 5 `by-construction (no machine
check)` and name what actually protects it forward: §12's frozen list, under which §4's registry
surface is `frozen`, so an accessor is a stop-and-ask. Three honest labels out of six is a better
handoff than four optimistic ones, and it costs no new guards. Row 3 becomes honest once B1 lands;
row 4 is genuinely `mechanical` and is the model the others should be measured against.

## B4 — Hand-composed tree ledgers, and bare counts, survive into the frozen spec (D5, D2, D6, D7)

F1 is the right rule and the design breaks it in the same document that states it.

**(a) §10's per-site count table is kept.** D5 says: "Keep the per-site table marked **as of
PDA-DEC-7, historical**." It is a hand-composed post-migration ledger of counts with no
re-derivable command, and it does not reproduce. Re-derived with the spec's own recipe
(`ProviderConfig\s*\{|ProviderConfig\s+[a-z_]+[{;]`, excluding `plans/`, `build/`,
`node_modules/`): `fastdds_main.cpp` **3** where the table says 2; `xrce_main.cpp` **1** where it
says 3; `test_interop.cpp` **4** where its two rows say 3 + 2. The table's eleven rows also sum to
**24 across 10 files** while the header above it says "8 files, 18 constructions" — that header
was PDA-DEC-6's figure and PDA-DEC-7 appended three rows without updating it, which is a second
cause of the §10 breakage that D5 does not diagnose (it attributes the mismatch solely to the
recipe now over-matching). Deleting the header while keeping the table leaves a reader an
un-derivable ledger and no total.

**(b) §12 row 2 carries an absence ledger** — "`shared_future` occurs **nowhere** in
`core/ pubsub/ pubsub-arrow/ gateway/ *-provider/` except two historical comments — verified this
step" — a survival claim, hand-composed, with no command, going into a frozen document.

**(c) Bare counts head for §12**: "the `SeamVocabulary` suite (7 entries)" (row 1), "two
historical comments" (row 2), "20 component + integration lanes" (D7), and D6's "**seven** inert
guards" whose own enumeration reads as ten nouns — "DEC-6's QoS guard, DEC-7's four unwitnessed
keys and its socket-leak probe, DEC-1H's unreached refusal arm, DEC-8's unreachable pair and its
`kOk`-on-null-schema wait" sums to seven only if "four keys" counts as one guard and "pair" as
two. That is the ninth figure of the round, in the document written to stop the first eight.

**Acceptable fix: delete, don't defend.** Drop §10's per-site table along with its total and
keep the one durable sentence the design already wrote ("the retired types name nowhere in code,
and the compile is the check") — which I verified is true: all 38 remaining occurrences of
`FastDDSProviderOptions|XrceConfig` in code are comments or gtest suite names, so there is no
coexistence window and no shim. Replace row 2's absence ledger with the command that derives it.
Then either give each surviving number its command inline or write the arithmetic (e.g. spell the
seven guards as a list, not a total). Per my standing instruction: a hand-composed post-change
ledger's acceptable fix is deleting it and naming the machine check instead.

## B5 — `append-only` names no actor and no allocation rule, and drops §4.1's stop-and-ask (D3)

The two classes are the deliverable that lets two teams act without asking. As written they do not
say **who may act**, and for one element they contradict the oracle.

- **`PubSubStatus` — collision.** D3 says the values are append-only. Nothing says who allocates
  the next number. Two rounds running in parallel, each finding it needs a cause, both append
  `= 10`; each ships that number to an application, and the failure mode is a deployed binding
  reporting one cause as another with no signal. That is precisely the drift the taxonomy exists
  to prevent, arriving through the door the freeze left open. §5.1 and `status.hpp:23-27` say
  "appended only, never reordered or reused" and are equally silent on the allocator.
- **`ProviderConfig` typed core — oracle-wins tripwire.** §4.1 reads: "It is **exactly those two
  fields** and it is append-only; a later field never changes `Create`. **Widening it because one
  protocol wants a setting typed is a stop-and-ask** (owner ruling 2026-09-02: 'Fletcher keeps
  exactly payload size and domain')." D3 carries the "append-only" half and the "never changes
  `Create`" half, and drops the stop-and-ask. A reader consulting §12's list alone concludes a
  typed field may be appended. That loosens §4.1 and the 2026-09-02 ruling: **the spec wins.**
- D3's closing sentence does gesture at this — "§1's stop-and-ask applies to everything above" —
  but then the two classes differ in **no observable way**, which makes the classification
  decorative rather than actionable, and F6's whole point ("no later round decides for itself
  whether a sentence binds it") is unmet by a different route.

**Acceptable fix: one sentence per class, in §12, saying who may act.** `frozen` — nobody; a need
is a stop-and-ask against this spec. `append-only` — the *shape* of any change is constrained to
an append (nothing renumbered, reordered, reused or removed), **and making one is still a
stop-and-ask**: the owner allocates the number or field, so two parallel rounds cannot collide,
and a `PubSubStatus` append carries its README row in the same change (B1's guard). Carry §4.1's
"widening it … is a stop-and-ask" through verbatim rather than paraphrasing it away.
**Forbidding is cheaper than handling** here: no allocation protocol, no reserved ranges — the
append is simply not a thing a round does alone.

## B6 — Brief decision 2, as worded, reverses the owner's 2026-09-01 blind-spot ruling (STOP-AND-ASK)

Brief decision 2 offers "(a) required — a guard ships with a recorded way it was made to fail, or
it is not evidence", default (a), and D6 turns that into spec text: "Under Brief decision 2 this
becomes **normative for both later rounds**: a guard ships with a recorded falsification or it is
not evidence."

The owner has already ruled on exactly this gate, against it, in this round. 2026-09-01, *Ship the
guard, hunt elsewhere*: PDA-DEC-1's design "made the falsification a close gate — clause 6 had to
go red … or 'the item is not done'. It did not go red, twice", and the ruling relieved the gate,
closing the item with the blind spot and the evidence written into the suite README. §8.1 already
carries the principle in its non-absolute form ("a live negative control ships with it: a guard
nobody has made go red is a guard nobody has measured"), scoped to this round's oracle.

So option (a) as worded would, if defaulted into the frozen spec, forbid the very outcome the
owner chose — and it would bind PDA-ABI-7 first, since that item inherits a defect whose
falsification is known to be unreachable from the existing harness (2026-09-01 blind-spot ruling;
`gateway-fastdds-ts` is the only harness that reproduces it). A decision put to the owner must not
hide that one option reverses a prior ruling of theirs.

**Acceptable fix:** re-pose decision 2 with the escape hatch the ruling itself established — "a
guard ships with a recorded falsification, **or with the recorded reason it could not be
reddened**, as PDA-DEC-1 did" — and write *that* into §9/§12, or strike the decision and restate
§8.1's existing sentence unchanged. Either is one clause. Marked **STOP-AND-ASK** because the
owner needs to see the tension before a default lands.

---

# What I verified about the tree (do not re-derive)

Against a round with eight miscounted figures, these are the ones I checked, and the design is
right about almost all of them. Bounded commands only; `build/` and `node_modules/` excluded
throughout; I did not run `ctest -N` or `--gtest_list_tests`, and the design writes no suite total,
so nothing turned on them.

- **§10's live migration figure: exactly right.** The spec's own recipe over the tree excluding
  `plans/` returns **96 occurrences across 21 files** (docs/ contributes 0, so the design's scope
  and mine agree exactly) against a stated 18/8. The diagnosis is right too: the matches now
  include `README.md` prose, `.test.ts` files and `provider_registry.hpp` itself, because after
  the migration `ProviderConfig` is how everything is configured.
- **`architecture-overview.md:164`: right, and worse than reported.** ":164" says "the provider
  returns a `SubscriptionResult` containing the publisher's schema as an `OwnedSchema`"; the type
  is `struct SubscriptionResult { SchemaArrival schema; }` (`pubsub/.../provider.hpp:39-41`), and
  no `OwnedSchema` member appears in its history as described. ":86" ("returns the schema
  automatically in the `SubscriptionResult`") is loose rather than false.
- **`README.md:155`: already correct, verified** — it names `ProviderRegistry`, runtime selection
  by name, and the typed core plus document. Leave it alone, as the design says.
- **P5 holds**: both docs carry `std::make_shared<fletcher::FastDDSPubSubProvider>()`
  (`README.md:142`, `docs/architecture-overview.md:258`) and the ctor still has its default
  argument (`fast_dds_pubsub_provider.hpp:104`).
- **design-debt C2-6 is discharged**: `integration-tests/pubsub-conformance/CMakeLists.txt:306-308`
  already reads 27 = 24 clauses + 1 `Registry` + 2 `ConformanceXrce`.
- **P3 holds**: ten values `0..9`, ten per-enumerator `static_assert`s at `status.hpp:67-83`,
  exactly as cited.
- **Row 4's guard is the real thing**: `provider_registry.hpp:292-297` asserts the member-pointer
  type of `Create`, so a defaulted extra parameter or a dropped `const` fails the build.
- **No coexistence window**: all 38 remaining `FastDDSProviderOptions|XrceConfig` occurrences in
  code are comments or gtest suite names (`xrce_dds_pubsub_provider.cpp:388,467,506`,
  `internal/xrce_document.hpp:16,76,131`, the public header's `:8` comment, and test suite names).
- **D7's CI shape is right**: `.github/workflows/ci.pr.yml` is `pull_request`-triggered (`:7`) and
  calls **9 component + 11 integration** reusable workflows = the 20 lanes D7 claims (I counted
  the `uses:` entries at `:310-484`; `setup-devcontainer` at `:294` is an image build, not a lane).
  Whether a run has ever happened on the branch I could not check and did not try — P4 states it
  as given, with the right stop condition.
- **Precedent P2 holds exactly as cited**: `xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`,
  run-time disk read, `#error` if the define is missing, `ReadWholeFile` returning empty for the
  caller to assert on (`test_xrce_document.cpp:61-78`).
- **`core_tests` exists and runs in the cache on both platforms** (`core/tests/CMakeLists.txt:6-15`;
  `ci.core.yml:55,107`).

*Not verified, and I say so plainly:* that no CI run exists on this branch (P4, given by the PM);
that `conan create core` is unaffected by the new `exports_sources` entry (P1, correctly carried as
a premise with a stop-and-ask); the `SeamVocabulary` count as *ctest entries* (I counted 7
`TEST(SeamVocabulary, …)` cases in `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp`
and nowhere else — cases, not entries; the design's word "entries" is fixed as a NIT).

---

# Notes on things I looked at and did **not** find fault with

Recorded so cycle 2 does not re-litigate them:

- **F7 is right**: the handoff belongs in the spec, not in a separate addendum. That follows
  directly from the 2026-09-01 split ruling and locked decision 1, and a second document would be
  the one that rots.
- **H1/H2/H3's *why not forbidden* lines are honest and correct.** H1 (unbounded wait unpinned)
  is a genuine unrepresentability-of-evidence argument, already carried in
  `schema_arrival.hpp:116-121` and measured both ways; H2 defers to the two 2026-09-01 rulings
  that assigned those blind spots to PDA-ABI-7; H3 is a fact about the trigger topology.
  None of the three is a case that could have been forbidden at the door.
- **Premises are unusually well covered** — P1..P5 each name a stop condition, and P4's ("rewrite
  D7 from the run's actual result; do not soften or widen by hand") is the right shape. I could
  not find an unstated premise beyond the build-flags one folded into B1.
- **Budget**: design 259 lines, brief 52, new public surface 0, declared **+360 / −70**. TD entries
  in `docs/technology-decisions.md` run ~16 lines each (TD-001…007 at `:7,23,39,55,71,87,103`), so
  TD-008 is small; the plausible total is ~280-330 added, i.e. the declaration has slack rather
  than the third-of-cost pattern this round saw three times. I am **not** about to demand more
  guards at close: B1 is the one guard the design already promised, and B3/B4/B5 are relabels and
  deletions.
- **Files-to-delete is present and real**, and its last line ("no tests and no code are retired,
  and here is why") is the right shape for a docs item.
