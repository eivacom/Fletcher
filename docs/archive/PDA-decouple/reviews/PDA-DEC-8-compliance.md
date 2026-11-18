# PDA-DEC-8 — architecture-conformance review (step 4a, independent)

Reviewed `git diff e99eaeb..b7b33f3` (7 files, **+710 / −25** derived — see RECORD)
against `plans/PDA-DEC-8-multi-instance.md` (rev 1, APPROVE-WITH-DEBT(6) ×2 cycles),
`plans/reviews/PDA-DEC-8-design-review.md` (both cycles), `plans/reviews/design-debt.md`
§PDA-DEC-8 + cycle 2, `plans/PDA-DEC-8-brief.md`, the oracle
`docs/pubsub-interface-spec.md` §4 third normative item, and
`plans/PDA-DEC-rulings.md` (31 entries; 2026-09-03 is this item's).

**Verdict: PASS-WITH-FINDINGS(4).** No blocking conformance item. Zero product code
changed, the mutation gate is real — I re-ran four of the six rows from scratch and
each reproduced the recorded text — and both the owner's ruling and debt C2-1/C2-3
landed. All four findings are wording/labelling on the *published evidence*, which is
this item's whole deliverable, so they are named rather than waved through.

---

## What I re-derived rather than accepted

**Counts, stated separately and derived.** `ctest --preset conan-release -N` →
**86 ctest entries**, and the full run is **86/86 passed** (24.5 s), with
`-DFLETCHER_CONFORMANCE_XRCE=ON` given explicitly on `cmake --preset conan-default`
and `conformance_xrce` observed to actually run (entry #86, 16.1 s, Passed).
`--gtest_list_tests` per binary: `conformance_fastdds` **29 cases**
(5 `Registry.` + 12 + 12), `conformance_registry` 19, `conformance_inprocess` 11,
`conformance_inprocess_carrying` 12, `conformance_seam_vocabulary` 7,
`conformance_copy_accounting` 7, `conformance_xrce` **27 cases in 1 entry**.
`grep -c '^TEST('` on `subjects/fastdds_main.cpp` = 5 at `b7b33f3` vs 1 at `e99eaeb`
⇒ **+4 cases, 25 → 29**, and 82 → 86 entries. Reported figures check out; entries and
cases are not conflated in the design or the log (they are in one README sentence —
see RECORD). All 29 `conformance_fastdds` entries carry
`RESOURCE_LOCK conformance_fastdds` and `TIMEOUT 180` (P3 holds for the new cases).

**The build under test is the committed tree, not a stale package.** `conan export`
of `core/`, `pubsub/` and `fastdds-pubsub-provider/` reproduces recipe revisions
`91d54d64`, `cd7249b2`, `976d81c5` — identical to the newest cached revisions, and the
installed package folders resolve to exactly those. "Already installed!" was checked,
not assumed.

**Mutation forensics (not in the brief, and decisive).** The Conan cache still holds
the six mutated recipe revisions from 2026-09-03 04:35–04:42, then clean re-exports at
04:51. Each mutated export differs from the clean one in **exactly one source file**
(so each mutation really was applied alone), and the diffs are verbatim the edits the
README describes: `create_participant(0, pqos)` (M1), a function-local
`static DomainParticipant* shared_participant` (M2), `Impl::topics` bound to a
function-local static table (M3), a `static std::map` memo in
`ProviderRegistry::Create` (M4), `out.clear()` deleted from `JoinSegmentsInto` (M5),
`static const uint32_t bound` (M6). When I re-applied M1/M2/M3/M6 by hand,
`conan create` produced **the same recipe revisions** (`48093031`, `cf632ac7`,
`845b8b0b`, `ee5c7c8d`) — the mutations audited are byte-identically the ones built.

**M1 — red *and* the control green, both halves verified.** Independent rebuild and run
of `--gtest_filter='Registry.TwoInstances*'`:

```
fastdds_main.cpp(418): Which is: { "B:0", "A:0" } / Which is: { "A:0" }
  instance A's subscription on the shared topic name did not see exactly A's own row
[  FAILED  ] Registry.TwoInstancesTwoDomainsStayIsolated (2026 ms)
[       OK ] Registry.TwoInstancesOneDomainDoInterfere (279 ms)
[  FAILED  ] Registry.TwoInstancesStayIsolatedUnderConcurrentTraffic (1718 ms)
[       OK ] Registry.TwoInstancesKeepTheirOwnPayloadBounds (1694 ms)
```

Verbatim match to the README row, and **the same-domain control stayed green at
279 ms** — so the red is interference, not an environment fault. (Control crossing on
the clean tree: 269 ms inside the 1500 ms `kSettle`; the log's "~260 ms" is honest.)

**M6 — reddens the bound pair only, checked across the whole binary.** I ran all **29**
cases (not the recorded 4-case filter): exactly one red,
`TwoInstancesKeepTheirOwnPayloadBounds`, with the recorded text
`{ "A:0", "A:1", "A:2" }` vs `{ "A:0", "A:2" }`. Nothing else silently reddened.
Run *alone*, the same mutation reddens the mirror image — `{ "B:0", "B:2" }` vs
`{ "B:0", "B:1", "B:2" }`, "the 65536-byte-bound instance did not receive all three
rows" — i.e. the design's originally predicted direction is reproducible, and the
README's "reddens in either construction order" is sound rather than a leftover.

**M2/M3 — the crash and the clean-environment procedure, checked as behaviour.**
M2 reproduced exactly: `[PARTICIPANT Error] Topic with name : pdadec8/shared already
exists -> ...create_topic`, then `C++ exception with description "FastDDS: failed to
create topic: pdadec8/shared" thrown in the test body` (that message throws
`PubSubStatus::kTransportFailure`, `fast_dds_pubsub_provider.cpp:307` — the typed
refusal is real), then `SEH exception with code 0xc0000005` on the other three.
`C:\ProgramData\eprosima\fastdds_interprocess` held **0 entries before and after**.
I then ran **M3 immediately after M2**, the same ordering the implementer used, and got
its predicted typed refusals — two `kSchemaConflict` ("topic already declared with a
conflicting schema") on the two differing-shape cases and two
`FastDDS: already subscribed to` on the two single-shape cases, no crash. A false
`0xC0000005` from an orphaned segment did **not** occur, so the procedure is followed
in effect and not merely stated. No `MicroXRCEAgent` before or after any run; shm
directory empty at every checkpoint; tree left clean and 86/86 green afterwards.
M4/M5 were not re-run (a `pubsub` rebuild); they rest on the export forensics above.

**C2-3 — no remaining race.** Every `Journal` mutation and read is under `mu_`
(`Record` locks around `push_back`; `Snapshot()` and `Count()` are `lock_guard`ed and
`Count()` is the only thing `WaitForCount` touches); every comparison is against a
fresh `Snapshot()`. All three **absence** assertions pay the full window —
`std::this_thread::sleep_for(kSettle)` after the positive `WaitForCount`s, in the
isolation, concurrent and bound cases — none sample. The concurrent case joins both
publisher threads inside an inner scope, before either `Instance` (hence either
provider) is destroyed, and both thread outcomes come back as `std::string` data
asserted on the main thread after the join. `provider_` is declared **last** in
`Instance`, so it is destroyed **first**, and `~Impl` deletes each `DataReader` before
the journals go out of scope — no listener can reach a dead journal (spec §6 clause 5).

**Converse — what survived that should not.** `Files-to-delete: None`, so the converse
question is whether the two candidates the design said it *keeps* are still there:
`Registry.EachCreateReturnsAnIndependentInstance` (`src/registry.cpp:393`) and
`FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` (`test_profile_document.cpp:536`)
both present. No `GTEST_SKIP` anywhere in the new file (rung 2 item 7). No product
code, no `PubSubProvider` method, no ABI, no config parser, no dependency and no link
line changed; `conformance_registry`'s SDK-free link line is untouched and the only
`CMakeLists.txt` edit is a comment (26 → 27 cases, debt C2-6 — and 27 is what I
derived). Nothing names `FastDDSPubSubProvider` or calls `PayloadBytes()` in the new
cases; handles are base-typed. Domains **154–160** appear nowhere else in the tree and
the topic prefix `pdadec8` appears only in this file and the suite README — no new
collision, and `pubsub-arrow-fastdds`'s pre-existing domain-137 cross-talk was not
touched.

**The ruling.** Design §8, `README.md`, spec §4 clause 3 and the progress log all carry
"**one application on one machine**" and the three exclusions in the owner's terms
(cross-host; vendor process-wide state both instances would set identically; the shared
memory two *separate* processes on one machine use, with locked decision 12 as the
reason). Debt C2-1's split landed: "exchange no rows" now attaches only to the
different-**domains** pair, and the different-**bounds** pair claims each instance
honours its own bound. §4's normative sentence itself is unmodified.

---

## Findings

### F1 (minor) — M5's recorded failure point is mislabelled, and M5 is the row whose whole value is that the label matches the observation

README:
> filtered to these four cases, the forcing case fails at **A's own second publish**:
> *"FastDDS: unknown topic: pdadec8/sharedpdadec8/only-a"*

The landed publish order in `TwoInstancesTwoDomainsStayIsolated` is
`b.PublishShared(0); a.PublishPrivate(1); b.PublishPrivate(1); a.PublishShared(0)`.
The quoted concatenation `pdadec8/shared` + `pdadec8/only-a` is therefore B's shared
publish followed by **A's *first*** publish — the case's second publish overall, not
A's second (A's second is `pdadec8/shared` at the end). The "second publish" phrasing
is inherited from cycle-1 DEBT-1, which was written against the design's §2 ordering
(shared-A, private-A, …); the implementation reordered to put B first and the label did
not follow. The verbatim text is correct and self-disambiguating, which is why this is
minor — but §6 requires the recorded mechanism to match the observation, and this row
exists only to be read.
*Acceptable fix:* one word — "fails at **A's first publish** (the case's second)".

### F2 (minor) — "each turning a named assertion red" is not what three of the six rows do

Spec §4's "As landed" paragraph ("six mutations to product code, **each turning a named
assertion above red**"), the README's intro to the table, and `fastdds_main.cpp`'s
header comment ("each row a minimal edit … that turns a NAMED assertion here red") all
generalise to an assertion. I reproduced M2 and M3: those cases die on an **uncaught
typed refusal thrown from the `Instance` constructor**, before any assertion in the
case is evaluated (M4 is the same class per the record). The design knew this — cycle-1
DEBT-2 rewrote the "Must redden" column to typed refusals — and the README's table
records each mechanism verbatim, so no reader of the table is misled; only the summary
sentence over-claims, and it does so in the one direction this item is fussy about
("it threw" as a result).
*Acceptable fix:* "turning a named **case** red — by a named assertion or by the typed
refusal recorded below", in the three places.

### F3 (minor) — "that pair claims no crossing in either direction" reads as the claim C2-1 struck

Spec: *"That second pair claims **no crossing in either direction**: the bound is part
of the registered DDS type name, so those two instances could not have discovered each
other whatever the registry did."* Same construction in the README and in design §8
("That pair claims **no crossing either way**"). The intended reading is the
disclaiming one (P1b: it makes no such claim, because the outcome is free), and the
following clause supplies it — but the literal English asserts non-crossing for the
bound pair, which is the exact sentence debt C2-1 required to stop being published, in
the artefact third parties read.
*Acceptable fix:* "that pair **makes no crossing claim** in either direction — the
bound is part of the registered DDS type name, so it could not cross regardless."

### F4 (minor) — the suite README's domain-isolation paragraph is now incomplete, and the item's seven domains are published nowhere

`README.md:582` still reads "Isolation: fixed DDS domains 151/152/153 and Agent UDP
ports 2019 … 2119", while `conformance_fastdds` now also joins **154–160**; the new
section names no domain numbers at all. Design §4 makes owning those domains outright a
load-bearing claim ("none used in the tree"), and that paragraph is where the next item
will look before picking a domain. This is the one place the tree contradicts a README
sentence in a way a future item can act on wrongly.
*Acceptable fix:* extend that sentence — "151/152/153, and 154–160 for the
`Registry.TwoInstances*` cases".

---

## Judgements the PM asked for, decided rather than deferred

- **Recording M2's and M5's deviations as observed was right**, and it invalidates
  nothing the design claimed. M2's class (a typed refusal at declaration time, reached
  through the process-wide participant) is what §6 and DEBT-2 predicted; the call site
  moving from `register_type` to `create_topic` is one step inside the same refusal,
  and I reproduced it. M5's scope-dependence was predicted verbatim by debt C2-4. P5
  asks only that a named case redden; all six do. Adjusting the record to match the
  prediction would have been the defect — see F1 for the one label that slipped.
- **Markers as readable strings, not raw bytes, is conformant.** Rung 2 item 6 demands
  "a delivered marker byte in an instance-distinct journal"; `Journal::Record` builds
  the string *from* `data[0]`/`data[1]` and the comparison is a whole-vector equality
  against exactly the markers that instance published, so nothing is weakened. It is
  also what makes M1's red interpretable, which §6 explicitly demands ("a red nobody
  can interpret is not evidence"). A row shorter than its marker records the sentinel
  `"<row shorter than its marker>"`, so a short row reddens rather than vanishing.
- **The 305-line design against a 300 cap is acceptable.** The +5 is C2-2's required
  premise beside P1b plus the landed-numbers line; no unapproved content rode in.
- **Two shape deviations from the design's letter, both harmless.** The helper is a
  class `Instance(registry, domain, bound, shape, tag)` rather than §2's
  `MakeInstance(...)` free function (it must own the journals whose lifetime §7
  constrains, so this is the stronger form); and the publish order is
  B-shared/A-private/B-private/A-shared rather than §2's literal
  "shared-A, private-A, shared-B, private-B". §2 stated two constraints that its own
  literal ordering contradicts — consecutive publishes must alternate instance *and*
  topic name (satisfied: shared→only-a→only-b→shared), and "B publishes **before** A"
  (only satisfied by the landed order). The implementation kept the stronger pair. F1
  is the only consequence.

---

## RECORD (PM corrects in place; not blocking, opens no cycle)

- **Landed line count is +710 / −25, not +705 / −25** (design §Risks last bullet;
  progress log §Numbers). Per-file: 473 + 108/−2 + 22 + 2/−2 + plans 105/−21 = 710/−25.
  The design's "100/−21 these plans and the log" is the 5-line gap; the
  excluding-`plans/` figure **+605 / −4** is correct.
- **README entries-vs-cases slip** (`README.md:362-365` and the
  `Registry.* cases that live in a PROVIDER binary` sub-section): "five more `Registry.`
  entries … in the two PROVIDER binaries" and "(4 more entries, and 1 in
  `conformance_xrce`)". Derived: **24** `Registry.` ctest entries (19 in
  `conformance_registry` + 5 in `conformance_fastdds`) and **6** `Registry.` gtest cases
  outside `conformance_registry`; `Registry.XrceResolvesAsABuiltIn` is a *case* inside
  the single `conformance_xrce` entry, not an entry.
- **Commit message and progress log** say M5 "fails at A's second publish when filtered
  and at **its first** when not". The unfiltered failure text
  (`registry/fastdds-probepdadec8/shared`) is B's shared publish, not A's — the README
  wisely leaves it unattributed there.
- **`plans/PDA-DEC-8-brief.md`** still opens with the pre-C2-1 framing: "on two
  domains, **and with two different message-size limits** — and it is now proved they
  exchange no messages and share no topic declarations **or settings**". The PM's
  `As landed` close line (still `<date>`) is the place to reconcile it with the ruling.
- **Design `Files-to-touch` omits `integration-tests/pubsub-conformance/CMakeLists.txt`**,
  which the commit edits (comment-only, 26 → 27 cases). Sanctioned by debt C2-6 ("fix
  wherever it is cheapest"), and §Risks does book the `2/−2`; only the file list lags.
