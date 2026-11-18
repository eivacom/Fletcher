# PDA-DEC-2 — Copy-accounting oracle (makes zero-copy falsifiable)

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §8, §8.1,
§3.1, §3.2. Locked decisions 6, 7 (arbiter), 2, 3, 11, 13, 14. Rulings ledger
2026-08-31 ("also for attachments"), 2026-09-01 (blind-spot ruling).

## Summary

An oracle that decides, by **address provenance**, whether the payload bytes a
subscriber sees are the very bytes the publisher wrote — for rows and for
attachments — and a **live negative control** that proves the instrument can go
red. It lands as a second suite, `CopyAccounting`, inside PDA-DEC-1's harness. It
measures the **seam**, per §8 ("a property *of this seam*"); it measures no
transport, and says so.

## Design

### What counts as a copy (forbid before you measure)

- **Payload bytes** = the bytes the `RowEncoder` writes, and the bytes of each
  attachment `Blob`.
- **A copy** = those bytes coming to exist at a second address, or moving to a
  different address, by code at or above the seam, between the encoder's first
  write and the subscriber callback's return.
- **Not a copy:** the encode itself (the first write); anything a transport does
  with the bytes after they leave the seam (§8 scopes the property to the seam);
  a window refill inside a provider's own growable buffer — §3.1 clause 4
  sanctions it, and Decision 1 in the brief puts that to the owner.

### Mechanism: provenance, not interposition

The oracle records four pointers per publish and compares them. No counters, no
allocator hooks, no sanitizer.

`CopyLedger` (test-only): `encode_base`/`encode_len` (the window base after the
encoder's last append, from `WriteBuffer::Data()` + `Position()`);
`refill_moves`/`refill_bytes` (sampled by the accounting encoder: if `Data()`
changes across an `Append` while `Position() > 0`, the prior position is bytes
relocated); `delivered_data`/`delivered_len` (captured in the callback); and, per
attachment, `published_data` and `delivered_data`.

`CopyVerdict Judge(const CopyLedger&)` — a **pure function**:

- `row_copies` = 0 iff `delivered_data == encode_base && delivered_len ==
  encode_len` — **strict equality, not containment (DEBT-2)**. Containment
  degenerates to identity only when the lengths match; for a shorter delivered
  range it admits an identity-preserving in-place `memmove` to the window base,
  where every payload byte has moved, the address is unchanged and `memcmp`
  passes by construction. Both registered subjects satisfy strict equality today.
  A subject delivering a sub-range must say why before the rule is relaxed.
- `attachment_copies` = the number of attachments whose delivered `data()` is not
  the published `data()`.
- `refill_bytes` passes through, reported.

Content equality (`memcmp`) is asserted **before** the verdict is read, so
"arrived at a different address" and "arrived garbled" are different failures and
neither can be mistaken for the other.

**Why this mechanism, against the alternatives.** *Global `operator new`
replacement / allocation counting*: catches only allocation-shaped copies, misses
copies into a pooled or reused buffer, is noisy (every `std::string` in the
harness), and on Windows does not interpose allocations made inside a provider DLL
carrying its own CRT — blind exactly where a driver would live. *Sanitizer /
valgrind / callgrind memcpy counting*: no MSVC-side equivalent, and orders of
magnitude too slow for `inner_loop_cmd`. *`LD_PRELOAD` memcpy interposition*:
Linux-only. *Throughput proxy*: a copy of a 64-byte row is invisible in a timing,
so it is not falsifiable at all. Provenance is a handful of pointer comparisons,
is plain portable C++ with identical behaviour on MSVC and gcc, costs
microseconds, and — unlike counting — a copy into recycled storage still lands at
a different address than the live encode window.

### Subjects

`CopySubject { std::string label; std::function<std::unique_ptr<CopyRunner>()> make; }`  *(as landed)*
— same-process by construction. Registered:

| Subject | What it is | Role |
|---|---|---|
| `SeamProbe` | ~120-line `PubSubProvider` in the harness: `Publish` takes a slot from a fixed arena, hands the encoder a `FixedWriteBuffer` over it, delivers `slot.base` synchronously, passes `Attachments` by const-ref | positive control — proves the seam *permits* zero-copy |
| `InProcessLoopback` | the real `InProcessPubSubProvider` (`pubsub/`) | real-code subject |
| `StagingProbe` | `SeamProbe` but copies the row into a scratch vector and deep-copies each blob | **negative control** (own test) |

A third subject, `InProcessViaPubSub`, routes the same loopback through `Publisher`
and `Subscriber` (**DEBT-1**), so the layers above the seam are measured too — §8
words the attachment claim "publisher → provider → subscriber", and without it a
`std::vector` materialised in `Subscriber`'s fan-out would be silent.

Rows are exercised at **two sizes** — 64 B and 4 KiB — because a large row is what
forces a growable provider buffer to relocate, and relocation is the way row
identity plausibly breaks. **The 4 KiB row is written in 64-byte appends
(DEBT-5)**: one `Append(payload, 4096)` takes `VectorWriteBuffer`'s `len >= kChunk`
bulk path, relocates nothing and reports `refill_bytes == 0`, which would leave the
4 KiB leg indistinguishable from the 64 B one. Sub-`kChunk` appends reallocate at
512/1536/3584, so `refill_bytes > 0` is **expected** there for a growable buffer
(measured: 3 moves / 5632 B). Attachments: two entries, 1 KiB each.

### The legs

1. **Row, publish → delivery:** `row_copies == 0`, both sizes, every subject.
2. **Attachment, publish → delivery:** `attachment_copies == 0`, two attachments.
3. **Receive-side borrowed memory** (own test, not the forcing test): `SeamProbe`
   delivers an attachment whose bytes live in its arena — a stand-in for a
   transport-loaned sample. `Blob = shared_ptr<const vector<uint8_t>>` cannot
   alias foreign memory (§3.2), so the provider must copy.
   `CopyAccounting.BorrowedAttachmentCostsExactlyOneCopy` asserts **exactly 1**
   and names PDA-DEC-3. When PDA-DEC-3 makes borrowed memory carryable this test
   goes red on purpose; that is how the baseline cannot be fixed silently or
   forgotten.

### Falsification: the control is part of the item

PDA-DEC-1's lesson was that a guard nobody made go red is a guard nobody has
measured. `CopyAccounting.StagingIsCaught` runs `StagingProbe` through the same
capture and the same `Judge()` and requires `row_copies == 1` and
`attachment_copies == 2`. It runs on every CI run on both platforms, in the inner
loop, and is the thing that fails if the instrument is broken, stubbed or deleted.
A second, cheaper check constructs a `CopyLedger` by hand and checks `Judge()`'s
arithmetic — the pure function makes that free.

### Threading

For every registered subject, delivery happens synchronously inside `Publish` on
the publishing thread, so the ledger needs **no lock and has none**. Delivery
count is asserted `== 1` before the verdict is read, so a missing delivery can
never read as "no copies". This is also the rung-1 reason an asynchronous or
cross-process subject cannot be registered (below).

### The one public change

`WriteBuffer::Data()` — `const uint8_t*`, the current window base. Documented
(**DEBT-6**) as: only `[Data(), Data() + Position())` is defined, and the pointer
is invalidated both by an append that refills AND by `VectorWriteBuffer::Finish()`,
which nulls it. Without it no copy accounting of any
kind is possible from outside a provider. §3.1 already declares the window to be
`{data, capacity, pos}` and that a C view must honour it, so this exposes nothing
the spec does not already require to be visible. PDA-DEC-3 may fold it into a
window struct — a rename, not a bridge.

### Home and wiring

The suite lives in PDA-DEC-1's harness (`integration-tests/pubsub-conformance/`),
new TU + new CMake target, so it inherits the CI lane, the format/licence gates and
the subject-parameterised shape PDA-ABI registers a driver-backed subject into.
It links no provider SDK, so it runs in milliseconds.

Inner loop `-R`: **`'CopyAccounting\.'`** — the whole oracle including the control,
7 ctest entries, all in-process. Scoping to the forcing test alone would let an
implementer green it with a broken instrument.
Forcing test alone: `ctest -R 'CopyAccounting\.PublishAndReceivePerformNoPayloadCopies'`.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *A cross-process or asynchronous copy-accounting subject.* Provenance is an
   address; `CopySubject` has no peer verb and the ledger is deliberately
   unsynchronised, so a subject that delivers off-thread or in another address
   space cannot be built at all.
2. *A per-subject "expected copies" trait.* `CopyVerdict` carries no subject-keyed
   expectation; every registered subject faces the same numbers. A provider
   cannot declare its way to green — locked decision 11's forbidden pinned
   divergence wearing a trait.
3. *A silently skipped subject or leg.* No `GTEST_SKIP`; the subject list is
   compile-time, so an absent subject shows in `ctest -N`.
4. *Confusing a copy with corruption.* Content equality is asserted before the
   address verdict is read.
5. *A copy hidden by allocator reuse.* Addresses are compared, never allocation
   counts — the reason the counting mechanism was rejected outright rather than
   used as a second signal.
6. *Any ABI surface.* No `extern "C"`, C header, `dlopen`, vtable, host-callback
   struct or negotiation (decision 14). Nothing above the seam branches on
   built-in vs loaded (decision 3) — the oracle sees only `PubSubProvider`.
7. *Wire-format assertions.* The oracle reads addresses and `memcmp`s bytes
   against what it wrote; it cannot see payload layout (decision 13).

**Rung 2 — refused typed at the door**

8. *Zero or more than one delivery for one publish* ⇒ the test fails naming the
   count, before any verdict is computed. No partial mode.
9. *A provider that throws during publish* ⇒ fails naming the exception; never a
   silent `row_copies == 0`.
10. *An empty attachment map on the attachment leg* ⇒ the leg requires ≥2
    attachments, so `attachment_copies == 0` cannot be satisfied vacuously.

**Handled residue**

11. *Window refill in a growable provider buffer.* Counted and reported, not
    failed. **Why not forbidden:** §3.1 clause 4 states "a fixed-capacity buffer
    reports overflow; a growable one refills" — forbidding it would contradict the
    spec, and no growable buffer can reach zero relocations for an arbitrary row
    size. Decision 1 in the brief puts the tension to the owner.
12. *The one copy today's `Blob` forces on borrowed receive memory.* Asserted as
    exactly 1. **Why not forbidden:** locked decision 6 assigns that fix to
    PDA-DEC-3; forbidding it here would be doing PDA-DEC-3's work inside a guard
    item, and the guards must land first (decision 11).

## Premises and stop conditions

- **P1 — `WriteBuffer` may expose its window base.** STOP-AND-ASK if exposing
  `Data()` is judged a seam-vocabulary change PDA-DEC-3 must own: then ask for the
  accessor to be pulled into PDA-DEC-3 and this item to wait. Do **not** substitute
  allocation counting — it is measurably blind on Windows DLL boundaries.
- **P2 — `InProcessPubSubProvider` delivers synchronously on the publishing
  thread and hands the callback the encode buffer's own bytes.** Verified at
  `pubsub/src/in_process_provider.cpp` (`VectorWriteBuffer` → `Finish()` moves the
  vector → `cb(buf.data(), …)` under the lock). STOP-AND-ASK if this changes: the
  ledger's lock-free rung-1 argument dies and a synchronised ledger is a different
  design.
- **P3 — `Attachments` crosses the seam by const-ref, with no map copy.**
  Verified in `provider.hpp:93-94`, `:123-125`. **Consequence corrected (DEBT-4):**
  if the const-ref disappeared, leg 2 would NOT go red — a by-value map copy copies
  `shared_ptr`s, so every blob's `data()` is unchanged and `attachment_copies == 0`
  is the *correct* answer here. A map copy is a per-delivery allocation, not a
  payload copy: raise it as a §3.2 finding in prose, not as a red leg.

- **P5 — encode-window liveness (DEBT-3).** The bytes `encode_base` names remain
  allocated and unfreed until the subscriber callback returns. That is what makes
  address provenance immune to allocator reuse: a live allocation cannot be handed
  out twice, so a same-address copy requires free-then-allocate — and a freed
  encode window handed to a callback is a use-after-free, a worse bug than a copy.
  Satisfied by every registered subject (`SeamProbe`'s arena is a member; the
  loopback's `buf` is alive across `cb(...)`, `in_process_provider.cpp:76-89`).
  **A subject that frees, recycles or pools its encode window before delivery
  makes provenance unsound and must not be registered — STOP-AND-ASK first.**
  PDA-ABI registers a driver-backed subject into this shape, so it is stated.
- **P4 — Fast DDS receive-side zero-copy does not exist and receive-side
  data-sharing is off by default** (live defect, owned by PDA-ABI-7). If that
  changes mid-item, do **not** add a DDS subject here; PDA-ABI-7 registers it.

## Forcing-test mapping

**`CopyAccounting.PublishAndReceivePerformNoPayloadCopies`**
(ctest name `<Subject>/CopyAccounting.PublishAndReceivePerformNoPayloadCopies/<Subject>`),
over `SeamProbe` and `InProcessLoopback`, at 64 B and 4 KiB.

- *Turns green by:* `Publish`'s inversion (decision 4) letting a provider hand the
  encoder its final destination buffer, so `delivered_data` lies inside the encode
  window; and `Attachments` crossing by const-ref, so each delivered blob is the
  published blob. Requires `WriteBuffer::Data()` to capture the window.
- *Red for the right reason:* the test is new, so its red is supplied by the live
  negative control `CopyAccounting.StagingIsCaught` — the same capture and the same
  `Judge()` scoring `StagingProbe` at `row_copies == 1`, `attachment_copies == 2`.
  Stub, weaken or delete the instrument and that control fails; delete
  `WriteBuffer::Data()` and the build fails.

**`CopyAccounting.BorrowedAttachmentCostsExactlyOneCopy`** — green today at exactly
1, and deliberately red the moment PDA-DEC-3 makes borrowed transport memory
carryable. That is what makes PDA-DEC-3's own forcing test
(`SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy`) expressible.

*Machine checks that prove the intent, so no ledger is hand-composed here:* the
compiler (removing `Data()`), the control test (a broken instrument), and
`ctest -N` (a subject that stopped being registered).

## Risks / Unknowns

- **The oracle proves the seam, not any transport.** A green run is **not**
  evidence about Fast DDS data-sharing, loaned samples, or receive-side
  zero-copy — none of which exist or are enabled today (PDA-ABI-7). This is the
  same failure mode the 2026-09-01 blind-spot ruling recorded for PDA-DEC-1, and
  the README carries the clause verbatim in that spirit. Measuring the DDS receive
  leg today would measure the serialize-and-copy path, which is not the zero-copy
  path — so it is not measured, rather than measured wrongly.
- **Decision 1 (brief) is an oracle-wins tripwire**: §3.1 clause 4 sanctions
  refills; the 2026-08-31 ruling says "a copy anywhere on either path is a
  violation, not a trade". Raised, not designed around. If the owner answers (a),
  the loopback needs a pre-sizing change (~40 lines, in-round under decision 11)
  and even then cannot reach zero for arbitrary rows — that consequence is stated
  in the brief.
- **No coexistence window.** Nothing in this item is scheduled for deletion in a
  later stage. `WriteBuffer::Data()` survives into PDA-DEC-3, possibly renamed.
- **CI path filter:** verified — `core/**` is already in the
  `integration-pubsub-conformance` filter (`.github/workflows/ci.pr.yml:251`), so a
  `WriteBuffer` change runs the lane. Nothing owed.
- **Budget:** fits. No stage split proposed.

## Files-to-touch

- `core/include/fletcher/core/write_buffer.hpp` — add `Data()` (+ doc line).
- `integration-tests/pubsub-conformance/include/fletcher/conformance/copy_accounting.hpp` — **new**: `CopyLedger`, `CopyVerdict`, `Judge`, `CopySubject`, arena.
- `integration-tests/pubsub-conformance/src/copy_accounting.cpp` — **new**: `SeamProbe`, `StagingProbe`, arena, `Judge`, subject registry.
- `integration-tests/pubsub-conformance/src/copy_clauses.cpp` — **new**: the `CopyAccounting` gtest suite (forcing test, control, baseline).
- `integration-tests/pubsub-conformance/CMakeLists.txt` — new target + `gtest_discover_tests`.
- `integration-tests/pubsub-conformance/README.md` — what the oracle proves and, explicitly, what it does not.
- `docs/pubsub-interface-spec.md` §8.1 — record the mechanism and its scope (≤12 lines); §3.1 gains a sentence for `Data()`.
- `.claude/runbook.PDA-DEC.config.md` — `inner_loop_cmd` `-R 'CopyAccounting\.'`.

## Files-to-delete

**none.** Justified: this is an add-only test-guard item under the round's
guard-first rule (decision 11); a grep of the tree found **no existing test that
asserts zero-copy**, so nothing is superseded, and the only existing artefact it
changes is `WriteBuffer`, which gains an accessor rather than losing one.

## Numbers

Declared net lines: **+560 / −5**. **As landed: +1182 / −12** (`666ced8` +
`581e28a`) — both step-4 reviews judged this not scope creep: the third subject was
owed by DEBT-1, and the fix cycle's growth is the blocking falsifiability fix plus
three should-fixes. New public surface: **1** (`WriteBuffer::Data()`), as declared.
