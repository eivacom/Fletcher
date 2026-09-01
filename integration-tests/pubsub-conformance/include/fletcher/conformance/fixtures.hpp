// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Everything a clause body needs besides the subject, and everything a PEER
// child needs: the two schemas, the opaque row, the delivery collector and the
// budgets. Deliberately gtest-free — the peer binaries include this and must not
// link a test framework. The gtest fixture itself lives in suite.hpp.
//
// Rows are 8 opaque bytes (magic + seq). No codec, no generated type, no Arrow
// C++: locked decision 13 says the wire format does not change, so the suite is
// built unable to see payload layout and no divergence it forces can be a
// wire-format change.

#ifndef FLETCHER_CONFORMANCE_FIXTURES_HPP_
#define FLETCHER_CONFORMANCE_FIXTURES_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

/// Whole-clause budget for every asynchronous arrival. One deadline per clause,
/// shared by every wait in it, so a clause cannot pass by spending N budgets.
inline constexpr std::chrono::seconds kClauseBudget{20};

/// Budget for a wait that is EXPECTED to time out (clause 11: nothing may
/// arrive after Unsubscribe returned). Separate because it is a cost, not a
/// ceiling — every negative wait pays it in full.
inline constexpr std::chrono::milliseconds kSettleBudget{1500};

/// Ceiling on how long Subscribe itself may take (clause 10). Generous: it
/// covers creating DDS entities, and the property under test is that Subscribe
/// does not wait for a publisher to exist — which would be unbounded.
inline constexpr std::chrono::seconds kSubscribeCeiling{5};

// ── Schemas ─────────────────────────────────────────────────────────
/// `kA` = struct<seq:int32>, `kB` = struct<seq:int32,extra:float64>,
/// `kNone` = an empty (unset) schema. A and B are provably different shapes, so
/// a re-declaration from one to the other is a conflict no comparison excuses.
OwnedSchema MakeConformanceSchema(SchemaId id);

// ── The opaque row ──────────────────────────────────────────────────
inline constexpr uint32_t kRowMagic = 0x464C4331u;  // "FLC1"
inline constexpr size_t kRowBytes = 8;

/// Writes magic + seq straight into the provider-supplied buffer. No
/// intermediate buffer, so the suite itself introduces no copy on the row path.
inline void EncodeRow(WriteBuffer& buf, uint32_t seq) {
    buf.AppendFixed<uint32_t>(kRowMagic);
    buf.AppendFixed<uint32_t>(seq);
}

/// The seq carried by `data`, or nullopt when the bytes are not one of our rows.
std::optional<uint32_t> DecodeRow(const uint8_t* data, size_t len);

/// A topic no other clause, subject or process uses.
Topic FreshTopic(const std::string& clause);

// ── Collecting deliveries ───────────────────────────────────────────
/// Records what a subscription actually saw, and nothing else: the seq, whether
/// the schema was non-null, and how many callbacks were ever in flight at once.
///
/// The in-flight counter is an atomic bumped BEFORE the record mutex is taken,
/// so the collector's own locking cannot mask an overlap it is there to detect.
class Collector {
   public:
    struct Delivery {
        uint32_t seq;
        bool had_schema;
    };

    /// A callback bound to this collector. The collector must outlive the
    /// subscription.
    SubscribeCallback Callback();

    /// Widen the window a delivery occupies, so a genuinely concurrent second
    /// delivery has something to overlap with. A busy wait, not a sleep: a
    /// sleeping callback would be a provider-thread stall, which is not what is
    /// under test.
    void SetHoldWindow(std::chrono::microseconds hold) { hold_ = hold; }

    std::vector<Delivery> Snapshot() const;
    size_t Count() const;
    /// Deliveries whose payload was not one of the harness's rows. Asserted to
    /// be 0, not assumed, so a schema sample leaking onto the data channel
    /// cannot be mistaken for a row.
    size_t Foreign() const { return foreign_.load(); }
    size_t MaxInFlight() const { return max_in_flight_.load(); }

    /// Bounded predicate waits. Return false on deadline.
    bool WaitForCount(size_t n, std::chrono::steady_clock::time_point deadline) const;
    bool WaitForSeq(uint32_t seq, std::chrono::steady_clock::time_point deadline) const;

    /// The seqs in arrival order.
    std::vector<uint32_t> Seqs() const;

   private:
    void Record(const uint8_t* data, size_t len, bool had_schema);

    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::vector<Delivery> deliveries_;
    std::atomic<size_t> in_flight_{0};
    std::atomic<size_t> max_in_flight_{0};
    std::atomic<size_t> foreign_{0};
    std::chrono::microseconds hold_{0};
};

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_FIXTURES_HPP_
