// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// `WriteBuffer::AppendInPlace` — the one member PDA-DEC-A1 adds, and the whole
// reason spec §8's row property is reachable by a CLIENT rather than only by
// Fletcher's own encoder. Contract: docs/pubsub-interface-spec.md §3.1 clause 6.
//
// These cases link the IN-TREE header (`core/tests` links the INTERFACE target
// `fletcher-core`), so a mutation to write_buffer.hpp reddens them with no
// package step in between. The conformance harness measures the same member
// against the PACKAGED core; the two halves are deliberately not the same
// evidence.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <functional>
#include <stdexcept>
#include <vector>

using namespace fletcher;

namespace {

/// A growable window that RELOCATES on every refill — the same shape the
/// conformance harness's probe uses, kept here so `core_tests` can pin the
/// refill half of AppendInPlace without the harness.
class RelocatingWriteBuffer : public WriteBuffer {
   public:
    RelocatingWriteBuffer() : WriteBuffer(nullptr, 0) {}

   private:
    static constexpr size_t kStep = 128;

    void AppendSlow(const uint8_t* data, size_t len) override {
        Grow(len);
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendZerosSlow(size_t len) override {
        Grow(len);
        std::memset(data_ + pos_, 0, len);
        pos_ += len;
    }

    void Grow(size_t len) {
        std::vector<uint8_t> next(pos_ + (len > kStep ? len : kStep));
        if (pos_ > 0) std::memcpy(next.data(), buf_.data(), pos_);
        buf_ = std::move(next);
        data_ = buf_.data();
        capacity_ = buf_.size();
    }

    std::vector<uint8_t> buf_;
};

/// A deterministic pattern, so "the bytes moved" and "the bytes are wrong" are
/// distinguishable failures.
uint8_t PatternByte(size_t i) { return static_cast<uint8_t>((i * 31u + 7u) & 0xFFu); }

/// The NUMBER a refusal carries, which is what a language binding will see —
/// asserting on the type alone would let any typed failure pass.
PubSubStatus StatusOf(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const PubSubError& e) {
        return e.status();
    } catch (...) {
        return PubSubStatus::kInternal;
    }
    return PubSubStatus::kOk;
}

}  // namespace

// ── The window's write end exists at all ────────────────────────────
//
// THE point of the member: the producer is handed the buffer's own write
// cursor, not scratch memory that is copied in afterwards. `dst` must be
// literally `Data() + Position()`. An implementation that stages behind the
// caller's back satisfies every other case in this file and fails this one.
TEST(WriteBufferInPlace, WriterReceivesTheWindowCursor) {
    std::vector<uint8_t> storage(256);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    buffer.AppendByte(0xAA);  // a non-zero start, so `dst == Data()` cannot pass by luck

    const uint8_t* cursor = buffer.Data() + buffer.Position();
    const uint8_t* seen = nullptr;
    size_t room_seen = 0;

    buffer.AppendInPlace(16, [&](uint8_t* dst, size_t room) -> size_t {
        seen = dst;
        room_seen = room;
        for (size_t i = 0; i < 16; ++i) dst[i] = PatternByte(i);
        return 16;
    });

    EXPECT_EQ(seen, cursor) << "the writer was handed scratch memory, not the window's write "
                               "cursor — a client composing here still pays a copy";
    EXPECT_EQ(room_seen, storage.size() - 1)
        << "`room` must be the WHOLE remaining window, so a variable-length producer never "
           "needs a second crossing to ask how much is left";
    ASSERT_EQ(buffer.Position(), static_cast<size_t>(17));
    EXPECT_EQ(storage[0], 0xAA);
    for (size_t i = 0; i < 16; ++i) EXPECT_EQ(storage[1 + i], PatternByte(i));
}

// ── The commit is the reported count, not the request ───────────────
TEST(WriteBufferInPlace, OnlyTheReportedBytesAreCommitted) {
    std::vector<uint8_t> storage(256);
    FixedWriteBuffer buffer(storage.data(), storage.size());

    buffer.AppendInPlace(64, [](uint8_t* dst, size_t) -> size_t {
        for (size_t i = 0; i < 10; ++i) dst[i] = PatternByte(i);
        return 10;  // wrote fewer than it asked room for — a variable-length row
    });

    EXPECT_EQ(buffer.Position(), static_cast<size_t>(10))
        << "the commit must be the count the writer REPORTED, not the room it asked for";

    // A second fill continues from there, so the commit is a position and not a
    // one-shot.
    buffer.AppendInPlace(4, [](uint8_t* dst, size_t) -> size_t {
        dst[0] = 0xEE;
        return 1;
    });
    EXPECT_EQ(buffer.Position(), static_cast<size_t>(11));
    EXPECT_EQ(storage[10], 0xEE);
}

// ── Rung 2: the writer's count is foreign code and cannot be trusted ─
TEST(WriteBufferInPlace, OverReportedLengthIsRefused) {
    std::vector<uint8_t> storage(64);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    buffer.AppendByte(0x01);

    const size_t before = buffer.Position();
    const PubSubStatus status = StatusOf(
        [&] { buffer.AppendInPlace(8, [](uint8_t*, size_t room) -> size_t { return room + 1; }); });

    EXPECT_EQ(status, PubSubStatus::kInvalidArgument)
        << "a writer reporting more than it was lent must be refused, typed";
    EXPECT_EQ(buffer.Position(), before) << "nothing may be committed on a refusal";
}

// ── Rung 2: a writer that re-enters invalidates its own lend ────────
//
// The commit is ABSOLUTE (`pos0 + used`), so a re-entrant append that moved
// `data_` or `pos_` would otherwise have its bytes overwritten or orphaned. One
// comparison at return catches every re-entry THROUGH THE INLINE APPEND PATH
// that can invalidate the lend, and costs that path nothing. A nested
// `AppendInPlace` is the one shape that comparison cannot see, so it is refused
// at the door instead — `NestedFillIsRefusedOnAGrowableWindowToo` is where that
// is measured, on the buffers where it is reachable.
TEST(WriteBufferInPlace, ReEntryFromTheWriterIsRefused) {
    std::vector<uint8_t> storage(256);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    buffer.AppendByte(0x01);
    const size_t before = buffer.Position();

    EXPECT_EQ(StatusOf([&] {
                  buffer.AppendInPlace(8, [&](uint8_t* dst, size_t) -> size_t {
                      dst[0] = 0x02;
                      buffer.AppendByte(0x03);  // moved pos_ under the lend
                      return 1;
                  });
              }),
              PubSubStatus::kInvalidArgument)
        << "an append from inside the writer must be refused";

    // The refusal is that NOTHING THE WRITER PRODUCED is committed. `pos_` is
    // left where the re-entrant append put it — this is not a rollback to a
    // snapshot the call never promised.
    EXPECT_EQ(buffer.Position(), before + 1)
        << "the lend's own bytes must not be committed on top of the re-entry";

    // A nested AppendInPlace is the same class and must also be refused. Note
    // this leg would be green even without the door refusal, because on a fixed
    // buffer the inner commit necessarily moves `pos_`; it pins the REFUSAL, not
    // the mechanism behind it.
    std::vector<uint8_t> other(256);
    FixedWriteBuffer nested(other.data(), other.size());
    EXPECT_EQ(StatusOf([&] {
                  nested.AppendInPlace(8, [&](uint8_t*, size_t) -> size_t {
                      nested.AppendInPlace(4, [](uint8_t* inner, size_t) -> size_t {
                          inner[0] = 0x09;
                          return 1;
                      });
                      return 1;
                  });
              }),
              PubSubStatus::kInvalidArgument)
        << "a NESTED in-place fill is a re-entry too";
}

// ── The nested fill a RETURN comparison structurally cannot see ─────
//
// The case above is green on a `FixedWriteBuffer` for the wrong reason: there the
// inner call cannot refill, so its commit necessarily moves `pos_` and the return
// comparison catches it. On a GROWABLE buffer with spare storage the inner fill
// can refill without relocating and commit 0, leaving `data_`, `pos_` and
// `capacity_` all exactly as the outer lend left them — while having scribbled
// over the outer writer's span. Nothing at return can distinguish that from a
// well-behaved writer, which is why nesting is refused AT THE DOOR (code review
// S2, reproduced). Reverse the order inside the writer and the inner's bytes
// silently replace the outer's in the committed, published row.
TEST(WriteBufferInPlace, NestedFillIsRefusedOnAGrowableWindowToo) {
    VectorWriteBuffer vector_buffer;
    RelocatingWriteBuffer relocating_buffer;
    WriteBuffer* subjects[] = {&vector_buffer, &relocating_buffer};
    const char* labels[] = {"VectorWriteBuffer", "RelocatingWriteBuffer"};

    for (int i = 0; i < 2; ++i) {
        SCOPED_TRACE(labels[i]);
        WriteBuffer& buffer = *subjects[i];
        buffer.AppendZeros(4);  // a window exists, with storage to spare

        bool inner_ran = false;
        PubSubStatus inner_status = PubSubStatus::kOk;
        const PubSubStatus outer_status = StatusOf([&] {
            buffer.AppendInPlace(8, [&](uint8_t* dst, size_t) -> size_t {
                for (size_t b = 0; b < 8; ++b) dst[b] = PatternByte(b);
                inner_status = StatusOf([&] {
                    buffer.AppendInPlace(4, [&](uint8_t* inner, size_t) -> size_t {
                        inner_ran = true;
                        for (size_t b = 0; b < 4; ++b) inner[b] = 0x77;
                        return 0;  // commits nothing: the return comparison sees nothing
                    });
                });
                return 8;
            });
        });

        EXPECT_EQ(inner_status, PubSubStatus::kInvalidArgument)
            << "the nested fill must be refused before it is lent anything";
        EXPECT_FALSE(inner_ran) << "a refused nested fill must not invoke its writer, so it "
                                   "cannot scribble over the span the outer writer was lent";
        EXPECT_EQ(outer_status, PubSubStatus::kOk)
            << "the outer fill itself is well-formed and must still commit";

        ASSERT_EQ(buffer.Position(), static_cast<size_t>(12));
        for (size_t b = 0; b < 8; ++b) {
            EXPECT_EQ(buffer.Data()[4 + b], PatternByte(b))
                << "byte " << b << " of the published row is not the outer producer's";
        }
    }
}

// ── A length no window could satisfy is refused BEFORE the refill ───
//
// The refill virtual computes `pos + n` on every growable subclass, so a
// `min_bytes` near SIZE_MAX wraps inside it and SHRINKS the window; restoring
// `pos_` afterwards then leaves `pos_ > capacity_`, the post-condition's own
// subtraction underflows into a pass, and the writer is handed a `room` of nearly
// 2^64 over a few real bytes — after which the buffer is invariant-broken and the
// next ordinary `Append` memcpys past the end. §3.1 clause 3's subtract-never-add
// rule, on the one member whose C form takes a length straight from foreign code
// (code review B1, reproduced).
TEST(WriteBufferInPlace, HugeMinBytesRefusesLoudly) {
    VectorWriteBuffer vector_buffer;
    RelocatingWriteBuffer relocating_buffer;
    std::vector<uint8_t> arena(256);
    FixedWriteBuffer fixed_buffer(arena.data(), arena.size());
    WriteBuffer* subjects[] = {&vector_buffer, &relocating_buffer, &fixed_buffer};
    const char* labels[] = {"VectorWriteBuffer", "RelocatingWriteBuffer", "FixedWriteBuffer"};

    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(labels[i]);
        WriteBuffer& buffer = *subjects[i];

        std::vector<uint8_t> prefix(40);
        for (size_t p = 0; p < prefix.size(); ++p) prefix[p] = PatternByte(p);
        buffer.Append(prefix.data(), prefix.size());

        bool ran = false;
        size_t room_seen = 0;
        EXPECT_THROW(
            {
                buffer.AppendInPlace(SIZE_MAX - 20, [&](uint8_t*, size_t room) -> size_t {
                    ran = true;
                    room_seen = room;
                    return 0;
                });
            },
            std::overflow_error);

        EXPECT_FALSE(ran) << "the writer was invoked with room=" << room_seen
                          << " — a span that does not exist";
        ASSERT_EQ(buffer.Position(), prefix.size());

        // The refusal must leave the WINDOW INTACT, not merely throw: this is the
        // half that fails when `pos_ > capacity_` survives the refusal, because
        // every later append then takes its inline path through an underflowed
        // `capacity_ - pos_`.
        std::vector<uint8_t> more(64, 0x5A);
        buffer.Append(more.data(), more.size());
        ASSERT_EQ(buffer.Position(), prefix.size() + more.size());
        for (size_t p = 0; p < prefix.size(); ++p) {
            ASSERT_EQ(buffer.Data()[p], PatternByte(p))
                << "byte " << p << " did not survive a refusal that should have changed nothing";
        }
        for (size_t b = 0; b < more.size(); ++b) {
            ASSERT_EQ(buffer.Data()[prefix.size() + b], 0x5A)
                << "the buffer did not accept an ordinary append after the refusal";
        }
    }
}

// ── Patching BELOW the lend point is permitted, and stays permitted ─
//
// `PatchU32`/`PatchByte` from inside the writer move neither `data_` nor `pos_`
// and are bounded by `offset < pos0`, which is disjoint from the lent span. They
// are the back-patch route a variable-length producer needs, so the re-entry
// check must NOT catch them (design review A1-DEBT-3).
TEST(WriteBufferInPlace, PatchingBelowTheLendPointIsPermitted) {
    std::vector<uint8_t> storage(256);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    const size_t length_at = buffer.WriteLengthPlaceholder();

    // Asks for exactly what it writes ON PURPOSE: the commit arithmetic is
    // `OnlyTheReportedBytesAreCommitted`'s subject, and pinning it here too would
    // make one mutation redden two cases and hide which one measured it.
    buffer.AppendInPlace(12, [&](uint8_t* dst, size_t) -> size_t {
        for (size_t i = 0; i < 12; ++i) dst[i] = PatternByte(i);
        buffer.PatchU32(length_at, 12);  // below pos0 — disjoint from the lend
        return 12;
    });

    EXPECT_EQ(buffer.Position(), static_cast<size_t>(sizeof(uint32_t) + 12));
    uint32_t patched = 0;
    std::memcpy(&patched, storage.data() + length_at, sizeof(patched));
    EXPECT_EQ(patched, 12u) << "back-patching a length prefix from inside the writer must work; "
                               "it is the route a variable-length row needs";
}

// ── A fill of no bytes names nothing ────────────────────────────────
TEST(WriteBufferInPlace, ZeroMinBytesIsRefused) {
    std::vector<uint8_t> storage(64);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    bool ran = false;

    EXPECT_EQ(StatusOf([&] {
                  buffer.AppendInPlace(0, [&](uint8_t*, size_t) -> size_t {
                      ran = true;
                      return 0;
                  });
              }),
              PubSubStatus::kInvalidArgument);
    EXPECT_FALSE(ran) << "a refused call must not invoke the writer at all";
    EXPECT_EQ(buffer.Position(), static_cast<size_t>(0));
}

// ── An exception from the writer commits nothing ────────────────────
//
// Safe by construction — the commit is the last statement — but a binding author
// cannot infer that, so it is written into the header contract and pinned here
// (design review A1-DEBT-2).
TEST(WriteBufferInPlace, AnExceptionFromTheWriterCommitsNothing) {
    std::vector<uint8_t> storage(256);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    buffer.AppendByte(0x01);
    const size_t before = buffer.Position();

    EXPECT_THROW(
        {
            buffer.AppendInPlace(16, [](uint8_t* dst, size_t) -> size_t {
                dst[0] = 0x02;
                throw std::runtime_error("the producer gave up mid-row");
            });
        },
        std::runtime_error)
        << "the writer's own exception must propagate UNCHANGED, not be translated here";

    EXPECT_EQ(buffer.Position(), before)
        << "a writer that threw committed nothing, so the position is untouched";
}

// ── Steps 2 and 3: room is MADE, and the bytes below survive it ─────
TEST(WriteBufferInPlace, GrowableRefillPreservesBytesAndDeliversRoom) {
    VectorWriteBuffer vector_buffer;
    RelocatingWriteBuffer relocating_buffer;
    WriteBuffer* subjects[] = {&vector_buffer, &relocating_buffer};
    const char* labels[] = {"VectorWriteBuffer", "RelocatingWriteBuffer"};

    for (int i = 0; i < 2; ++i) {
        SCOPED_TRACE(labels[i]);
        WriteBuffer& buffer = *subjects[i];

        // Bytes below the lend point, which the refill must preserve verbatim at
        // whatever base it ends up with (§3.1 clause 1).
        std::vector<uint8_t> prefix(40);
        for (size_t p = 0; p < prefix.size(); ++p) prefix[p] = PatternByte(p);
        buffer.Append(prefix.data(), prefix.size());

        // Far more than the window this buffer currently has, so step 2's forced
        // refill is the only way the room can appear.
        constexpr size_t kWanted = 4096;
        size_t room_seen = 0;
        const uint8_t* seen = nullptr;

        buffer.AppendInPlace(kWanted, [&](uint8_t* dst, size_t room) -> size_t {
            seen = dst;
            room_seen = room;
            for (size_t b = 0; b < kWanted; ++b) dst[b] = PatternByte(b + 1000);
            return kWanted;
        });

        EXPECT_GE(room_seen, kWanted) << "the refill did not deliver the room that was asked for";
        EXPECT_NE(seen, nullptr) << "the writer was never invoked";
        ASSERT_EQ(buffer.Position(), prefix.size() + kWanted);

        // Deliberately NOT re-asserting `seen == Data() + Position()` here:
        // `WriterReceivesTheWindowCursor` owns that claim, and pinning it twice
        // makes one mutation redden two cases so neither says which measured it.
        // A lend at a STALE pre-refill base is still caught, by the byte loop
        // below — those bytes would not be in the final window at all.

        for (size_t p = 0; p < prefix.size(); ++p) {
            ASSERT_EQ(buffer.Data()[p], PatternByte(p))
                << "the refill did not preserve byte " << p << " below the lend point verbatim";
        }
        for (size_t b = 0; b < kWanted; ++b) {
            ASSERT_EQ(buffer.Data()[prefix.size() + b], PatternByte(b + 1000))
                << "produced byte " << b << " is not in the window";
        }
    }
}

// ── A fixed buffer with no room refuses LOUDLY (§3.1 clause 4) ──────
TEST(WriteBufferInPlace, FixedBufferWithoutRoomRefusesAsPayloadTooLarge) {
    std::vector<uint8_t> storage(32);
    FixedWriteBuffer buffer(storage.data(), storage.size());
    bool ran = false;

    EXPECT_THROW(
        {
            buffer.AppendInPlace(64, [&](uint8_t*, size_t) -> size_t {
                ran = true;
                return 0;
            });
        },
        std::overflow_error);
    EXPECT_FALSE(ran) << "the writer must not run when the room could not be produced";
    EXPECT_EQ(buffer.Position(), static_cast<size_t>(0));

    // The number a language binding actually sees: §3.1 clause 4's payload bound,
    // translated by the same seam entry point every other overflow goes through.
    EXPECT_EQ(StatusOf([&] {
                  TranslateSeamFailure([&] {
                      buffer.AppendInPlace(64, [](uint8_t*, size_t) -> size_t { return 0; });
                  });
              }),
              PubSubStatus::kPayloadTooLarge);
}
