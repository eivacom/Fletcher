# BIND-2 — code review, all four slices

**Subject:** the code BIND-2 landed — `c-abi/src/` (codec, containment, handles,
write window, single-copy scan, entry points), `c-abi/tests/`, the copy oracle's
binding leg in `integration-tests/pubsub-conformance/`, and the c-abi lane's size
and export checks. Slices `28cdcd4` (2a), `bec139e` (2b), `3b13cb5` + `0811091`
(2c), `b95affc` + `b6b89b3` + `f9d3ece` + `5762239` (2d).
**Reviewed at:** `5762239`, tree clean, CI green 51/51.
**Why a code review and not a conformance one:** BIND-2's deliverable is *code*,
and the acceptance is checked separately (`BIND-2-conformance.md`, still owed).
This pass asks what the code does, not whether the bullets are ticked. Where the
two overlap — the two recorded deviations, the fusion's acceptance — it defers to
the conformance pass rather than pre-empting it.

**Outcome: 1 BLOCKER, 5 DEBT, 4 NITs, and one coverage statement that belongs in
the tracker rather than in a diff.** No defect was found in the wire format, in
the containment taxonomy, or in the handle lifetimes — the three places a defect
would have been expensive. The BLOCKER is in test code and is invisible today; it
is a BLOCKER because it is aimed squarely at BIND-3.

---

## What I verified (claim → result)

Each row is something the code claims about itself, checked against the code
rather than against the commit message.

| Claim | Result |
|---|---|
| One containment site, and the two mapping tables are selected by origin | ✅ `Contain` is the only `catch (...)` in the component; `Capture`'s seam arm reproduces spec §5.1 exactly and the codec arm adds only `invalid_argument`/`out_of_range`, both documented at the site |
| The origin slot is saved and restored, never assumed | ✅ `WindowBuffer::Grow` and `WriteThroughCaller` both save the ambient value and restore it **only on success**, so a throwing callback keeps `FL_ORIGIN_CALLBACK`. The fusion re-attributes to `FL_ORIGIN_CODEC` inside the encoder lambda and back after, and the containment site reads the slot at catch time — verified by `TheFusedPublishRunsOverTheWholeChain`, which publishes row 99 of a 3-row batch and requires `FL_ORIGIN_CODEC` out of a seam entry point |
| `fl_rows` holds a `shared_ptr` share of the codec, so close-before-unbind is survivable | ✅ `handles.hpp`; asserted by `ClosingTheCodecBeforeUnbindingTheRowsIsSafe` |
| The array is BORROWED and never consumed | ✅ `~BoundRows` resets the *view* only; `BindBorrowsTheArrayAndNeverConsumesIt` releases the export afterwards and would double-free if that were wrong |
| The shim does not re-validate topics; it inherits `RequireSegments` | ✅ for every path that publishes — with one exception, **N1** |
| Every refusal on malformed bytes is the reader's | ✅ `fl_decode_rows` is a pass-through; the three argument checks it *does* make (`out`, `count < 0`, `bytes == NULL && len != 0`) are argument refusals, not byte refusals, and do not weaken `DecodeRefusalsComeFromTheReader` |
| `AppendInPlace`'s two refusals arrive as `FL_INVALID_ARGUMENT`, as `binding.h` promises | ✅ and it is not luck: `core/write_buffer.hpp` throws `PubSubError(kInvalidArgument)`, not `std::invalid_argument`, so the promise survives the seam table's "everything unlisted is kInternal". Attribution is a separate question — **N3** |
| Positions survive a refill, so the back-patched bitfields land where they were reserved | ✅ `WindowBuffer::Grow` re-reads `data`/`capacity`/`pos` after the callback and re-checks `capacity - pos >= min_bytes`; `EncodeElements` captures `Position()` before `AppendZeros` and patches before any further append |
| A poisoned shim refuses before the body runs | ✅ the check is the first statement of `Contain`, and `APoisonedShimRefusesWithoutRunningTheCall` asserts the body did not run — the clause that matters |
| The Linux scan's two false-positive classes are fixed | ✅ main executable skipped in `CollectModuleName`, dedup by symbol address in `ScanModules`. Both reasoned at the site, both with the measurement that produced them |
| 20 of the header's 40 entry points are implemented, and the other 20 are exactly BIND-4's | ✅ mechanically: the absent set is the subscriber half, attachments, blobs and the schema arrival — D-BIND-31's line, with nothing straddling it |

---

## BLOCKER

### B1 — the containment tests free a shim allocation from the test binary's heap, and BIND-3 is where that stops working

`c_abi_tests` links **both** the shim and the object library it is built from
(`c-abi/tests/CMakeLists.txt:53-58`), so the process holds two copies of
`containment.cpp`. That is deliberate and correct: `test_containment.cpp` exists
to reach two branches that have no path through the export table.

The consequence is not. In that file the error is filled by the **test binary's**
copy of `Capture` → `SetMessage`, which allocates with `new[]` in the test
binary; it is then released by `fl_error_dispose`, which is the **shim's**
exported function and does `delete[]` inside the shim
(`test_containment.cpp:85, 95, 125, 136, 148, 155`).

Today both modules link the MSVC CRT dynamically, so that is one heap and the
tests pass. **BIND-3's acceptance says the shim links the MSVC CRT statically**,
and at that point it is two heaps — which is the entire argument D-BIND-32 is
built on, quoted in `binding.h` and again in `write_window.hpp`. The round has
already reasoned its way to this exact hazard once; the test suite embeds the
mirror image of it.

It will not present as a review finding when it fires. It will present as heap
corruption in whichever test happens to run first after the CRT flag flips, in a
file whose subject is the containment taxonomy.

**Fix, two lines:** a test-local `DisposeLocal(fl_error&)` that does
`delete[] err.message` and zeroes the struct, used by the six call sites in
`test_containment.cpp` only — `test_binding_entry_points.cpp` and
`binding_producer.cpp` go through the export table in both directions and are
correct as they stand. A comment saying why that file, and only that file, does
not use `fl_error_dispose` is the part worth more than the code.

> **FIXED 2026-09-18.** `DisposeLocal` added in the file's anonymous namespace
> with that reasoning at the definition, and all six call sites moved to it. No
> other file changed: the two that release through the ABI are releasing what the
> shim allocated, which is what `fl_error_dispose` is for.

---

## DEBT

Carried forward. None of these loops the design, and none blocks BIND-3.

### D1 — a `grow` that refuses without a message is `std::string(nullptr, 0)`

`write_window.hpp:121` builds the message unconditionally:

```cpp
std::string message(reinterpret_cast<const char*>(err.message), err.message_len);
```

`err` is zero-initialised one line above, and nothing in `binding.h` requires a
refusing `grow` to set `message` — the very next line handles the empty case
(`message.empty() ? "fl_write_window: grow refused" : message`), so the author
expected it. But `basic_string(const charT* s, size_type n)` requires `s` to
point at an array of `n` elements; `(nullptr, 0)` is UB by the letter and is
what a UBSan build flags. Both toolchains currently accept it.

`AGrowThatRefusesKeepsItsStatusMessageAndOrigin` always sets a message, so the
path is unexercised in either direction. **Fix:** guard on `err.message !=
nullptr`, and give the existing test a second grow thunk that refuses silently.

> **HALF FIXED 2026-09-18.** The guard is in, with the reason at the site. **The
> test is not**: the silently-refusing grow thunk was not added, so the guarded
> path is correct and still unexercised. Carried forward to whoever next touches
> that file.

### D2 — a caller's `grow` can put an arbitrary int32 into `err->status`

`write_window.hpp:122` casts the callback's return straight through:

```cpp
throw PubSubError(static_cast<PubSubStatus>(status), …);
```

`PubSubError::Sanitize` only coerces `kOk`/`kPending`/`kSubscriptionEnded`; every
other value passes (`core/status.hpp:143-152`), and `Capture` then casts it back
into `err->status`. A binding whose grow thunk returns a number that is not an
`fl_status` — a `GetLastError`, a negative errno, an uninitialised local — gets
that number back as the ABI's status, and the managed `switch` D-BIND-19 builds
on it falls through to a default that says nothing.

The caller here is the binding itself, so this is robustness rather than trust.
It is cheap: map anything outside the enum to `FL_INTERNAL` and keep the
callback's message, which still says what happened.

### D3 — `AHealthyProcessIsNotPoisoned`'s first assertion reads a slot the shim does not use

`test_single_copy.cpp:165`:

```cpp
EXPECT_TRUE(SingleCopyRefusal().empty())
    << "the shim latched a single-copy conflict in a process that has one copy: " …
```

`SingleCopyRefusal()` resolves to the **test binary's** copy of the slot — the
shim's is local on Linux (the version script) and undeclared on Windows. Nothing
ever writes that copy except `SetSingleCopyRefusalForTest`, which
`test_containment.cpp` restores under RAII. So the assertion cannot fail, and its
message describes something it did not read.

The row is not worthless: its *second* half calls `fl_provider_create` through
the export table and would go red if the shim had latched. That is the real
assertion and it should be the only one. **Fix:** delete the first `EXPECT_TRUE`,
or re-point it at what it claims to read — which it cannot, which is the point.

> **FIXED 2026-09-18.** The assertion and its now-unused `using` declaration are
> gone; the row's doc comment states why it asserts through the ABI and nowhere
> else, so the deleted check is not reinstated by someone reading it as an
> omission. The paragraph below it — the evidence bound — is unchanged and
> remains true.

Worth stating alongside it, because it is the honest bound on D-BIND-17's
evidence: **the shim's own wiring of the check is proven only in the negative.**
The algorithm is tested thoroughly, in the test binary's copy, with a real decoy
module. That the *shim* runs it at load is asserted by nothing — if the dynamic
initializer in `binding.cpp:119` were dropped, every test would still pass,
because a shim that never checks and a shim that checks and finds one copy are
observationally identical. `fl_single_copy_marker` touching `kSingleCopyChecked`
is the defence, and it is a good one; it is just not a test. Proving it needs two
real shims in one process, which 2d priced and declined for good reason. This is
BIND-0's F2 shape — a property that holds for a different reason than the one the
reader assumes — and it belongs in the record rather than in a fix.

### D4 — three files still carry claims the round has already retired

BIND-1's review closed B2 by replacing a stale inventory in `binding.h`. The same
three sentences live on elsewhere:

1. `.github/workflows/ci.c-abi.yml:75` — *"No budget is ENFORCED yet: BIND-9 sets
   the threshold"* — sits six lines above the step that enforces a 12 MiB
   ceiling. The Linux leg's equivalent comment was rewritten by 2d; the Windows
   one was not.
2. `c-abi/CMakeLists.txt:77` — *"the shim exports only `fl_binding_abi_version`"*.
   It exports twenty functions.
3. `c-abi/tests/test_binding_entry_points.cpp:221` — *"Who frees a message a
   CALLBACK put in an `fl_error` is an open question against binding.h"*. Ruled
   2026-09-17 by D-BIND-32 (`f73cfb4`); `write_window.hpp` was updated and this
   was not.

Each is one line. They are grouped as one finding because the pattern is the
finding: a claim gets fixed where the reviewer was looking and survives in the
three places nobody re-grepped.

> **FIXED 2026-09-18**, all three. The CI comment now says a ceiling is enforced
> and what it is *not*; the CMake comment states the property as being about the
> exported SET and notes when it stopped being about the one function; the test
> comment cites D-BIND-32 and explains that static storage is the shape the rule
> was written to permit. `actionlint` 1.7.12 clean after the workflow edit —
> a YAML parse would not have been enough, and the comment added there contains
> apostrophes.

### D5 — the export check asserts the `fl_` prefix while its message claims the declarations

`ci.c-abi.yml:197` greps `^fl_[A-Za-z0-9_]*$` and reports, on failure, *"the shim
exports symbols that `binding.h` does not declare"*. A hypothetical `fl_helper`
that no header declares would pass the check that says it would not.

Already noted during 2c and not acted on. It stays DEBT: the strong form is
`grep -oE 'FL_ABI_EXPORT[^(]*\b(fl_[a-z_0-9]+)\('` over the header, sorted
against `nm -D` output, which is a handful of lines and would have caught nothing
so far. Worth doing when BIND-4 doubles the export table, not before.

---

## NITs

### N1 — `fl_publisher_publish_rows(…, count = 0)` returns `FL_OK` for a malformed topic

`binding.cpp:377-388` converts the segments once and then enters the loop. With
`count == 0` the loop body never runs, `Publish` is never called, and
`RequireSegments` — the whole basis of the file header's *"every call goes through
the code that refuses"* — never sees the topic. An empty topic, a segment with a
`/`, a leading `__`: all `FL_OK`.

Harmless (nothing was published) and inconsistent (the same topic through
`publish_row` is refused). Either validate the topic before the loop or say in
the header comment that a zero-row publish is a no-op that validates nothing. The
second is cheaper and honest.

### N2 — `BoundRows`'s constructor leaks the view on one of its two failure paths

`nanoarrow_codec.cpp:652` refuses without `ArrowArrayViewReset` when
`ArrowArrayViewInitFromSchema` fails; the `ArrowArrayViewSetArray` path four
lines below does reset. The destructor does not run for a throwing constructor,
so a partially built view leaks. Reaching it needs a schema `PlanNode` accepts
and nanoarrow's view builder rejects, which may well be unreachable — which is
why it is a NIT and not DEBT. One line, and it makes the two paths read the same.

### N3 — an over-reporting writer's refusal is attributed to the seam

`WriteThroughCaller` restores the ambient origin before returning the count, so
`AppendInPlace`'s post-return check (*"a writer reporting more than `room`"*)
throws with `FL_ORIGIN_SEAM`. The status is right; the origin says Fletcher
refused when the caller's writer misbehaved. `binding.h` specifies the status for
this case and not the origin, so nothing is violated — but D-BIND-19 rule 3 made
`FL_ORIGIN_CALLBACK` mean exactly "your thunk did this", and this is a case of it.

### N4 — a zero-field schema makes `count` unbounded by `len`

A struct schema with no children is accepted at open, encodes to zero bytes per
row, and decodes from zero bytes per row — so `fl_decode_rows(codec, NULL, 0,
INT64_MAX, …)` builds rows until memory runs out, with `offset != len` never
tripping. An empty proto message produces exactly this schema. The caller is the
binding and the count is its own, so this is a footgun rather than an exposure;
a `count` sanity bound against `len` when the row size is zero would close it.

---

## Coverage — what is green but unobserved

Not a defect, and it does not belong in a diff. It belongs in BIND-2's state
table, because the acceptance reads as though it is proven.

**Nothing in the tree observes what the fused publish actually puts on the wire.**
`TheFusedPublishRunsOverTheWholeChain` asserts `FL_OK` from
`fl_publisher_publish_row`, the codec origin from an out-of-range row, and the
topic list — all real. What it cannot assert is the bytes: `inprocess` with no
subscriber delivers to nobody, and the subscriber half of the ABI is BIND-4's
(D-BIND-31). So the fusion has **two** unverified properties, not one:

* its **copy** claim — recorded, ruled, and explained (D-BIND-34: the oracle can
  only score a producer it lends a window to);
* its **byte** claim — unrecorded anywhere I can find.

Both are structurally near-certain: the fused path calls the same `EncodeRow`
with a different `WriteBuffer`, and that buffer is the provider's. Near-certain
is the right standard here and the alternative — a test-only subscriber in the
shipped artifact — was correctly refused elsewhere in this round. But *"the
publish fusion: 🟢 2c"* should read as *green for the chain, owed to BIND-4 for
the payload*, or BIND-4 will inherit a bullet nobody knows is half-open. The
suite already says this for `publish_rows`' partial-failure count
(`test_binding_entry_points.cpp:561`); the fusion deserves the same sentence.

---

## What this review did not check

- **I did not build or run anything.** Every finding above is read from the
  source at `5762239` and reasoned against the headers it cites. CI's 51/51 is
  the evidence that the suite passes; it is not evidence about B1, whose whole
  point is that it passes today.
- **The wire format itself.** Byte identity against `arrow-bridge` is the oracle,
  it is a real one (both sides encode from the same Arrow export, the corpus is
  non-empty by assertion, the comparison count is pinned at 15), and re-deriving
  the layout by hand would be a worse check than the one already running. I read
  the switches for *drift* between encode and decode — the widths match arm for
  arm — not for conformance to `docs/wire-format-specification.md`.
- **The two recorded deviations** (the byte-identity corpus not being
  `emit_vectors`; 2b's composite map-key refusal narrowing 2a). Both are open,
  both want decision numbers, and both are the conformance pass's to put in front
  of the maintainer.
- **Linux behaviour**, except by reading. The single-copy scan's two fixes were
  verified in a container during 2d; I re-read the reasoning, not the container.
- **`fl_provider_create`'s path-selector refusal** beyond confirming it is
  forwarded from the registry rather than invented.

---

## Outcome

| | |
|---|---|
| Slices reviewed | 4 (2a, 2b, 2c, 2d) |
| Source read | ~2 000 lines of `c-abi/src`, ~1 700 of `c-abi/tests`, the conformance leg, the lane |
| BLOCKER | 1 (B1, test-only, fires at BIND-3) — **fixed 2026-09-18** |
| DEBT | 5 (D1 null message, D2 unvalidated status, D3 vacuous assertion + D-BIND-17's evidence bound, D4 three stale claims, D5 export check) — **D1 half fixed, D3 and D4 fixed**; D2 and D5 carried |
| NIT | 4 — all carried |
| Defects in the wire format, the taxonomy, or handle lifetimes | 0 |

**Fixed in the follow-up to this review (2026-09-18):** B1, D3, D4 in full, and
D1's guard without its test. **Still open:** D1's untested path, D2 (a callback's
status reaching `err->status` unvalidated), D5 (the export check's prefix grep),
and all four NITs. Each is annotated in place above.

The code is in better shape than the finding count suggests. Three of the four
NITs and two of the five DEBT items are one line each, and the BLOCKER is two.
What is worth carrying past this review is not any of them: it is **D3's second
half** — that the single-copy check's deployment in the shim is unobservable by
construction — and the **coverage statement** about the fusion's payload. Both
are properties the record currently presents as proven, and neither is.
