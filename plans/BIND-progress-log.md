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
  (`fl_publisher_publish_rows` already hoists it). BIND-3's per-row publish benchmark settles it;
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
  link the MSVC CRT dynamically — **two the moment BIND-3 links it statically**, which is
  D-BIND-32's own argument pointed the other way. Fixed with a file-local `DisposeLocal`.
  **Watch for the shape again at BIND-3:** anything pairing an allocation in one module with a
  free in another.
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
rebase cannot falsify. One thing F1 did not reach is left for the maintainer: buckets 1+2+3+4
sum to exactly 300, so `test_owned_schema` is not inside that total, yet Q10 reads "275 of 300
(300 less the 24 provider-internal cases and `test_owned_schema`)" — a pre-existing off-by-one
in a ruled figure, which bucket 4's move to 104 compounds (300→302, 275→277). **Unruled.**

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
