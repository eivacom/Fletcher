# PDA-DEC-A1 — a row can be composed *in* the delivered window, and the oracle can see when it isn't

Item: PDA-DEC-A1 · Round: PDA-DEC (amendment 1 of the eight authorised 2026-09-03)
Oracle: `docs/pubsub-interface-spec.md` §3.1, §8, §8.1 (all `frozen`; §12.1 governs)
Evidence: `plans/reviews/PDA-DEC-9-bind-stopask-verification.md` — A1, *Ranking*, *Contract gap vs record gap*

## Summary

A client handed a `WriteBuffer&` cannot put a byte into it without supplying that byte
from somewhere else, so a language binding must compose each row in scratch memory and
`Append` it — one whole-row copy that frozen §8 says cannot exist and that `CopyAccounting`
cannot see. This item adds **one** member, `WriteBuffer::AppendInPlace`, which lends the
window's write cursor to a producer for exactly one call, and extends the copy oracle to
measure **where the producer wrote**, not only where the provider delivered from.

## One defect, not two — the root cause, named

§8's row property and §8.1's measurement are both stated over *the window onwards*: §8.1
starts at "the window base after the encoder's **last append**", and the harness README
excludes "the encode itself" from the definition of a copy. The window's **write end was
never part of the contract**. That single omission is why the API cannot express the
promise *and* why the oracle cannot falsify it — the same missing concept, seen from two
sides. Fixing one alone yields either an unfalsifiable promise or a measured property no
client can achieve. Per the owner's 2026-09-04 ruling (fix at the scope the guarantee is
actually about, once the root cause is **named**), both halves land here, in one item.

Scope bound, stated so it is not creep: **attachments are not affected.** A binding can
already hand over its own pinned bytes via `Blob(owner, data, size)` (§3.2, pinned by the
`static_assert`s in `copy_accounting.hpp`). A1 is the **row** half of §8 only.

## Design

### The one new member (`core/include/fletcher/core/write_buffer.hpp`)

```cpp
// Non-virtual, header-only, on the fast path. `Writer` is invoked exactly once as
//   size_t writer(uint8_t* dst, size_t room)
// C form (normative, §3.1 clause 6): size_t (*writer)(void* ctx, uint8_t* dst, size_t room)
template <typename Writer>
void AppendInPlace(size_t min_bytes, Writer&& writer);
```

Sequence, and every load-bearing choice in it:

1. **Refuse `min_bytes == 0`** — a fill of no bytes names nothing (`kInvalidArgument`).
2. **Make room.** If `min_bytes > capacity_ - pos_`, force a refill through the *existing*
   virtual: remember `pos0 = pos_`, call `AppendZerosSlow(min_bytes)`, restore `pos_ = pos0`.
   No new virtual — PDA-ABI mirrors the vtable it already has, and `VectorWriteBuffer`'s
   refill zero-fills via `resize` anyway, so this costs nothing extra. A fixed buffer's
   `AppendZerosSlow` throws `std::overflow_error` → `kPayloadTooLarge`, which is §3.1
   clause 4 unchanged.
3. **Post-condition:** if `capacity_ - pos_ < min_bytes` after the refill, throw
   `std::overflow_error`. Guards a subclass whose refill does not deliver contiguous room.
4. **Lend.** Sample `base0 = data_`, `pos0 = pos_`, `room = capacity_ - pos0`; invoke
   `writer(data_ + pos0, room)`. `room` is the *whole* remaining window, not `min_bytes`,
   so a variable-length producer never needs a second crossing to ask "how much is left".
5. **On return, in this order:** (a) if `data_ != base0 || pos_ != pos0`, the writer
   re-entered the buffer — refuse, `kInvalidArgument`, commit nothing; (b) if `used > room`,
   refuse, `kInvalidArgument`, commit nothing; (c) `pos_ = pos0 + used`.

Check (a) is the whole re-entrancy answer at the scope the guarantee is about: the commit
is **absolute** (`pos0 + used`), not a delta on whatever `pos_` became, and any mutating
re-entry — `Append`, `AppendByte`, `AppendZeros`, a nested `AppendInPlace` — moves `data_`
or `pos_` and is caught by one comparison at return, with **no branch on the hot inline
append path**. Const reads (`Data()`, `Position()`) inside the writer stay legal, which is
what lets the oracle measure from inside the writer.

**Naming call, made here so the implementer does not make it:** the new member is
`AppendInPlace`, in the existing `Append*` family, because it advances `pos_` exactly as the
others do. There is therefore **no `WriteBuffer::Reserve`** and no shadowing of
`VectorWriteBuffer`'s private `Reserve`. That private helper is nonetheless renamed
`ReserveStorage` (3 call sites, one file, zero public surface) so no reader can mistake a
`std::vector::reserve` helper for a window reserve.

**Not a coexistence window.** `Append(data, len)` stays and is *not* a legacy path: a
producer that genuinely holds bytes elsewhere (forwarding a payload it was handed) should
copy them once, and that is the honest call for it. Nothing is deprecated, nothing is
bridged, nothing is scheduled for deletion in a later stage.

### The oracle extension (`integration-tests/pubsub-conformance`)

`CopyLedger` gains three fields and `CopyVerdict` one:

- `Address produced_at` / `size_t produced_len` — where the row's bytes were **written by
  the producer**, sampled at production time.
- `bool produced_in_window` — sampled inside the producer: was `produced_at` exactly
  `buffer.Data() + buffer.Position()`, the subject buffer's own write cursor?
- `CopyVerdict::encode_copies` = `produced_in_window ? 0 : 1`.

One new driver, one scoring path:

```cpp
enum class ProducerMode { kInPlace, kStaged };
RoundTrip RunProducerRoundTrip(CopyRunner&, const Topic&, size_t row_bytes, ProducerMode);
```

`kInPlace` generates the payload directly into the lent span via `AppendInPlace`.
`kStaged` builds the identical payload in its own vector, records that vector's address,
and `Append`s it once — **exactly the workaround a binding is forced into today**. Refill
sampling around each call is unchanged in shape, so the published refill numbers keep
their meaning (2026-09-01 ruling). Legs 1–3 and `EncodeAccounted` are untouched; the
existing entries stay green and keep measuring what they measured.

### Spec amendments (all inside the 2026-09-03 authorisation for A1)

- **§3.1 gains clause 6** — the in-place fill: the C form, the pointer's borrow window (one
  call), `room` ≥ `min_bytes`, commit-by-return, and the two refusals.
- **§8's rows bullet** extends from "already there, via `Publish`'s inversion and
  `FixedWriteBuffer`" to cover the producer end — see brief decision 1.
- **§8.1** — provenance is measured from where the **producer** wrote, not from the window
  base after the last append, with the staging-producer control named.

## Corner cases forbidden

**Rung 1 — unrepresentable:**

- *A reservation that outlives a refill, is committed twice, or is committed without being
  taken.* There is no `Reserve`/`Commit` pair and no reservation object; the pointer exists
  only for the writer's frame. None of those three states can be spelled.
- *A writable window pointer with no length.* No non-const `Data()`, no `Capacity()`. The
  only writable pointer obtainable arrives paired with its `room`.
- *A committed count chosen independently of the lent room.* Both are produced by the same
  call, so they cannot disagree by construction of the caller.
- *A partial commit on any failure.* The commit is one assignment after all validation;
  there is no intermediate state to observe or unwind.
- *Re-entrancy from a binding.* The C form of the writer receives `{ctx, dst, room}` and
  **no buffer handle**, so foreign code cannot re-enter at all.

**Rung 2 — one typed refusal, no recovery, no partial mode:**

| Case | Refusal | Why not rung 1 |
|---|---|---|
| `min_bytes == 0` | `kInvalidArgument` | a runtime value; matches A5's empty-refusals |
| writer reports `used > room` | `kInvalidArgument`, nothing committed | the count is produced by foreign code; C++ cannot constrain a returned `size_t` |
| writer moved `data_`/`pos_` (C++ lambda captured the buffer) | `kInvalidArgument`, nothing committed | representable only in C++, where a lambda may capture; unrepresentable in the C form |
| room cannot be produced (fixed buffer, or a refill that under-delivers) | `std::overflow_error` → `kPayloadTooLarge` | the transport's payload bound is a runtime fact (§3.1 clause 4) |

**Handled residue — exactly one:**

- *A writer that writes past `room` without reporting it.* Memory is corrupt before any
  check can run. **Why not forbidden:** the pointer is raw because zero-copy requires it,
  and a bounds-checked view is not C-expressible without handing the writer a length it
  already has. This is the **same** exposure `Append(const uint8_t*, size_t)` has carried
  since the file was written — no new class, and it is disclosed in the header.

**Deliberately not added, and why — this is the answer to verification's "four members":**
`Capacity()` (the writer is *given* `room`, so nobody needs to ask), a non-const `Data()`
(a writable pointer with no bound and no commit discipline — the unsafe shape itself), and
the `Reserve`/`Commit` pair (creates the stale-reservation state rung 1 removes). The
capability is complete with one member: a producer can compose an arbitrary row, of unknown
length, with back-patching (`WriteLengthPlaceholder`/`PatchU32`/`PatchByte` are already
public), entirely inside the window that is delivered.

## Premises and stop conditions

- **P1 — every `WriteBuffer` subclass's `AppendZerosSlow(n)` leaves at least `n`
  contiguous writable bytes at the restored position and preserves bytes below it verbatim
  (§3.1 clause 1).** In-tree the class is closed at three: `VectorWriteBuffer`,
  `FixedWriteBuffer`, and the harness's `GrowableProbeBuffer`; step 3's post-condition
  turns a violation into a loud refusal. **STOP-AND-ASK** if a subclass exists whose refill
  cannot produce contiguous room without appending bytes — that means §3.1's window model
  is wrong and a new protected virtual is needed; ask, do not add one.
- **P2 — attachments are already constructible by a binding** via `Blob(owner, data, size)`.
  **STOP-AND-ASK** if that constructor is not public: A1's scope then doubles and the owner
  must be told before it does.
- **P3 — the 2026-09-03 absorption ruling authorises A1's amendments to §3.1, §8 and §8.1.**
  Extending §8's *published promise* to the producer end is brief decision 1 and carries
  **no default** (frozen text; 2026-09-04 idempotence ruling). **STOP-AND-ASK / do not
  guess:** if it is unanswered when implementation reaches the spec edit, the item does not
  close — do not land §3.1 clause 6 with §8 left saying rows are already fine.
- **P4 — no `PubSubStatus` append is needed.** Both refusals are "the caller broke this
  call's contract" = `kInvalidArgument`. **STOP-AND-ASK** if review holds that a re-entrant
  fill deserves `kReentrantCall = 10`: that value's allocation is owner-authorised but is
  PDA-DEC-A3's to land, and A1 must not append a status.

## Forcing-test mapping

**Forcing test:** `CopyAccounting.InPlaceEncodeWritesIntoTheDeliveredWindow` — `TEST_P` over
`CopyAccountingSubjects()`, so three ctest entries
(`CopySubjects/CopyAccounting.InPlaceEncodeWritesIntoTheDeliveredWindow/{SeamProbe,InProcessLoopback,InProcessViaPubSub}`).
Binary: **`conformance_copy_accounting`**.
Whole-suite scope: **`ctest -R 'CopyAccounting\.|SeamVocabulary\.|WriteBufferInPlace\.'`**
(the last is in `core_tests`).

| Test | What turns it green | Red for the right reason, before |
|---|---|---|
| `…InPlaceEncodeWritesIntoTheDeliveredWindow` (×3) | `AppendInPlace` lends the window cursor, so `encode_copies == 0` **and** `row_copies == 0` on the same publish | **does not compile** — `AppendInPlace` does not exist. For an added capability that is the only truthful red; the *substantive* red is the control below |
| `CopyAccounting.StagingProducerIsCaught` (control) | the staged producer scores `encode_copies == 1` **while `row_copies == 0`** — the blindness itself, pinned as a test | compiles and passes **today**, using only `Append`; written first, it is the standing evidence that the pre-change oracle was green over a lost property |
| `WriteBufferInPlace.WriterReceivesTheWindowCursor` | `dst == Data() + Position()` | member absent |
| `WriteBufferInPlace.OnlyTheReportedBytesAreCommitted` | `pos_ = pos0 + used` | member absent |
| `WriteBufferInPlace.OverReportedLengthIsRefused` | check (b) | member absent |
| `WriteBufferInPlace.ReEntryFromTheWriterIsRefused` | check (a) | member absent |
| `WriteBufferInPlace.GrowableRefillPreservesBytesAndDeliversRoom` | steps 2–3 | member absent |
| `SeamVocabulary.AWriteBufferReferenceCanBeFilledInPlace` | compiles and runs through a plain `WriteBuffer&` | member absent |

**Mutations, and what must redden (the in-tree/packaged split is handled):**

- **M1** — in `core/include/fletcher/core/write_buffer.hpp`, reimplement `AppendInPlace` as
  `scratch → writer → Append(scratch)`. Reddens `WriteBufferInPlace.WriterReceivesTheWindowCursor`
  in **`core_tests`, which links the in-tree header directly** — so M1 is *never inert* — and
  reddens all three forcing entries after a `core` package rebuild, since the harness
  consumes the packaged `core`.
- **M2** — `Judge` returns `encode_copies = 0` unconditionally → `StagingProducerIsCaught`
  and `JudgeArithmeticIsSound` red.
- **M3** — commit `min_bytes` instead of `used` → `OnlyTheReportedBytesAreCommitted` red.
- **M4/M5** — delete check (b) / check (a) → `OverReportedLengthIsRefused` /
  `ReEntryFromTheWriterIsRefused` red.
- **Unpinned, disclosed:** deleting the §8 / §8.1 sentences reddens nothing. No machine
  reads them; the contract is carried by review.

## Risks / Unknowns

- **Size.** Verification's "four members on `WriteBuffer`" is a floor on *effort*, not on
  surface: A4 ("an in-flight count in `Fanout`") landed +1657 and A5 ("two or three checks")
  +1055. Declared net below is my itemised estimate; the realistic band is **+600…+1100**.
  If it exceeds that, the split to propose is *oracle extension first, API second* — but
  I do **not** recommend it: the round has logged five times that a half-landed guarantee
  is what recurs.
- **Owner gate.** Brief decision 1 has no default. Unanswered, the item cannot close (P3).
- **Coexistence windows: none.** Nothing is deprecated, no dual path, no shim, nothing
  scheduled for a later stage's deletion.
- **Platform.** Per the 2026-09-03 handoff ruling, all runs here are local Windows; treat
  Linux as unverified and route any Linux-only difference to the owner.
- **Claim limit.** Green says the *API* permits an uncopied encode, measured with an
  in-process stand-in producer — not that BIND-C# achieves it. Brief decision 2.

## Files-to-touch

| Path | Change |
|---|---|
| `core/include/fletcher/core/write_buffer.hpp` | `AppendInPlace` + its normative comment block (locked decision 5); `VectorWriteBuffer::Reserve` → `ReserveStorage`; include `status.hpp` |
| `core/tests/test_write_buffer.cpp` | **new** — `WriteBufferInPlace.*`, five cases |
| `core/tests/CMakeLists.txt` | one source line |
| `integration-tests/pubsub-conformance/include/fletcher/conformance/copy_accounting.hpp` | ledger/verdict fields, `ProducerMode`, `RunProducerRoundTrip` |
| `integration-tests/pubsub-conformance/src/copy_accounting.cpp` | the two producers, the driver, `Judge`'s `encode_copies` |
| `integration-tests/pubsub-conformance/src/copy_clauses.cpp` | forcing test + control + `JudgeArithmeticIsSound` rows |
| `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp` | one representability case |
| `integration-tests/pubsub-conformance/README.md` | the copy definition, the entry table (7 → 11), the new claim limit |
| `docs/pubsub-interface-spec.md` | §3.1 clause 6; §8 rows bullet; §8.1 |
| `plans/PDA-decouple-progress-log.md` | item entry |

**Declared net: ≈ +640 / −25.** Itemised: header +75/−4 · `core_tests` new file +185 and
CMake +1 · harness header +55 · `copy_accounting.cpp` +95 · `copy_clauses.cpp` +80 ·
`seam_vocabulary.cpp` +25 · harness README +50/−12 · spec +35/−9 · progress log +25.
The test apparatus is **+385 of the +640** and is named explicitly here because the last two
items' declarations went wrong by under-counting exactly that.

**New public surface: 1** — `WriteBuffer::AppendInPlace`. Nothing is retired to make room
and nothing needs to be. Harness-header additions (`ProducerMode`, `RunProducerRoundTrip`,
four struct fields) are test apparatus, which §12.1 explicitly holds open for both later
rounds; counted strictly they bring the total to 3, still inside budget.

## Files-to-delete

Not `none`. This stage retires, each with its replacement:

- **`docs/pubsub-interface-spec.md` §8.1's "the window base after the encoder's last
  append"** as the start of the measured interval → replaced by the producer's write site.
- **`docs/pubsub-interface-spec.md` §8's rows bullet** as written ("already there, via
  `Publish`'s inversion and `FixedWriteBuffer`") → replaced per brief decision 1.
- **`integration-tests/pubsub-conformance/README.md`'s "Not a copy: … the encode itself"**
  → replaced; the encode *is* measured now, and the sentence is the written form of the
  blindness this item removes.
- **`VectorWriteBuffer::Reserve` (the name)** → `ReserveStorage`; private, no caller
  outside the file.

No test is deleted: every existing `CopyAccounting` entry keeps measuring what it measured,
which is the point of leaving `EncodeAccounted` untouched.
