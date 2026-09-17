# BIND — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[BIND-csharp-bindings.md](BIND-csharp-bindings.md) for the tracker and
[BIND-locked-decisions.md](BIND-locked-decisions.md) for the decision digest.

<!-- Entries appended below by the round runbook -->

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
