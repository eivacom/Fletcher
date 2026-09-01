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
