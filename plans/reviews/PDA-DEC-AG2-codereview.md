# PDA-DEC-AG2 — code review (step 4b)

Diff base `102e56d`, working tree, uncommitted. 28 files, +1278 / −106.

Built and ran: `core_tests` (42/42), `conformance_seam_vocabulary` (13/13),
`conformance_registry` (22/22), `conformance_fastdds` (40/40, run from
`build/Release`). Full `ctest` in `integration-tests/pubsub-conformance/build`
reports 141 entries, 13 of which fail with `cannot start peer …
CreateProcess error 2` — a launcher/DLL-search artefact of ctest's working
directory, **not** this diff: the same 17 cross-process cases pass when
`conformance_fastdds.exe` is run from `build/Release`, and this diff touches no
CMake file in that tree. `pubsub/build` and `fastdds-pubsub-provider/build` could
not be reconfigured (stale Conan cache entries for `fletcher-core` /
`fletcher-pubsub`); their changes are mechanical migrations reviewed by eye.

Counts: **1 blocking · 2 should-fix · 4 nits · 3 RECORD**.

---

## BLOCKING

### B1 — decode is now quadratic in a wire-supplied attachment count, and the bound the code claims for `k` does not exist on one of the two paths

*Confidence: high on the measurement. The only judgement call is whether hostile
wire input is in scope for this seam; if the PM rules it out this downgrades to
should-fix — but the stated bound is wrong either way.*

`Attachments::Set` (`core/include/fletcher/core/types.hpp:174-191`) inserts into a
sorted vector. Keys arriving in ascending order append in O(1) amortised; keys
arriving in **descending** order cost O(k) element moves each, i.e. O(k²) per
envelope. `std::unordered_map::insert_or_assign`, which this replaced, was O(1)
amortised for **any** order. Measured on this machine, MSVC 19.4 `/O2`, on
`Attachments` alone (`Set` of `k` distinct 8-byte keys, empty `Blob`):

| k | descending | ascending |
|---|---|---|
| 1 000 | 1.2 ms | 0.13 ms |
| 7 000 | **57 ms** | 0.98 ms |
| 20 000 | **447 ms** | 3.0 ms |
| 60 000 | **4 190 ms** | 9.1 ms |
| 200 000 | **58 278 ms** | 36 ms |

Both decode paths are reachable with attacker-chosen ordering:

- **Fast DDS listener thread.** `ParseEnvelopeBody` bounds `att_count` only by
  `(total - pos) / 8` (`fastdds-pubsub-provider/src/internal/envelope_codec.hpp:90`).
  At the default bound `kDefaultPayloadBytes = 64 * 1024`
  (`fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp:190`) one sample can carry
  ~6 500 distinct two-byte keys in descending order → ~50 ms of `memmove` inside
  `on_data_available` / `deserialize()`, per sample, streamable. The reader's history
  overflows behind it, so the observable result is **dropped samples with only an
  `on_sample_lost` warning** — data loss, not a refusal.
- **Gateway WebSocket publish.** `core::DeserializeEnvelope`
  (`core/include/fletcher/core/envelope.hpp:154`) reads `attach_count` and loops with
  **no count bound at all** beyond the buffer length. `gateway/src/ws_session.cpp:279`
  calls it on a client publish frame; nothing in `gateway/` sets `read_message_max`, so
  Beast's 16 MiB default applies → up to ~1.5 M attachments → extrapolating the table,
  the session thread is wedged for tens of minutes on a single frame.

What makes this blocking rather than a nit is the claim attached to it.
`core/include/fletcher/core/types.hpp:161-162` states:

> "Nothing here bounds `k`; the wire checks that read a count off the buffer are what
> bound it (AG2-C2-DEBT-3)."

and `plans/reviews/design-debt.md` records AG2-C2-DEBT-3 as **discharged** on the
strength of that sentence. Read off the code rather than the prose: the Fast DDS check
bounds `k` to `O(total/8)`, which is not a bound on a quadratic term at all, and
`DeserializeEnvelope` — the path the gateway and the XRCE provider both use — has **no
such check to point at**. The debt was discharged against a bound that is ineffective on
one path and absent on the other. This is the same failure mode the design cycles hit
twice: a claim about what a value can hold asserted from prose rather than read off the
code.

**Acceptable fixes** — either alone closes this; (a) is the forbidding direction and is
about four lines:

- **(a) Refuse it at the door.** Give both decoders a stated per-envelope attachment
  ceiling — one named constant in `core/`, used by `ParseEnvelopeBody` and
  `DeserializeEnvelope` alike — so `k` is bounded by a number the format admits rather
  than by the buffer size. `ParseEnvelopeBody` returns `false` beside its other
  malformation checks; `DeserializeEnvelope` throws `std::invalid_argument` beside its
  other wire-fault checks. State the constant in `docs/wire-format-specification.md` and
  correct `types.hpp:161-162`.
- **(b) Make the decode build linearithmic.** Give `Attachments` one bulk entry point
  used only by the decoders, which appends in arrival order and sorts-uniques once
  (O(k log k)), keeping `Set` as the sole public mutator. More surface than (a) and it
  widens the container's API, which A1 exists to keep narrow.

Whichever is chosen, `types.hpp:161-162`'s sentence must be re-derived from the code, and
AG2-C2-DEBT-3 re-opened or re-discharged naming the real bound.

---

## SHOULD-FIX

### S1 — "`Set`'s throw is unreachable from every decode path" is held by three hand-copied `memchr` lines, not by the type

*Confidence: high. Forbidding-direction simplification.*

`Set` throws `PubSubError` on a NUL-bearing key. The invariant that a wire path never
reaches that throw is maintained by repeating an identical guard in three independently
written parsers:

- `core/include/fletcher/core/envelope.hpp:168`
- `fastdds-pubsub-provider/src/internal/envelope_codec.hpp:105`
- `fastdds-pubsub-provider/benchmarks/legacy_fletcher_topic_type.hpp:128`

Nothing in the type stops a fourth decoder — or an edit to any of these three — from
handing wire bytes to `Set`. The consequence is not a wrong answer but a `PubSubError`
unwinding out of `TopicDataType::deserialize()` / `on_data_available`, which §5.3 and
`xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:340` both describe as process
termination rather than an unwind. The diff defends the property with a comment in each
of the three files plus a test that reddens only if *that specific* path regresses; each
comment is correct and each is a copy.

**Acceptable fix:** add a non-throwing door — `[[nodiscard]] bool TrySet(std::string key,
Blob value)` returning `false` for a NUL key — and route the three decoders through it,
deleting the three `memchr` lines. The throwing `Set` then belongs to the caller door
only, and the property becomes structural: a wire path has nothing throwing to call.
`EnvelopeCodecTest.AnArrivingAttachmentKeyWithAZeroByteIsDroppedAsMalformed` and
`SeamVocabulary.AnAttachmentKeyThatWouldTruncateIsRefused` leg 2 continue to assert
exactly what they assert today.

### S2 — `KeyAt(i)` and `ValueAt(i)` are a pair every call site makes, and nothing binds the two indices

*Confidence: medium. Forbidding direction.*

All eight positional call sites in the tree (`envelope.hpp:52`, `:74-75`,
`envelope_codec.hpp:47-48`, `legacy_fletcher_topic_type.hpp:68-69`,
`copy_accounting.cpp:165-166`, `:510-512`, `seam_vocabulary.cpp:571-572`) call both with
the same `i`, and pay two bounds checks to do it. `KeyAt(i)` with `ValueAt(j)`, `i != j`,
is representable, compiles, and produces a silently mispaired attachment — on the two
wire encoders included. A single `EntryAt(i)` returning `{std::string_view key, const
Blob& value}` makes the mismatch unrepresentable, halves the checks, and takes nothing
out of the published form: spec §3.2 A1 names the pair, not the two functions. Filed as
should-fix rather than blocking because no current call site is wrong.

---

## Verified, no finding

Recorded only because these were named as risk areas and each has a concrete answer, so a
later cycle need not re-litigate them.

- **The order rule is read off the type, not off the prose.** `std::string` and
  `std::string_view` ordering both go through `char_traits<char>::compare`, specified to
  compare as `unsigned char`. Probed on this toolchain: keys `01 7F 80 FF` enumerate in
  that order, and `"Z" < "a" < "ab" < "\xC3\xA9"`. A2's "ascending unsigned-byte order,
  `memcmp` order, shorter-is-first on a common prefix" is exactly what the container does.
  The two comparators (`Set` probes with `const std::string&`, `Find` with
  `std::string_view`) induce the same total order.
- **Sort-unique invariant.** `Set` replaces in place when `lower_bound` lands on an equal
  key and inserts at that position otherwise. Probed with re-set, prefix keys, an empty
  key, reverse insertion, a 1024-byte key, and post-move reuse: one entry per key, sorted
  on every path. The temporary `Entry` is built before `insert`, so there is no aliasing
  into the vector's own storage.
- **Bounds.** `RequireInRange` is `i >= entries_.size()` on `size_t` — correct at
  `size()-1`, at `size()`, and on an empty container. No wrap, no clamp, no sentinel.
- **`Find` / `KeyAt` lifetimes.** All sixteen `Find` and eight `KeyAt` call sites consume
  the pointer/view before any `Set` or `Clear` on the same container.
  `copy_accounting.cpp:165-166` reads one container and builds into a different one.
- **`Clear()` on the reuse paths.** `ParseEnvelopeBody` clears as its first act after the
  row bounds check; the loaned listener's reused `attachments`
  (`data_reader_listener.hpp:161`) and `ReceivedData::decoded_attachments` are refilled
  only through it, and every `continue` / `return false` between parse and delivery
  discards the container rather than delivering it. `DataReaderListener::Take` exits its
  loop on any non-`RETCODE_OK`, so a failed `deserialize()` cannot re-deliver the previous
  sample's row. No stale entry survives into a later sample.
- **NUL escape.** `Escape` is a no-op unless a zero byte is present; all-NUL, empty and
  mixed messages all render correctly (`"a\0b\0"` → `a\x00b\x00`, 10 bytes). No overrun —
  `reserve` is a hint and `operator+=` grows. It cannot put a throw into a `noexcept`
  context: `std::runtime_error`'s own construction already allocates. It is idempotent on
  an already-escaped message, which is what
  `Registry.ARefusalIsReconstructibleFromItsNumberAndMessageAlone` leg 1 rests on.
- **§5.1's "only class of message" claim is accurate.** `RequireSegments`
  (`pubsub/include/fletcher/pubsub/internal/segments.hpp:104,111`) is the only place that
  concatenates unvalidated caller bytes into a message ahead of the per-byte NUL check;
  `provider_registry.cpp`'s `Quoted` already renders `\x00` itself, so registry messages
  are unaffected.
- **Concurrency.** `PublishData::attachments` is a borrowed `const Attachments*` owned by
  one `Publish` call; nothing shared changed hands, and positional enumeration reads the
  same memory iterator enumeration did.
- **Wire compatibility.** Duplicate keys on the wire still resolve last-wins exactly as
  `insert_or_assign` did, and `ATTACH_COUNT` may still exceed the resulting entry count
  for a duplicate-bearing producer — unchanged from base. Empty keys are accepted and
  round-trip through both encoders, as A3 says they do.
- **Conformance counts in the README** (`thirteen` seam-vocabulary entries, `22` registry
  entries) match the tree.

---

## Nits

- `integration-tests/pubsub-conformance/src/copy_accounting.cpp:571` dereferences
  `Find("owned")` with no null check — a crash rather than a test failure if it ever
  returns null.
- `Escape`'s "allocates nothing extra" is true only because `return what;` on a by-value
  parameter is an implicit move; a later change to `const std::string&` would silently
  make it a copy.
- `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp:576` inserts from
  `value.data()` for an empty `Blob`, i.e. `[nullptr, nullptr)` — a valid empty range, but
  it reads as a null deref.
- `FletcherSamplePubSubTypeTest.ASampleWithAZeroByteAttachmentKeyIsRejected` finds the key
  by scanning the whole payload for `"axb"`; a row body containing those three bytes would
  patch the wrong place. `Row(16)` today, so not reachable.

---

## RECORD (paperwork the tree contradicts — non-blocking, PM corrects in place)

- Three **source** comments still assert `Attachments` is an `unordered_map` and reason
  from its sentinel-node allocation:
  `fastdds-pubsub-provider/src/internal/transport_data.hpp:53-55` (the stated reason
  `PublishData` and `ReceivedData` are separate structs),
  `fastdds-pubsub-provider/src/internal/data_reader_listener.hpp:160` ("a fresh empty
  unordered_map costs 51 ns on MSVC" — this diff's own README now measures 0.616 ns), and
  `fastdds-pubsub-provider/benchmarks/bench_pub_sub_type.cpp:239`. AG2-C2-DEBT-2 is
  recorded discharged with "all three published statements edited"; those were the
  README/doc statements, and these are a different three.
- `core/include/fletcher/core/types.hpp:193-196` — `Clear()`'s "without this they would
  pay an allocation per sample instead" is inherited from the `unordered_map` rationale.
  A default-constructed vector-backed `Attachments` allocates nothing; what `Clear()` now
  saves is re-growth of the entries vector.
- `core/include/fletcher/core/types.hpp:161-162` — "the wire checks that read a count off
  the buffer are what bound it" (see B1; this sentence is the recorded discharge of
  AG2-C2-DEBT-3 and the code does not support it for `DeserializeEnvelope`).
