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
