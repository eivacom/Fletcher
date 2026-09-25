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
