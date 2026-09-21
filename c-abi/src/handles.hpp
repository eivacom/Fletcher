// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// What the ABI's opaque handles actually are (BIND-2c).
//
// `binding.h` declares each of these as an incomplete struct tag and nothing
// else, which is the whole point: a binding holds a pointer it cannot read, so
// the layout here is free to change within a major version without touching a
// line of C#. They are defined in the GLOBAL namespace because the C header's
// `typedef struct fl_codec fl_codec;` names `::fl_codec` and a definition inside
// `fletcher::abi` would be a different, unrelated type.
//
// Ownership, once, so the entry points do not each restate it:
//   * `fl_provider` owns a `shared_ptr` to the seam's provider. The seam's
//     lifetime rule (§6) is that a provider outlives everything built on it, so
//     `fl_publisher` holds a copy of that pointer rather than a raw reference —
//     a binding that destroyed its provider first would otherwise take the
//     publisher's transport out from under it, and "undefined" in the header is
//     a statement about the CALLER's obligation, not a licence to crash.
//   * `fl_rows` borrows its codec and its array, and owns neither. `BoundRows`
//     already refuses to consume the array; the raw codec pointer here is the
//     same borrow one level up, and `fl_rows_unbind` releases only the view.
#ifndef FLETCHER_C_ABI_SRC_HANDLES_HPP_
#define FLETCHER_C_ABI_SRC_HANDLES_HPP_

#include <atomic>
#include <fletcher/core/types.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nanoarrow_codec.hpp"

namespace fletcher::abi {

/// The C form of shared ownership, and it is NOT a `shared_ptr` in disguise.
///
/// `binding.h` says `owner` is opaque, not layout-compatible with the C++
/// `shared_ptr` it is built from, and that only retain/release may be applied to
/// it. That rules out the obvious implementation — a heap-allocated `shared_ptr`
/// — for a reason worth stating: `fl_blob_retain` receives a `const fl_blob*`
/// and must not change `owner`, so the reference count has to be reachable
/// FROM the pointer the caller already holds. A control block with its own
/// atomic count is the shape that satisfies that.
///
/// `keep` is whatever keeps the bytes alive: a vector Fletcher allocated, a
/// transport's loan token, an arena. The count is atomic because the header
/// promises retain and release are safe from any thread, concurrently.
struct BlobOwner {
    std::shared_ptr<const void> keep;
    std::atomic<long> refs{1};
};

}  // namespace fletcher::abi

/// Shared, not owned outright, and the reason is a lifetime the header does not
/// state: nothing in `binding.h` says `fl_codec_close` may not precede
/// `fl_rows_unbind`, and a binding whose handles are collected by a GC has no
/// control over the order its finalisers run in. A raw back-pointer would make
/// that ordinary mistake a use-after-free with no symptom until the field plan
/// happened to be reused. One `shared_ptr` makes the order not matter.
struct fl_codec {
    explicit fl_codec(const ArrowSchema& schema)
        : codec(std::make_shared<fletcher::abi::NanoarrowCodec>(schema)) {}
    std::shared_ptr<fletcher::abi::NanoarrowCodec> codec;
};

struct fl_rows {
    fl_rows(std::shared_ptr<fletcher::abi::NanoarrowCodec> c, const ArrowArray& array)
        : codec(std::move(c)), rows(std::make_unique<fletcher::abi::BoundRows>(*codec, array)) {}

    std::shared_ptr<fletcher::abi::NanoarrowCodec> codec;
    std::unique_ptr<fletcher::abi::BoundRows> rows;
};

struct fl_provider {
    explicit fl_provider(std::shared_ptr<fletcher::PubSubProvider> p) : provider(std::move(p)) {}
    std::shared_ptr<fletcher::PubSubProvider> provider;
};

/// A sealed attachments set, plus one control block per entry.
///
/// The control blocks are built ONCE, at seal time, and not per accessor call.
/// `binding.h` says a blob from `fl_attachments_value_at` is BORROWED from the
/// set and valid for as long as the set is; a block minted per call would either
/// leak (nobody releases a borrow) or dangle (freed while the caller still reads
/// it). Building them with the set gives the borrow exactly the lifetime the
/// header promises, and a caller that wants longer calls `fl_blob_retain`, which
/// is why the set holds ONE reference of its own and drops it on dispose rather
/// than deleting the blocks outright.
struct fl_attachments {
    explicit fl_attachments(fletcher::Attachments a) : set(std::move(a)) {
        owners.reserve(set.size());
        for (size_t i = 0; i < set.size(); ++i) {
            // The entry's own owner keeps the bytes; this block keeps the entry's
            // owner. One indirection, and it is what lets a retained blob outlive
            // the set it came from.
            // A COPY OF THE BLOB, not its owner: `Blob` keeps `owner_` private
            // and exposes no accessor, and copying one copies the `shared_ptr`
            // inside it — so holding the copy keeps the bytes alive exactly as
            // holding the owner would, without core growing an accessor that
            // exists only for this boundary.
            auto* block = new fletcher::abi::BlobOwner();
            block->keep = std::make_shared<const fletcher::Blob>(set.ValueAt(i));
            owners.push_back(block);
        }
    }

    ~fl_attachments() {
        for (auto* block : owners) {
            if (block->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete block;
        }
    }

    fl_attachments(const fl_attachments&) = delete;
    fl_attachments& operator=(const fl_attachments&) = delete;

    fletcher::Attachments set;
    std::vector<fletcher::abi::BlobOwner*> owners;
};

/// The write end. The seam's `Attachments::Set` already inserts in key order, so
/// "the builder sorts on build" is satisfied by construction rather than by a
/// sort at the end.
struct fl_attachments_builder {
    fletcher::Attachments pending;
};

struct fl_publisher {
    explicit fl_publisher(std::shared_ptr<fletcher::PubSubProvider> p)
        : provider(std::move(p)), publisher(std::make_unique<fletcher::Publisher>(provider)) {}

    /// Held so the provider cannot die before the publisher built on it, whatever
    /// order the caller destroys its handles in.
    std::shared_ptr<fletcher::PubSubProvider> provider;
    std::unique_ptr<fletcher::Publisher> publisher;
};

struct fl_string_list {
    explicit fl_string_list(std::vector<std::string> v) : items(std::move(v)) {}
    std::vector<std::string> items;
};

#endif  // FLETCHER_C_ABI_SRC_HANDLES_HPP_
