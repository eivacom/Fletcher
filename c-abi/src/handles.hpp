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

#include <fletcher/core/types.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nanoarrow_codec.hpp"

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

/// Defined here in 2c so `fl_publisher_publish_row`'s signature is honoured
/// rather than faked, but NOTHING CAN CONSTRUCT ONE until BIND-4 lands the
/// builder (D-BIND-31): every entry point that takes one accepts NULL, meaning
/// none, and no exported function returns one yet.
struct fl_attachments {
    fletcher::Attachments value;
};

#endif  // FLETCHER_C_ABI_SRC_HANDLES_HPP_
