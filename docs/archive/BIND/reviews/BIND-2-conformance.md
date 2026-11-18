# BIND-2 — conformance review

**Subject:** BIND-2's acceptance, ten bullets, as written in
`plans/BIND-csharp-bindings.md` Part 7, together with its "State — 2026-09-16"
table and its two recorded deviations.
**Reviewed at:** `5762239`, tree clean, CI green 51/51.
**Relationship to the code review:** `BIND-2-codereview.md` asked what the code
does. This asks whether the acceptance is met. The one BLOCKER that pass raised
(B1, a cross-module `new[]`/`delete[]` in `test_containment.cpp`) is not an
acceptance failure and is not counted here.
**Method:** each bullet states what I did to check it. The state table is treated
as a claim to verify, not as evidence — which is how BIND-0's review found its
stale matrix, and it earns its keep again below.

**Outcome: CONFORMS on 9 of 10 bullets. Bullet 4 is met in substance and not in
letter, and the letter is wrong** — the corpus it names is the wrong *shape* for
this test and narrower than what was built, so the recommendation is to amend the
acceptance rather than widen the test. **2 deviations need decision numbers, 4
findings carried forward, 0 blocking.**

---

## Bullet-by-bullet

| # | Acceptance bullet | Check performed | Result |
|---|---|---|---|
| 1 | `nanoarrow_codec.{hpp,cpp}`: ctor (deep copy, field plan), `Bind → BoundRows`, `EncodeRow` driving `PositionalWriter`, `DecodeRows` driving `PositionalReader` + nanoarrow's builders; one `switch` per mapping row, recursive for list/struct/map; links core, pubsub and the three providers, **never** `arrow-bridge` | Read all four entry points; traced the encode and decode switches arm for arm against each other; read the link list three ways | ✅ all present. `never arrow-bridge` is enforced in **three** independent places, not asserted: the `fletcher-c-abi` target links only pubsub + the three providers (`CMakeLists.txt:142`), the recipe gates `fletcher-arrow-bridge` and `arrow` behind `run_tests` with `run_tests` deleted from `package_id` (`conanfile.py:61-73`), and CI's size ceiling measures the packaged artifact |
| 2 | The publish fusion: `fl_publisher_publish_row(s)` calls `Publisher::Publish` with a `RowEncoder` lambda running `EncodeRow` into the provider's window | Read `binding.cpp:336-395`; ran the acceptance's own reading against the test | ✅ structurally, with a coverage caveat the code review records: nothing observes the **bytes** the fused path writes, because `inprocess` delivers to nobody and the subscriber half is BIND-4's. Green for the chain; the payload is owed to BIND-4 |
| 3 | `fl_encode_row` through an `fl_write_window` → `WriteBuffer` adapter; the C writer adapter for `PublishRaw` closes over `ctx` and throws `PubSubError(kInternal, …)` when the thunk reports a captured exception (D-BIND-19 rule 3) | Read `write_window.hpp`; matched the throw against rule 3 | ✅ both adapters, in opposite directions, named and reasoned at the site. `WriteThroughCaller` throws `kInternal` on a zero-byte report and leaves the origin at `FL_ORIGIN_CALLBACK`, which is rule 3 exactly |
| 4 | **Byte identity** across the whole `emit_vectors` scenario corpus (D-BIND-11), against whichever `arrow-bridge` `Codec` `main` carries when it first runs | Read `emit_vectors.cpp` in full; compared its corpus against the fixtures actually used; read the oracle's pin | ⚠️ **MET IN SUBSTANCE, NOT IN LETTER — deviation 1, see below.** Also: the oracle is a *pinned* `fletcher-arrow-bridge/0.5.0-alpha`, not "whichever `main` carries" — see F4 |
| 5 | The borrow rule is tested: release count stays zero across N publishes, and is one after `fl_rows_unbind` plus the caller's release | Read `BindBorrowsTheArrayAndNeverConsumesIt` | ✅ in effect, by a different instrument — see F3. Three encodes while bound, `release` non-null at three points, and the caller's own release last (a double free if unbind had consumed it) |
| 6 | Malformed-input parity with HARD-1..7 **through `fl_decode_rows`**; bounds checks not `#if DEBUG`-gated; every message preserved | Grepped `positional_io.hpp` for `NDEBUG`/`assert`; read both parity tests | ✅ **all three clauses.** No debug gating exists — every bound is an `if (…) throw`, unconditional. The sweep (`DecodeRefusalsComeFromTheReader`, a 4-byte `0xFF` window) is at the codec level; the ABI level adds one witness plus a control, which is sound because `fl_decode_rows` *is* a pass-through. The state table still calls this 🔴 — see F1 |
| 7 | `CopyAccounting.BindingProducerWritesInPlace`, scoring `encode_copies == 0`, retiring the README's "stand-in" caveat for the client half | Read the leg, its control, and the README diff | ✅ and better than the bullet asked: the leg has a **live negative control** (`BindingProducerStagingIsCaught`) because the first draft was a tautology. The README caveat is not deleted but *narrowed*, and states precisely what is still not claimed |
| 8 | The single-copy check (D-BIND-17): the shim enumerates loaded modules for a second marker export and **refuses to initialise**, naming both | Read the scan, the latch and the refusal path | ✅ on the reading the slice adopted and flagged — see F2. Both modules are named in the message, and `APoisonedShimRefusesWithoutRunningTheCall` asserts the body does not run |
| 9 | A packed-size budget for the shim in CI (D-BIND-1a rider ii) | Read both legs of `ci.c-abi.yml` | ✅ enforced on both, **per platform from each platform's own measured baseline** (win-x64 12 MiB over 7.61; linux-x64 20 MiB over 13.79). The state table still says one 12 MiB ceiling for both — see F1 |
| 10 | Prior art read first: commit `0050365` (`EncodeRow(values, WriteBuffer&)`, `batch_decoder.hpp`) | Fetched the commit and compared shapes | ✅ **evidenced rather than asserted.** `0050365` moves `EncodeRow` to write straight into a `WriteBuffer` and adds a `BatchDecoder` that decodes into Arrow builders; BIND-2's codec reproduces both — `EncodeRow(const BoundRows&, int64_t, WriteBuffer&)` returns nothing, `DecodeRows` appends into nanoarrow's builders. The influence is legible in the code |

---

## The two deviations, for a ruling

Both are recorded in the plan as open and unruled, and both were raised by the
slice that caused them. This is the review they were waiting for.

### Deviation 1 — the byte-identity corpus (bullet 4)

**What the acceptance says:** byte identity *"across the whole `emit_vectors`
scenario corpus"*.
**What was built:** five hand-written Arrow fixtures of three rows each — scalars
at their extremes, timestamp and duration, a nested struct in its three distinct
states, the three composites each populated / empty / null, and (2b) a fixed-size
list. 15 comparisons for identity, 5 for the round trip, both counts asserted.

**What I found when I read `emit_vectors.cpp`, which nothing in the plan record
appears to have done since the bullet was written:** it is the wrong *shape*, and
it is smaller.

* **Shape.** It encodes through the **protoc-generated row class**
  (`telemetry.fletcher.pb.h`, `row.Encode()`), emitting base64 for the TypeScript
  side to decode and re-encode. It is a *proto-row* corpus serving a
  cross-language check. BIND-2's codec takes an **Arrow array**. Running bullet 4
  over it means hand-translating each scenario into an Arrow array first — and
  what the test would then compare is still nanoarrow against `arrow-bridge`,
  because that is the only other Arrow-array encoder in the tree. The generated
  C++ encoder never enters the comparison.
* **Size.** Three scenarios (`basic`, `zero`, `negative`) over **one flat
  message**: an int32, a double, a string, a bool and a repeated int32. No
  struct, no map, no fixed-size list, no timestamp, no duration, no null
  composite. Substituting it for what 2a and 2b built would **reduce** coverage
  on every axis the wire format has.

So the bullet names a corpus that cannot carry the property it is attached to.
2a did not fall short of it; 2a improved on it and did not say so loudly enough,
and the plan has carried the discrepancy since.

**Recommendation — amend the acceptance, keep the test.** State bullet 4 as
*"byte identity against `arrow-bridge` across the codec's own Arrow fixture
corpus, which covers every arm of both switches; counts asserted deliberately"*,
and leave D-BIND-11's `emit_vectors` reference where it belongs, on the
cross-language generated-code property it was written for.

**The one thing worth adding, and not here:** the three telemetry scenarios *as a
sixth fixture* would chain nanoarrow ≡ `arrow-bridge` ≡ generated C++ ≡ TS over
the same bytes, which no single test does today. That is a genuine gain and it is
not BIND-2's — it needs the generated C++ row class in the c-abi test binary, and
it belongs with BIND-5/7 where the generated tiers are the subject.

### Deviation 2 — the composite map-key refusal (2b narrowing 2a)

A map's key must be a scalar, refused at open by field name. The wire is
`[COUNT][keys][value bitfield][values]`, nanoarrow finishes a struct element only
when every child is exactly one longer, so an entry's key and value must be
appended together and the keys must be staged — a composite key is not decodable
in one pass without a second decoder. Proto restricts map keys to integral, bool
and string, so the mapping produces none.

This tightens what 2a's encoder accepted, which makes it a deviation under the
round's STOP-AND-ASK rule, and the plan says it *"wants a decision number, not a
paragraph in a commit message"*. I agree with the judgement on the merits: the
refusal is at open (not at row 4711), it names the field, it is tested
(`RefusesAMapWithACompositeKeyNamingTheField`), and the alternative is a second
decoder for a case the mapping cannot produce.

**Recommendation — lock it as ruled, not as an exception.** The reason it should
be a *decision* rather than a note is that it is a statement about the wire
format's reach, and BIND-Rust binds the same codec next round.

---

## Findings

### F1 — the state table is stale in two rows, and one of them is the red one

Both were true when written and were overtaken by later slices. The pattern is
BIND-0's F1 again: a table that records a number or a colour, in a round whose
own policy is to keep landing work underneath it.

1. **The size-budget row** says *"🟢 2d — a **12 MiB ceiling** on both legs"*.
   That is exactly the thing `5762239` exists to correct: one ceiling for both,
   taken from the Windows figure, was rejected by the Linux leg, and the ceilings
   are now **per platform from each platform's own measured baseline** (12 MiB
   over 7.61 measured; 20 MiB over 13.79 measured). The row records the mistake,
   not the fix.
2. **The malformed-input row is still 🔴**, on the stated grounds that
   *"`fl_decode_rows` does not exist until 2c, where it is a pass-through and the
   property carries unchanged"*. 2c landed. The entry point exists, it is the
   pass-through the row predicted, and
   `MalformedBytesCarryTheReadersMessageAndTheCodecOrigin` exercises it through
   the export table — refusal forwarded with the reader's prefix, `FL_ORIGIN_CODEC`,
   and `out` untouched on failure. **The bullet is met; the row should be 🟢.**

A red row in a table of green ones is what a reader scans for, and this one now
reports a gap that was closed a slice later. Also minor, same cause: the table is
headed *"State — 2026-09-16"* while carrying 2d rows from the 17th and 18th, and
the slice table lists 2c and 2d as landing in *"this commit"*, which was true in
the commit that wrote it and is not true in the file.

### F2 — bullet 8's "refuses to initialise" is met by a reading, and the reading is the better one

The bullet says the shim *"refuses to initialise"*. It does not: it latches a
verdict at load and refuses every fallible entry point through the one
containment site, carrying both module paths in the message.

The slice flagged this as a reading rather than doing it quietly, and the reading
is right — a shared library that fails to initialise takes the host down with no
diagnostic a managed runtime can surface, whereas a refusal becomes a
`FletcherException` with both paths in it under D-BIND-19. Recorded so the
wording is not read later as an unmet bullet, and so BIND-3's managed side knows
it will meet this as a status, never as a load failure.

Its evidence bound is in the code review (D3): the *algorithm* is tested
thoroughly against a real decoy module, but in the **test binary's** copy of the
code; that the shim itself runs the check at load is asserted by nothing, because
a shim that never checks and a shim that checks and finds one copy are
observationally identical.

### F3 — bullet 5's "release count" is discharged by pointer-nulling, not by counting

The bullet asks that an array's *release count* stay zero across N publishes and
be one afterwards. The test asserts `c_array.release != nullptr` at three points
and then calls it once. That is a proxy: the C Data Interface convention is that
a release callback nulls itself, and Arrow C++'s exported callback does, so
"still non-null" does mean "not released". A release callback that did not null
itself would slip through, and no such callback is in play here.

The "is one" half is carried by the final call not being a double free rather
than by a counter. Adequate; worth a sentence in the record because the bullet's
phrasing implies an instrument that was not built, and BIND-3's `BoundRows.Dispose()`
is the place where an actual counter would pay for itself.

### F4 — the oracle is a pinned version, not "whichever `main` carries"

Bullet 4 says the comparison runs *"against whichever `arrow-bridge` `Codec`
`main` carries when it first runs"*. The test links
`fletcher-arrow-bridge/0.5.0-alpha`, an exact Conan pin
(`c-abi/conanfile.py:69`). If `arrow-bridge`'s encoder changed, this lane would
go on comparing against 0.5.0-alpha and the drift would be invisible until
someone moved the pin.

The exact pin is right for a reproducible build and is this repo's convention.
The property still holds, but by a mechanism the bullet does not name: the
versioning rule bumps every package's shared MAJOR.MINOR in lockstep, so a
wire-format change in `arrow-bridge` forces this pin to move, and the byte-identity
test re-runs against the new encoder at that moment. Worth stating because the
bullet reads as though the lane tracks `main` continuously, and nothing does.

---

## What this review did not check

- **I did not build or run anything.** CI's 51/51 at `5762239` is the evidence
  that the suite passes. Every conformance judgement above is read from the tree.
- **The wire format against `docs/wire-format-specification.md`.** Byte identity
  against `arrow-bridge` is the mechanism the acceptance chose, and re-deriving
  the layout by hand would be a weaker check than the one already running.
- **D-BIND-11's own cross-language property.** I read `emit_vectors.cpp` to judge
  deviation 1; I did not check that the TS byte-compat test still passes over it.
  That belongs to its own lane and is not BIND-2's.
- **The `arrow-bridge` oracle's correctness.** It is the oracle by ruling; if it
  is wrong, both encoders are wrong together and this test cannot say so.
- **Anything on Linux by execution**, including the per-platform ceiling's
  headroom against a real run.

---

## Outcome

**CONFORMS on 9 of 10.** Bullet 4 is met in substance by a better corpus than the
one it names, and the recommendation is to amend the bullet.

| | |
|---|---|
| Acceptance bullets | 10 |
| Met as written | 9 |
| Met in substance, letter wrong | 1 (bullet 4 — the `emit_vectors` corpus) |
| Deviations needing a decision number | 2 (the corpus; the composite map-key refusal) |
| Findings carried forward | 4 (F1 stale state table ×2, F2 the "initialise" reading, F3 the release-count proxy, F4 the pinned oracle) |
| Blocking findings | 0 |

F1 is the one to act on before the item closes, and it is two edits: the size row
records the mistake rather than the fix, and the malformed-input row is red over
a gap that 2c closed. Neither changes what the code does; both change what the
next reader believes.
