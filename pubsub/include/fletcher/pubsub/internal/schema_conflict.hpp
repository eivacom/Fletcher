// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: the ONE comparison that decides whether re-declaring a topic is
// idempotent or a conflict. Extracted from Publisher::CreateTopic, which had it
// inline, so that the provider-level check the delivery contract now requires
// (spec §7 clause 3, "must be rejected") is the same comparison rather than a
// second one that could drift from it.

#ifndef FLETCHER_INCLUDE_PUBSUB_INTERNAL_SCHEMA_CONFLICT_HPP_
#define FLETCHER_INCLUDE_PUBSUB_INTERNAL_SCHEMA_CONFLICT_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#include "fletcher/pubsub/schema_ipc.hpp"

namespace fletcher {
namespace internal {

/// A topic's declared schema in the only form a conflict check ever needs it:
/// Arrow IPC bytes. Keeping the bytes rather than an OwnedSchema saves a deep
/// copy per topic and one IPC encode per re-declaration.
struct DeclaredSchema {
    /// Empty for a declaration that carried no schema at all, which is a real
    /// value: re-declaring a schema-bearing topic without one stays a conflict.
    std::vector<uint8_t> schema_ipc;

    /// False only when a schema was supplied and could not be IPC-encoded
    /// (nanoarrow's writer rejects dictionary types, for one). A conflict cannot
    /// be proven either way against bytes that could not be produced, so such
    /// topics accept any re-declaration.
    bool encodable = true;

    /// Encode `schema` (may be null / empty, which yields empty bytes). Never
    /// throws: an unencodable schema becomes `encodable == false`.
    static DeclaredSchema Encode(const ArrowSchema* schema) {
        DeclaredSchema out;
        if (schema == nullptr || schema->release == nullptr) {
            return out;
        }
        try {
            out.schema_ipc = SerializeSchemaIpc(schema);
        } catch (const std::exception&) {
            out.encodable = false;
        }
        return out;
    }

    /// True when declaring *this over an already-declared `existing` is a
    /// genuine conflict — provable different shapes. Symmetric.
    [[nodiscard]] bool ConflictsWith(const DeclaredSchema& existing) const {
        return encodable && existing.encodable && schema_ipc != existing.schema_ipc;
    }
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_INTERNAL_SCHEMA_CONFLICT_HPP_
