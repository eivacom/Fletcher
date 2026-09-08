# PDA-DEC-2 — code review (step 4b, independent)

Diff base `1f5d229..666ced8`, branch `feature/protocol-driver-abi`.
Built and ran: `conformance_copy_accounting.exe` — 7/7 pass, ~0 ms.
`clang-format 18.1.3 --dry-run -Werror` clean on all four changed C++ files.

Counts: **1 blocking · 2 should-fix · 8 nits.**

---

## Blocking

### B1 — `BorrowedAttachmentCostsExactlyOneCopy` asserts a structural constant; it can never go red (confidence: high)

`src/copy_clauses.cpp:171-183`, instrument at `src/copy_accounting.cpp:RunBorrowedAttachmentRoundTrip`.

The leg builds its own copy and then measures that it exists:

```cpp
const uint8_t* loaned_base = provider->StageLoanedBytes(loaned);
attachments.emplace("loaned", std::make_shared<const std::vector<uint8_t>>(
                                  loaned_base, loaned_base + loaned.size()));
...
trace.published_data = loaned_base;   // arena slot, a live member array
```

`Judge()` then compares `trace.delivered_data` (the fresh vector's `data()`, which
`SeamProbeProvider` forwards unchanged) against `loaned_base`. Those two addresses
belong to two distinct live objects, so `same_bytes` is **false by construction**,
for every build, forever. And `ledger.attachments` holds exactly one trace, so
`attachment_copies` is confined to {0,1} — the value 2 is not representable.

Consequences, all of which the tree currently asserts as true:

- `EXPECT_EQ(verdict.attachment_copies, 1)` cannot fail on any regression.
- The failure message's "If it is >1, something copies the blob a SECOND time"
  branch is unreachable — a second copy inside the provider changes the delivered
  address from one non-matching value to another non-matching value, and the count
  stays 1.
- The "If this is 0, PDA-DEC-3 has landed" branch is unreachable too: PDA-DEC-3
  changes `Blob` so it *can* alias foreign memory, but the copy this test counts is
  made by the harness, not by the seam, so the test would keep printing 1 until
  someone rewrites it — which is exactly the silent half-landing it advertises
  itself as preventing.

This is not a cosmetic overclaim, because two documents now rest on it:
`docs/pubsub-interface-spec.md` §8.1 ("pinned at **exactly one**, so removing it
turns the guard red rather than letting the fix land silently") and
`integration-tests/pubsub-conformance/README.md` ("a red-on-fix tripwire, not an
accepted divergence"). One of the seven entries in a suite whose entire premise is
falsifiability is an entry that cannot be falsified.

Two honest exits, either acceptable:

1. **Make the seam perform the copy.** Give `SeamProbeProvider` a publish path that
   takes the borrowed span and is itself responsible for turning it into a `Blob`
   (probe builds the vector, not the test). Then `attachment_copies` is measuring
   provider behaviour, 0 becomes reachable the moment PDA-DEC-3 lands, and the
   tripwire claim becomes true.
2. **Forbid it at the door.** Delete the leg and replace it with the static fact it
   actually encodes — `static_assert(std::is_same_v<Blob, std::shared_ptr<const
   std::vector<uint8_t>>>, "PDA-DEC-3: Blob can now alias; revisit §3.2's forced
   copy")`. That *does* fire on the day PDA-DEC-3 lands, costs one line, and lets
   `StageLoanedBytes` and the 4-slot arena rotation go with it. Then correct §8.1
   and the README to say the copy is a property of the type, not a measurement.

Exit 2 is the smaller change and the more truthful one.

---

## Should-fix

### S1 — `Judge()` compares pointer values that are already invalid, in the mainline subjects (confidence: high; materiality: medium)

For `InProcessLoopback` and `InProcessViaPubSub`, both `ledger.encode_base` and
`ledger.delivered_data` name `buf.data()` inside `InProcessPubSubProvider::Publish`,
whose `const std::vector<uint8_t> buf` is destroyed when `Publish` returns. `Judge()`
runs in the test body, after `RunRoundTrip` has returned. So the equality that decides
`row_copies` is performed on two **invalid pointer values** ([basic.stc.general]/4:
any use of an invalid pointer value other than the enumerated ones is
*implementation-defined*, and an implementation is permitted to trap or to fold the
comparison). `StagingProbe`'s `staged.data()` is in the same position.

The *logic* is sound — the addresses are sampled while the storage is live, so P5's
"a live allocation cannot be handed out twice" argument holds and there is no
allocator-reuse hole. It is only the representation that is on
implementation-defined ground, and the header spends 40 lines arguing this
instrument is well-defined on both toolchains. Fix costs four lines: capture as
integers at sample time, while the pointers are still valid, and let `Judge()`
compare integers.

```cpp
// CopyLedger
uintptr_t encode_base = 0;      // 0 == "no window"
uintptr_t delivered_data = 0;
```

with `reinterpret_cast<uintptr_t>(...)` at the two capture sites (`EncodeAccounted`,
`MakeCapture`) and in the `AttachmentTrace` pair. Everything downstream — including
`JudgeArithmeticIsSound` and the `static_cast<const void*>` in the failure messages —
is a mechanical rename. Nothing about the semantics changes; the instrument stops
depending on a rule the standard declines to pin down.

### S2 — P5 (encode-window liveness) is documented and STOP-AND-ASK'd, never checked (confidence: high; materiality: medium)

The header, the README and §8.1 all state that a subject which frees, recycles or
pools its encode window before the callback returns makes provenance unsound, and
the remedy offered is "MUST NOT be registered. STOP-AND-ASK." PDA-ABI is explicitly
expected to register a driver-backed subject into this shape — a subject whose
buffer management the harness will not own. A registered subject that violates P5
produces a **silent green**: the freed window's address gets handed to a same-sized
reallocation and `delivered_data == encode_base` reads as zero copies. That is a
third failure mode beyond the two the implementer measured (stubbed `Judge()`,
inert window base), and neither the control nor any current assertion sees it.

There is a cheap partial enforcement worth adding: at the top of `MakeCapture`,
before anything else, verify that the encode window still holds the payload —

```cpp
ledger.window_still_intact =
    ledger.encode_base != nullptr && ledger.encode_len == expected_row.size() &&
    std::memcmp(ledger.encode_base, expected_row.data(), ledger.encode_len) == 0;
```

and assert it in `COPY_MUST_DELIVER_CLEANLY`. That read happens *inside* the
callback, where P5 says the bytes are live, so it is well-defined under the stated
precondition and catches the freed-and-reused window (which will almost never come
back byte-identical) as its own named failure rather than as a zero. It does not
make P5 airtight, but it converts the most likely violation from silent-green to
red, which is the standard this suite sets for itself everywhere else.

*(Verified NOT findings, for the record: `VectorWriteBuffer::Finish()`'s
`resize(pos_)` + move preserves the heap pointer, so `encode_base == buf.data()` in
the loopback subjects is a real identity and not a coincidence. The `ValuesIn`
subject labels **do** reach the ctest entry names — CMake's `gtest_discover_tests`
parses the `# GetParam() = SeamProbe` comment out of `--gtest_list_tests`, so
`PrintTo` earns its keep and `ctest -N` shows
`.../PublishAndReceivePerformNoPayloadCopies/InProcessLoopback`. The refill
arithmetic checks out by hand and on the wire: 512+1536+3584 = 5632 across 3 moves.
`WriteBuffer::Data()` is correctly const-qualified, hands out no write access, is
safe in every buffer state including pre-allocation and post-`Finish()` (null in
both), and its invalidation contract is stated.)*

---

## Nits

- `src/copy_accounting.cpp:22` — `#include <mutex>` is unused; the header makes a point of the ledger having no lock.
- `Arena::kSlots == 4` with rotation is unexercised: every provider instance in the suite publishes at most once (twice on the borrowed leg), so the rotation and 32 KiB of slots buy nothing. Goes away with B1 exit 2.
- `SeamProbeProvider::StageLoanedBytes` `memcpy`s without checking `payload.size() <= Arena::kSlotBytes` — the one place in the file that can overrun silently, in a class whose sibling path is documented as failing loudly on overflow.
- `MakeCapture`'s attachment loop `continue`s on an absent key, so an entirely missing attachment surfaces through the "arrived GARBLED" assertion message — right verdict, wrong noun.
- `COPY_MUST_DELIVER_CLEANLY`'s `ASSERT_EQ((trip).ledger.attachments.size(), expected)` compares the *published*-side vector, built 1:1 from the input map, so it cannot fail either; the delivered count is never asserted directly.
- `RefillMovementIsCountedNotFailed` asserts `flat_verdict.refill_moves == 0` but not `refill_bytes == 0`.
- `EncodeAccounted` misses a refill whose reallocation happens to return the same base address (report-only undercount of a permitted number; never a missed copy).
- `PublishRefillCost` narrows `size_t` to `int` for `RecordProperty`; fine at today's 5632, silently wrong past 2 GiB.

---

## RECORD (for the PM; not blocking, no fix cycle)

- Diff is **+1097 / −11** against a declared **+560 / −5**. The attribution to doc-comment does not hold: of the 910 new TU lines, 299 are comment, 118 blank, **493 are code** — the code alone is nearly the whole declared budget and the docs sit on top of it. `copy_accounting.hpp` is the doc-heavy file (149 comment / 74 code); the two `.cpp`s are ~28% comment, in line with the rest of this harness.
- README's "seven entries" and the per-entry table are accurate (`ctest -C Release -N -R 'CopyAccounting\.'` → 7).
- README's "the number is on stdout and in the JUnit XML" is accurate — verified; gtest emits the `/`-bearing keys as `<property name=...>` attribute values, so the XML stays well-formed.
- `docs/pubsub-interface-spec.md` §8.1 and the README both describe `BorrowedAttachmentCostsExactlyOneCopy` as a red-on-fix tripwire. That wording must change with B1, whichever exit is taken.

---

# Re-check after fix cycle 1 — 2026-09-01

Diff `666ced8..581e28a` (the fix), whole item still `1f5d229..581e28a`.

Built and mutation-tested `581e28a` in an isolated worktree (`/c/tmp/pda-dec2-rc`),
because the shared tree was being mutated by the parallel 4a review at the time.
Pristine: `conformance_copy_accounting.exe` 7/7, 0 ms. `clang-format 18.1.3
--dry-run -Werror` clean on all three C++ files. Worktree restored.

Counts: **0 blocking · 1 should-fix · 4 nits.**

## The three original findings

**B1 (blocking) — CLOSED.** The leg is genuinely falsifiable now, verified by
building, not by report.

- `SeamProbeProvider::Publish` constructs the `Blob` (`delivered.insert_or_assign(
  loan_key_, std::make_shared<...>(loan_base_, loan_base_ + loan_len_))`). The
  copy is provider code; the harness only parks bytes and records the arena base.
- **0 is reachable.** I mutated `LoanForDelivery` to park the payload in a
  provider-held `Blob` and `Publish` to hand that same `Blob` over (the shape a
  seam that could carry borrowed memory would take). Result: red, both
  `EXPECT_NE(loaned.delivered_data, loaned.published_data)` and
  `attachment_copies` `Which is: 0 / Which is: 1`.
- **2 is reachable** and is asserted standing, not by inspection, via the
  `copying_provider` leg — which passes today at 2.
- **The two attachments cannot mask each other.** I built the adversarial case
  where the total stays 1 but is wrong in both halves (loaned delivered free,
  owned deep-copied). `EXPECT_EQ(verdict.attachment_copies, 1)` did **not** fire —
  it is the per-attachment `EXPECT_EQ(owned...)` / `EXPECT_NE(loaned...)` pair
  that catches it, and it does, with the right nouns. The pinned total alone
  would still have been a silent green; the fix put the weight in the right place.
- The `static_assert` is true (`Blob` is exactly
  `std::shared_ptr<const std::vector<uint8_t>>`, core/types.hpp:20) and load-
  bearing, not decorative: it is the only thing that fires on the PDA-DEC-3 shape
  no provider-side measurement can see. Its residual (a parallel borrowed-blob
  type) is stated in the header rather than papered over.

**S1 (dangling pointer comparison) — CLOSED.** `Address = uintptr_t` throughout;
every comparison in `Judge()`, `JudgeArithmeticIsSound` and the leg-3 assertions
is integer. The only pointer round-trips left (`reinterpret_cast<const uint8_t*>`
in `MakeCapture` for the P5 memcmp and the attachment content memcmp) are *reads*,
performed inside the callback where the storage is live by precondition, on values
obtained from valid pointers of the same type — the round trip is guaranteed to
recover the original value. No implementation-defined comparison remains.

**S2 (P5 unchecked) — CLOSED as scoped.** The check is real and cannot be bypassed
(it defaults `false`, is computed first in the callback, and is an `ASSERT_` ahead
of the length and content checks). Verified: mutating the growable probe to recycle
its window before delivery goes red as `P5 VIOLATED`, not as a copy count.

I did also confirm the boundary of what it buys, since I am the one who proposed
it: when `row_copies == 0`, `window_intact` is implied by `row_content_ok`, so the
check is a **diagnostic relabel plus a catch for the clobbered window**, not a new
detector. The exact silent green S2 named — window freed, allocator hands the same
address back holding the row — still reads as zero; I built it and it passes green.
The README says this in as many words ("Not airtight — a window freed and
immediately re-handed the same bytes at the same address is indistinguishable"), so
the record is honest. Only the header comment overstates it (see RECORD).

## Should-fix

### S3 — the loaned bytes' liveness rests on an uncounted publish budget; give the loan its own slot (confidence: high; materiality: low)

`copy_accounting.cpp`, `Arena` / `LoanForDelivery`. The loan takes a slot from the
same 4-slot rotation the row buffer draws from. "The loaned bytes and the row of
one publish never share an address" is therefore true only because no provider in
this suite publishes more than once; the 4th `Publish` on a loaned provider hands
the row buffer back the loan's slot and the delivered `loaned` blob would carry row
bytes. Nothing enforces or states the bound, and `kSlots` reads as a tuning
constant rather than as a safety one.

Unreachable today (one publish per provider, and the consequence would be a loud
`arrived GARBLED`), so this is not a bug — it is machinery kept to tolerate a state
that can be made unrepresentable. A dedicated `std::array<uint8_t, kSlotBytes>
loan_slot_;` member, used by `LoanForDelivery` instead of `arena_.NextSlot()`,
removes the coupling entirely and costs two lines. PDA-ABI is the plausible tripper,
since a driver-backed subject is the first thing likely to publish twice.

## Nits

- A provider that frames the row into the same `WriteBuffer` (prefix, or a
  pre-filled `VectorWriteBuffer`) makes `encode_len != expected_row.size()` and is
  accused of `P5 VIOLATED` — a use-after-free diagnosis for a framing choice. Red
  either way, wrong noun; worth a word in the message for PDA-ABI's sake.
- `Arena::kSlots == 4` (32 KiB) still buys nothing: at most two slots are ever
  live. Two would do, and with S3 applied, one.
- The `copying_provider` bool default parameter reads as a flag at the two call
  sites; an enum or a second named entry point would say which probe is running.
- `EncodeAccounted`'s "refill with the same base" gap is a non-issue, and by a
  stronger argument than the implementer's: a reallocation that returns the same
  base *did not move the bytes*, so there is nothing to count. It is not specific
  to `GrowableProbeBuffer`'s allocate-before-free — no allocator can produce the
  missed case. Both directions are also asserted against harness-owned buffers, so
  an undercount would fail loudly rather than under-report.

## Volume

The +117 net code lines are all fix: `GrowableProbeBuffer` and `ProbeMode::kGrowable`
(the harness-owned refill control, which is also what stops a provider pre-sizing
its send buffer from turning `RefillMovementIsCountedNotFailed` red), the rewritten
loan/deliver path, `window_intact` + `delivered_attachments` capture, the second
attachment and its per-key assertions, the `copying_provider` leg, `TraceNamed`/`Hex`,
the `static_assert`. Comment lines went *down* 70. Six of my eight nits are also
fixed in passing (`<mutex>`, the `LoanForDelivery` overflow check, `refill_bytes == 0`,
the `RecordProperty` int narrowing, the published-side attachment count, MISSING vs
GARBLED). No scope creep; nothing in the +117 fails to carry weight.

## RECORD (for the PM; not blocking)

- `copy_accounting.hpp`, `window_intact` comment: "would otherwise read as a silent
  zero once the allocator handed the address back out" overclaims — that is precisely
  the case the check still misses, as README's own "Not airtight" paragraph states
  correctly. Align the header with the README.
