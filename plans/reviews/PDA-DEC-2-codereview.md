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
