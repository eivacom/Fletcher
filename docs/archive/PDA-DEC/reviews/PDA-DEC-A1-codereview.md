# PDA-DEC-A1 — independent code review

**Scope:** `git diff a4f2d41` (uncommitted) + untracked `core/tests/test_write_buffer.cpp`.
**Branch:** `feature/protocol-driver-abi`.

Verified by building and running: `core_tests --gtest_filter=WriteBufferInPlace.*` (9/9 pass),
`conformance_copy_accounting` (11/11 pass), `conformance_seam_vocabulary` (9/9 pass).
Three findings below were reproduced with standalone probes against the in-tree header.

**Counts: 1 blocking · 4 should-fix · 5 nits.**

---

## BLOCKING

### B1 — A large `min_bytes` silently leaves `pos_ > capacity_`, hands the writer an absurd `room`, and corrupts the buffer for every later append
*Confidence: high (reproduced).* `core/include/fletcher/core/write_buffer.hpp:183-195`

`AppendInPlace` bounds its own entry check by subtraction, then delegates the refill to
`AppendZerosSlow`, which on every growable subclass **adds**:
`VectorWriteBuffer::Refill` does `buf_.resize(pos_ + grow)`; `GrowableProbeBuffer` and the new
`RelocatingWriteBuffer::Grow` do `std::vector<uint8_t> next(pos_ + max(len, kStep))`.
For `min_bytes > SIZE_MAX - pos_` that wraps to a tiny number, so the refill *shrinks* the window
instead of growing it. `AppendInPlace` then restores `pos_ = before_refill` — which is now
**greater than `capacity_`** — and the contiguity guard that was supposed to catch exactly this,
`capacity_ - pos_ < min_bytes`, underflows to `SIZE_MAX - k` and passes.

Reproduced (`VectorWriteBuffer`, 40 bytes already written, `min_bytes = SIZE_MAX - 20`):

```
WRITER RAN: dst=0000021A2DBBD5A8 room=18446744073709551595
no throw
pos=40                      // capacity_ is 19
```

Three contract clauses break at once, all silently:

* clause 2 promises the writer "is not invoked" when the room could not be made — it is invoked.
* clause 3 promises `dst`/`room` describe real memory — `room` is 2^64-21 over a 19-byte vector.
* §3.1 clause 3 ("bounds by subtraction, never addition, so a hostile length cannot wrap") is
  violated by the one member whose normative C form takes a `size_t` straight from foreign code.
  A `static_cast<size_t>(-1)` out of a C `int`, or an overflowing `count * elem_size` in a
  binding, is exactly how this value arrives.

Worse than the bad lend: the buffer is left permanently invariant-broken (`pos_ > capacity_`), so
every subsequent *ordinary* member takes its inline path through an underflowed `capacity_ - pos_`.
A follow-on `Append(p, 4096)` `memcpy`s past the vector's end; the probe that does so dies with no
output and a non-zero exit. On `GrowableProbeBuffer`/`RelocatingWriteBuffer` it is worse still —
the `memcpy(next.data(), buf_.data(), pos_)` inside `Grow` overflows the undersized replacement
*during* the refill.

This state is new to A1. Plain `AppendZeros(huge)` wraps too, but its `pos_ += len` wraps back in
step and leaves `pos_ <= capacity_`; it is A1's `pos_ = before_refill` restore that manufactures
the broken invariant.

**Acceptable fix:** refuse the un-satisfiable request before touching the refill virtual —
`if (min_bytes > SIZE_MAX - pos_) throw std::overflow_error(...)` (→ `kPayloadTooLarge`, the same
number clause 4 already uses) — **and** make the post-refill guard detect a subclass that broke the
invariant rather than be defeated by it:
`if (capacity_ < pos_ || capacity_ - pos_ < min_bytes) throw std::overflow_error(...)`.
Either half alone turns the reproduced case into the loud refusal the header already promises;
both are one line. Pin it with a `WriteBufferInPlace.HugeMinBytesRefusesLoudly` case over both
growable subclasses.

---

## SHOULD-FIX

### S2 — The re-entry guard does not catch every nested `AppendInPlace`, though the header and a test say it does
*Confidence: high (reproduced).* `write_buffer.hpp:203-213`; `core/tests/test_write_buffer.cpp:170-183`

The header states the comparison catches "a nested AppendInPlace". It does not. If the inner call
refills **without relocating** (the vector already has spare capacity, so `buf_.data()` is
unchanged) and the inner writer commits `0`, then `data_` and `pos_` are both back where the outer
lend left them and the outer commits normally:

```
nested refused? NO  pos=12
len=12 bytes: 01 02 03 04 77 77 77 77 77 77 77 77
```

In that probe the outer happened to write last, so its bytes won. Reverse the order inside the
writer — outer fills `dst`, *then* nests — and the inner's bytes silently replace the outer's in
the committed, published row, with no refusal.

The test that pins the nested case (`ReEntryFromTheWriterIsRefused`) is green only because it uses
a `FixedWriteBuffer`, where the inner call cannot refill and its commit necessarily moves `pos_`.
It generalises a narrow case into a claim the code does not hold.

Note also what a post-hoc comparison structurally cannot do: by the time it runs, a *relocating*
nested refill has already freed the block `dst` points into and the outer writer has already
written to it. The guard detects; it cannot prevent.

**Acceptable fix (forbidding direction, preferred):** refuse re-entry *at the door* with a one-bit
`bool lending_` checked at the top of `Append`/`AppendByte`/`AppendZeros`/`AppendInPlace`/`Finish`.
That makes the invalid state unrepresentable instead of detected-too-late; the header's "no flag,
no depth counter, no state" boast is buying a guard that is both incomplete and after the fact.
**Minimum acceptable:** also compare `capacity_` (which catches the reproduced case) and weaken the
header claim to name only what is actually caught.

### S3 — The header's stated mitigation for residue (b) is false on the growable subclasses
*Confidence: high (reproduced).* `write_buffer.hpp:164-174`

The header argues residue (b) is tolerable partly because "on the two growable subclasses the lent
span is already zeroed (`resize` / value-initialisation), so the leak is of zeros". It is not:
`AppendInPlace` is the first member that can leave *written* bytes above `pos_` (writer scribbles
N, reports `used < N`), and the next lend starts inside them.

```
VectorWriteBuffer vb;
vb.AppendInPlace(64, [](uint8_t* d, size_t){ memset(d, 0xAB, 64); return 4; });  // reports 4
vb.AppendInPlace(8,  [](uint8_t*, size_t){ return 8; });                         // writes 0
// Finish() -> len=12 bytes: AB AB AB AB AB AB AB AB AB AB AB AB
```

Bytes 4..11 are the *previous* writer's payload, committed and published having been written by
nobody in this call — the same class the header reserves for `FixedWriteBuffer`. The mitigation is
precisely what a binding author would use to decide the residue is benign on the vector path.

**Acceptable fix:** delete the "the leak is of zeros" mitigation and state the residue uniformly —
`used` is trusted against `room` on every subclass, and a partially-filled prior lend is one of the
things that can be in the span.

### S4 — `Judge()` manufactures a verdict for every leg that never sampled the producer
*Confidence: high.* `copy_accounting.hpp:66-72`, `copy_accounting.cpp:442-446`

`CopyLedger` documents that `produced_at == 0` means "the sampler never ran and NO verdict may be
read", but `Judge()` reads `produced_in_window` unconditionally and returns `encode_copies == 1`.
Every pre-existing leg (`RunRoundTrip`, `RunBorrowedAttachmentRoundTrip`) now carries a verdict
field asserting "the client copied the row" about a client that was never measured. Nothing reads
it today; the only thing between that and a wrong published number is `COPY_MUST_HAVE_PRODUCED`, a
macro a future leg can simply omit.

This is the forbidding-direction case: a state the oracle documents as unreadable, tolerated by a
default instead of refused. **Acceptable fix:** make it unrepresentable — either have `Judge()`
throw on `produced_at == 0` (the suite is already `ASSERT`-guarded, so nothing regresses), or make
`encode_copies` an `optional<size_t>` that is empty for an unsampled leg.

### S5 — clang-format 18.1.3 violations will red the PR CI
*Confidence: high (checked with the pinned version).*

`core/tests/test_write_buffer.cpp:142-143`,
`integration-tests/pubsub-conformance/src/copy_accounting.cpp:503-505`,
`integration-tests/pubsub-conformance/src/copy_clauses.cpp:94-101`.
Run `clang-format -i` on the three files.

---

## Nits

* `EncodeProduced`'s `take = room < payload.size() ? room : payload.size()` is a dead branch —
  `AppendInPlace` already guarantees `room >= min_bytes`; as written it would silently truncate the
  row rather than fail if that guarantee broke.
* `RunProducerRoundTrip`'s refill accounting can never fire (`pos_before` is always 0 on this leg),
  so `PublishRefillCost(tag + "/in-place", ...)` publishes a structural zero, not a measurement.
* A writer returning `used == 0` is accepted silently while `min_bytes == 0` is refused because "a
  fill of no bytes names nothing" — the two ends of the same call disagree.
* `PatchU32`'s bound `offset + sizeof(value) > pos_` adds rather than subtracts (pre-existing,
  §3.1 clause 3); A1 newly documents patch-from-inside-the-writer as a supported route, which
  widens who reaches it.
* `WriteBufferInPlace.GrowableRefillPreservesBytesAndDeliversRoom` loops two subjects in one case,
  so a failure in the first leaves the second's trace unrun and the `SCOPED_TRACE` is the only
  discriminator.

The `Reserve` → `ReserveStorage` rename is clean: `WriteBuffer` never had a `Reserve`, no subclass
shadows one, and no Reserve/Commit pair was introduced. No two-mechanisms confusion.

---

## RECORD (for the PM, not blocking)

* `docs/pubsub-interface-spec.md` §3.1 still has only **five** clauses and §8/§8.1 are unchanged,
  but the header, both test files and the harness cite "**§3.1 clause 6**" as normative in ~8
  places. `plans/PDA-DEC-A1-writebuffer-constructible.md:170` makes that spec edit a close
  condition ("do not land §3.1 clause 6 with §8 left saying rows are already fine").
* `integration-tests/pubsub-conformance/src/copy_clauses.cpp:5` still says "Seven ctest entries";
  the suite now runs 11 gtest cases.

---

# RE-REVIEW after the fix cycle (diff base `3051b94`, uncommitted + untracked `core/tests/test_write_buffer.cpp`)

Verified by building and running, not by reading:
`conan create core` → `core_tests` **40/40 pass** (12 `WriteBufferInPlace.*` entries),
conformance rebuilt against the freshly packaged core → `conformance_copy_accounting` **11/11**,
`conformance_seam_vocabulary` **9/9**. `clang-format 18.1.3` reports **zero** replacements on all
seven touched files. Findings below marked *(reproduced)* were re-run with a standalone MSVC probe
against the in-tree header.

**Counts: 0 blocking · 1 should-fix · 6 nits.**

## B1 and S2 are closed. S3, S4, S5 and the two named nits are closed.

**B1 — closed, and the refusal is loud.** The door check `min_bytes > SIZE_MAX - pos_` runs before
the refill virtual and throws `std::overflow_error` → `kPayloadTooLarge`, and the post-condition is
now `capacity_ < pos_ || capacity_ - pos_ < min_bytes`. I re-audited every path that writes `pos_`
or `capacity_` for a residual `pos_ > capacity_`:

* `AppendInPlace` — the only manufacturer of the broken state, now refused at the door; and if a
  future subclass produced it anyway, `capacity_ - pos_` underflows at the `min_bytes >` test,
  the refill is skipped, and the `capacity_ < pos_` half fires. Loud either way.
* `VectorWriteBuffer::AppendZeros` on a wrapping length — `Refill` sets `capacity_ = pos_ + grow`
  and `AppendZerosSlow` then does `pos_ += len` with `grow == len`, so the two wrap in step and
  `pos_ == capacity_` comes out. No broken invariant.
* `AppendSlow`, `Finish`, `Sync` — no path leaves `pos_` above `capacity_`.

Boundary probed *(reproduced)*: `min_bytes == SIZE_MAX - pos_` exactly (largest value the door lets
through) and `SIZE_MAX - pos_ - 1`, on both `VectorWriteBuffer` and a relocating subclass — all four
throw before the writer runs, `ran=0`, `Position()` unchanged.

**S2 — closed, and the flag cannot be stranded.** `lending_` is set and cleared by a private RAII
`Lend` whose destructor runs on *every* exit: normal return, the writer's own exception, and each of
the two post-return `PubSubError` refusals (the `Lend` object outlives them, so it unwinds). Probed
*(reproduced)*: after a writer that throws, and after a re-entry refusal, the next `AppendInPlace`
on the same buffer succeeds — no permanent unusability. `Lend` is non-copyable, and it is
constructed only after the refill, so no refill path can see the flag set.
`NestedFillIsRefusedOnAGrowableWindowToo` genuinely pins the fix rather than passing for the old
reason: on both growable subjects the inner call needs no refill at all (spare capacity), so without
the door check the inner writer runs, overwrites the outer's first four bytes with `0x77`, and the
test's per-byte loop reddens.

**PatchU32's changed bound is genuinely equivalent for every offset the unrebuilt callers can
produce.** For `pos_ >= 4` and any `offset <= SIZE_MAX - 4`, `offset + 4 > pos_` ⇔
`offset > pos_ - 4`; the boundary `offset == pos_ - 4` is accepted by both forms and
`offset == pos_ - 3` refused by both. For `pos_ < 4` the old form always threw (no non-wrapping
offset can satisfy it) and the new form throws unconditionally on the first clause. The two forms
differ **only** where `offset > SIZE_MAX - 4`, which the old form wrongly accepted. Every in-tree
caller passes a `WriteLengthPlaceholder()` result, i.e. a position ≤ `pos_ - 4`
(`envelope_codec.hpp:38`, `legacy_fletcher_topic_type.hpp:61,75`, `seam_vocabulary.cpp:239`, plus
the two `core/tests/test_positional_io.cpp:228-229` cases, which still pass). Nothing in the
unrebuilt components can observe the change.

## SHOULD-FIX

### R1 — `AppendZeros` still silently truncates on a wrapping length; A1 fixed the wrap one member at a time
*Confidence: high (reproduced).* `write_buffer.hpp:69-76` + `VectorWriteBuffer::Refill`

```
VectorWriteBuffer v; v.Append(p, 40);
v.AppendZeros(SIZE_MAX - 20);   // NO THROW, pos=19
v.Finish();                     // len=19  <- a request for ~2^64 bytes yields 19
```

`Refill` wraps `pos_ + grow` to 19, `resize` *shrinks*, `pos_ += len` wraps back to 19, and the
invariant is intact — so the new post-condition never sees it and the caller gets a silently short
row instead of a refusal. This is **not in A1's diff** and is **not a gate on this item**: no
in-tree caller can reach it (`positional_io` derives every length from a `uint32_t` count, bounded
at 2^29), so it is not reachable-and-silent today.

It is filed because A1 chose the *per-member* fix. The header now argues clause 3's
subtract-never-add rule specifically for "the one member whose normative C form takes a length
straight from foreign code" — but PDA-ABI will give `Append` and `AppendZeros` C forms taking a
`size_t` from exactly the same place, and each will need its own copy of the same guard. **Acceptable
fix:** hoist it — one `if (len > SIZE_MAX - pos_) throw std::overflow_error(...)` at the top of the
two slow paths (or, better, inside `ReserveStorage`/`Refill`, which is where the addition actually
lives), so the rule is enforced once at the place that adds rather than re-argued at each entry
point. Cheaper than three guards, and it makes the residue unrepresentable rather than
member-by-member refused.

## Nits

* `copy_accounting.hpp:158` says reading an empty `encode_copies` "throws `std::bad_optional_access`" — that is true of `.value()`, but both call sites use `operator*`, which is UB on empty. Say `.value()` in the doc, or use it in the harness.
* `HugeMinBytesRefusesLoudly`'s `FixedWriteBuffer` leg would pass without the fix (clause 4's `Overflow()` throws the same `std::overflow_error`); only the two growable legs carry the evidence.
* `min_bytes` just inside the door (e.g. `SIZE_MAX - pos_`) surfaces as `std::length_error` from `<vector>` → `kInternal`, not the `kPayloadTooLarge` that `status.hpp`'s new three-cause comment implies for an unsatisfiable row. Loud, just mistyped; same shape as a pre-existing `bad_alloc`.
* A re-entrant `VectorWriteBuffer::Finish()` from inside the writer is still detected-not-prevented: it is refused loudly at return, but it has already freed the block `dst` points into. `lending_` is private, so a `Finish` guard would need a protected accessor.
* A zero-length re-entry (`Append(p, 0)` / `AppendZeros(0)` from the writer) moves nothing and is undetected — harmless, but it is the one hole in the header's "every mutating re-entry moves the window" claim.
* Carried from the first pass, still open: a writer returning `used == 0` is accepted silently while `min_bytes == 0` is refused because "a fill of no bytes names nothing".

## RECORD (for the PM, not blocking)

* Both prior RECORD items are now discharged: `docs/pubsub-interface-spec.md` §3.1 gained clause 6 and §8/§8.1 were rewritten; `copy_clauses.cpp:5` now says "Eleven ctest entries", which matches the 11 cases the binary runs.
* `core/tests/test_write_buffer.cpp` is still **untracked** — it must be `git add`ed or the CI header/format scans and `core_tests` will not see it.
