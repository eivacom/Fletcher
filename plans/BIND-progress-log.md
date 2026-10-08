# BIND — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[BIND-csharp-bindings.md](BIND-csharp-bindings.md) for the tracker and
[BIND-locked-decisions.md](BIND-locked-decisions.md) for the decision digest.

<!-- Entries appended below by the round runbook -->

## BIND-0 — Kickoff: decisions, skeletons, lanes and the test matrix (2026-09-17)

**Forcing test:** `ci.dotnet.yml` + `ci.c-abi.yml` green on both platforms → 🟢   (⚪ → 🔴 → 🟢)
**What landed:** the round's kickoff — D-BIND-1 … D-BIND-28 recorded; `.devcontainer` on the
.NET 10 LTS SDK with `dotnet/global.json` pinned to the same band; `c-abi/` building a shim that
exported one function; `dotnet/` packing three empty packages on `net8.0;net10.0`;
`ci.c-abi.yml` and `ci.dotnet.yml` green on Windows **and** Linux; Part 4's test matrix
re-derived and committed; `Apache.Arrow` pinned exactly at `[23.0.0]`; the per-RID shim size
measured. Commits `57f565c`, `2caf38a`, `791baba`.
**Reviews:** conformance, `plans/reviews/BIND-0-conformance.md` — **CONFORMS**, 9 live bullets,
9 met, 0 blocking findings. Each bullet was checked against the tree rather than against the
state table: the decision digest enumerated, both SDK pins read, the package list taken from
disk, all four CI jobs read from the run, the Arrow pin grepped from both `.csproj`, and **the
Part 4 matrix re-derived by its own documented command and reconciled bucket by bucket**.
**The tenth bullet was struck, not met** (D-BIND-33): BIND-0's acceptance carried the
self-hosted publish runner and `NUGET_EIVA_API_KEY`, annotated "due before BIND-9" — an item's
acceptance cannot hold something whose deadline is a later item, and BIND-9 already carries the
same requirement more completely. A duplicate removed; nothing moved and nothing relaxed.
**Verification:** `ci.pr` green end to end at `0811091` — `c-abi / build-linux`,
`c-abi / build-windows`, `dotnet / linux`, `dotnet / windows` all `completed:success`.
**Commit / push:** `feature/csharp-bindings` → `origin feature/csharp-bindings`
**Carry-forwards:**
- **F1 — the Part 4 matrix is stale, and the mechanism will repeat.** Bucket 1 (104) and bucket 3
  (38) reproduce exactly, and the documented exclusions reconcile to the byte
  (`test_fletcher_sample_pub_sub_type` is exactly the 24 provider-internal cases,
  `test_owned_schema` exactly the 1). Bucket 4 does not: `test_xrce_document` holds 11 macros
  where the matrix records 9, so the total is **80, not 78**. The cause is not a miscount — #130
  (SIGPIPE, authored 2026-09-16) sits *below* the 2026-09-15 kickoff after D-BIND-30's
  rebase-at-item-boundaries policy, so the matrix was right when derived and a rebase invalidated
  it. **BIND-3 and BIND-4 state their acceptance as these totals** ("Bucket 1 (104) green",
  "Bucket 4 (78)"), so this is owed to BIND-3, not to BIND-0. Cheapest durable fix: state the
  buckets by file set and compute the count when it is needed.
- **F2** — "exports only `fl_binding_abi_version()`" was true at BIND-0 because the ABI was
  empty. It is true now for a different reason (`c-abi/cmake/exports.map`), and on Windows it has
  never been true at all (~3531 names from the static Fast DDS). BIND-1's B3 records the same.
- **F3** — the recorded **7.61 MiB** is a kickoff baseline against an empty ABI, not a current
  measurement. The lane re-measures every run; BIND-9 should read the budget off a current number.
- **Not checked, and stated as such:** the ADO items (needs `devops.eiva.com`), the
  `Apache.Arrow` C Data Interface round-trip results (the lane is green; the cases were not
  re-run), and `linux-x64`'s stored shim size.

## BIND-1 — The binding ABI header, reviewed as a specification (2026-09-17)

**Forcing test:** `BindingAbi.CompilesAsC99AndIsSelfContained` → 🟢   (⚪ → 🔴 → 🟢)
**What landed:** `c-abi/include/fletcher/abi/binding.h` — the pure C surface every language
binding calls Fletcher through: 40 declarations covering status and `fl_error`, strings and
topics, blobs and shared schemas, the schema arrival, attachments and their builder, the write
window and writer, provider/publisher/subscriber, the delivery callback, the three-step codec
surface, and the single-copy marker. Pure C99, self-contained, no Fletcher C++ header reachable
from it. Committed at `696a42f`; the spec review and its fixes at `5ebb2d5`, `f73cfb4` and this
commit.
**Reviews:** design/spec review, **two cycles**, `plans/reviews/BIND-1-design-review.md` —
3 BLOCKERs opened, 3 closed; 3 findings reviewed and KEPT with reasoning; 4 DEBT + 2 NITs
carried forward. **None of the BLOCKERs was a design error and none needed a signature change**,
so the append-only rule never bound and no version bump was required. All three were the same
failure mode: the header stating something that had stopped being true, or stating a rule it did
not finish.
  - **B1** — `fl_error` documented its message as shim-allocated and `fl_error_dispose`-released,
    while `fl_grow_fn` handed an `fl_error*` to caller-written code. Ruled **D-BIND-32**: an
    `fl_error` a callback fills is BORROWED — the callback keeps the bytes alive until it
    returns, the shim copies and frees nothing. Stated on `fl_error` rather than on `fl_grow_fn`
    so the next such callback inherits it, and ruled for **all** bindings.
  - **B2** — "Status of the surface" still claimed only `fl_binding_abi_version()` was
    implemented, some fifteen entry points after that stopped being true. Replaced by the
    property it was guarding (a declaration is exported only once its item lands, so an
    unimplemented one is a link error) with no inventory left to restale.
  - **B3** — the visibility comment claimed the export table equalled the declarations *because
    of* hidden visibility and `__declspec`. Cycle 1 found the mechanism wrong; **cycle 2 found
    cycle 1 had understated it** — the conclusion was true only on Linux, and on Windows the
    statically linked Fast DDS re-exports ~3531 names, so the claim was false there from the day
    it was written. Now stated per platform with the mechanism that actually produces it
    (`c-abi/cmake/exports.map` on Linux), and plainly saying it does not hold on Windows.
**Verification:** `conan create` with `run_tests=True` on Windows after every header edit —
23/23 ctest, zero compiler warnings, `BindingAbi.CompilesAsC99AndIsSelfContained` green each
time (the one test a comment change can plausibly break, since the probe compiles the header as
strict C). Block-comment nesting checked mechanically (no nested open, balanced at EOF). Every C
snippet in the review verified verbatim against the header by script — cycle 2's against the
current file, cycle 1's deliberately quoting the text it found.
**Commit / push:** `feature/csharp-bindings` → `origin feature/csharp-bindings`
**Carry-forwards:**
- **DEBT D1** — the OUT convention says "on any failure the callee leaves the object untouched",
  but `fl_schema_arrival_wait` documents `FL_PENDING` and `FL_SUBSCRIPTION_ENDED` as *values
  rather than failures* and both also leave OUT untouched. Read it as "on anything but FL_OK".
- **DEBT D2** — `fl_string_list_at` leaves out-of-range unspecified where its identical-shaped
  twin `fl_attachments_key_at` specifies `{NULL, 0}`; BIND-2c had to guess. Rider: the sentinel
  is unambiguous for `list_topics` only because the topic rules make an empty name
  unrepresentable.
- **DEBT D3** — `FL_NOT_SUPPORTED`'s prose says "this provider", but it answers for a path
  selector (no provider yet) and for the not-yet-grown schema-watch pair. "This build" covers all
  three.
- **DEBT D4** — `fl_attachments_find`'s complexity is unstated, though the set's byte-order
  invariant is what would make a binary search available.
- **NIT N1** — "empty blob" is used but never defined as `size == 0`. **NIT N2** — the delivery
  callback's raw `uint64_t subscription_id` versus BIND-4's managed `Subscription` handle.
- **Risk B-2, raised against D-BIND-32 and recorded with it:** the hot-path borrow cost is
  `ToSegments` materialising a `std::vector<std::string>` per `fl_publisher_publish_row`
  (`fl_publisher_publish_rows` already hoists it). BIND-3's per-row publish benchmark settles it
  — *moved to BIND-4 on 2026-09-21 by D-BIND-37, because a publish benchmark needs a managed
  publisher and B-2 is a per-publish cost*;
  the fix if needed is a pre-converted topic handle following this ABI's own open-once shape.

## BIND-2 — The nanoarrow codec, the publish fusion, and the oracle's producer (2026-09-18)

**Forcing tests:** `NanoarrowCodec.ByteIdenticalToArrowBridge` + `CopyAccounting.BindingProducerWritesInPlace` → 🟢   (⚪ → 🔴 → 🟢)
**What landed:** the round's long pole, in four slices, because one commit of it would have
been unreviewable. **2a** (`28cdcd4`) `NanoarrowCodec(schema)`, `BoundRows`, `EncodeRow` — the
encode half, on nanoarrow and `positional_io`, **never** `arrow-bridge`, because wrapping the
Arrow C++ codec would drag full Arrow C++ into the per-RID native asset. **2b** (`bec139e`)
`DecodeRows` and the malformed-input parity property. **2c** (`3b13cb5`, `0811091`) the one
containment site, the codec surface, both write-window adapters and the publisher chain — 20 of
the header's 40 entry points, the other 20 being exactly BIND-4's (D-BIND-31). **2d** (`b95affc`,
`b6b89b3`, `f9d3ece`, `5762239`) the copy oracle's binding producer, the single-copy check and
the per-platform size ceilings.

**Reviews:** code review (`plans/reviews/BIND-2-codereview.md`) and conformance review
(`plans/reviews/BIND-2-conformance.md`). **CONFORMS on 9 of 10 acceptance bullets; 1 BLOCKER,
5 DEBT, 4 NITs; zero defects in the wire format, the containment taxonomy or the handle
lifetimes** — the three places a defect would have been expensive.

- **The BLOCKER passed every run, which is why it is worth recording.** `c_abi_tests` links
  BOTH the shim and the object library it is built from, so `test_containment.cpp` filled an
  `fl_error` through the object library's `SetMessage` (`new[]` in the test binary) and released
  it with the shim's exported `fl_error_dispose` (`delete[]` in the shim). One heap while both
  link the MSVC CRT dynamically — **two the moment the shim stops sharing a runtime**, which is
  D-BIND-32's own argument pointed the other way. Fixed with a file-local `DisposeLocal`.
  **Watch for the shape:** anything pairing an allocation in one module with a free in
  another. *(D-BIND-38, 2026-09-21, kept the CRT dynamic and moved the static question to
  BIND-9, so the hazard is deferred rather than imminent. The fix landed anyway: the pairing
  is wrong under a shared runtime too, and D-BIND-32's rule now rests on allocator
  provenance rather than on runtime linkage.)*
- **Bullet 4 was met in substance and not in letter, and the letter was wrong** → **D-BIND-35**.
  The acceptance named the `emit_vectors` scenario corpus; reading it showed it encodes through
  the protoc-generated row class (proto rows, not Arrow arrays) and is three scenarios over one
  flat message — the wrong SHAPE for an Arrow-array codec and NARROWER than the five fixtures
  built. The acceptance was amended and the test stands; D-BIND-11's own `emit_vectors`
  reference, which sits on the cross-language property, is untouched.
- **2b's composite map-key refusal** → **D-BIND-36**: map keys are scalar only, refused at open
  by field name. Locked as a decision rather than a note because it states the wire format's
  REACH and BIND-Rust binds the same codec next round.
- **The state table was stale in two rows and both were corrected:** the size row recorded the
  MISTAKE (one 12 MiB ceiling for both legs) rather than the fix `5762239` landed (per-platform
  ceilings from per-platform baselines), and the malformed-input row was still 🔴 over a gap 2c
  had closed.

**Two properties the record presented as proven and are not, now stated as such:**

- **The shim's own single-copy wiring is unobservable.** Drop the dynamic initializer in
  `binding.cpp` and every test still passes, because "never checked" and "checked, found one"
  are the same observable. The algorithm is tested thoroughly against a real decoy module — in
  the TEST BINARY's copy of it. Proving it for the shim needs two real shims in one process,
  which 2d priced and declined.
- **Nothing observes what the fused publish puts on the wire.** `inprocess` delivers to nobody
  and the subscriber half is BIND-4's, so the fusion has two unverified properties: the copy
  claim (recorded, D-BIND-34) and the byte claim. **Both owed to BIND-4.**

**BIND-0's F1 is discharged** (2026-09-18): the Part 4 matrix was re-derived against the tree —
three numbers had moved (`test_xrce_document` 9→11 so bucket 4 is 104 and its port target 80,
`pubsub-conformance` 80→82, `CallerTier` 20→21, the last of which the tracker already carried
while the development plan did not) — and **BIND-3's, BIND-4's and BIND-8's acceptance bullets
now name FILE SETS rather than counts**, which is F1's recommendation and the only form a
rebase cannot falsify. One thing F1 did not reach went to the maintainer and is now
**ruled (2026-09-18): the denominator is 302, so 278 port.** Q10's derived figure had been
*"275 of 300"*, low by three for two independent reasons: the total is buckets 1–4 while
`test_owned_schema` is bucket 6, so subtracting it removed a case the total never held; and
bucket 4 was carrying `test_xrce_document` at 9 where the file has 11. Both readings of the
scope converge on **278 ported** (303−24−1 and 302−24 alike), so only the denominator was
open. Corrected in all four places it appeared, and the round-exit bullet that carried it now
names the FILE SETS instead. **ADO 18786's scope comment (22604) carried the old figure and is
corrected there by comment 23048.** The story is **Active** — it is the one that must *not* be
closed before the native-provider bucket is green over `fastdds` and `xrce`, which is what the
plan sentence says and which an earlier draft of this entry misread as "closed".

**Carried forward:** D1's missing test (the guard is in, the silently-refusing `grow` thunk was
never added); **D2** — a callback's `grow` status reaches `err->status` unvalidated, which
**BIND-3 meets directly** because its managed error handling switches on `Status`; D5 — the CI
export check greps the `fl_` prefix while its message claims the declarations; and four NITs,
including N2, itself a one-liner (`ArrowArrayViewReset` missing on one of `BoundRows`' two
constructor failure paths).

**Found by building rather than by reasoning, and worth not rediscovering:** the copy oracle's
first binding leg was a **tautology that passed 142/142** — it recorded the span it had just
lent, so any producer scored zero by construction; the fix was making the producer self-report,
with `BindingProducerStagingIsCaught` as the live negative control. A `gcc:13` container found
two false-positive classes in the module scan that Windows structurally cannot express
(`dlopen(nullptr)` is the global symbol scope, not the executable; `dlsym` on a library handle
searches its dependency chain). And libstdc++ gives a handful of `shared_ptr` internals explicit
default visibility, so one `std::make_shared` exported six symbols regardless of
`CXX_VISIBILITY_PRESET` — settled by an anonymous version script, which makes the export table a
property of the LINK rather than something CI notices afterwards.

## BIND-3 — `Eiva.Fletcher.Interop` and the codec/Arrow tier (2026-09-21)

**Forcing tests:** every PROTO-MAPPING type round-trips through the binding (D-BIND-39, restated
from "Bucket 1 green in C#") + `ErrorTests.EveryHardCaseKeepsItsMessage` → 🟢   (⚪ → 🔴 → 🟢)
**What landed:** the managed tier, in four slices. **3a** (`88d7ed0`, `63c1a58`, `3a135b9`) the
interop foundation — the resolver, an EXACT version handshake (not `>=`: `binding.h` says there
is no compatibility guarantee before 1.0), 21 declarations cross-checked against the header's
exports, and a `SafeHandle` per native handle with the publisher→provider borrow enforced by the
COMPILER rather than by a comment. **3b** (`668e68d`) the error tier — one `ThrowIfFailed`,
`FletcherFormatException` when the origin is the codec, rule 3 checked first, plus D1 and D2
from BIND-2's review closed in the shim. **3c** (`e12d484`, `934512c`) the codec tier —
`FletcherCodec`, `BoundRows`, the C Data Interface hop, the round's first
`[UnmanagedCallersOnly]` thunk and N-1's reflection test, the HARD-1..7 sweep, and Arrow IPC.
**3d** (`f9ddf37` … `a5fabed`) dictionaries, the proto-mapping parity suite,
`integration-tests/binding-abi-conformance`, and the IPC parity test.

**Three maintainer rulings, and the first one changed the item's shape.** **D-BIND-39** — the
maintainer challenged "port bucket 1 to C#" and was right to: bucket 1 is `arrow-bridge`'s
**Arrow-native** suite, so porting it asked the binding to implement a tier it does not serve,
and roughly half of it exercises unions, decimals, intervals, half-float, the view types and
fixed-size binary, which no `.proto` can produce. Checking the mapping field by field showed
type support was ALREADY identical across C++, C#, TS and Rust on the generated path — with one
genuine exception. **The shim refused every dictionary while the wire spec and `arrow-bridge`
both carried them**, so a C# caller could not send what a C++ caller could over one format. The
bullet was CONFIRMED rather than softened and the shim changed. **D-BIND-40** ruled the
conformance corpus: the codec's own five Arrow fixtures, lifted into `codec_corpus.hpp` and
INCLUDED by the emitter so one definition serves both consumers. **D-BIND-41** moved the glibc
floor to BIND-9 and made it a MEASUREMENT.

**Reviews:** code review (`plans/reviews/BIND-3-codereview.md`) and conformance review
(`plans/reviews/BIND-3-conformance.md`). **CONFORMS on 9 of 9 live bullets; 0 BLOCKERs, 4 DEBT,
3 NITs**; no defect in the handle lifetimes, the containment discipline, the wire-format path or
the error taxonomy. **Both documents state their own limit first:** same author, same session,
hours after the code — not a fresh-eyes pass, and the mechanical claim table is the part that
carries weight. **D1 is the finding to act on, owed to BIND-4/5:** `DecodedSchema.Resolve` and
`ResolveDictionaries` are two independent derivations of one rule and their AGREEMENT is proven
for two shapes only; the native side has no nested dictionary test at all.

**Verification, and the two halves are stated apart because they are not equally strong.**
**CI-CONFIRMED at `4d3382d`:** `ci.pr` green 46/46, `dotnet` **102/102** on all four legs, and
`integration-test-binding-abi-conformance` green on BOTH platforms (5 fixtures, 11/11 each) —
each read BY COUNT rather than by badge, because the failure mode this round already shipped
once produced 63 passing tests instead of 74 rather than a red one. **LOCAL ONLY at `a5fabed`:**
c-abi 41/41 (was 36) and managed **122/122** — the twenty rows added by the IPC parity test
postdate the last complete CI run. **`a5fabed` has no green `ci.pr`, and the reason is
external:** GitHub returned sustained `HTTP 504`s to *every* lane that downloads from it,
including Conan fetching the EXACTLY PINNED `gtest/1.17.0` tarball. Not a defect in this item,
and not one pinning would have prevented — which is worth recording, because the first reading
blamed an unpinned protoc lookup and that reading was wrong. **Measured, first reading of D-BIND-41: the
linux-x64 shim requires `GLIBC_2.38`** — which excludes Ubuntu 22.04 LTS, Debian 12, RHEL 9 and
RHEL 8, and is BIND-9's to answer.

**Commit / push:** `feature/csharp-bindings` → `origin feature/csharp-bindings`

**Found by building rather than by reasoning, and worth not rediscovering.** The documented
FAIL-FAST for an exception escaping an `[UnmanagedCallersOnly]` method **is not what Windows
does** — narrowing the thunk's catch let the exception unwind through the shim's C++ frames and
reach the managed caller intact, so the behavioural test still passed; the rule is enforced
structurally for that reason. The first N-1 reflection test was toothless, matching a catch-all
nested inside the handler, and now requires the OUTERMOST clause. A **null struct's children are
not on the wire**, so the parity suite's first comparison asserted that Fletcher transmits
values the format deliberately drops — caught only because a fixture carries a label under a
null struct. `find_package(arrow)` resolves on NTFS and fails on ext4 because Conan writes
`ArrowConfig.cmake` with a capital A while naming the target lowercase — the fourth Linux-only
defect this round. And `ci.dotnet.yml` checks out SPARSELY: the goldens `SchemaIpcTests` reads
were simply absent in CI, which produced 63 passing tests instead of 74 rather than a red one —
**a lane that gains a FILE dependency needs its checkout AND its path filter in the same
commit.**

## BIND-4 — Pub/sub in `Eiva.Fletcher` (2026-09-25)

**Forcing tests:** bucket 3 over `inprocess`; bucket 4 over `fastdds`/`xrce` by selector; the C#
arm of `CallerTier` with a total mapping; the per-row publish benchmark recorded → 🟢
(⚪ → 🟢; the row was never set 🔴 while the item ran, which is a bookkeeping slip, recorded
rather than rewritten)
**What landed:** the pub/sub tier, in four slices, then a review and its fixes.
**4a** (`029aed3`, `883de25`, `1ab3e70`) the ABI's value tier — attachments, blobs, shared
schemas — and the subscriber half, the arrival, `fl_blob_create` (D-BIND-42) and
`fl_schema_retain` (D-BIND-43). **4b** (`0d85e67`, `a79cd5f`, `5435fea`) the provider vocabulary,
the attachments write end (D-BIND-44), and `Publisher` with `minBytes` explicit (D-BIND-45).
**4c** (`b46391f`, `f45b683`) the delivery path, `fl_schema_copy` and the completeness sweep
(D-BIND-46), and the managed refusal layer. **4d** (`828be6d` … `8cdb44b`) BIND-3's D1 closed,
bucket 3 ported with two cases ruled unportable (D-BIND-47), bucket 4 as one body over every
transport, the CallerTier arm with a case given back to the C++ suite, and the per-row publish
benchmark (D-BIND-48/49).

**Thirteen maintainer rulings, D-BIND-42 to D-BIND-54.** Three were missing declarations found by
writing their callers (42, 43, 46 — each an ABI minor bump); two were managed signatures frozen
without a correct native contract (44, 45 — no ABI change); one narrowed what the round counts
as owed (47); two shaped and then accepted the benchmark (48, 49 — the fused per-row path at
1.95× generated C++, batch at 1.23×, a lone non-Arrow row at 19.6× and two-thirds of that
Apache.Arrow's; no D-BIND-1 STOP-AND-ASK); and **five came out of the review**: 50 and 51 amend
D-BIND-18, 52 implements the schema-watch pair (ABI 0.5), 53 amends bullet 8, and 54 narrows
the format exception to a codec-origin `InvalidArgument`, so the overflow bullet 8 proves arrives
as exactly `FletcherException` rather than as the malformed-input subclass.

**The review is the part of this item worth reading first** (`plans/reviews/BIND-4-codereview.md`,
`BIND-4-conformance.md`). **Method:** three reviewers with FRESH context — native shim, managed
tier, tests and CI — each finding then re-verified end to end, several by reproduction or
measurement. **Outcome on first reading: 6 BLOCKERs, conformance 6 of 12, and BIND-4 did not
close.** Two blockers were design defects in the lifetime and refusal layer, both carried in from
D-BIND-18 and both reproduced: a SIBLING cancelled from inside a delivery could have its
`GCHandle` freed under a delivery on another thread, because the seam's "begins" is passing the
gate and the managed counter increments later (D-BIND-50); and the refusal was keyed per
Subscriber where the seam's rule is per PROVIDER, so disposing another Subscriber from a handler
TERMINATED THE PROCESS — the fixed tests crash the host against the old check (D-BIND-51). The
other four were checks that did not guard what they claimed: `pr_gate` ignored the
transport-conformance lane's result; the CallerTier checker counted strings (a skipped or
commented-out mirror passed — the old checker accepts eight of ten fixture fakes); nine mirrors
could not fail for their property, one mirroring a different case entirely; and six drain tests
passed on `inprocess`'s own mutex. **Making the mirrors faithful found a seventh defect (B7):**
`Unsubscribe` returned at once for a subscription already marked retired, so a cancel racing a
self-cancel came back while the handler still ran. **Writing a missing test found an eighth
thing, in a provider rather than the binding:** Fast DDS dropped an oversized row silently on
its only live publish path — recorded as D-BIND-53 / risk B-6, then decided (report it) and,
by the maintainer's ruling, fixed ON THIS BRANCH rather than on `main` (`9255c19`), so bullet 8
holds over a real transport; `main` keeps the drop until #129 lands. Fixes: `1fe40fa` (B1,
B2), `319ad02` (B3–B6, B7, bullet 11's missing tests, stale records), `1e4cc50` (Q1), `a16c083`
(Q2), `9255c19` and `c42763e` (the provider fix and bullet 8's transport test), `309c321`
(D-BIND-54). **All six blockers, B7 and both questions are resolved, and the conformance
review's re-grade reads 12 of 12; the 17 DEBT and 8 NIT items other than those fixed stay open
and, by the round's convention, do not loop the item. So does the public-surface disagreement the
conformance review lists, which is code or document owed row by row and not yet decided.**

**The contrast with BIND-3 is the finding to keep.** BIND-3's same-author review found no defect
in the handle lifetimes. BIND-4's riskiest properties were enforced by a comment and by tests
that could not fail, a same-author pass read both as sound, and fresh context found them in an
afternoon. Every remaining BIND review should be run that way.

**Verification, with the two halves stated apart because they are not equally strong.**
**CI-CONFIRMED at `a16c083`** (`ci.pr` run 36128538622, first attempt): **50/50** jobs green,
read by count, not badge. Managed **261/261** on all four legs (Linux and Windows, net8.0 and
net10.0); c-abi **71/71** on both platforms; cross-transport **12/12** on both;
binding-abi-conformance **11/11** on both. **LOCAL ONLY at `309c321`** (Windows): managed
**263/263** on both TFMs, cross-transport **13/13** over a shim rebuilt from the tree, c-abi
**71/71**, the format lane clean. The three commits after `a16c083` (`9255c19`, `c42763e`,
`309c321`) postdate that run, and were held back so as not to cancel it. **CI-CONFIRMED at
`75eb19e`** (`ci.pr` run 36132087821): **50/50**, managed **263/263** on all four legs,
cross-transport **13/13** and c-abi **71/71** on both platforms, binding-abi **11/11** on both -
so the Fast DDS fix and D-BIND-54 hold on Linux as well. Every new test in the review fixes was shown to fail against the old
behaviour by a compiling mutation, except where the property is the native Subscriber's (the drain
mirrors), which the review states.

**Found by building rather than by reasoning, and worth not rediscovering.** A mutation that
"fails as expected" must be checked to COMPILE: an `if (true)` mutation left unreachable code, CS0162
became an error under TreatWarningsAsErrors, and every "failure" was a build failure. The Edit tool
decodes `\uXXXX` in the text it is given — a C# `"\uE000"` landed as the raw, invisible character;
build such values in code (`char.ConvertFromUtf32`). `inprocess` delivers one at a time PER
INSTANCE, so a case that needs two deliveries in flight on one Subscriber cannot be built from
managed code — but one that needs them on two Subscribers can, over two instances. A repeat
cancel must reach native: the seam's no-op for a fully cancelled id and its wait for a retiring
one are two different answers a managed short-circuit collapses into one. And a gateway test that
publishes once over Fast DDS failed 2 times in 340 runs, never under forced CPU load, with a
TRANSIENT_LOCAL + RELIABLE QoS that should have made "dropped before the match" impossible —
flagged separately, cause not established.

**After close (D-BIND-55, same day).** The conformance review's one undecided list — the public
surface note against the shipped API — was ruled in four questions. Re-deriving it from the code
rather than from the review corrected the review three times: a promise it attributed to D-BIND-22
that D-BIND-22 does not make, four differences it missed, and one "naming" difference that was a
leak (an owned `SchemaHandle` had no finaliser, where every other native handle is a
`SafeHandle`). Six rows of the note were amended to the code; `IsSchemaless`, the finaliser,
`Diagnostics.AbsorbedTotal` and `SchemaArrival.WaitAsync` were built; `BlobHandle` was deferred with
a trigger. Managed **277/277** on both TFMs locally, eight compiling mutations each caught. **A
process-wide observable needs its own non-parallel collection** — `ProcessWideTests` is the first
in the suite, and it is what lets the counter test assert an exact delta instead of "at least".

**Reopened (D-BIND-56, same day).** Reporting progress against Feature 16353's requirements found
that no C# had ever published or subscribed over XRCE-DDS — bucket 4's XRCE row was a typed refusal
with no Agent, the round trip it deferred to lives in a C++ lane, and **the close-out re-grade had
marked bullet 10 met without answering the code review's D15, which said exactly that.** The
transport lane now builds a MicroXRCEAgent from a recipe shared with the interop lane, the suite
proves it owns the Agent (a C# copy of PDA-DEC-1H's rule, with both forcing tests), and `xrce` joins
every theory, plus one case across the Agent's bridge to Fast DDS. Locally **21/21**, with three
falsifications. **Re-closed on CI at `78ac363`** (`ci.pr` run 36144203527, 50/50): the transport lane **21/21** on
Linux and Windows, the Agent built cold from the shared recipe in both lanes on both platforms,
managed **277/277** on all four legs, c-abi **71/71**, binding-abi **11/11**. **The lesson is the same one
this entry already names**: a re-grade written by the author of the fixes read "resolved" off the
BLOCKER list and never re-read the DEBT that carried an acceptance bullet's other half.

**Reopened again (D-BIND-57, same day).** Laying out the `TopicOptions` gap for BIND-5 found that
#128 had reached this branch four hours AFTER bucket 3 was ported, grown its file set from 23 cases
to 49, added per-topic options to the seam that the ABI did not carry, and changed five more
matrix files - and that nothing re-derived the matrix after the merge, so both closes today claimed
a file set 26 cases short. The ABI gains a `*_with_options` pair (0.6, 46 entry points), C# gains
`TopicOptions` and the schema watch, the 26 are mapped (12 over `inprocess`, 11 over Fast DDS, 3
excluded), and **`scripts/check_test_matrix.py` runs on every pull request** - without a path
filter, because a merge of `main` changes counts without putting the files in the PR's diff.
Locally: c-abi **76/76**, managed **286/286**, transport **32/32**, eight compiling mutations each
caught. **The lesson is about a rule this log already wrote down**: "a lane that gains a FILE
dependency needs its checkout AND its path filter" was learned for CI; the matrix needed the same
treatment - a table that says "re-run the command" is a table nobody re-runs.

**Re-closed (D-BIND-57).** CI-confirmed at `bdd4fba` (`ci.pr` run 36153470901, 50/50 on its second attempt): managed **286/286** on all four
legs, transport **32/32** and c-abi **76/76** on both platforms, binding-abi **11/11**, pubsub-conformance
**146/146**, the XRCE interop lane green. The first attempt failed ONE case on Windows -
`XrceCrossProcess/ProviderConformance.SubscribeNeverBlocksSchemaArrivesLater`, "XRCE: failed to create
subscriber participant (status=255)" just after the Agent established the session - in a suite this
push did not touch (nothing under the providers, `pubsub`, `core` or the conformance suite changed;
the lane ran only because `c-abi` did). It passed on the re-run and has passed on every earlier run of
this branch: recorded as an intermittent XRCE failure, cause not established, not as fixed.

## BIND-5 — `SubscriberArrow` and the copy oracle from C# (2026-09-28 to 2026-09-29, CLOSED)

**5a - `SubscriberArrow`, committed at `0da07c7`.** Batch-first as C++: flush on `MaxRows`, on a
deadline armed by the window's first event, or on close; one native decode per window, a
row-by-row second pass only when a row in it is corrupt, so attachments stay aligned. The surface
was ruled first (D-BIND-59): one `RecordBatchHandler` for both forms, owned `AttachmentsBuilder`
copies, the handler owns a `RecordBatch?`. `test_pubsub_arrow`'s 33 cases: 32 mapped, 1 excluded by
D-BIND-8; the mapping is the header of `SubscriberArrowTests.cs`.

**The port found a real defect (D-BIND-60).** Porting `PublishTypeMismatchThrowsToCaller` showed the
shim published rows bound under ANOTHER schema unchecked - an int64 row reached an int32 topic's
subscriber as 9 bytes. C++ cannot reach this (one codec per topic); a binding's rows carry their own.
Both publish entry points now refuse it. **The fix surfaced something not understood:** throwing while
a `lock_guard` was held left the mutex locked for the next call, which standalone MSVC repros did not
reproduce. The shipped code throws after release; the cause is an open finding, not a fix.

Locally: managed **313/313** (net8, net10), c-abi **78/78**, transport **25 of 33** - the 8 `xrce` cases
need a MicroXRCEAgent this machine does not have - with the new Fast DDS schema-watch case green. **CI-confirmed at `0da07c7`** (`ci.pr` run
36405073609, 50/50 on its first attempt): managed **313/313** on all four legs, transport **33/33**
and c-abi **78/78** on both platforms - the `xrce` cases included, and the shim fix on Linux -
binding-abi **11/11**, pubsub-conformance **146/146**.
Falsified: six managed mutations (fallback off, no closing flush, drop-only window skipped, silent
absorb, misaligned attachments, options dropped) and the shim check's removal, each caught by the
cases named for it. **A lesson from the tooling, again:** a `conan build` that needed remote
credentials sat on a prompt for ten minutes, and the mutation script that was meant to run first had
already failed on a cp1252 decode - so for that stretch nothing was mutated. Mutate bytes, and build
the existing tree with `cmake --build`.

**5b - the copy oracle from C#, committed at `38d6d78`.** Two rulings came first. D-BIND-61:
the fused publish is scored by the SOURCE of the window's payload bytes, which
`WriteBuffer::Append` exposes on a window kept exactly full. The controls are a staged publish and a
copied export. D-BIND-62: the instrument moves into a test-only package, `fletcher-copy-probe`, and
c-abi builds `fletcher-c-abi-probe` only under `with_probe_shim`. The mechanism was explained in
detail before the second asking; the first asking was set aside unanswered.

**What was built.**
- The package holds the ledger and `Judge()`, moved unchanged; `SeamProbeProvider`, now public, with
  a fourth mode `kTracing`; and the tracing window.
- pubsub-conformance names the moved types through `using` declarations, so no clause changed.
- c-abi's probe shim is a second SHARED target over the same object library. Its own builtins file
  adds `probe`, and it exports two `fl_test_*` entry points from a test header.
- `Fletcher.CopyOracle.Tests` stages the probe shim under the shipped shim's name, so
  `Eiva.Fletcher.Interop` loads it exactly as it loads the real one.
- The CI lanes are wired in: c-abi creates the package with its tests and runs the probe suite;
  dotnet passes both shims; pubsub-conformance creates the package.

**Measured, locally.** C#'s fused publish scores `encode_copies == 0`: the payload is appended from
the RecordBatch's own buffer. It also scores `row_copies == 0`: the handler's span IS the window.
The staged managed publish and the copied export each score exactly 1. Two mutations of the SHIPPED
managed code went red: `Publisher.Publish` encoding into managed memory then publishing raw, and
`FletcherCodec.Bind` exporting a deep copy. Those are the two managed failure modes D-BIND-58 named,
and the acceptance case caught each one.

**A mutation survived, and it found a hole.** Disabling the tracing window's "records tile the window"
check left all nine of its tests green. The check could never fire, because records are pushed where
the last one ended by construction. The guard D-BIND-61 actually relies on is the position check in
`Reconcile`, and that one returned early when nothing had been recorded yet. So bytes written unseen
before the first record would not have faulted. The unreachable loop is gone and the early return is
closed. Four tests now drive the fault path through a subclass that writes past the window's back,
standing in for the future inline append the guard exists for. Five instrument mutations are now each
caught, and the package has 13 tests.

**CI-confirmed at `38d6d78`** (`ci.pr` run 36443379137, 50/50 on its first attempt): copy-probe **13/13** and c-abi **86/86** on both platforms, managed **313/313** and the oracle **5/5** on all four legs, pubsub-conformance **146/146**, transport **33/33** and binding-abi **11/11** on both platforms. The Linux probe shim, its version script and the staged `libfletcher-c-abi.so` name all held on their first run.

**Lessons, again, from the tooling.**
- A mutation loop leaves the LAST mutant's build in the Conan cache as the latest revision. Every
  result after it was checked against the clean revision, the stale ones were removed, and everything
  was rebuilt from clean sources before these numbers were taken.
- A heredoc ate a backslash twice. The scripts that need escapes are now written as files.

**A review while 5b's CI ran: the C++ and C# calling sequences, side by side.** Asked for by the
maintainer, and read-only until a finding was ruled on.

- **Publish side.** C# is at least as zero-copy as C++, and better on the fused path. C++'s
  `PublisherArrow::Publish(ArrowRow)` stages every row in a `thread_local` scratch vector, so that a
  type mismatch reaches its caller as `std::invalid_argument`. C#'s `fl_publisher_publish_row`
  encodes in the provider's window. D-BIND-60's pre-check refuses the mismatch, and Fast DDS now
  rethrows encoder failures after `write()`.
- **Arrow subscribe side.** C# copies rows and attachments where C++ keeps borrowed bytes, as ruled
  (D-BIND-25, D-BIND-58, D-BIND-59).
- **One real divergence, ruled and fixed the same hour (D-BIND-63):** C++ splits a batch window
  before it overflows Arrow's 32-bit offsets, and C# had no ceiling at all. CI-confirmed at `255ad43`
  (`ci.pr` run 36447834274, 50/50 on its first attempt): managed **316/316** and the oracle **5/5**
  on all four legs.
- **Smaller findings, recorded for the fresh-context review:**
  - the copy oracle measures `fl_publisher_publish_row` but not `fl_publisher_publish_rows`;
  - `fl_publisher_publish_rows` does not check `first + count` against the batch before publishing;
  - a codec failure inside a fused publish probably carries a transport-dependent status (not
    verified);
  - C++'s batch callbacks can overlap across its two threads, where C# serialises them, so a
    row-limit flush on the delivery thread can wait behind a timer-thread handler;
  - C++'s staging copy is not written down on the public-surface note.

**The BIND-5 review, and its rulings.** Three fresh-context reviewers found 9 BLOCKERs, 5 questions,
23 DEBT and 17 NITs (`plans/reviews/BIND-5-codereview.md`); the conformance pass graded 5 of 11
bullets as conforming, so BIND-5 does not close. The copy oracle held: no reviewer found a way to
make it report a false zero-copy. The maintainer ruled the five questions as D-BIND-64 to D-BIND-67
plus the diagrams.

**B1 fixed, and D-BIND-60's open finding explained.** The helper that threw had C linkage, because
it was declared inside the `extern "C"` block, and MSVC's `/EHsc` assumes such a function never
throws. An experiment with the original lock-held throw settled it: inside the block it reproduced
"resource deadlock would occur", and moved out it passed. The earlier standalone repros had missed
it because their helper had C++ linkage. **The lesson:** "cause not established" was the right
record, and it held long enough for someone else to find the cause.

**Closed on CI.** `ci.pr` run 36563607601 at `a43c384`, 50/50 on its first attempt: c-abi **93/93** (Windows with `/EHs`, and Linux) and copy-probe **13/13** on both platforms; managed **326/326** and the copy oracle **5/5** on all four legs; transport **34/34** on both platforms, the `xrce` cases included; binding-abi **11/11** and pubsub-conformance **146/146** on both. This was the first CI for ABI 0.7, `/EHs`, and D-BIND-67 in the lanes that
publish bound rows, and nothing needed a second attempt.

**What BIND-5 took, start to close:**
- 5a (`SubscriberArrow`) and 5b (the copy oracle from C#);
- one side-by-side C++/C# sequence review (D-BIND-63);
- a fresh-context review that found 9 BLOCKERs, all fixed;
- rulings D-BIND-58 to D-BIND-68, and ABI 0.6 to 0.7.

**The lesson worth carrying into BIND-6** is the one BIND-4 already taught: the port's own tests hid
three behaviours the C++ original has, because they changed the parameters of the case they mirrored
(`MaxRows = 100000` and a closing flush, where C++ used `max_rows = 2`). A mirror that changes the
inputs can no longer fail for the reason its name gives. Port the parameters too, or say in the
header why not.

**Parked for BIND-9 and BIND-10**, all in `plans/reviews/BIND-5-codereview.md`:
- T-D6 to T-D10;
- N-D1, N-D2, N-D5, N-D6;
- M-D3 to M-D5;
- the NITs.

**After close, 2026-09-29: a handover the close-out missed.** BIND-3 handed BIND-5 one sentence:
the copy oracle's scope note must say that the UTF-16↔UTF-8 transcode (D-BIND-1b) happens in
`Apache.Arrow`, before the scored path, so zero copies says nothing about the cost of converting a
.NET string. The round's definition of done asks for it too. Only `FletcherCodec`'s XML docs said
it. It is now in the header of `CopyOracleTests.cs` and in pubsub-conformance's README, "What green
does NOT prove". The same sweep fixed two stale lines: that README still said no C# binding exists,
and D-BIND-58's deferred `fl_blob_wrap` still named ABI 0.7, which D-BIND-68 took. Documentation only:
no code, test or result changed, and BIND-5 stays closed. PR #129 and ADO 18787 and 16353 were
updated for BIND-5 the same day.

## Between BIND-5 and BIND-6 — the generator's design, and a topic-path fix (2026-09-29 to 2026-10-02)

**Four rulings before any BIND-6 code.** The five boundary questions became D-BIND-69 to D-BIND-71,
and ADO 18789 added D-BIND-72:
- **D-BIND-69:** C# namespace `Fletcher.Gen.<PascalPkg>`, flat `Outer_Inner`, protoc's identifier rule.
- **D-BIND-70:** enum members by protoc's rule (`Color.Red`).
- **D-BIND-71:** consumers run the plugin as C++ and TypeScript consumers do, through a thin locator
  (`Eiva.Fletcher.Protoc`, BIND-9).
- **D-BIND-72:** one generated shape for every language; the C# gateway client consumes the same
  generated class and Arrow schema.

D-BIND-72 came from ADO 18789, whose review comment by Oliver Monberg-Jensen was answered there; his
three packaging pitfalls became BIND-9 requirements. Checking D-BIND-72 against a real generated C++
header corrected three loose statements the same day, and added point 6: C#'s topic segments follow
C++'s.

**Point 6 surfaced a cross-language defect, now fixed on this branch.** For a dotted package the
C++ and TypeScript backends named different topics: C++ `{"eiva.nav", "Svc", "Method"}`, TypeScript
`'eiva/nav/Svc/Method'`, which the gateway splits into four segments. Both forms date from the
plugin's first commit. `protoc/tests/test_topic_path.cpp` runs the real generator in memory and
requires the TypeScript constant to equal C++'s `TopicKey()` and, split on `/`, C++'s
`TopicSegments()`. It was red on the dotted case only; the single-segment case was always green. The
maintainer ruled C++'s form canonical, so `ts_backend_visitor.cpp` drops `DotToSlash`. By the
maintainer's choice the fix lives in #129, not in its own PR to `main`, as an exception to D-BIND-30
recorded there.

**Verified** in the Linux devcontainer (`eivaorg/fletcher-devcontainer:main`), on this branch's
tree: **101/101** plugin tests, test_package **3/3**, `TsVisitor.DescriptorByteIdentical` unchanged,
clang-format-18 clean. The local machine had lost Conan, Python and CMake to a crash and restore, so
the container is now the local build route until the Windows toolchain is reinstalled.

**A lesson from the tooling:** `conan create` on sources identical to a package already in the cache
skips the build, and with it the tests: the first run on this branch reported only test_package's 3.
Force the component with `--build="fletcher-protoc/*"` when the point is to run its tests.

## BIND-6 — the C# generator (2026-10-03, IN PROGRESS)

**Slices:** 6a plumbing, type table, visitor, classes and enums with scalar fields; 6b `Schema`,
`ToArrow`/`FromArrow` and `integration-tests/protoc-dotnet`; 6c composite and temporal types; 6d topics
and the native pair; 6e cross-file, NativeAOT, side by side with protoc's C#, and the review.

**Ruled first (D-BIND-73).** D-BIND-69 and 70 adopted "protoc's C# rule", so protoc's source
(`compiler/csharp/names.cc`, `csharp_helpers.cc`, `csharp_enum.cc`) was read and became the
specification. It answered D-BIND-69's open package question and D-BIND-70's enum edge cases. It also
showed that protoc suffixes a property on its own reserved member names too; the maintainer ruled that
our class's members (`Schema`, `ToArrow`, `FromArrow`) join that list, so every property is named as
protoc names it and none can collide with ours.

**6a.** `--fletcher_opt=csharp` emits `<stem>.fletcher.cs` from `csharp_backend_type_table` and
`csharp_backend_visitor`, a third backend pair on the IR beside C++ and TypeScript: the
`Fletcher.Gen.<Package>` namespace, one `enum` per proto enum, one `sealed class` per message with a
`{ get; set; }` property per scalar field, nullable where the schema is. Composite and temporal fields
are marked in the output as 6c's, not dropped silently.

- **Tests first:** 25 new cases were red against a stub (24 failing; the one negative case passed
  trivially), then green. `test_csharp_visitor`'s no-drift case requires every existing output to stay
  byte-identical with `csharp` added, under four option sets, and exactly one file more.
- **One test was wrong, not the code:** proto3 refuses two enum values that collide once the prefix is
  stripped unless they are aliases, so the collision case is an alias pair.
- **Compiled, not just string-matched:** C# generated from a 16-field `.proto` built on Windows with
  every analyser as an error. The first build failed on 9 analyser findings (CA1707, CA1819, CA1069)
  caused by names our rules produce by design; the file now opens with protoc's `<auto-generated>`
  block, which Roslyn's analysers skip, and the same build is clean.
- **Falsified:** five mutations (a reserved name dropped, the duplicate-enum loop removed, the file
  emitted without the token, nullability ignored, the marker removed) are each caught by the test
  named for them; each mutant compiled, on a scratch copy outside the Conan cache.

**6b-1, the generator half of 6b (2026-10-05).** 6b was split: 6b-1 emits `Schema`, `ToArrow` and
`FromArrow`; 6b-2 adds `integration-tests/protoc-dotnet` and its lane.

- **One schema, by construction.** The plugin already had the right tool: `SchemaVisitor` drives an
  abstract `SchemaSink`, and two sinks turned one walk into C++ source and a live `ArrowSchema`. A
  third, `CsSchemaSink`, records the same calls as a tree and renders Apache.Arrow C# construction
  code. It reproduces nanoarrow's own defaults (a list's child is "item", nullable; a map's
  "entries" and "key" are not), and copies a nested message inline by running the visitor into the
  subtree, as the in-process sink deep-copies it. So the C# `Schema` carries C++'s fields,
  nullability and metadata, `metadata_from_option` extras included, without a second walk.
- **Conversion only where it is whole.** `ToArrow(IEnumerable<T>)` and `FromArrow(StructArray, int)`
  are generated for a message whose fields are all mapped scalars; any other message gets its full
  `Schema` and a marker naming the field 6c must add first. A `ToArrow` that skipped a field would
  build a batch its own `Schema` disagrees with.
- **Tests first, shown red against the committed 6a code:** the 5 new visitor cases failed there and
  the other 126 passed. One 6a case had asserted a whole class body and was narrowed to how the class
  opens. 133/133 now, with two type-table cases added.
- **Measured against the real oracle.** Outside the suite, the generated C# for five messages
  (lists, an inline struct, a map, a list of structs, Timestamp, Duration, a wrapper) was compiled
  against Apache.Arrow 23.0.0 with every analyser as an error, and each `Schema` compared with the
  `.ipc` file the plugin writes from nanoarrow: names, types, nullability, metadata and key order,
  all the way down. They are equal. A planted difference in an item's nullability, and another in
  metadata order, were each reported, so the comparison can fail. Three rows with nulls, extremes
  and non-ASCII text round-trip through `ToArrow` and `FromArrow`.
- **A wrong guess the build caught:** `DurationType` has no public constructor in Apache.Arrow 23.
  Reflection over the restored assembly showed `FromTimeUnit`, which the type table now emits, and a
  type-table case pins every expression the sink renders.
- **Falsified:** six mutations (list item and map key nullability, conversion despite an unmapped
  field, the wrong column, a non-nullable string read back as null, root metadata dropped) are each
  caught by the test named for them; each mutant compiled.

**6b-2, the consumer's side (2026-10-05).** `integration-tests/protoc-dotnet` makes 6b-1's hand-run
measurement a test, and `ci.integration-test.protoc-dotnet` runs it on Linux and Windows.

- **The project is a consumer.** It references Apache.Arrow and xunit, nothing of Fletcher's, and its
  `FletcherGen` target is D-BIND-71's recipe: protoc from an MSBuild `Exec` before compile, the plugin
  from `$(FletcherProtocPlugin)`, the generated file added to `@(Compile)` inside the target. So the
  build itself proves the model needs only Apache.Arrow (D-BIND-72), and the recipe works.
- **16 cases:** every generated `Schema` against the `.ipc` the same run wrote, with a case proving
  the comparison can fail; the round trip with nulls and extremes; the slice boundary (`Player` has
  no conversion yet, and the case says it is the one 6c must change); the namespace, the names and the
  enum numbers; no Fletcher reference in the compiled assembly. Every analyser runs as an error.
- **Falsified end to end:** a plugin built outside the Conan cache with a map key rendered nullable
  makes exactly one case fail, naming `Player.tags/entries/key`.
- **The lane** follows the Rust lane's discovery block, with one change it states: on Windows both
  paths go through `cygpath -w`, since MSBuild reads them as Windows paths and Git Bash's `find`
  returns `/c/...`. 16/16 locally on Linux; Windows is CI's.
- **Deferred to 6d:** the C#-versus-C++ wire-bytes comparison needs the native shim, so it lands with
  the native publisher pair rather than growing this lane.

**6c begins: where the temporal types live (2026-10-05, D-BIND-74; scaffold `8ceaa1e`).** The
lossless `Timestamp` and `Duration` (D-BIND-26) needed a home that keeps the model layer on
Apache.Arrow alone (D-BIND-72).

- **The first ruling was wrong, and a question caught it.** I recommended emitting the types once per
  protoc run from `GenerateAll`, and the maintainer took it. The maintainer then asked whether three
  packages were still right given the Apache.Arrow reference. They were not the point: generated code is
  compiled into the CONSUMER's assembly, so two assemblies that each run protoc each define `Timestamp`
  in the same namespace, and an app referencing both gets CS0433. Per file fails earlier (CS0101).
  I had weighed the single-project case and not the several-assemblies one.
- **Re-ruled the same day:** a fourth managed package, `Eiva.Fletcher.Model`, `Apache.Arrow` only, holding
  exactly `Timestamp` and `Duration`; user messages stay in the consumer's assembly, and anything else
  joining it needs its own ruling. protoc's C# does likewise with `Google.Protobuf` (read in
  protocolbuffers/protobuf `main`). D-BIND-14' now says four. The package name is open to rename until
  BIND-9.
- **Scaffolded at `8ceaa1e`:** `dotnet/src/Fletcher.Model` and `tests/Fletcher.Model.Tests`, in
  `Fletcher.slnx`. Second, millisecond and microsecond values convert exactly; a nanosecond value floors
  to a 100 ns tick toward negative infinity; overflow throws; equality is structural (one second is not
  1000 milliseconds); the timezone is metadata. 47 cases per TFM (net10.0 and net8.0), six mutants each
  caught, the CI format command clean. The model tests reference Model alone, so Arrow-only is shown by
  construction. The first format run missed an IDE1006 because `--include` passed vacuously; only the
  exact CI command caught it.
- **Not yet:** `Eiva.Fletcher` does not reference Model, and `protoc-dotnet` has no reference to it; both
  wait for the 6c generator, which must write `global::Eiva.Fletcher.Model.Timestamp`.
- **CI:** `ci.pr` 37451032552 on `8ceaa1e` was green on the rerun. One job, the Fast DDS cross-process
  case `BacklogNeverInterleavesWithLiveSamples`, failed once with "nothing arrived at all" on a commit
  that touched no C++; across 40 runs that is one data point, so it is on a watch list, not fixed.

**6c-1, every field gets its property (2026-10-07, D-BIND-75).** 6c was split as 6b was: 6c-1 gives each
field a typed property, 6c-2 converts the composite ones. The maintainer ruled one question first, the
map shape, and the rest followed from earlier rulings.

- **A map is an ordered list of pairs.** `List<KeyValuePair<K,V>>`, because the Arrow map keeps entry
  order and duplicates on the wire, and C++'s row class holds `std::vector<std::pair<K,V>>`. A
  `Dictionary` would enumerate in an order .NET leaves unspecified, so 6d's byte comparison against C++
  could fail for a reason the model hides. A lookup type of our own was declined: it would be a third
  type in `Eiva.Fletcher.Model`.
- **The other shapes:** a repeated field is `List<T>`, a message field is the generated class (`?` when
  absent), and Timestamp and Duration are the `Eiva.Fletcher.Model` types, `?` only where the schema's
  column is nullable. `CsFieldTypeOf` in the type table is the one place that decides.
- **The test found two things I had wrong.** I had written `Timestamp?` for a plain Timestamp field; the
  schema says its column is non-nullable, so a plain field is always present and only an `optional` one
  is `?`. And `repeated google.protobuf.Timestamp` is not a list of timestamps in this IR but a list of
  the struct `seconds`/`nanos`, declared in another file, so naming its C# type would emit a class that
  does not exist. Such a field is now a comment naming BIND-6e, and conversion waits on it.
- **Verified:** 139/139 plugin tests, clang-format-18 clean, and the generated file builds with every
  analyser as an error in `integration-tests/protoc-dotnet`, now referencing `Fletcher.Model`: 20/20 on
  Linux. Windows is CI's.
- **A test that could not fail, found on the way.** The model-references-nothing test looked for assembly
  names starting `Eiva.Fletcher`; the assemblies are named after their projects, so it would never have
  fired for the codec, the interop package or the gateway client. It now looks for `Fletcher`,
  `Fletcher.*` and requires exactly `Fletcher.Model`.
- **The format lane changed with it.** Referencing `Fletcher.Model` made `dotnet format`'s design-time
  load, which runs no protoc, report every generated type as CS0246 where it used to fail quietly; the
  lane now excludes that one diagnostic for this project only, and the ci.pr filter and the lane's
  comment name `Fletcher.Model`, `Directory.Build.props` and `.editorconfig` as inputs.

**6c-2, conversion for the composite and temporal fields (2026-10-07).** Every message whose fields all
convert now gets `ToArrow` and `FromArrow`, and an `internal ToArrowColumns` that a message embedding it
reuses, so nesting is one generated class calling another.

- **Probed first, as 6b-1 did for `DurationType`.** Apache.Arrow 23 has public constructors for
  `ListArray`, `StructArray` and `MapArray` that take prebuilt children, so a nested column is built from
  the element class's own columns rather than through builders that cannot hold a struct. The same probe
  answered a BIND-7 question in passing: `StructArray.Fields` returns children already sliced to a sliced
  parent's offset (offset 2 and length 2 after `Slice(2, 2)`), which D-BIND-10 asked to be settled
  empirically.
- **The schema decides every type.** A composite column's Arrow type is read from the class's own
  `Schema`, so `ToArrow` cannot build a column its `Schema` disagrees with, which was 6b's rule for
  scalars.
- **Writing Timestamp and Duration never drops a digit.** The column is in nanoseconds; a value is
  recounted into the column's unit by a new `WithUnit` on the `Eiva.Fletcher.Model` types, which
  multiplies exactly and refuses a coarser unit that would leave a remainder. Reading returns the column's
  unit, so a value written in seconds comes back as the same instant in nanoseconds; structural equality
  sees two values, `ToDateTimeOffset` one instant. Fourteen Model cases cover it, 61/61 on both TFMs.
- **A test that would have hidden layout errors.** `RecordBatch`'s constructor does not check a column
  against its field, so a wrong offsets buffer would round-trip through our own `FromArrow` and look
  right. Every batch in the consumer tests is therefore written as an Arrow IPC stream and read back,
  which does check it.
- **Blocking is explicit and propagates.** A field without a class here (a message from another file) and
  a message embedding one whose conversion waits both leave a marker naming the field and the reason, so
  no class calls a `ToArrowColumns` that does not exist.
- **Verified.** 144/144 plugin tests and clang-format-18 clean; the consumer project 24/24 on Linux with
  every analyser as an error (the first build failed on `GetValueOffset`, obsolete in Apache.Arrow 23, now
  `ValueOffsets`). Eight mutants of the generated C# (validity, offsets, unit recount, unit on read, null
  struct read, list start, map key slot) were each caught by those C# tests, not by a failed build. Two
  of the first sed patterns had missed the reformatted source and one hit a different line than named;
  they were retargeted by line and rerun rather than counted. Windows is CI's.

**6d-1, the generated native pair (2026-10-07, D-BIND-76 to 79).** Four questions first, each
answering what D-BIND-49 and D-BIND-72 had left to BIND-6, one at a time:

- **D-BIND-76, ruled twice.** The first answer was a second token, `csharp_native`, to ask for
  `<stem>.fletcher.native.cs`. Before it was committed the maintainer asked why one `csharp` was not
  enough. The reason was D-BIND-72's Arrow-only model layer, but the repository already answers that
  differently for C++: its header always carries the pair and `schema_only` opts out. Re-ruled the same
  day to match: `csharp` writes the pair beside the model, and `csharp_model_only` opts out, leaving the
  model byte-identical (a test holds that). `integration-tests/protoc-dotnet`, the Arrow-only consumer,
  now passes it, and a case there checks that no native file was written. Inside the plugin the two
  tokens fold into one `CsharpOutput` (`None`, `ModelAndPair`, `ModelOnly`) rather than two booleans,
  so the meaningless fourth combination, the modifier alone, cannot be represented; a parser case pins
  the mapping in every token order.
- **D-BIND-77:** the topic is a model-layer `<Svc>_<Method>Topic` with `Segments` (C++'s form, the
  package one segment) and `Key`; the pair's `Topic` is `TopicPath.Of` over them.
- **D-BIND-78:** `Publish(T)` stays per-row and synchronous, as C++'s does, its documentation stating
  D-BIND-49's measured price and pointing at the batch form.
- **D-BIND-79, a frozen signature that could not compile.** Public surface 2.7 said
  `Subscribe(Action<Msg, AttachmentsView>)`. `AttachmentsView` is a ref struct, and `Action<,>` takes one
  only from .NET 9, so a net8.0 consumer fails with CS9244; measured before asking, on SDK 10.0.401.
  The subscriber now takes a generated `<Svc>_<Method>Handler` delegate, which compiles on both
  frameworks and keeps the attachments borrowed for the call.

**Where the pair is tested, ruled with them:** the plugin's output for a fixture `.proto`
(`protoc/tests/golden/csharp_pair.proto`) is committed as goldens beside the `.ipc` ones, a plugin test
fails if they differ from its output by a byte, and `Fletcher.Tests`, which already stages the shim and
the `.ipc` goldens, compiles them. So no new CI lane: the dotnet lane's sparse checkout and path filter
already carry `protoc/tests/golden`. The goldens were written by protoc itself, as a consumer's call
writes them, and the in-process test agreed with them on the first run.

- **Verified.** 150/150 plugin tests; `Fletcher.Tests` 337/337 on both frameworks with 11 new cases over
  `inprocess`: a row with every kind arrives whole, an empty row arrives empty, the batch form keeps
  order, attachments travel, the bytes are the codec's own encoding of `ToArrow`, a throwing handler is
  absorbed and reported, nothing arrives after `Unsubscribe`. Six mutants of the generated pair
  (attachments dropped, batch truncated, bytes ignored, topic renamed, subscriber and publisher on the
  wrong topic) were each caught, none by a build failure.
- **Two of my own test assumptions were wrong, and the tests said so.** One asserted that the native file
  never contains `SubscribeInPlace`, and my own doc comment names it to say where to go instead; the case
  now looks for a member. The other compared a round-tripped row's Timestamp including its unit, but a
  default Timestamp is `(0, Second)` and comes back as `(0, Nanosecond)`, the same instant recounted as
  6c-2 specified; it now compares the instant.
- **Still owed, as 6d-2:** the C#-versus-C++ wire-bytes comparison, which needs a C++ build of the same
  `.proto`'s generated class beside the shim.

**6d-2, C# against C++ on the wire, and the gap it found first (2026-10-07).**

- **The C++ side was already in the tree.** `integration-tests/protoc-coverage` pins the generated C++
  class's `Encode()` of the coverage fixture rows as twelve committed `.v1.bin` goldens (its parity
  oracle). So no C++ build beside the shim was needed: C# only had to produce the same bytes for the
  same `.proto` and the same values.
- **The first probe found a 6c gap, fixed first as 6c-3 (the maintainer chose that order).** Running
  the plugin's C# over `coverage.proto` showed that lists of lists, which flatten wrappers around a
  repeated message produce, got no property at all, with a marker blaming "a message from another file",
  and that this blocked `CompositeCoverage` and `ServiceRequest`, the richest four goldens. Now
  `List<List<T>>` to any depth, built and read level by level, and a field without a type says why in
  the classifier's own words.
- **How the comparison is wired.** The plugin's C# for `coverage.proto` is committed beside the
  `.v1.bin` files and held to the plugin by a new `CsGolden.ModelByteIdentical` in that lane, the way its
  TypeScript golden is. `Fletcher.Tests` compiles it, transcribes `coverage_fixture.hpp` into C#, and for
  each golden encodes the fixture row through `ToArrow` and the native codec, then decodes the golden,
  reads it back through `FromArrow` and re-encodes it. A transcription slip shows as a byte difference,
  never as a pass.
- **Verified.** 25/25 on net8.0 and net10.0 on the first run, so five mutants were made to be sure it
  can fail: a fixture value, a map in sorted order, an absent message written as present, depth-3
  lists dropped, a duration off by one nanosecond. Each was caught. The sorted-map one is D-BIND-75's
  ruling seen on the wire: a `Dictionary` would have changed the bytes.

**6e-1, types from another file (2026-10-07, D-BIND-80).**

- **What was missing.** A field whose message was declared in another file had no property, only a
  marker naming BIND-6e, and its message no conversion. The commonest case is `repeated
  google.protobuf.Timestamp`, which the IR maps as timestamp.proto's struct. Reading the code also
  turned up a latent bug: an enum from another package was named bare and would not have compiled.
- **The naming follows C++.** C++ names `::fletcher_gen::<pkg>::<Name>` and includes the other
  file's header, so its consumer generates every file its imports reach. C# now does the same:
  `CsTypeRef` names the other file's class `global::Fletcher.Gen.<Pkg>.<Name>` (bare in the same
  namespace), for messages and enums alike.
- **The one ruling.** The embedding class used to build a struct column from the nested class's
  `internal ToArrowColumns`. A consumer that keeps shared messages in a contracts assembly would then
  not compile, so the maintainer ruled the cross-file path through the public `ToArrow` (D-BIND-80),
  keeping the same-file call so single-file output did not move by a byte. The mutant that undoes it
  measured the failure as CS0117, not CS0122 as first said: from another assembly an internal member is
  not inaccessible but absent.
- **What changed in the tests because the refusal is gone.** After 6e no `.proto` reaches the "not
  generated" path: the IR drops recursion, oneofs and Any/Struct before C# sees them, and produces no
  kind the C# table lacks. Four plugin cases used `Timed.marks` as their example of a blocked
  conversion; they now assert the cross-file conversion instead, and the "no pair" branch is reached by
  a flatten-wrapper input, the one reason a `.proto` still produces.
- **Verified.** Plugin 156/156. The golden `Marks` gained its pair and the plugin's
  `google/protobuf/timestamp.fletcher.cs` became a golden, so `Fletcher.Tests` (362/362, both TFMs)
  publishes and receives a cross-file row over the native shim. `protoc-dotnet` gained
  `ProtocDotnetShared`, a separate assembly generated from `shared.proto` and timestamp.proto, and
  `Route`, which uses its types five ways: 28/28 on the first run, so four mutants were made, one
  behavioural (two tests fail) and three that the compiler catches with exactly the error each rule
  exists to prevent.

**Tooling now.** The machine lost Python, Conan, CMake and Node in a crash on 2026-10-02. The plugin is
built and tested in the Linux devcontainer (`eivaorg/fletcher-devcontainer:main`); the image has no
.NET SDK, so generated C# is compiled on Windows, where the SDK survived.
