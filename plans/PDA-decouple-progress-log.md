# PDA-decouple — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See [PDA-decouple-interface.md](PDA-decouple-interface.md) for
the tracker and [../docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
for the oracle.

## Round history (2026-08-31 / 2026-09-01)

Designed 2026-08-31 as a **single** round `PDA` covering the seam and the C ABI
together. The design session was lost before writing any artifact and was recovered
from its transcript (`c:\tmp\PDA-abi-design-recovered.txt`).

**Split by the maintainer on 2026-09-01** into **PDA-decouple** (this round — the
seam) and **PDA-ABI** (the C boundary below it), so that PDA-ABI and
BIND-C#/BIND-Rust can then run **in parallel and meet only at the seam spec**. The
split also corrected a real error in the original design: it had claimed the driver
ABI and the binding ABI must share a literal buffer struct and "cannot be designed
independently". That was wrong — a built-in provider hands the encoder a plain C++
`WriteBuffer&` with no ABI object in the path, so a binding defined over
driver-ABI types would have worked only for loaded drivers, breaking the
built-in/loaded transparency the maintainer required. Both C boundaries now mirror
the **seam** independently.

Two citation errors from the recovered design were corrected in the specs and must
not be reintroduced:

- The MCU `<75 KB` Flash figure comes from **TD-004** (rationale) and **TD-007**
  (context) — *not* TD-005, which is schema transport via companion topics.
- The recorded dynamic-linking skepticism is **TD-007's** "alternatives
  considered", and it is narrower than it reads: it rejected dynamic linking as a
  way to make **Arrow C++** optional in the edge/server tier split, not plugin ABIs
  in general. The counter-argument PDA-ABI owes is correspondingly narrower.

One estimate was also corrected: the Fast DDS config blast radius was recorded as
"4 code sites and 2 docs". Measured, it is **4 external consumer files / 19
occurrences**, plus **39 provider-internal occurrences across 7 files** (24 of them
in the provider's own QoS test TU) and **10** in XRCE. The external *file* count
was right; the total churn was understated.


## Merged `feature/fastdds_modernization/19645` (2026-08-31, pre-round)

Mads Christiansen's Fast DDS modernization (5 commits, 73 files, +5.5k/−0.8k) merged into
the round branch **before** PDA-1, at the maintainer's request, because it reworks the two
types the ABI mirrors. Base was `f779c2f`; it predates both `#124` (HARD) and `#125` (GIR),
so 12 files conflicted across 34 hunks.

**What it brings that matters to this round.** `WriteBuffer` becomes a **window plus a refill
hook** — appends are non-virtual and write straight into `{data_, capacity_, pos_}`, and only
`AppendSlow`/`AppendZerosSlow` are virtual; `VectorWriteBuffer` owns its bytes and `Finish()`
hands them over; bounds are checked by subtraction so a hostile length cannot wrap. **Spec §4
was rewritten against this**: the ABI's buffer is now a window + `grow`/`grow_zeros`, which
costs one crossing per refill instead of one per append, so the merge strictly improved the
shape PDA-3 has to specify. Also arriving: a bounded, plain DDS sample type (so Fast DDS can
data-share), loan-based publish and receive, `payload_bound.hpp`, a `shared_lock` publish path,
and `<Class>::DecodeInto` for allocation-reusing decode.

**Conflict resolutions worth knowing.** The recurring shape was "HARD added `[[nodiscard]]` and
richer diagnostics; the branch changed signatures to const-ref and improved the prose" — kept
both. Three needed real judgement:

- **`protoc/src/generator.cpp` took main's IR side.** The branch patched the flat emitters GIR-3/4
  deleted; a textual merge would have resurrected ~1400 lines of superseded code. Its three
  generator improvements were ported onto the IR emitters instead: `DecodeFrom_`/`DecodeInto`
  (already half-merged and non-compiling — it called the retired `EmitFieldDecode`) and the
  **nullable-decode reset**, which is a correctness prerequisite for `DecodeInto`: without
  clearing a null field, a reused row keeps the previous row's value. Four sites in
  `cpp_backend_decode_visitor.cpp`. Still **not** ported, deliberately: `kNumArrowFields` (avoids
  building a schema at encode time) and `IsBulkCopyableScalar`/`WriteFixedArray` (one memcpy for a
  repeated scalar run) — both pure performance, both wanting the parity oracle to prove
  wire-identity, and both fine as separate work.
- **#60 was restored, differently.** The branch's rewrite dropped HARD's
  `LastSerializeError`/`ClearSerializeError` sink, so `Publish` only logged a failed encode where
  HARD requires it to throw with the cause (H-INV-2), and `test_fast_dds_pubsub_provider.cpp` has
  an explicit test for that. Main's mechanism could not simply be re-applied: it relied on a sink
  on the shared type instance being safe because `Publish` held the lock **exclusively**, and the
  branch publishes under a **shared** lock, so concurrent publishes would race it. The diagnostic
  now rides on the per-call `PublishData` — no shared state, no race, and it works with
  concurrent publishes. `serialize` still never rethrows (H-INV-3). Reconciling it with the
  branch's own oversized-row tests required distinguishing the two failures: a `std::overflow_error`
  from `FixedWriteBuffer` is a **capacity** outcome of a bounded type and is dropped and logged;
  any other encoder exception is a **defect** and is recorded so `Publish` throws. HARD's tests
  and the branch's now both pass.
- **HARD-4's lock discipline was preserved** where the branch had regressed it: teardown still
  takes no lock (its `~Impl` honours this — the rationale comment was re-attached), and the XRCE
  schema path keeps main's full-sequence `try` (the branch's narrower one left `MakeSharedSchema`
  and `promise.set_value` outside it, both of which can throw through C frames) and its
  copy-to-locals before dispatch.

`internal/fletcher_topic_type.hpp` was deleted (dead — `FletcherSamplePubSubType` replaces it) and
HARD's two `FletcherTopicTypeTest` cases re-anchored onto the new class; the second, which existed
only to prove a *shared* sink is cleared before each write, was retired and replaced by one pinning
that the per-publish diagnostic cannot leak between publishes. `docs/archive/HARD/` was left
untouched — the branch had edited the pre-archive copy, and an archive should stay a faithful record
of what HARD found; the parts still true are recorded here instead.

**One genuine defect found in the incoming branch, and fixed.** `integration-tests/gateway-fastdds-ts`
went from green on `main` to failing 2 of 4 tests on most runs: a cross-process subscriber that joins
after the rows were published received only part of the `TRANSIENT_LOCAL` backlog, often just the
newest sample, with no error anywhere. Root-caused to **receive-side data-sharing** — the writer side
is blameless, and `data_sharing().off()` on the default reader QoS restores reliable delivery while
leaving `loan_publish` and the zero-copy publish path intact. Evidence, alternatives and the cost
(zero-copy receive, by default) are in `fastdds-pubsub-provider/README.md`, which previously argued
that turning data-sharing off could only ever remove a correct choice — corrected there with the
measurements. `OrderedDelivery`'s trim was ruled out (it warns when it drops, and never fired).

Two things this exposed that outlive the merge, both PDA's business:
- **The provider's unit suite cannot see data-sharing at all.** It is single-process, so Fast DDS
  serves it over intra-process delivery, which bypasses data-sharing entirely — including the
  branch's own "forced data-sharing" test, which proves only that the endpoints are *accepted*.
  `gateway-fastdds-ts` is the sole cross-process coverage. PDA-1's conformance suite should have a
  cross-process subject; this is a concrete instance of the "three providers never mechanically
  compared" problem that motivates it.
- A regression test now pins late-joiner replay under the **shipped** default QoS
  (`DefaultQosReplaysEveryRetainedRowToALateJoiner`). It passes in-process and would have caught
  the class of bug had a cross-process subject existed; the branch's data-sharing tests all use
  `BoundedOptions()` (KEEP_LAST 10) and subscribe *before* publishing, so neither the defaults nor
  the late-joiner direction was covered.

**Verification.** All 7 components green (core 28, arrow-bridge 61, pubsub 19, pubsub-arrow 16,
fastdds 70, xrce 11, protoc 99 + 3). Integration: protoc-arrow-bridge 91, protoc-coverage 20,
pubsub-arrow-fastdds 4, fastdds-xrce-interop 1, gateway-end-to-end 21, gateway-fastdds-ts 4/4
across **3 consecutive runs**. Accepted skips unchanged (tsc/rustc absent, gated
`RegenerateGoldens`). **GIR's `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` is green against the
committed goldens**, so the `WriteBuffer` rework and the decode port moved **no wire bytes** —
decision 12 and GIR's decision #2 both hold. clang-format (18.1.3) and the license-header gate clean.

Pre-existing, untouched: `gateway-fastdds-ts`'s binary lookup does not know the MSVC multi-config
layout (`build/gateway_build/Release/gateway.exe`, not `build/Release/gateway_build/`), so it needs
`GATEWAY_BIN`/`FASTDDS_PEER_BIN` on this box. The merge changes nothing in that directory.

## Starting baseline (2026-09-01, `e1868b2`)

Full suite run on the Windows box before any round item, so a later failure can be
classified as inherited or introduced. **Everything green.**

| Subject | Result |
|---|---|
| core / arrow-bridge / pubsub / pubsub-arrow | 28 / 61 / 19 / 16 |
| fastdds-pubsub-provider / xrcedds-pubsub-provider | 70 / 11 |
| protoc | 99 unit + 3 test_package |
| protoc-arrow-bridge / protoc-coverage | 91 / 20 |
| pubsub-arrow-fastdds / fastdds-xrce-interop | 4 / 1 |
| gateway-end-to-end | 21 |
| gateway-fastdds-ts | 4/4 across **3 consecutive runs** |

`ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` green against the committed
goldens — the wire contract is intact at the baseline, which is what decision 13
must keep true. clang-format 18.1.3 and the license-header gate clean.

Accepted skips at baseline: tsc/rustc-dependent checks (toolchain absent), the
gated `ParityOracle.RegenerateGoldens`, XRCE tests needing a MicroXRCEAgent.
`gateway-fastdds-ts` needs `GATEWAY_BIN`/`FASTDDS_PEER_BIN` on this box because its
binary lookup does not know the MSVC multi-config layout — pre-existing, and a
remediation owed before round close.

**Anything outside this table is introduced by the round, not inherited.** In
particular, intermittent row loss in `gateway-fastdds-ts` is the receive-side
data-sharing defect signature (see the merge entry above), never flake.

<!-- Entries appended below by the round runbook -->

## PDA-DEC-1 — Conformance suite for the delivery contract (2026-09-01)

**Forcing test:** `ProviderConformance.SchemaBeforeDataAcrossHandoff` → 🟢 (⚪→🔴→🟢).
Red for the right reason first: XRCE refused every subscriber-first declaration.
**Design:** `plans/PDA-DEC-1-conformance-suite.md` · **Brief:** `plans/PDA-DEC-1-brief.md`

**What landed:** spec §7 plus §6 clause 1 are now 12 executable clauses over five
subjects — InProcess, Fast DDS and XRCE local, Fast DDS and XRCE cross-process, the
latter publishing from a child process over a tagged line pipe. A new protocol
inherits the suite by registering a subject, not by copying tests.

**Interfaces:** NEW `integration-tests/pubsub-conformance/` + CI lane · NEW
`fletcher::InProcessPubSubProvider` (public surface 1) · CHANGED spec §7 clause 3:
conflicting re-declaration **must** be refused, not may · DELETED the gateway's
private loopback provider.
**Deleted:** `retired: FastDDSPubSubProviderTest.DefaultQosReplaysEveryRetainedRowToALateJoiner`
— replaced by `LateJoinerBacklogIsAllOrNothing` in-process *and* cross-process, its CI
coverage landing in the new lane in the same change. Also the 68-line gateway-local
provider, and the word "may" in spec §7.3 and `provider.hpp`.

**Divergences fixed in-round (3 distinct, 7 clause×subject pairs), no wire bytes moved,
nothing pinned:** loopback silently overwrote a conflicting schema; XRCE refused
identical re-declaration; XRCE refused every subscriber-first declaration. Plus a
latent defect: the XRCE provider never declared `ws2_32`, resolving only because
consumers also linked Fast DDS.

**Reviews:** design `APPROVE-WITH-DEBT(6)` (3 BLOCKERs in cycle 1) · compliance `PASS`
(5 blocking resolved) · code `0 blocking / 10 should-fix resolved / 3 nits accepted`.
Two defects they caught and tests did not: a use-after-free where a background delivery
could `Record` into a destroyed stack-local collector, and a `SIGPIPE` that killed the
test binary instead of returning the documented `nullopt`.
Full: `plans/reviews/PDA-DEC-1-{design-review,compliance,codereview,verification}.md`.

**Verification:** conformance 36/36 twice; full suite green at `a963211`. Residuals:
`pubsub-arrow-fastdds` 1–2 of 4 under 28-way parallelism — **pre-existing** (shared
domain 137 + shared topic names, `-j1` green, absent from this diff) — plus inherited
toolchain/Agent skips. **Every component package was cached from a `run_tests=False`
build and needed a forced rebuild before its unit suite ran at all.**

**The falsification gate was never met.** Clause 6 had to go red against a provider
with reader-side data-sharing re-enabled; it did not, twice — the harness-shape
hypothesis refuted by the `gateway-fastdds-ts` control (same shape, does reproduce),
the sentinel hypothesis by measurement after removal. Owner ruled: ship the guard,
blind spot documented, defect owned by **PDA-ABI-7**, which now carries the evidence
handoff. `gateway-fastdds-ts` is the only harness that reproduces it — do not weaken it.

**Close gate:** PASS. **Cycle meter:** design 2/2 · fix 2 · launches 3/5 · owner touches 3.
**Numbers:** declared +1750/−115 · actual **+3652/−201** (+108%, under-costed) · surface 1.

**Owner decisions:** re-declaration refused by every protocol (spec amended here); suite
ships with the blind spot. Both verbatim in `plans/PDA-DEC-rulings.md`.
**PM plan-shape calls:** the `InProcessProvider` lift moved here from PDA-DEC-5 (a type
in an anonymous namespace inside a `main.cpp` is unlinkable), so PDA-DEC-5 shrinks to
registration; the loopback ships schema-**less**, PDA-DEC-3 owning its schema arrival
and the 6th subject; the 415-line pipe helper accepted over the ~250 premise, whose
remedy collides with locked decision 12.
**Records corrected:** spec §0/§10 no longer site the loopback in the gateway; §7.2 now
denies being a coverage claim; declared-vs-actual annotated; config baseline harnesses
named (order-ambiguous, I mis-read them once); review files must be `-compliance.md` or
the close gate cannot see them.

---

## PDA-DEC-2 — Copy-accounting oracle (2026-09-01) 🟢

**What landed.** Zero-copy stops being prose. Suite `CopyAccounting` in the PDA-DEC-1
harness decides copying by **address provenance** — encode-window base vs the pointer the
subscriber callback receives — scored by a pure `Judge()`. Three in-process subjects
(`SeamProbe`, `InProcessLoopback`, `InProcessViaPubSub`) plus a **live negative control**
`StagingIsCaught`: a deliberately-copying provider the same `Judge()` must convict.
Allocation counting was rejected in design (blind to Windows DLL CRTs; pool reuse hides
copies). New public surface: **1**, `WriteBuffer::Data()`.

**The finding that justified the review round.** `BorrowedAttachmentCostsExactlyOneCopy`
shipped in cycle 1 asserting a **structural constant**: the harness itself made the
copy, so the count was always 1 — it could not reach 0 when PDA-DEC-3 lands, nor 2 on
a regression. Both reviewers found it independently; compliance proved it by swapping the
provider and watching it stay green — PDA-DEC-1's lesson recurring inside the item built
to prevent it. Fixed: `Publish` now builds the `Blob` itself, with a caller-owned blob
riding along. Five mutations, each rebuilt and run, re-derived by the reviewer rather than
taken on report: provider swap → red; copy removed → red at 0; extra copy → red at 2;
window recycled → red as "P5 VIOLATED"; counter inert → red on `GrowableProbe`.

**Residual, booked not escalated.** The pin goes red at 0 for any removal a *provider* can
express, but cannot see PDA-DEC-3 adding a **parallel** borrowed-blob type beside an
untouched `Blob`. A SFINAE probe for a guessed future ctor was rejected — a wrong guess
keeps copying silently. Booked as a PDA-DEC-3 obligation; the re-check endorsed that call.

**Verification.** `CopyAccounting` 7/7; full suite green on every target at baseline;
`pubsub-conformance` **43/43** with XRCE ON and a live MicroXRCEAgent (`conformance_xrce` ran,
not skipped); `gateway-fastdds-ts` 4/4 ×3, no row loss; `pubsub-arrow-fastdds` 4/4 at `-j1`
(pre-existing domain-137 cross-talk). **Residuals, logged not fixed:** `LoanForDelivery`
shares the row buffer's 4-slot rotation (loan liveness rests on an uncounted publish budget,
unreachable today); `EncodeAccounted` misses a same-base realloc — report-only undercount.

**Two false-green traps in our own verification, both now fixed in the config.** (1) `cmake
--preset conan-default` does **not** reset a cached `FLETCHER_CONFORMANCE_XRCE=OFF`, so the
first gate run silently dropped the XRCE entry and both XRCE subjects — 42 entries, all
"green"; `-DFLETCHER_CONFORMANCE_XRCE=ON` is now explicit. (2) `-o run_tests=True` **can be
a no-op**: every `package_id()` drops the option, so a cached binary is reused and ctest
never runs — five packages said "Already installed!" with zero fresh evidence. Also: 35
per-clause entries is **correct** — `CallbackNeverSeesNullSchema` is absent from the
schema-less `InProcessLocal` by design, gated at link; an agent mis-called it a missing
test and the tree disproved it.

**Close gate:** PASS. **Cycle meter:** design 1/2 · fix 1 · launches 2/5 · owner touches 1.
**Numbers:** declared +560/−5 · actual **+1182/−12** (+111%) · surface 1 as declared. Both
reviews judged the overrun *not* scope creep (3rd subject owed by DEBT-1; fix-cycle growth
is the blocking fix + 3 should-fixes); the implementer's "it's doc-comment" account was
wrong — 493 lines were code, and the code reviewer counted them.

**Owner decisions (verbatim in `plans/PDA-DEC-rulings.md`, entries 20–22):** refill
movement permitted and published as a number; the guard claims the interface, not the
transport; the receive-side copy pinned at exactly one — all three answered with the
recommendation. The design raised a spec-vs-ruling tripwire on refill; the review found
it was not real — §3.1 clause 1 sanctions movement inside a refill, and "a copy
anywhere" was the ledger's editorial gloss, not the owner's prose.

**Records corrected in place (no fix cycle):** the plan said the oracle "counts copies"; the
design's `CopySubject` was stale and my first correction of it was *also* wrong (it is a
`std::function` factory); no as-landed figure; the window-intact comment overclaimed. In the
evidence file the known-accepted `pubsub-arrow-fastdds` line was restated as its authoritative
`-j1` result so the gate's token scan would not read a documented non-regression as a failure
— verbatim run preserved as `verify-PDA-DEC-2-raw.txt`.

---

## PDA-DEC-3 — The crossing vocabulary (2026-09-01) 🟢

**What landed.** A vocabulary two ABI rounds can derive from without consulting each other.
`Blob` becomes an owner-plus-span triple, so borrowed transport memory crosses without a
copy — the copy PDA-DEC-2 pinned at one is gone. Schema arrival becomes `SchemaArrival`/
`SchemaResolver` with a **typed** outcome keeping "schema-less" (`kOk`+null, reserved for
that alone), "pending" and "subscription ended" distinct; the first design collapsed all
three into one null, deleting a signal `ws_session.cpp` branches on today. Failure collapses
to one `PubSubError` over a `static_assert`-pinned `PubSubStatus`. All three providers, the
gateway and the Arrow tier were rewritten to it; the five retirements are simultaneous and
source-breaking. The loopback gains `SchemaCarriage`, giving the 6th conformance subject.

**Design took both cycles.** Four blockers in cycle 1, all closed in cycle 2, no escalation owed.
The budget blocker was mine: waiver granted rather than the design's 3a/3b split, which would
have stood up the coexistence bridge rung-1 item 9 forbids. Review settled premise P2
(`SharedSchema` needs no reshaping) and confirmed the vocabulary is genuinely C-expressible —
the two C boundaries never exchange structs; each wraps the same `Blob`.

**Two vacuous guards here, the second invisible from reading.** The forcing test's ownership
half passed with or without a real owner: first because the test kept the arena alive, then —
once fixed — because it compared retained bytes against `published_data`, the *same buffer*
when provenance holds (`memcmp(p, p, n)`). It now compares against a harness-owned
expectation, and `Arena` poisons freed slots with `0xDD`; without that poisoning the reviewer
measured the bystander mutation goes **wholly undetected**, the freed arena surviving intact
in the MSVC release heap. Verified at 20 runs per configuration.
**A third guard was deleted, not repaired.** The huge-finite-timeout assertion never read its
timeout, and a corrected one *still* could not falsify the clamp — this MSVC's `wait_for`
clamps internally. The clamp stays (it defends libraries that do not); the rule is written in
four places including spec §3.4, which says it is pinned by no test and why a test would be
worse than none.

**Verification.** Full suite green everywhere. `pubsub-conformance` **62/62** with XRCE ON and
a live Agent (`conformance_xrce` ran, not skipped); `gateway-fastdds-ts` 4/4 ×3, no row loss;
`pubsub-arrow-fastdds` 4/4 at `-j1`. **Reviewers corrected both ways:** compliance's "XRCE-ON
is 71" was a phantom (one Agent, one UDP port, so one ctest entry — I verified 62 with
`ctest -N`); it accepted that and reported one of its own mutations inert, the harness linking
the *packaged* library. The code reviewer withdrew its own collapse suggestion on measurement.
Both counted the overrun themselves, as PDA-DEC-2 taught.

**A fourth false-green trap, found by the gate run and now fixed in the config:** neither
gateway harness builds its own C++ binary, so `npm test` alone runs whatever `gateway.exe` is
in `build/`. Both predated this item's own gateway rewrite (one by ~9 h), so a literal run
would have reported a convincing green over pre-rewrite code. The config now builds both
first. Re-confirmed too: `-o run_tests=True` was a no-op for **6 of 7** components this run.

**Close gate:** PASS. **Cycle meter:** design 2/2 · fix 2 · launches 3/5 · owner touches 6.
**Numbers:** declared +950/−350 · actual **+3338/−1113** code (excl. `plans/`), ~1071 of them
genuinely new code — judged real scope, not padding. Surface **8**, ratified (waived 7 +
`TranslateSeamFailure`): step 4 pushed `PubSubStatusName` and `EnvelopeAttachmentCount` into
`internal/`, the latter deleting an unchecked invariant with it. `PubSubStatusName`'s
"mechanically required" claim was false — zero product callers — and was withdrawn.

**Owner decisions (verbatim, ledger 23–25):** a live subscription holds its schema mode
rather than switching mid-stream (gateway `schemaIpc` unchanged); one error type with a stable
numbered cause; one waiting mechanism, the C++-only future retired not deprecated.

**Records corrected in place (no fix cycle):** `fastdds-pubsub-provider/README.md` was the last
place presenting the retired `shared_future` API as current; `kEffectivelyForever` was described
three ways, the public header justifying the threshold as the overflow point when it sits far
below it. Evidence: the known-accepted `pubsub-arrow-fastdds` line restated as its authoritative
`-j1` result so the gate's token scan would not read a non-regression as a failure — verbatim
run preserved as `verify-PDA-DEC-3-raw.txt`.

---

## PDA-DEC-4 — Provider registry (2026-09-02) 🟢

**What landed.** One frozen call, `ProviderRegistry::Create(selector, config)`.
`ProviderSelector::Parse` decides by shape (a name is `[A-Za-z0-9_-]+`, anything else a path),
totally, disjointly, never consulting the registry — so a string means the same thing in every
build. Config is the typed core `{max_payload_bytes, domain_id}` plus an opaque document
Fletcher copies and never reads. Built-ins register by explicit call on a caller-owned object:
no global table, no static-initialiser registration (the linker has been measured dropping
those). Nothing cached; each `Create` is fresh.

**The no-signature-change claim is executable, not prose.** The path branch exists and is
routed **now**, refusing `kNotSupported` (distinct from an unknown name's
`kInvalidArgument`) until a resolver is installed, and a test drives a stand-in resolver
through the *identical* helper with only the config string differing. Compliance attacked the
classification with 28 selectors built against the committed blobs and found no
misclassification repairable by an overload, flag or disambiguator, and settled that a mutable
module cache works against a `const Create`. **PDA-ABI will not need to widen this call.**

**Six behaviours were asserted by nothing, every one on the branch PDA-ABI fills blind.**
Found only by mutation: the resolver's exception translation and its null-return check; the
path branch's config forwarding (a loaded driver would have come up silently on domain 0);
the name alphabet's digits and capitals (normatively `[A-Za-z0-9_-]`, pinned only as
`[a-z_-]`); and the load-bearing `Anchor` member order. Meanwhile the suite restated
unknown-name→`kInvalidArgument` three times. **Coverage was thinnest exactly where the code
already worked.** All six now redden; recorded in the harness README.

**The lifetime rule became mechanical.** DEBT-1 was 15 lines of prose PDA-ABI had to honour;
it is now a rule it cannot violate — both seats held by shared handle, `Create` returning an
aliasing handle owning `Anchor{seat, provider}`, provider released first, so a driver's module
outlives every provider it made whatever the author did. One allocation per `Create`;
signature and surface untouched. **The code reviewer's own suggested form was wrong** — it
cannot adopt the provider's control block (ASan: `heap-use-after-free`), so as published every
`Create` would have returned a dangling handle. The implementer refused it; the reviewer
verified and withdrew.

**A specification defect no mechanism could catch.** Spec §4 clause 2 then called its residue
list **exhaustive** — "two residues, because no seam can reach them" — in the one document
PDA-ABI and BIND meet at, omitting the third and likeliest: handles the seam itself hands out
(`Blob`'s owner, `SharedSchema`, loan releases), whose deleters are module code and outlive
the provider — what PDA-ABI-7's zero-copy receive is built from. Fixed by naming it, dropping
the false reasoning, and adding a closed rule so a fourth case is classifiable.

**Verification.** core 28 / pubsub 19 / pubsub-arrow 16 / fastdds 69 / xrce 11;
`pubsub-conformance` **76/76** with XRCE ON and a live Agent (`conformance_xrce` ran, not
skipped). No full suite — mandated at items 1/2/3/6 and round close, and nothing existing
changed behaviour. **4 of 5 components hit "Already installed!"** and needed forcing before
ctest ran; the harness cache sat at `XRCE=OFF`. Both traps fired exactly as the config warns.

**Owner decisions:** ledger 26–28 (unloadable path fails distinctly; shape decides name vs
path; protocol-specific settings move into the document).
**Close gate:** PASS. **Cycle meter:** design 1/2 · fix 2 · launches 5/5 · owner touches 3.
**Numbers:** declared +640/−25 · actual **+1411/−9** (code + spec), counted by the code
reviewer with no unexplained bucket; the header is 48 code lines to 182 of normative contract
PDA-ABI implements against blind. Surface **5**, exactly the waiver. **Two launches wrote
nothing** (a wedged stream, a machine sleep) so three were productive — not a thrash signal;
the design was never revised.

**Records corrected in place:** four overclaims (`provider.hpp`'s present-tense config claim;
"nothing above the seam can tell" — `use_count`/`weak_ptr` do observe the anchor; "nothing
else escapes" — `bad_alloc` does, untyped; "covers 1 and 2" — it cannot cover 2), plus the
README's stale count and a mutation paragraph predating five of the suite's best guards.
**And a false rationale relayed as fact:** the escaping fix was justified by
`C:\x64\driver.dll` colliding with the escape for byte 0x64. It never did — 0x64 is `d`,
printable, emitted raw. The defect was real (the encoding was not injective; a collision
needs a non-printable byte); the stated reason was not, and had reached two comments.

---

## PDA-DEC-5 — `InProcessProvider` as a registered built-in (2026-09-02) 🟢

**What landed.** `RegisterInProcessProvider` makes the loopback selectable as `inprocess`.
**`SchemaCarriage` left the public header and its constructor overload was retired**, so
the only construction API takes `ProviderConfig` and the mode arrives solely as the
document key `schema_carriage=as_declared|carried` — PDA-DEC-4's document-key requirement
made *structural*, with no second construction path and no back door (the enum survives
only in the provider TU's anonymous namespace). The gateway dropped its `if`-chain **and
its own name validation**, registers both names before the selector is read, and makes
exactly one `Create`; exit code 2 preserved. This is the first real proof of the owner's
"mix of built-in and runtime-loaded" — the built-in half now goes through the same call a
loaded driver will.

**The gateway half was proved, not asserted.** `npm test` runs whatever `gateway.exe` sits in
`build/` — which produced a meaningless green earlier this round — so the design turned that
weakness into a detector: provider-selection cases pinned to wording the *pre-change* binary
cannot emit. Compliance ran both binaries and confirmed the old one emits none of the three
pinned substrings, so a green `npm test` now **entails** a post-change binary. Red at
10:17:30, green at 10:29:31; the gate binary (11:37:53) postdates HEAD (11:25:24).

**Three more guards that asserted nothing — and one found after everything else passed.**
The document reader's own `\r`-strip and blank-entry skip were **17/17 green** under either
mutation, though without the strip a CRLF document is valid on Linux and refused on Windows
— a silent cross-platform divergence in a file operators hand-edit on both. Then, after
those closed, a survivor: **memoising one built-in instance per registry passed 19/19
Registry and 80/80 conformance.** No test made two providers from one registry with two
*different documents*; PDA-DEC-4 had pinned "each `Create` is fresh" only against a probe.
That is exactly the property **PDA-DEC-8** exists to prove, so it would have surfaced there
as a puzzling failure instead of here as ten lines.

**A NUL fix taken in the forbidding direction.** A document NUL truncated the refusal message
at `what()`. Rather than escaping it, the reader now refuses a NUL-bearing document at the
door — which **deleted** `QuoteEntry` and the `<sstream>` include. The mutation reproduced the
predicted truncation verbatim, and compliance measured this test as now the seam's *only*
guard against a NUL-truncating boundary. No contradiction of the seam's NUL sanction: §4.2
assigns the format to the provider, and the document stays length-authoritative.

**`--help` deliberately not derived from the registry.** Both reviewers corrected the premise
(the `static_assert` pins only `Create`'s signature) and reached the same conclusion by
different routes. The binding reason is stronger: after PDA-ABI installs a resolver an
enumeration could only list **built-ins**, handing code above the seam a
built-in-versus-loaded distinction (decision 3). Recorded in `main.cpp`.

**Verification.** core 28 / pubsub 19 / pubsub-arrow 16 / fastdds 69 / xrce 11;
`pubsub-conformance` **81/81** with XRCE ON and a live Agent (`conformance_xrce` ran);
`gateway-end-to-end` 24/24. No full suite — mandated at items 1/2/3/6 and round close.
**4 of 5 components hit "Already installed!"** and needed forcing before ctest ran, again.
`gateway-fastdds-ts` was excluded because its binary is stale (2026-09-01 22:05) — any
green from it today would be vacuous, which is itself worth knowing before round close.

**Close gate:** PASS. **Cycle meter:** design 1/2 · fix 2 · launches 3/5 · owner touches 0.
**Numbers:** declared +270/−60 · actual **+626/−78** (code + spec), counted by both
reviewers — prose and mutation evidence, no scope creep. Surface **0** at its strictest.

**Owner touches: none.** Both Brief decisions were PM-decided — one already answered by spec
§4.2 and the 2026-08-31 ruling, the other having no viable alternative, since keeping the old
refusal message requires the gateway to retain the name list this item removes.

**Records corrected in place:** the design's surviving copy of a mutation claim the review had
disproved; the public surface being **0, not −1** in design and brief (the count charged the
retired constructor but not the added one); a public header naming the now-private `kCarried`;
spec §4.1's NUL sentence lacking a subject; the harness README stale four ways. **One reviewer
record item did not survive checking** — `main.cpp` was said to cite the `static_assert`; it
never did, and the comment was improved on the merits instead.

## PDA-DEC-6 — Fast DDS configured by document; retire `FastDDSProviderOptions` (2026-09-02) 🟢

**Forcing test:** `FastDdsConfig.ProfileDocumentConfiguresQos` — green, and it asserts what
the endpoint *announces on the network*, not merely that the document loaded.
**Commits:** `6a66a15` (implementation) · `7f6a310` (fix cycle 1) · `68ac6b5` (fix cycle 2).

**What landed.** An operator configures Fast DDS QoS with a native XML profile document and
compiles against no eProsima header. `FastDDSProviderOptions` is deleted; 18 `ProviderConfig`
constructions across 8 files were migrated, and `gateway/src/main.cpp` lost the last concrete
provider type. `--provider-config FILE` closes PDA-DEC-5's DEBT-5: charter requirement (b) is
now reachable from `gateway.exe`, which it was not before this item.

**The design's own guard was unfalsifiable, and the implementer found it.** Mutation M12 — a
non-empty document silently falling through to Fast DDS's defaults — left every test green,
because `DataWriterQos()`'s durability is TRANSIENT_LOCAL and Fast DDS's writer defaults are
bit-identical to Fletcher's on both policies discovery can carry. Closed by
`AnAnchorOnlyDocumentResolvesToFletchersBuiltIn` (whole-struct, in-process). Declared
deviation from the design's test table; compliance judged it genuinely additive.

**Two review cycles, both finding a *claim* rather than a bug.** (1) The drift guard for the
published starting-point profile held its own copy of the README block it claimed to protect,
so the README's "cannot drift without a test going red" was false. It now reads the block off
disk; cycle 2 reproduced the mutation in both directions, plus the reader half and the
*packaged* route, and confirmed a dropped export fails loudly rather than degrading. (2) The
header promised every document error is refused in the constructor; three refusals actually
fire later (end of ctor, or out of `Publish`/`Subscribe`). Now disclosed in source, header and
spec §4.1 — the last of those matters because **PDA-DEC-7 derives XRCE's document handling
from §4.1** and would have inherited the promise.

Also turned silence into refusals: an empty `--provider-config` file (was indistinguishable
from "unconfigured") and a correctly-spelled `fletcher.*` property in the wrong profile
(measured refusable, so refused rather than documented). 43 spurious `[XMLPARSER Error]` lines
removed from happy paths — the widening proof was fixed too: gating on the *document*
containing whitespace fires on every XML file; the precondition is on the yielded *name*.

**Verification.** Full suite green at `7f6a310` with every package cache-cleared first, so no
"Already installed!" false green: core 28 · arrow-bridge 61 · pubsub 19 · pubsub-arrow 16 ·
fastdds **85 ctest / 84 gtest** · xrcedds 11 · protoc 99+3 · pubsub-conformance **82**
(XRCE=ON, `conformance_xrce` ran against a live Agent) · pubsub-arrow-fastdds 4 ·
fastdds-xrce-interop 1 · protoc-arrow-bridge 91 · protoc-coverage 20 · gateway-end-to-end
**29** · gateway-fastdds-ts 4/4 ×3, no row loss, binaries confirmed to postdate HEAD.
Fast DDS–touching subset re-confirmed at final HEAD `68ac6b5`.

**A false red cost a review cycle.** ~127 leaked shm segments in
`C:\ProgramData\eprosima\fastdds_interprocess` fault `create_participant` with `0xC0000005`
for *any* QoS. Cycle-1 compliance reported a crashing test and a 78/79 suite; code review
diagnosed the environment and cleared it; I re-verified 6/6 green. Recorded in the round
config — clear that directory before believing any provider access violation.

**Records corrected in place.** Spec §10's construction count, twice — "12 sites, all
migrated" contradicted its own table and my first correction still didn't reconcile; the true
figure is 18 constructions over 8 files, re-derivable by grep. Also §10's "Docs:" line (claimed
two untouched files), `architecture-overview.md` §7.4's include path (wrong since \#26), a test
comment asserting a mutation that measurably reddens nothing, and the config's conformance
baseline, two items stale (62 → 82).

**Numbers.** Declared +780/−310; actual **+2870/−628** over 30 files (3.7× on adds).
Production `src/` + public headers **+663/−182** — near the declared budget alone; the overrun
is tests +1713 and docs +408, both review-ordered. Surface **net 0 (+2/−2)**. Note: production
totals were misreported three times, always by excluding `src/internal/profile_document.hpp`
(+30/−9 claimed vs +149/−15 actual once) — a pattern, not a slip.
**Close gate:** PASS. **Cycle meter:** design 2/2 · fix 2 · implementer launches 3/5 ·
owner touches 0.

## PDA-DEC-7 — XRCE configured by document (`key=value`) (2026-09-02) 🟢

**Forcing test:** `XrceConfig.DocumentConfiguresTransport` — green, asserting the connection
arrives at the port the *document* named, on a test-owned listener.
**Commits:** `c682be1` design · `c50f7e5` revision 1 · `5756e67` review cycle 2 ·
`4baac0f` implementation · `33d7514` fix 1 · `8553b96` fix 2.

**What landed.** XRCE is the built-in `xrce`, configured by a `key=value` document only it parses;
`XrceConfig`/`XrceTransport` retired. Of twelve fields: five become four keys (`agent_ip`+`agent_port`
collapse to one `agent=host:port`, so a half-address is unrepresentable), five deleted, two moved to
the typed core. `connect_timeout_ms` is `chrono::milliseconds` and serial has no enumerator — both
whole defect classes made uncompilable rather than checked.

**A premise became a fact.** Neither design review could verify whether `uxr_init_tcp_transport`
connects eagerly (client is FetchContent'd, no source in the repo). The implementer found it in the
Conan build tree — blocking `connect()` inside init, both platforms — so the listener is a sound
oracle and the assertion did not need the Agent gate.

**The design's blocker, caught before code.** Review cycle 1: four of six keys had no guard that
an accepted value lands anywhere — a build that range-checked them and then used hard-coded
constants would have passed every row, and two keys had no witness in the tree at all. Closed by
**shrinking**: those two keys deleted as constants, one whole-struct row witnessing the rest.

**Then a fix introduced the round's worst bug.** Wiring `connect_timeout_ms` through was itself a
design-review debt item; closing it mapped the budget with `floor((ms-1)/1000)` against a client
that takes a *total attempt count* and, at zero, sends once without listening. **Every budget
from 1–1000 ms could never connect, even to a healthy Agent**, reported as "is the Agent
running?". Both step-4 reviewers found it independently. It survived because only `=0` was
tested. Proved live both ways; the interior is now covered and reddens with 12 failures under
the old mapping. **Lesson: a debt fix needs the same red-first discipline as the feature.**

**And that fix shipped a guard that could not fail.** The socket-leak fix (no `Impl` destructor →
one handle per failing construction, measured) landed with its probe *deleted after measuring*.
The cycle-2 re-reviewer found the case in a stale binary and in no source file. Now landed, and
it reddens: 200 handles leaked on UDP, 159 on TCP, when the close is suppressed. That is three
unfalsifiable guards this round — the recurring failure mode, and worth a process note.

**A divergence I created, and the ruling on it.** I directed whitespace inside an entry to be
*refused* rather than kept-and-failed-later, making the XRCE reader stricter than PDA-DEC-5's,
which the design had adopted "verbatim". Put to an independent reviewer as a possible violation
of the 2026-08-31 divergence ruling. Verdict: **no violation, and nothing to pin** — the loopback's
closed key and value sets already refuse every such entry, so the rule changes zero outcomes.
Spec §4.1 was then corrected: stricter in *rule*, identical in *outcome*. Rule later extended to
`0x7F`, which had been accepted while a comment claimed otherwise.

**Verification.** Provider **16 ctest / 15 gtest** (was 11/10), genuinely compiled on both `conan
create` passes. `pubsub-conformance` **82/82** with XRCE=ON explicit, `conformance_xrce` re-run
standalone 25/25; interop 1/1 (3 cases); `pubsub` 19/19. No full suite — not mandated here
(1/2/3/6 + close), and the last was one item ago at `7f6a310`.

**Records corrected in place.** Spec §4 clause 4's "no caller names a concrete provider type",
contradicted by five in-tree sites (now scoped to the gateway). The plan's field arithmetic, wrong
twice — my own first correction dropped the two fields that moved to the typed core. Premise P5
ordered a stop-and-ask on reads that *do* happen, so it would have halted step 3 on the design's own
predicted finding. Plus a stale "24 clauses", a census summing nine of eight, and a mutation count
of 11 that measured 12.

**Routed out: `ROUND-1`, now PDA-DEC-1H by owner ruling.** The re-reviewer observed a full run at
`conformance_xrce` 25/25 PASSED served by a foreign Agent. **PM correction:** I first recorded this
as the harness "accepting an Agent it did not start" — wrong; `SpawnedAgentAlive()` does check the
process this binary spawned. The gap is that liveness is not port ownership, and the mechanism is
unconfirmed — PDA-DEC-1H's first job. **Every XRCE green since PDA-DEC-1 is conditional on "no
stray Agent was listening"**, including ones reported today.

**Numbers.** Declared +1900/−400; actual **+1979/−247** over 20 files excluding `plans/` — **adds
within 5%**, the first item this round to cost what it said. Production **+715/−121**. Surface
**net −1 (+2/−3)**. **Close gate:** PASS. **Cycle meter:** design 2/2 · fix 2 · implementer
launches 3/5 · owner touches 0.

## Process observation — three unfalsifiable guards in one round (2026-09-02, PM)

Recorded for the round-close retrospective at the owner's direction; **no process rule changes
mid-round.** Measured instances, each costing a cycle:

1. **PDA-DEC-6** — the design's discovery-based guard for the whole-QoS ruling could not fail:
   Fast DDS's own writer defaults are bit-identical to Fletcher's on both policies discovery can
   carry, so a build ignoring the document entirely passed every row. Found by the *implementer*,
   by mutation, after the design was approved.
2. **PDA-DEC-7** — four of six document keys had no guard that an accepted value lands anywhere;
   two had no witness in the tree at all. Found by *design review cycle 1*, before code.
3. **PDA-DEC-7** — the socket-leak fix landed with its probe deleted after measuring. Found by
   the *cycle-2 re-reviewer*, in a stale binary and `CTestCostData.txt`.

The pattern is not "guards are missing" — it is that **a guard's falsifiability was asserted
rather than executed**. All three were caught, by three different steps, which is the system
working; but each cost a cycle that naming the reddening mutation *and running it* would have
saved. Note also that (3) arrived inside a fix for (2)'s sibling finding: **a debt fix received
less red-first discipline than a feature**, which is the sharper version of the lesson.

Adjacent, same round: line-count reporting was wrong four times, always by excluding a new
`src/internal/` header; and two consecutive progress-log entries (63 and 67 lines) exceeded the
60-line budget. Both are candidates for the same retrospective, with numbers attached.

## PDA-DEC-1H — the harness proves it owns the Agent answering the port (2026-09-03) 🟢

**Forcing tests:** `AForeignAgentDoesNotSatisfyTheHarness` and
`AFailedOwnershipQueryDoesNotSatisfyTheHarness`, one copy of each in **both** XRCE harnesses.
**Commits:** `4d7d342` conformance fix · `5af2bfb` extended to interop · `ecc7b2c` fix 1 ·
`de5d2cb` design+brief · `2cfa401` fix 2. Added to the round by owner ruling 2026-09-02
(denominator 9 → 10). Closes debt `ROUND-1`.

**Ran compressed by PM decision** — no architect step, no architecture-review cycle; the
dispatch brief was the design, recorded afterwards and labelled as such. **The compliance
reviewer's verdict on that call: defensible, but it left a gap** — two false premises reached
the owner and the platform fork doubled unreviewed surface. Its recommendation is sharper than
a rule and goes to the retrospective: *do not compress an item that forks by platform*; what
saved this one was not a design doc but a reviewer told to re-derive its premises.

**The mechanism, measured twice because the first measurement was wrong.** The PM hypothesis —
a spawned Agent that fails to bind but stays alive — was refuted: it exits. The real cause is a
race against a true-but-stale predicate: Windows `Spawn` has no reap loop (POSIX-only), so the
first probe answers from the *leftover* Agent while the doomed child is still dying, leaving
`WaitForSingleObject(handle,0) == WAIT_TIMEOUT` true when asked. **The ~876 ms child lifetime
first published was an instrument artifact** — `Measure-Command { Start-Process -Wait }`, which
reports 1025–1122 ms around `cmd /c exit`, so ~1 s was the wrapper's floor. Real OS lifetime is
**28–89 ms over nine trials**, matching the Agent's own log (2.7 ms from `bind error` to `server
stopped`). That **sharpens** the finding rather than softening it: the race is ~10–90 ms wide,
so the pre-existing XRCE greens were a **coin flip**, not a near-certainty.

**The fix.** Ownership of the certified endpoint, not liveness of a process:
`GetExtendedUdpTable(UDP_TABLE_OWNER_PID)` vs the child's pid on Windows, `/proc/net/udp` inodes
∩ the child's fds on Linux. Foreign beats ours, accumulated across the whole table so iteration
order cannot invert it. Liveness demoted to a diagnostic. **No third state**: an unsupported
platform is a *compile* refusal; a runtime query failure refuses.

**Three defects the reviews found in the fix itself**, all closed with reddening proof:
1. `kUnprovable` fell back to bare liveness and **returned success** — re-admitting this item's
   own defect behind one stdout line. Deleted.
2. That deletion then had **no standing guard** — nothing reached the refusal path, so restoring
   the fallback reddened nothing. **The fourth unfalsifiable guard this round, inside the item
   created to fix an unfalsifiable guard.** Now held by a one-pointer seam whose only assignment
   in the tree is the test; restoring the fallback reddens both copies.
3. "Byte-identical" was doing work it could not: Windows checked IPv4 only while Linux also read
   `/proc/net/udp6` under a comment claiming one rule (Linux-only false *refusal*, never a false
   pass). Closed by **narrowing** — dropping the v6 read — so the copies genuinely answer the
   same question. Identity re-verified by sha256 over 221 lines, not by eye.

**Also found: the interop harness was worse** — no liveness check at all, so a leftover Agent
satisfied it outright, no race needed. Extending the fix there was a PM scope call under the
owner's ruling. Duplicated rather than shared: the first reason recorded (a CI sparse-checkout
cost) was **checked and false**, and the PM had repeated it to the owner; the true reason is
package self-containment — sharing would export a test-only header from a shipped package.

**Verification.** conformance **82 ctest entries / `conformance_xrce` 27 gtest cases**, 0
skipped (was 25); interop **1 entry / 5 cases**, 0 skipped (was 3). Zero stray Agents before and
after all seven runs — this item's own subject, so a leak would have been self-refuting.
**No CI run exists on this branch, by design**: both lanes are `workflow_call` from
PR-triggered `ci.pr.yml`, and opening the PR is the owner's step. The Linux path is therefore
verified by local compilation only (WSL, g++ 13.3, `-Wall -Wextra` clean, 6/6 verdicts) — stated
in the docs rather than implied as CI coverage. **Round-level consequence worth naming: eight
items of Linux-side correctness have never run anywhere but locally on Windows.**

**Process finding — the close gate cannot express mutation evidence.** The gate's failure-signature
scan (`FAILED|panicked at|error[`) matched this item's *deliberate* reds, so an item that records
the reddening proof the runbook demands elsewhere fails its own gate. Rephrased as "went RED",
substance unchanged, and recorded here rather than silently worked around. For the retrospective.

**Numbers.** Not declared up front (compressed) · **+1309 / −94** over `integration-tests/`;
the `+1263/−246` first reported was a per-commit churn sum that double-counts rewritten lines —
the **fourth** line-count misreport this round. Public surface **0**. **Close gate:** PASS on
re-run. **Cycle meter:** design 0 (compressed) · fix 2 · implementer launches 3/5 · owner
touches 1 (the ruling that created the item).

## PDA-DEC-8 — Multi-instance proof: two instances, two domains, one registry (2026-09-03) 🟢

**Spec §4's third normative item, made executable.** Four cases in
`conformance_fastdds` beside `Registry.FastDdsResolvesAsABuiltIn` — the forcing test
(`Registry.TwoInstancesTwoDomainsStayIsolated`), its same-domain **positive control**,
a concurrent-traffic variant, and a per-instance-payload-bound pair. **No product code
changed**: the tree already had the property, which is exactly why the deliverable is
not the green.

**The green proves nothing by itself, and this is the item that says so out loud.**
Five guards in this round were unfalsifiable and each was plausible on the page. So the
gate here is six mutations to *product* code, each applied alone, reverted, and its
failure text recorded verbatim in the suite README — with the four cases observed green
on the unmutated build **immediately before** each row (a row whose green precondition
was not observed is void), and `C:\ProgramData\eprosima\fastdds_interprocess` cleared
after the one row that crashed, since a stale segment makes the next row's
`create_participant` fail with a false `0xC0000005` that proves nothing.

**All six rows reddened a named assertion.** Two behaved differently from the design's
prediction, and both are recorded as **observed** rather than as predicted:

1. **M2** (a process-wide participant) was predicted to be refused at `register_type`;
   Fast DDS 3.4 accepts the second registration and refuses one step later, at
   `create_topic` — *"Topic with name : pdadec8/shared already exists"* → a typed
   `kTransportFailure`. Same class (typed refusal at declaration time), different call.
   It is also the row that crashed: the first `~Impl` deletes the shared participant and
   the remaining three cases die `0xc0000005`, precisely the teardown the design named.
2. **M5** (an appending `JoinSegmentsInto`) fails at A's **second** publish when the four
   cases are run filtered, and at its **first** when the whole binary runs, on scratch
   left over from the preceding case — because the scratch is `static thread_local` and
   outlives a case. Review debt C2-4 predicted exactly this; both runs are recorded.

**What made the arrangement worth building.** The cycle-1 design gave the two instances
different payload bounds — and the bound is part of the registered DDS type name, so
they could never have discovered each other **on any domain**. That version would have
passed identically with process-wide state present: a sixth unfalsifiable guard, caught
in review. The landed arrangement holds **one `kBound`** everywhere a crossing is
asserted or denied, so `domain_id` is the only wire-visible difference, and moves the
bound claim to its own pair that claims no crossing either way. The standing
**positive control** measures that a real crossing fits inside the very `kSettle` the
isolation case pays for its absence claim. **Corrected in fix cycle 1:** the "~260 ms"
(and compliance's 279 ms) is the **case's wall time**, not the crossing — the crossing is
**0 ms**, stable over five runs, because both participants are matched before either
publish and Fast DDS serves same-process endpoints inline. Two reviewers and the PM all
quoted a real number for the wrong quantity, which is a subtler failure than a
miscount: the figure is now emitted by the test itself via `RecordProperty("crossing_ms")`
so nobody has to infer it again.

**The journals are mutexed, and that was a proof requirement, not hygiene** (debt C2-3).
They are appended on Fast DDS listener threads and read on the main thread; unguarded, a
foreign marker arriving during the read could be *missed* — a green the arrangement did
not earn, which is this item's own defect class.

**The claim was narrowed on the owner's ruling** (2026-09-03): **one application on one
machine**, with **three exclusions stated rather than implied** — nothing about isolation
between machines, nothing about vendor process-wide state both instances would set
identically, nothing about the shared memory two *separate* processes on one machine use.
Debt C2-1 rode with it: the design's §8 published "exchange no rows" for the
different-bounds pair too, which its own premise P1b makes **unearned**. Split in the
design, the README and the spec: the domain pair claims no crossing inside the measured
window; the bound pair claims each instance honours its own bound.

**Verification.** `pubsub-conformance` **82 → 86 ctest entries**, 86/86 passed;
`conformance_fastdds` **25 → 29 gtest cases**, `conformance_xrce` unchanged at **1 entry
/ 27 cases** and confirmed to have run (`-DFLETCHER_CONFORMANCE_XRCE=ON` stated
explicitly, because `cmake --preset` does not reset a cached OFF and the run would still
report all passed). Zero stray Agents before and after; shm directory empty before and
after. Counts derived from `ctest -N` and `--gtest_list_tests`, not remembered.

**Numbers.** Declared **+570 / −0** · landed **+710 / −25** all files (`git diff
e99eaeb..b7b33f3`), **+605 / −4** excluding `plans/`, of which **473 is the one test
file**. Public surface **0**, product code **0**. The counting went wrong twice here and
both are worth naming: the implementer's draft of this line said +633/−19 (the sixth
miscounted figure this round), and the PM's correction to +705/−25 was **itself stale** —
derived before editing this very paragraph, which added the five lines that make it 710.
Compliance caught it. The lesson is not "count more carefully" but **derive after the
last edit, against the commit, not the index**. Design landed at **305 lines
vs a 300 cap** (+5): debt C2-2 required a new premise while forbidding file growth, and
the PM accepted the overrun rather than cut approved content for five lines.
**Cycle meter:** design 2/2 · fix 0 · implementer launches 1/5 · owner touches 1 (the
scope ruling).

**Close (PM, 2026-09-03).** Gate FAILED first run — the Stage Brief still carried its
template placeholder instead of an "As landed" delta, which is exactly what the gate
exists to catch, and it caught it against me. Fixed and re-run: **PASS**. Also corrected
the brief's opening sentence, which still folded the two pairs into one over-claim that
review debt C2-1 had struck.

**Round-close full suite at `eb69297`: zero failures.** All seven components reported
"Already installed!" on the first pass and were `conan remove`d and genuinely rebuilt, so
the round's most-cited false-green trap was caught rather than reported around. Notable
figures, entries and cases stated separately: `pubsub-conformance` **86 entries / 112
cases** (`conformance_xrce` is ONE entry bundling 27; `conformance_fastdds` 29),
`fastdds-pubsub-provider` **85 / 84**, `xrcedds` **16 / 15**, `gateway-end-to-end` **29**,
`gateway-fastdds-ts` **4/4 three times with no row-loss signature**, all three TS binaries
confirmed to postdate HEAD. Two accepted skips (rustc, tsc) and one gated oracle.

**Cycle meter:** design 2/2 · fix 1 · implementer launches 2/5 · owner touches 1.
**Close gate:** PASS on re-run.

**PROCESS BREACH, recorded because it is the one this gate exists to prevent (PM,
2026-09-03).** `b7b33f3` — the *implementation* commit — flipped this item's tracker cell
to 🟢. That was the implementer's edit (`plans/PDA-decouple-interface.md +7/−1` in its own
file list) and **I committed it without reading that hunk**. Consequence: the tracker
asserted 🟢 from the implementation commit onward — through both step-4 reviews, a fix
cycle that closed a dead guard and a domain collision, and a **first close-gate run that
FAILED** on the missing brief delta. For that whole window the tracker was lying, and
`d611419..a07b5da` was pushed with it lying.

The end state is sound: the gate passed on re-run and the item is genuinely green. But the
ordering was inverted, and the runbook is explicit that the flip is the PM's step *after*
the gate passes, precisely because a stage once closed with its gate never run took ~10
owner touches to unwind. **Two things follow, both mine:** an agent must not edit
`plan_path`'s status column at all, and the PM must read the tracker hunk of any diff
before committing it. Neither is a new rule — the second is just doing the job.
For the retrospective, with the number attached: **1 item, 4 commits, ~2 hours** spent
claiming a green the evidence did not yet support.

## PDA-DEC-9 — the seam becomes a signed contract: spec §12, the published taxonomy, TD-008 (2026-09-03) 🟢

**The round's last item is documentation plus exactly one guard.** The spec goes
`proposed` → **frozen** and gains **§12**: what is frozen and **who may act on each class**,
how each of the six handoff conditions was *actually* verified, what the two later rounds
inherit, and what the evidence does not cover. `core/README.md` gains the one published
*Error taxonomy* table; `docs/technology-decisions.md` gains **TD-008**; the two top-level docs
stop describing a schema handoff the seam abandoned in PDA-DEC-3 and a transport that arrives
by "implementing one interface" alone. No product code, no public surface, no seam method
touched.

**The one guard, and why it is the only new machine in the item.**
`Taxonomy.PublishedNumbersMatchTheEnum` (`core_tests`) reads the published table **off disk**
at run time and compares it row for row to `PubSubStatus`. It holds **no count and no copy** of
the numbers: the expected set is derived from the file (a contiguous prefix from 0, which
§5.1's *appended only, never reordered or reused* is what licenses), because a row-count
equality against a number the test itself carried would **be** the held-copy defect the guard
exists to close. Totality is a **compile** matter, not a test matter: `StatusName` is one
`switch` over every enumerator with no `default:` label, and the unhandled-enumerator
diagnostic is promoted to an error on that one source file.

**The guard was reddened three ways before it was believed** — the whole mechanism hangs on a
compiler flag whose failure mode is a silent green (review debt C3-2), and P3b carried a
stop-and-ask on exactly that. **Four** mutations were applied alone, observed, and reverted
(the fourth — a malformed added README row — was added by fix cycle 1, which also proved the
configure-time check firing by neutering the flag to `/wd4062`):

1. **Append an enumerator, touch nothing else → the build fails.** The flag takes under MSVC
   19.4:
   `test_status_taxonomy.cpp(74,5): error C4062: enumerator 'fletcher::PubSubStatus::kThrowawayMutationDoNotShip' in switch of enum 'fletcher::PubSubStatus' is not handled [core_tests.vcxproj]`
2. **Add the `case` but not the README row → red at part 3.**
   `Expected equality of these values: StatusName(...(rows.size())) Which is: "kThrowawayMutationDoNotShip" / ""` —
   *an enumerator exists one past the last published row.*
3. **Edit a name on either side → red at part 2.** (`kNotSupported` → `kUnsupported` in the
   README):
   `StatusName(static_cast<PubSubStatus>(row.number)) Which is: "kNotSupported" / row.name Which is: "kUnsupported"`

Two further reds were observed *on the way in*, and they are the non-vacuity proof the design
asked for: with the README not yet exported the test failed on the **empty read**
(`could not read the published taxonomy from …/README.md`), and with it exported but the table
not yet written it failed on **zero rows parsed** — never a silently green loop. P3b is
**not** triggered; P1 holds (core's package ID is `da39a3ee…` before and after the new
`exports_sources` entry, and `conan create` is unaffected).

**What §10 said that this PR would have frozen.** The section still asserted, in the present
tense, that "`SubscriptionResult` and its `shared_future` are consumed by 10 sites outside
`provider.hpp`" with a per-site breakdown that summed to **12**. Both halves were false: the
`shared_future` was retired outright by the 2026-09-01 *One mechanism only* ruling. Deleted,
with no replacement count. The **whole-section sweep** that review debt C3-1 asked for (the
defect class recurred at a new address between cycles) found three more: the section's own
"Measured" framing, read as a live measurement of the tree; "their 'implementing one
interface' claims stand", an inspection of *vocabulary* reported as a check of *accuracy*; and
`InProcessProvider` "moves … and becomes", present tense for work that landed in PDA-DEC-1 and
PDA-DEC-5. §12.1 now names §10 and §11 as **records, not contract**, so correcting a stale
record is maintenance rather than a stop-and-ask, and scopes the no-free-floating-count rule to
counts that claim something about the *current tree* — past-tense records of what landed are
fine.

**One label came down, again.** §12 row 2a is no longer flatly `mechanical`: *regressing* to a
`shared_future` return stops the tree compiling, but **adding one beside `SchemaArrival`
compiles and reddens nothing**, so the row reads **mechanical (regression only) ·
by-reading (addition)** and says that the forward protection is §3's place in the frozen list,
not a machine. The honest tally in §12.2 is therefore: **two of six mechanical end to end
(3, 4)**, one split (2a), three by-reading with a reader and a date (1, 2b, 6), one
by-construction with no machine check (5).

**The platform evidence is stated exactly** (owner ruling 2026-09-03): every green in this
round is a local run on one Windows machine plus one WSL compile of the single platform-forked
file, **no automated build has ever run on `feature/protocol-driver-abi`** (the lanes are
`workflow_call` entries from a `pull_request`-triggered workflow, so opening the PR is the
owner's step), and §12.4 *instructs* rather than only recording: both later rounds treat Linux
as unverified, the first PR of either runs the lanes, and a Linux-only difference in seam
behaviour is a **question for the owner, not a local fix** — a local fix by one round would
silently change the seam both rounds share. Condition 3's diagnostic is witnessed under MSVC
**only**; the `core` lane compiles `core_tests` on a Linux runner too, which is what would
expose a flag that fires under one compiler and not the other, and that lane has not run.

**Verification.** `core` **28 → 29 ctest entries** and **28 → 29 gtest cases**, both derived
(`ctest -N` → *Total Tests: 29*; `--gtest_list_tests` → 29), 29/29 passed. Both required
`conan create` passes genuinely compiled — `Package 'da39a3ee5e6b4b0d3255bfef95601890afd80709'
created` on each, not "Already installed!". No other package was rebuilt, and the reason is
stated rather than assumed: no product header changed (`status.hpp` is byte-identical to
HEAD), `package()` is untouched, so nothing downstream sees a different `fletcher-core`.

**Deliberate deviation from the design, flagged.** The design's Files-to-touch has this item
flip its own tracker cell to 🟢. It does not: the 2026-09-03 process breach recorded in this
log ruled that **an agent must not edit `plan_path`'s status column at all**, and the flip is
the PM's step after the close gate passes. The DoD checklist's new verification column landed;
the status cell was left alone.

**Numbers.** Declared **+330 / −100** · landed **+1131 / −70** all files, **+574 / −58**
excluding `plans/` (`git diff 7740a9d..60331b0`, derived after the last edit). Over on adds:
the 188-line guard, an 89-line log entry, the spec's §12, and rewrites counted on both sides.
Public surface **0**, `core`'s package ID unchanged, `package()` untouched. Compliance judged
the overrun not a breach — every file was ordered and no deletion under-delivered.
