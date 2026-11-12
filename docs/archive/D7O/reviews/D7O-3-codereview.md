# D7O-3 code review (4b) — any row count is used as given

Diff: `957a514..c18e95e`. Production: `pubsub-arrow/src/subscriber_arrow.cpp` (`SetSchema`, `PreSize`,
`Close`). Tests: three new in `pubsub-arrow/tests/test_pubsub_arrow.cpp`.

Counts: blocking 0 · should-fix 1 · nit 3

## S1 — should-fix (confidence: medium-high) — a failed `PreSize` leaves a builder that crashes the next `Append`

Arrow 23 `NumericBuilder`/`BinaryBuilder`/`FixedSizeBinaryBuilder::Resize` grow the data buffer and
then call `ArrayBuilder::Resize`, which sets `capacity_` **before** `null_bitmap_builder_.Resize`
(the d7 doc, (d)2, records the same ordering). If the bitmap allocation (≤ 8 KiB at the 65 536 bound)
fails, `capacity_` says 65 536 while the bitmap still has no buffer: `PreSize` only ever runs on a
reset builder (fresh, or right after `Finish()`), so `null_bitmap_builder_` has a null data pointer.
The next `Append` -> `Reserve(1)` is a no-op (1 <= capacity_) -> `UnsafeAppendToBitmap` writes
through that null pointer: an access violation on the transport's delivery thread (an L10 break). Not
silent, and it needs an allocation failure between two allocations, hence not blocking. But the change
makes it newly reachable at `SetSchema` (before, `decoder_.reset()` made that path safe), and the
doc's "as in every `Append`'s own growth" is not equivalent: an `Append` failure goes to
`DiscardPending`, whose `Finish()` resets every builder; a failed `PreSize` gets no reset.
Fix, either way: (a, preferred, forbidding direction) **delete `PreSize`**: Arrow's geometric growth
already covers a window, and removing it also removes `kMaxPreSizeRows`, its 2^47 overflow argument
and the `fixed_size_binary(INT32_MAX)` guard; or (b) in `PreSize`'s catch, call
`try { (void)decoder_->Finish(); } catch (...) {}`. The decoder holds zero rows at both call sites,
so this loses nothing and puts every builder back at capacity 0.

## Checked, no finding
- `std::min(max_rows_, kMaxPreSizeRows)`: both are `int64_t`, and `BatchDecoder::Reserve(int64_t)` →
  `ArrayBuilder::Reserve(int64_t)`. No narrowing. A `static constexpr` member bound to `const&` is
  implicitly inline (C++17+), so it is not an ODR problem.
- Partial `Reserve` across columns (column k throws after columns < k grew): only extra capacity.
  `Reserve` touches top-level builders only, so nested widths never multiply into the 2^47 bound.
- `catch (...)` in `PreSize` and `SetSchema` covers both `runtime_error` (from `ThrowInternal`) and
  `bad_alloc`. The constructor-failure-as-rejection issue is already recorded as (d)1/Q8.
- The malformed row in the guard test (bitmap byte 0x00, value bytes missing) fails the decoder's
  validation pass, so no 2 GiB element is ever appended.
- Sweep: refused sinks are in a `deque` declared before the cells (stable addresses, destroyed
  last). Accepted `TimedSink` is declared before `SweepCell`. The L5 check is one-sided (`t0` is
  taken before `Publish`). Waits happen only where a close is reachable (≤ 1 ms or `max_rows` 1).
  A refused call starts no thread. Runtime is small; (c)'s 131 074 synchronous publishes are the
  largest cost.

## Nits
- N1 (medium) `AFailedPreSizeDoesNotMakeTheSchemaUnusable` never checks that the pre-size actually failed. Where a 2^47-byte reservation can succeed lazily (arm64 Linux, 48-bit VA, `overcommit_memory=1`), it passes without testing anything. Under ASan with Arrow's system allocator, it aborts instead. No sanitizer CI exists today.
- N2 (low) `Close`: if `Finish()` fails and `DiscardPending`'s own `Finish()` fails too, `PreSize` runs on a builder that still holds rows. This was already the case before the change and is unchanged by it; S1(b) does not fix it either.
- N3 (low) (c) runs 131 074 publishes in one test. In a Debug build this may be the slowest test in the file, and moving it to `test_pubsub_arrow_heavy.cpp` would be worth considering.

## RECORD
- RECORD: `docs/domains/d7-batch-assembly.md` (d)2 says a failed `Reserve` is reached "as in every `Append`'s own growth". After an `Append` failure, `DiscardPending`'s `Finish()` resets the builders. After a `PreSize` failure nothing does (see S1).

## Cycle 2

Diff `c18e95e..57aa4bb` (+ records `1434325`). Counts: blocking 0 · should-fix 0 · nit 1. S1 closed.

**S1 fix, checked.** `PreSize`'s catch now runs `Finish()`, and that is safe on a part-grown builder.
In Arrow 23, `FinishInternal` (numeric, boolean, binary, fixed-size binary, list, struct) finishes
`null_bitmap_builder_` with length 0. A `BufferBuilder` with no buffer has a null `buffer_`; it takes
the `AllocateResizableBuffer(0)` branch, which returns the pool's zero-size area. `UnsafeAdvance(0)`
moves nothing, and `ZeroPadding` is skipped because the size is 0. So nothing writes through the null
bitmap. Each builder then ends with `capacity_ = length_ = 0` (or `Reset()`). The grown data buffer
goes to the discarded batch and is freed. Both call sites hold zero rows (fresh decoder, or right
after a successful `Finish` or `DiscardPending`), so nothing is lost and `impl_->rows` stays 0. The
residue the records already note is real and no worse than before: `BatchDecoder::Finish` interleaves
each column's `Finish` with that column's `DictionaryEncode`/`Cast`. A throw there leaves later
columns un-reset, so S1's state survives only if a dictionary encode of zero rows fails right after
the failed pre-size.

**Regressions: none.** `const max_rows_`/`timeout_`: the batcher is non-copyable, non-assignable and
never assigned; this is correct. The header wording "lost" fits. The records rewrite of (d)2 matches
the tree.

- N4 (low) Optional: `PreSize`'s catch and `DiscardPending` now repeat the same "`Finish()`, swallow" pattern. A private `ResetBuilders()` holding the try/`Finish`/catch would make both sites one line each. Cosmetic only.
