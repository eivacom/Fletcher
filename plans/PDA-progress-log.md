# PDA — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[PDA-protocol-driver-abi.md](PDA-protocol-driver-abi.md) for the tracker and
[../docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md) for the
oracle.

## Round kickoff (2026-08-31)

Round designed on 2026-08-31. Decisions L1–L7 were answered by the maintainer in
the design conversation; L8–L10 in the follow-up scope questions. The design
session was lost before any artifact was written; it was recovered from the
session transcript and the design is preserved verbatim at
`c:\tmp\PDA-abi-design-recovered.txt`.

Two citation errors in the recovered design were corrected while writing the spec
and must not be reintroduced:

- The MCU `<75 KB` Flash figure comes from **TD-004** (rationale) and **TD-007**
  (context) — *not* TD-005, which is schema transport via companion topics.
- The recorded dynamic-linking skepticism is **TD-007's** "alternatives
  considered", and it is narrower than it first reads: it rejected dynamic linking
  as a way to make **Arrow C++** optional in the edge/server tier split, not
  plugin ABIs in general. The counter-argument PDA-11 owes is correspondingly
  narrower — see spec §11.1.

One estimate was also corrected: the Fast DDS config blast radius was recorded in
the design conversation as "4 code sites and 2 docs". Measured, it is **4 external
consumer files / 19 occurrences**, plus **39 provider-internal occurrences across
7 files** (24 of them in the provider's own QoS test TU, which is substantially
rewritten) and **10** in XRCE. The file count for *external* consumers was right;
the total churn was understated. See spec §10.

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

<!-- Entries appended below by the round runbook -->
