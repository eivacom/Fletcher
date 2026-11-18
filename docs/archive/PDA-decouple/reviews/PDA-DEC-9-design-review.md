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

---

# Cycle 2 review (2026-09-03)

Reviewer: independent architecture review, cold context, **cycle 2 of a hard cap of 2**.
Scope as set: did revision 1 close the six BLOCKERs, and did closing them break anything?
Revision diff read (`9c69d23..739a80e -- plans/`). Design 300/300, brief 60/60.
Oracles re-read: `docs/pubsub-interface-spec.md` (§1, §4.1, §5.1, §5.2, §8.1, §9, §10, §11),
`plans/PDA-DEC-rulings.md` (31 rulings: 14 on 2026-08-31, 11 on 2026-09-01, 5 on 2026-09-02,
1 on 2026-09-03), `plans/PDA-decouple-locked-decisions.md`,
`plans/PDA-decouple-interface.md` §PDA-DEC-9 + DoD, `plans/reviews/design-debt.md`.

**Verdict: see the closing section.** Written as I went; the per-blocker audit is first.

## B1 — the guard that can fail: CLOSED, resting on one premise that is only half-observable

Mechanism as designed is sound and I could not break it on paper:

- **`/we4062`** (MSVC) fires only when a `switch` over an enum has **no `default:` label** and an
  enumerator is unhandled — which is exactly the shape D4 part 1 specifies, and the trailing
  `return ""` is a statement, not a label, so it does not suppress C4062 and does keep C4715
  quiet. `-Werror=switch` (GCC/Clang) implies `-Wswitch`, so it does not need `-Wall`. Both
  promote-by-flag routes are real.
- **`core/tests` has no other `switch`** — verified, 0 occurrences across the two existing
  sources (`test_envelope.cpp`, `test_positional_io.cpp`; `core/tests/CMakeLists.txt:6-8`), so
  the narrow-scope claim holds and a per-source-file promotion is the right blast radius.
- **The three claimed mutations do each redden**, given the flag takes: name/number edit → part
  2; enumerator appended → compile failure of `core_tests`; `case` added without the README row
  → part 3, because `rows.size()` is derived from the file and not held.
- **No count and no copy in the test** — confirmed. Part 3 uses `rows.size()`, and contiguity is
  asserted rather than assumed. F3 is now honoured by the guard, not just declared.
- `exports_sources` lacking `README.md` today is **exactly right**: `core/conanfile.py:24-29` is
  `("CMakeLists.txt", "include/*", "cmake/*", "tests/*")`. `package()` (`:58-66`) copies `*.hpp`
  and `*.cmake` only, untouched by the design.

**The residual, and it is load-bearing.** The whole guard hangs on part 1. Trace the append
mutation with the flag *not* firing: appending `kFoo = 10` and touching nothing else leaves part
1 silent (fallthrough returns `""`), part 2 green (all ten rows still match), and **part 3 green
as well** — `StatusName(static_cast<PubSubStatus>(10))` returns `""`, which is what part 3
asserts. So a flag that does not take restores the original B1 defect *in full*, silently, with
condition 3 labelled `mechanical` in a frozen document. P3b states this premise and names the
fallbacks (and forbids the bad one), which is why it is not the unstated-premise blocker class.
But P3b's stop condition — "**does not fire** the flag" — is not observable from a clean build;
only "rejects the flag" is. It is observable only by running the append once. The design's own
restated §8.1 ("a guard nobody has made go red is a guard nobody has measured") asks for that
record, and it costs one throwaway enumerator and one revert.

Filed as **DEBT (C3-1)**, not a BLOCKER: the premise is stated, the stop condition is named, the
fix is a line in the PR record rather than a design change, and demanding it as a gate at the cap
would be a manufactured cycle. But it is the item's single point of failure and the register
should say so in those words.

Two smaller inaccuracies, both DEBT-level:

- The cited precedent `pubsub-arrow/tests/CMakeLists.txt:57-59` is **`target_compile_options` on
  a dedicated `EXCLUDE_FROM_ALL` OBJECT target** (`discard_probe_tu`, `:45-60`), not
  `set_source_files_properties`. The precedent is for *promoting a diagnostic to an error in a
  narrow scope*, which is the load-bearing half, but it is not precedent for the per-source-file
  mechanism the design names. (DEBT C3-2.)
- Source-file `COMPILE_OPTIONS` are **directory-scoped** in CMake. Both the `add_executable`
  entry and the `set_source_files_properties` call must sit in `core/tests/CMakeLists.txt` — the
  design's Files-to-touch does put them there, so this is a note for the implementer, not a
  finding.

## B2 — packaging claim: CLOSED

No packaging claim survives in the design: `exports_sources` is justified only as "the *test's*
input", `package()` is stated unchanged, and D4 says in as many words that nothing claims the
README reaches a consumer's package folder. The brief's NEW interface row now reads "in the
repository beside the code defining the numbers", not "shipped in the core package". Nothing in
the proposed spec text publishes a distribution route.

One residue: the brief's risk line still says "**packaging does not change**" (`:52`) while the
design edits `core/conanfile.py`. Under the design's own reading (nothing reaches the package
folder) that sentence is defensible, but it is the one place a reader could still take a
packaging claim away. Recommend narrowing to "the package's contents do not change". Filed as
**DEBT (C3-3)**, and a NIT-adjacent one.

## B5 — who may act: CLOSED, and §4.1 is verbatim

Checked character by character against the oracle. Spec `§4.1:414-417` reads:

> It is **exactly those two fields** and it is append-only; a later field never changes `Create`.
> Widening it because one protocol wants a setting typed is a stop-and-ask (owner ruling
> 2026-09-02: "Fletcher keeps exactly payload size and domain").

D3 carries that sentence **verbatim** (only the nested quote marks change form). The owner's
2026-09-02 ruling *Protocol-specific settings move into the protocol's own document* backs it
exactly ("Fletcher keeps exactly payload size and domain"). The PM's instruction that it be
restored is honoured.

The hostile question — can PDA-ABI and BIND each append `PubSubStatus = 10` and ship? — is now
closed by wording, and closed at the right rung: an append is itself a **stop-and-ask** and **the
owner allocates**, so no round takes a number alone; §1 already forbids either round changing the
seam; and D4's guard makes the `core/README.md` row arrive in the same change. Two rounds that
both tried it would additionally collide in one file (the README table), though the design does
not claim that and does not need to. Forbidding rather than protocol-building is the cheaper
shape and the design took it.

## B3 — the relabelling: CLOSED on five rows of six, one label still over-claims

Audited row by row against D2's own bar ("`mechanical` **only where a named machine reddens on a
named mutation**", and F2's "must name the edit that reddens it"):

| Row | Label | Earned? |
|---|---|---|
| 1 | `by-reading` | **Yes.** Names the reader, and names `SeamVocabulary` as pinning *representability* without claiming it verifies the wording. The "(7 entries)" count is gone. |
| 2a | `mechanical` | **Over-claims — see below.** |
| 2b | `by-reading` | **Yes**, and the sentence explaining *why* ("an absence grep passes whether or not the form is coherent") is the strongest honesty in the document. |
| 3 | `mechanical` | **Yes**, and it names both reddening edits (compile failure; suite red until the row lands) — conditional on P3b, see B1. |
| 4 | `mechanical` | **Yes.** Re-verified: `provider_registry.hpp:292-297` asserts `std::is_same_v` on `decltype(&ProviderRegistry::Create)` as a whole member-pointer type, with the comment stating exactly why a return-type check is insufficient. This is the model row. |
| 5 | `by-construction (no machine check)` | **Yes, and the honesty is complete.** Checked every other mention: D2's preamble, H4, Risks ("two of six is the honest count"), the brief ("held by the freeze, not a machine"), and the row itself ("**No machine notices** a later round adding one … the frozen-signature assert pins `Create`'s type alone"). Nothing anywhere implies condition 5 is checked. |

**Row 2a.** Condition 2a is "no second schema-wait mechanism exists". The check is a compile plus a
grep the reader runs. Two different mutations, opposite answers:

- *regress to the old mechanism* (drop `SchemaArrival`, return a `shared_future`) → the tree stops
  compiling at a dozen call sites. Mechanical, real.
- *add a `shared_future` convenience **beside** `SchemaArrival`* → **compiles, and nothing reddens.**
  Only a human re-running the grep notices, and nothing in CI runs that grep.

The second mutation is the one the 2026-09-01 *One mechanism only* ruling exists to forbid, and it
is the one a `mechanical` label tells a later round is guarded. Verified the grep's own claim while
I was there: `shared_future` survives at exactly three sites tree-wide in `*.hpp`/`*.cpp`
(`pubsub/include/fletcher/pubsub/schema_arrival.hpp:7`,
`pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp:51`,
`integration-tests/pubsub-conformance/src/seam_vocabulary.cpp:120` — the third outside the row's
stated scope), **all three comments**. So the row's factual claim is true; it is the label's forward
reach that over-claims, by the design's own definition of the word.

Not a BLOCKER, for two reasons I want on the record: cycle 1's own acceptable fix **prescribed**
this label for this clause ("`mechanical` for 'no second mechanism exists' (with the command)"), so
blocking on it now is reviewer-versus-reviewer, not design-versus-oracle; and the fix is one
clause. Filed as **DEBT (C3-5)** with the clause named: either give the row its reddening edit
("re-add a `shared_future` member → the grep returns a non-comment hit") or say what row 5 says —
forward protection is §3's presence in the frozen list, not a machine.

## B4 — the ledgers: the named instances are gone; the **class recurred at a new instance**

Closed as specified. Verified by reading what the design proposes to *write*, not what it says
about itself:

- §10's eleven-row table, its "8 files, 18 constructions" header and its `grep -E` recipe are all
  in **Files-to-delete**, with "no replacement" stated. ✔
- Row 2's absence ledger is replaced by the derivation command **with no tally**, and the row says
  in words "Derive the survivors, do not trust a tally here". ✔ That is the right shape.
- The bare counts cycle 1 listed are gone: no "(7 entries)", no "two historical comments", no "20
  component + integration lanes" (D7 now says "no count written — the lanes are that file's `uses:`
  entries"), and D6's inert guards are a **list of six named guards with no total**. ✔
- The only numbers surviving into §12 are "two of six", which is derivable by counting the table
  immediately beneath it. ✔

**What survived, and it is the same defect at a new address.** `docs/pubsub-interface-spec.md`
§10:838-846 — the paragraph headed *"Consumers of the vocabulary change, which sit ABOVE the
seam"* — is untouched by D5, by Files-to-touch and by Files-to-delete, and reads, **in the present
tense**:

> `SubscriptionResult` and its `shared_future` are consumed by 10 sites outside `provider.hpp` —
> `pubsub/src/subscriber.cpp` (5), `pubsub-arrow/src/subscriber_arrow.cpp` and its header (4),
> `gateway/src/{main,ws_session}.cpp` (3), plus both `test_package` examples and the `pubsub` /
> `pubsub-arrow` test suites.

Three things wrong with freezing that sentence:

1. **It is false about the seam.** `SubscriptionResult` is `{ SchemaArrival schema; }`; the
   `shared_future` was retired by the 2026-09-01 *One mechanism only* ruling. §10 is the one place
   left in the spec that still describes the seam as carrying it — the very misreading D2 row 2a
   was rewritten to prevent.
2. **Its rows do not sum to its own total** — 5 + 4 + 3 = 12 against a stated 10, plus unquantified
   extras. That is the identical arithmetic defect the design diagnoses one section up ("its rows
   do not sum to its own header"), and it is the next miscounted figure of a round that has had
   eight.
3. **After this item it is frozen.** D3's rule is "anything unlisted is `frozen`", and §10 is
   unlisted — so correcting it later becomes a stop-and-ask against a frozen document. The freeze
   is exactly what makes leaving it expensive.

**This is DEBT, not a BLOCKER, and the distinction matters.** The design holds the *right*
position — §10's ledgers are deleted, not dated — and simply did not enumerate this instance of
it; there is no disagreement for a third cycle to settle, no owner question, and no design decision
to revisit. The fix needs **zero new design lines**: extend the existing Files-to-delete bullet to
name it, e.g. "…**and its eleven-row per-site count table, and §10's 'Consumers of the vocabulary
change' ledger**". Filed as **C3-1**, the register's priority item, and it must land in **this** PR
— it cannot be deferred, because the item freezes the document.

**Class recurrence, stated as instructed.** B4's defect class (a hand-composed, present-tense
count in §10 that does not reproduce) has now appeared in both cycles at different addresses. The
lesson is not about wording: the right instruction is **"sweep §10 whole for present-tense claims
and counts"**, not "delete the instances the review named". Anyone implementing C3-1 should read
§10 end to end with that question, not grep for the sentence I quoted.

## B6 — the struck decisions: CLOSED, and all three authorities check out

I checked each citation against the source rather than the design's paraphrase:

- *"a guard may ship with a recorded blind spot — your 2026-09-01 ruling 'Ship the guard, hunt
  elsewhere'"* — **accurate.** The ruling exists under that exact title, its context records that
  PDA-DEC-1's design made falsification a close gate, and its *Applies-to* says the gate "is
  **relieved by this ruling** and by nothing else". A decision offering "required — a guard ships
  with a recorded falsification or it is not evidence" would indeed have reversed it. Striking it
  and restating §8.1 unchanged is the outcome the ruling supports. ✔
- *"neither later round may edit the frozen contract — locked decision 1 and specification §1"* —
  **accurate, verbatim in both.** Locked decision 1: "A later round finding the seam insufficient
  is a **stop-and-ask against that spec**, not a local workaround and not a change landed inside an
  ABI round." Spec §1:59-62 says the same. C1-8's warning is also honoured: option (b)'s
  observations annex — which would have been a locked-decision breach offered as an ordinary
  alternative — is gone rather than relabelled. ✔
- *"who may add a new error cause — the same rule: you allocate it"* — **supported, and correctly
  not presented as a quote.** The chain is sound: a round needing a new cause has found the seam
  insufficient (§1), §5.1's published set is normative spec content, therefore stop-and-ask,
  therefore the owner answers. §5.1 constrains the *shape* of a change ("appended only, never
  reordered or reused") and is silent on who may make one, which is precisely the door D3 now
  shuts. Note for the record that §12 **states** this rule for the first time rather than
  restating it — within remit, since locked decision 1 makes this round the only one that may
  define the seam. ✔

One precision point, filed as DEBT rather than pressed: the 2026-09-01 blind-spot ruling's
*Applies-to* is **item-scoped** (PDA-DEC-1's gate, "and by nothing else"). D6 carries it into §12
as "the standing policy" for two future rounds. The generalisation runs in the safe direction — it
*permits* rather than forbids, and it stops PDA-ABI-7 re-posing a settled question — but it should
be attributed as *PDA-DEC-1's ruling, carried forward as policy by PDA-DEC-9*, not as a round-wide
ruling the owner issued. That is the second time this round an item-scoped ruling could harden
into round-wide precedent. (C3-10.)

## Also confirmed undisturbed (cycle 1's verified findings)

- **96 occurrences / 21 files** headline: unchanged in D5, and it carries its derivation inline. ✔
- **`architecture-overview.md:164`**: re-read — still "the provider returns a `SubscriptionResult`
  containing the publisher's schema as an `OwnedSchema`", and `:86` and `:15` are exactly as D5
  describes. `README.md:38` and `:315` likewise, word for word, including `:315`'s two promises of
  which only selection shipped — so D5 row 5's "keep the coupling clause" (C1-1) is right. ✔
- **design-debt C2-6**, **P3**, **P5**: all still recorded as cycle 1 found them; P5 keeps its
  `:104` citation and its stop condition. ✔
- **All eight cycle-1 DEBT items are genuinely folded in**, each at a named place: C1-1 (D5 row 5),
  C1-2 (D1.2 — "only name and number are machine-compared and the section says so", "who raises
  it" column dropped), C1-3 (D3 — by section number, with §3.4/§3.5/§5.3 named explicitly),
  C1-4 (D3's "contract text is not the test set"), C1-5 (D7's three instructing sentences),
  C1-6 (D4 part 2's non-vacuity assertions), C1-7 (D4's cited-exceptions clause), C1-8 (decision 3
  struck). ✔
- **The durable sentence that replaces §10's table — spot-checked, and it needs one word changed.**
  The *claim* is true: `FastDDSProviderOptions` and `XrceConfig` are declared nowhere and
  constructed nowhere. But the design's phrasing — "every remaining occurrence is a comment or a
  gtest suite name" — is **not** true of a re-derivation: `XrceConfigFor(...)`, a live local helper,
  accounts for 7 of the 36 `*.hpp`/`*.cpp` matches (`pubsub-conformance/subjects/xrce_main.cpp:111,
  122,647,651,852`, `fastdds-xrce-interop/tests/test_interop.cpp:169,733,906,971,1037`). Cycle 1's
  "all 38 occurrences are comments or gtest suite names" was imprecise for the same reason; noting
  it so the record does not harden. Word the frozen sentence as *declared nowhere, constructed
  nowhere — the compile is the check*, and drop the survival clause. (C3-6.)

## Brief decision 1 — genuine, and correctly phrased

Behaviour-visible throughout: option (a) is "state the evidence exactly … tell both later rounds to
treat Linux as unverified, and make a Linux-only difference in seam behaviour a question for you
rather than a local fix"; (b) "hold the handoff unsigned until you open the pull request and the
lanes pass"; (c) "say nothing about platforms". Recommendation (a) with the reason, default (a),
and the only mechanism word (`workflow_call`) is confined to the skippable background line. ✔

The design states the CI situation plainly enough that a downstream reader cannot borrow
confidence from it: D7 writes it positively (local Windows runs plus one WSL compile; the lanes are
`workflow_call` from a `pull_request`-triggered workflow; **no CI has run on
`feature/protocol-driver-abi`**), F6 forbids "portable", "both platforms" and "CI-green" from
appearing in §12 at all, and C1-5's instructing sentences make Linux-only seam failures a
stop-and-ask. The three-rulings citation ("2026-09-01 ×2, 2026-09-03") is accurate — those are the
copy-accounting scope, blind-spot and isolation rulings, and the last of them says in its own text
that it is "the third time this round the owner has chosen a narrow claim stated honestly". ✔

## Budgets

- **Design 300/300, brief 60/60** — both exactly at cap. I checked for squeeze: nothing cycle 1
  valued was dropped (F7/F8, H1–H4, P1–P5, Files-to-delete with its "no tests and no code are
  retired, and here is why" justification, the forcing-test mapping's "red for the right reason
  today" clauses, TD-008, Risks). The +41 lines over cycle 1 all went into the six fixes; neither
  document grew by restating the spec — the only quoted spec text is §4.1's sentence, which had to
  be verbatim, and §1's stop-and-ask.
- **At cap has a consequence, so I sized my asks to it:** every DEBT item below is implementation
  or wording, and the priority one (C3-1) is a **zero-line** extension of an existing bullet. I am
  **not** asking for another guard.
- **New public surface 0 — counted.** One new test TU (`StatusName` lives in the test file, not a
  header), one `core/README.md` section over an existing enum, one `exports_sources` entry, three
  CMake lines. The `kLastStatus` alternative is explicitly declined *because* it would be +1
  against a declared 0. ✔
- **+330 / −100: realistic, with the slack now on the deletions side.** My estimate of the adds is
  ~240 (§12 ~70, the README table ~18, TD-008 ~16, the new test ~100, the spec's small edits ~6,
  CMake/conan ~5, the plan/tracker/log ~25), so +330 still has headroom rather than the
  third-of-cost under-declaration this round saw three times. −100 is generous against ~40 lines of
  genuine deletion unless replaced-then-rewritten lines are being counted on both sides; say which
  at close so the *as landed* delta is not read as a miss.

## Verdict — APPROVE-WITH-DEBT(10)

All six BLOCKERs are closed. B1's mechanism is real and now holds no count and no copy; B2's
packaging claim is gone everywhere; B3's labels are honest on five of six rows; B4's named
instances are deleted outright; B5 carries §4.1 verbatim, names the actor, and shuts the
parallel-append door by forbidding rather than by protocol; B6's three strikes each rest on an
authority I checked and found accurate.

Nothing here warrants a third cycle, and I am not manufacturing one: the one finding with real
teeth (C3-1, §10's surviving consumers ledger) is a deletion the design's own rule already
mandates, needs no design lines, no owner input and no decision — it needs to be *named* to the
implementer, which is what the register is for. The two items an implementer must not skip are
**C3-1** (sweep §10 whole; it is frozen after this PR) and **C3-2** (redden the append mutation
once — the entire B1 guard hangs on a compiler flag whose failure mode is silent green).

Debt appended to `plans/reviews/design-debt.md` §PDA-DEC-9 (cycle 2), items C3-1..C3-10.

