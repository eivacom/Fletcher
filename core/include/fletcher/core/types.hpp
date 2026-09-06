// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_TYPES_HPP_
#define FLETCHER_INCLUDE_CORE_TYPES_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fletcher/core/status.hpp"

namespace fletcher {

// Binary-encoded row data.
using EncodedRow = std::vector<uint8_t>;

class Attachments;
namespace internal {
class AttachmentsWireBuilder;
}  // namespace internal

/// Opaque binary payload crossing the seam: an OWNER plus a SPAN.
///
/// It used to be `shared_ptr<const vector<uint8_t>>`, which could only ever name
/// bytes Fletcher had allocated. That is the single thing standing between the
/// seam and zero-copy receive (spec §3.2, §8): a transport holding a loaned
/// sample could not hand those bytes over without first copying them into a
/// vector. An owner plus a span can — the owner is whatever keeps the bytes
/// alive, and Fletcher never needs to know what it is.
///
/// ── The normative rule (spec §3.2 clauses 1-5), stated here because a C view
///    on either side of the seam must derive the SAME one ────────────────────
///
/// A blob is `{owner, data, size}`.
///
///  1. `owner` keeps `[data, data + size)` alive for as long as any copy of this
///     Blob lives. There is no view-only form: non-null `data` with a null
///     `owner` is refused at construction, so §3.2 clause 1's "a callee that
///     keeps it takes its own reference" is exactly true, and a C boundary
///     implements "keep it" as `retain(owner)`.
///  2. Bytes are **immutable** once they cross. `data()` is a `const uint8_t*`
///     and there is no `resize`.
///  3. The reference operations — the C form's `retain(owner)` / `release(owner)`
///     — must be safe from **any thread, concurrently**.
///  4. `release` must never throw and must never re-enter the seam.
///  5. Empty is a null data pointer and a zero size — **enforced, not merely
///     described**: a zero-size Blob normalises `data` to null however it was
///     built, so a C view may test `size == 0` and `data == NULL`
///     interchangeably. Documenting this without enforcing it would have been a
///     trap, because `std::vector<uint8_t>{}.data()` is not required to be null.
///
/// A blob passed as an argument is **borrowed for the duration of that call**; a
/// callee that wants it afterwards copies the Blob, which retains.
///
/// **The C form is conceptual, never a memory image.** `std::shared_ptr<const
/// void>` is two words and a control block; a C `{void* owner, const uint8_t*
/// data, size_t size}` is not a reinterpretation of it, and no layout
/// compatibility is implied or permitted. A boundary *constructs* a Blob from
/// the three fields. That is also why the two ABI rounds need no shared C header
/// (decision 2): they never exchange a struct with each other, they each wrap
/// this same C++ value from their own side, so their C spellings may differ
/// freely.
///
/// There is deliberately **no conversion from the retired
/// `shared_ptr<const vector<uint8_t>>` alias**. One would leave every call site
/// compiling and the copy in place, unnoticed — a coexistence window in a change
/// whose whole point is that there is none.
class Blob {
   public:
    /// Empty: null data, zero size.
    Blob() noexcept = default;

    /// The ONE general form. `owner` is anything whose lifetime covers the
    /// bytes: a vector, a transport's loan handle, an arena, a driver-side
    /// release token.
    ///
    /// Throws PubSubError(kInvalidArgument) for a blob that cannot honour the
    /// rule above: null `data` with a non-zero `size` (bytes that are not
    /// there), or non-null `data` with a null `owner` (bytes nothing keeps
    /// alive).
    ///
    /// A zero `size` needs no owner and keeps no pointer — there is no byte to
    /// keep alive — so `(null, anything, 0)` is the empty Blob rather than a
    /// refusal, and `data()` comes back null either way (clause 5).
    Blob(std::shared_ptr<const void> owner, const uint8_t* data, size_t size)
        : owner_(std::move(owner)), data_(size == 0 ? nullptr : data), size_(size) {
        if (data == nullptr && size_ != 0) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "Blob: a null data pointer cannot carry " + std::to_string(size_) + " bytes");
        }
        if (data != nullptr && size_ != 0 && owner_ == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "Blob: bytes crossing the seam need an owner that keeps them alive; "
                              "there is no view-only Blob");
        }
    }

    /// Bytes Fletcher allocated. The vector becomes the owner, so this is the
    /// general form with the owner filled in for you.
    explicit Blob(std::vector<uint8_t> bytes) {
        auto owned = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        // Clause 5 again: an empty vector's data() is not required to be null, so
        // it is normalised here rather than leaked into the invariant.
        size_ = owned->size();
        data_ = size_ == 0 ? nullptr : owned->data();
        owner_ = std::move(owned);
    }

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

   private:
    std::shared_ptr<const void> owner_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

/// Key-value sidecar data attached to a message during transit — a SEALED,
/// ORDERED set with exactly one way to walk it.
///
/// It was `std::unordered_map<std::string, Blob>` until PDA-DEC-AG2. That alias
/// published no form: the order entries came out in — and therefore the order
/// they were WRITTEN ONTO THE WIRE by `SerializeEnvelope` and the Fast DDS
/// envelope codec — was `std::hash`'s, a build artefact. Two binaries of the
/// same source could emit different bytes for the same message, and no boundary
/// could reproduce the sequence from the value.
///
/// ── The published form (spec §3.2) ──────────────────────────────────────────
///
/// 1. **The order is part of the value.** Entries are enumerated, and encoded,
///    in **ascending unsigned-byte order of the key bytes** — `memcmp` order,
///    shorter-is-first on a common prefix. UTF-8 is a stated convention for keys,
///    not a guarantee: nothing here validates it, and the order is defined over
///    bytes precisely so that no boundary has to reproduce a collation. (C#'s
///    ordinal string comparison is UTF-16 code-unit order and would differ.)
///    A boundary never sorts — it reads the sequence Fletcher publishes.
/// 2. **One enumeration, and it is positional.** There is deliberately no
///    `begin()`/`end()` and no map API: one mechanism, not two, so a producer
///    cannot walk the set by a route the published form does not fix. The
///    owner's 2026-09-06 ruling authorises moving the wire bytes **for
///    attachment ordering only**; a second walk is how that would quietly widen.
///    (There is exactly one *mutating* door besides `Set` —
///    `internal::AttachmentsWireBuilder`, below, which decoders build through.
///    It produces the same published sequence and adds no way to READ the set.)
/// 3. **One entry per key.** `Set` inserts or replaces; a duplicate key on the
///    wire resolves last-wins.
/// 4. **A key contains no zero byte** — refused by `Set` with
///    `kInvalidArgument`, and refused again on arrival, without an exception, by
///    `internal::AttachmentsWireBuilder::Append` below, which every decoder
///    builds through. A boundary marshalling a NUL-terminated string truncates
///    such a key silently, so `"a\0b"` and `"a"` would become one attachment
///    abroad and one would overwrite the other.
/// 5. **Absent is a value, not a failure.** `Find` returns a pointer; there is
///    no `at()` to throw and no `operator[]` to insert by accident.
///
/// `Blob` is unchanged; its five §3.2 clauses are inherited, not restated.
///
/// COMPLEXITY, stated because a decode path is a caller, and re-derived from
/// the code rather than from the shape it was expected to have (AG2-C2-DEBT-3).
/// `Set` keeps a sorted vector: appending a key that sorts after every key
/// already present is O(1) amortised, and one that sorts before them costs O(k)
/// element moves, so building `k` entries through `Set` alone is O(k²) when the
/// keys arrive in descending order. Measured, MSVC 19.4 /O2: k = 7 000 took
/// 57 ms, k = 60 000 took 4.2 s, k = 200 000 took 58 s.
///
/// **Nothing bounds `k` on a decode path, and neither wire check does either.**
/// `ParseEnvelopeBody` bounds the count by `(total - pos) / 8`, which bounds the
/// *buffer* and not a quadratic term — 64 KiB admits ~6 500 keys, ~50 ms of
/// `memmove` on a Fast DDS listener thread — and `DeserializeEnvelope` reads the
/// count off the wire with no count check at all, on a WebSocket frame capped
/// only by Beast's 16 MiB default. So **the decoders do not use `Set`**: they
/// build through `internal::AttachmentsWireBuilder` below, which appends in
/// arrival order and sorts at most once: O(k log k) whatever order the producer
/// chose, and O(k) with no sort and no binary search when the keys already
/// ascend — which is what every conforming producer emits, so the ordinary path
/// is cheaper than `Set` too, not merely bounded.
/// `EnvelopeTest.ADescendingAttachmentKeyOrderDoesNotMakeDecodeQuadratic` pins
/// that, as a ctest timeout.
///
/// The COUNT is still unbounded, deliberately and unchanged: the `unordered_map`
/// this replaced allocated `k` nodes for the same `k`, so no bound was lost and
/// inventing one here would publish a wire limit nothing has measured — the same
/// reason this item declined a key-length bound. The memory question is
/// registered as debt, not answered here.
class Attachments {
   public:
    /// Empty. `Publish(topic, encoder, {})` keeps working unchanged.
    Attachments() noexcept = default;

    /// Insert `key`, or replace the value already stored under it.
    ///
    /// Throws `PubSubError(kInvalidArgument)` if `key` contains a zero byte
    /// (form rule 4). This is the LOCAL door only: bytes arriving from a wire
    /// are refused by the wire checks before they ever reach here, so no
    /// exception enters a transport callback.
    void Set(std::string key, Blob value) {
        const size_t nul = key.find(static_cast<char>(0));
        if (nul != std::string::npos) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "Attachments: a key may not contain a zero byte (offset " + std::to_string(nul) +
                    "); a boundary marshalling it as a NUL-terminated string would truncate it "
                    "there, and two distinct keys would silently become one");
        }
        const auto at = std::lower_bound(
            entries_.begin(), entries_.end(), key,
            [](const Entry& entry, const std::string& probe) { return entry.key < probe; });
        if (at != entries_.end() && at->key == key) {
            at->value = std::move(value);
            return;
        }
        entries_.insert(at, Entry{std::move(key), std::move(value)});
    }

    /// Empties the set, keeping capacity. The receive paths reuse one
    /// `Attachments` across samples and reset it per sample — through
    /// `internal::AttachmentsWireBuilder`, which is where that call now sits.
    /// What the reuse saves is **re-growing the entries vector**, not the
    /// per-sample allocation the `unordered_map` used to charge — a
    /// default-constructed vector-backed `Attachments` allocates nothing at all
    /// (`fastdds-pubsub-provider/README.md`'s measured reason for the reuse, with
    /// its number now historical).
    void Clear() noexcept { entries_.clear(); }

    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    /// The key at position `i` of the published sequence. Throws
    /// `PubSubError(kInvalidArgument)` for `i >= size()` — no clamping, no
    /// sentinel entry, no index that is valid only sometimes.
    [[nodiscard]] std::string_view KeyAt(size_t i) const {
        RequireInRange(i);
        return entries_[i].key;
    }

    /// The value at position `i` of the published sequence. Same refusal.
    [[nodiscard]] const Blob& ValueAt(size_t i) const {
        RequireInRange(i);
        return entries_[i].value;
    }

    /// The value stored under `key`, or null when there is none. Absent is a
    /// value, not a failure.
    [[nodiscard]] const Blob* Find(std::string_view key) const noexcept {
        const auto at = std::lower_bound(
            entries_.begin(), entries_.end(), key,
            [](const Entry& entry, std::string_view probe) { return entry.key < probe; });
        if (at == entries_.end() || at->key != key) return nullptr;
        return &at->value;
    }

   private:
    friend class internal::AttachmentsWireBuilder;

    struct Entry {
        std::string key;
        Blob value;
    };

    void RequireInRange(size_t i) const {
        if (i >= entries_.size()) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "Attachments: index " + std::to_string(i) + " is outside [0, " +
                                  std::to_string(entries_.size()) + ")");
        }
    }

    // Kept sorted and unique by key: the invariant the published form rests on.
    std::vector<Entry> entries_;
};

namespace internal {

/// The DECODE-ONLY door into `Attachments`, and the only bulk one.
///
/// **Why it exists — a measurement, not a preference.** `Attachments::Set`
/// inserts into a sorted vector, so a producer emitting keys in descending order
/// costs O(k) element moves per key and O(k²) per envelope, where `k` is a count
/// read off the wire (see the COMPLEXITY note on `Attachments`). Building
/// through this class instead appends in arrival order and sorts at most once:
/// O(k log k) whatever order the producer chose, and O(k) with no sort at all
/// when the keys already ascend. The published sequence is identical either way.
/// Measured, MSVC 19.4 /O2, k keys into a reused container, `Set` against this
/// builder: k=4 ascending 62 → 54 ns, k=16 ascending 323 → 212 ns, k=64
/// ascending 1 765 → 776 ns, k=64 descending 7 224 → 4 195 ns, and a decode of
/// 200 000 descending keys 58.7 s → 38 ms. The `std::unordered_map` this
/// container replaced was O(1) amortised for any arrival order, so this restores
/// that parity rather than buying anything new.
///
/// **Why it is a second mutator rather than a bound at the door.** A per-envelope
/// attachment ceiling would publish a wire limit nothing in this tree has
/// measured — the same reason PDA-DEC-AG2 declined a key-length bound — and a new
/// wire refusal needs the owner's word on frozen §3.2. This buys the complexity
/// back with no new policy, no new refusal and no spec text.
///
/// **It is also where the arrival-side NUL refusal lives** (owner ruling
/// 2026-09-06). `Append` REPORTS a zero-byte key rather than throwing one, so
/// `Attachments::Set`'s `PubSubError` is unreachable from any decode path by
/// construction rather than by three hand-copied `memchr` lines — no exception
/// can enter a Fast DDS `deserialize()` or `on_data_available` frame from here.
/// Each decoder still turns the `false` into its own kind of refusal:
/// `ParseEnvelopeBody` returns `false` beside its other malformation checks, and
/// `DeserializeEnvelope` raises `std::invalid_argument`, the wire-fault type its
/// own contract reserves.
///
/// It lives in `internal` and is not seam surface: nothing above the seam, and no
/// language binding, builds an `Attachments` from wire bytes.
///
/// Usage — `Finish()` on the success path, and nothing on any other:
/// ```
/// AttachmentsWireBuilder build(out);          // `out` is cleared here
/// for (...) if (!build.Append(key, blob)) return false;   // malformed: drop it
/// build.Finish();
/// ```
/// An unfinished builder empties the container in its destructor, so a decoder
/// that bails part way can never hand on a half-built one: `Find`'s binary search
/// would silently miss on an unsorted vector, which is one way this could have
/// become a wrong answer instead of a dropped sample. `Append` refuses the
/// other: once `Finish()` has run, a further `Append` returns `false` rather
/// than leaving the container looking sorted when it no longer is.
class AttachmentsWireBuilder {
   public:
    // Through `Clear()`, which is still what empties a reused decode out-param —
    // the builder moved WHERE that call is made, not what makes it.
    explicit AttachmentsWireBuilder(Attachments& out) noexcept : out_(out) { out_.Clear(); }

    ~AttachmentsWireBuilder() {
        // Never sorts here: `Clear()` is noexcept and a destructor is not the
        // place to run a comparison sort. A dropped sample's container is empty,
        // which is valid; an unsorted one would not be.
        if (!finished_) out_.Clear();
    }

    AttachmentsWireBuilder(const AttachmentsWireBuilder&) = delete;
    AttachmentsWireBuilder& operator=(const AttachmentsWireBuilder&) = delete;

    /// Appends one wire entry in arrival order. Returns `false`, and appends
    /// nothing, for a key containing a zero byte (published form rule 4), or if
    /// `Finish()` has already run. It does not throw for either; those are the
    /// only two things it refuses.
    [[nodiscard]] bool Append(std::string key, Blob value) {
        if (finished_) return false;
        if (key.find(static_cast<char>(0)) != std::string::npos) return false;
        auto& entries = out_.entries_;
        // One comparison per key, so that a CONFORMING producer — every Fletcher
        // C++ encoder, which emits ascending order because that is the only
        // order `Attachments` has to offer — pays no sort at all below, and no
        // binary search either. That is what keeps this cheaper than `Set` on
        // the ordinary path as well as on the pathological one.
        if (ascending_ && !entries.empty() && !(entries.back().key < key)) ascending_ = false;
        entries.push_back(Attachments::Entry{std::move(key), std::move(value)});
        return true;
    }

    /// Restores the published form: sorted by key, one entry per key.
    ///
    /// The sort is STABLE and the dedupe keeps the LAST of each equal run, which
    /// is what makes a duplicate key on the wire resolve last-wins exactly as
    /// `Set` — and as `insert_or_assign` before it — resolved it. Pinned by
    /// `EnvelopeTest.ADuplicateAttachmentKeyOnTheWireResolvesLastWins`.
    void Finish() {
        auto& entries = out_.entries_;
        if (ascending_) {  // strictly ascending, therefore already sorted AND unique
            finished_ = true;
            return;
        }
        std::stable_sort(
            entries.begin(), entries.end(),
            [](const Attachments::Entry& a, const Attachments::Entry& b) { return a.key < b.key; });
        auto out = entries.begin();
        for (auto run = entries.begin(); run != entries.end();) {
            auto next = run;
            while (next != entries.end() && next->key == run->key) ++next;
            auto last = std::prev(next);
            if (out != last) *out = std::move(*last);
            ++out;
            run = next;
        }
        entries.erase(out, entries.end());
        finished_ = true;
    }

   private:
    Attachments& out_;
    bool finished_ = false;
    // Strictly ascending so far, which implies sorted and unique.
    bool ascending_ = true;
};

}  // namespace internal

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_TYPES_HPP_
