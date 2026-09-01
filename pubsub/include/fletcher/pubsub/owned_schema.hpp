// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_OWNED_SCHEMA_HPP_
#define FLETCHER_INCLUDE_PUBSUB_OWNED_SCHEMA_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace fletcher {

/// RAII wrapper for ArrowSchema (calls ArrowSchemaRelease on destruction).
class OwnedSchema {
   public:
    OwnedSchema() noexcept { std::memset(&schema_, 0, sizeof(schema_)); }

    ~OwnedSchema() {
        if (schema_.release) schema_.release(&schema_);
    }

    OwnedSchema(OwnedSchema&& other) noexcept : schema_(other.schema_) {
        std::memset(&other.schema_, 0, sizeof(other.schema_));
    }

    OwnedSchema& operator=(OwnedSchema&& other) noexcept {
        if (this != &other) {
            if (schema_.release) schema_.release(&schema_);
            schema_ = other.schema_;
            std::memset(&other.schema_, 0, sizeof(other.schema_));
        }
        return *this;
    }

    OwnedSchema(const OwnedSchema&) = delete;
    OwnedSchema& operator=(const OwnedSchema&) = delete;

    /// Access the raw ArrowSchema pointer.
    ArrowSchema* get() noexcept { return &schema_; }
    const ArrowSchema* get() const noexcept { return &schema_; }

    ArrowSchema* operator->() noexcept { return &schema_; }
    const ArrowSchema* operator->() const noexcept { return &schema_; }

    /// True if the schema has been initialized (has a release callback).
    bool valid() const noexcept { return schema_.release != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// Create a deep copy of src. Throws if the copy cannot be made — silently returning an empty
    /// schema would let a topic be declared with no schema at all, which the delivery contract
    /// (schema-before-data) relies on never happening.
    [[nodiscard]] static OwnedSchema DeepCopy(const ArrowSchema* src) {
        OwnedSchema copy;
        ArrowErrorCode code = ArrowSchemaDeepCopy(src, copy.get());
        if (code != NANOARROW_OK) {
            throw std::runtime_error(
                "OwnedSchema::DeepCopy: ArrowSchemaDeepCopy failed with code " +
                std::to_string(code));
        }
        return copy;
    }

   private:
    ArrowSchema schema_;
};

/// Shared, immutable handle to an ArrowSchema.
///
/// The shared_ptr keeps an OwnedSchema alive internally and exposes
/// a const pointer to its ArrowSchema.  Safe to pass into callbacks
/// and store across threads — the schema lives as long as any copy
/// of the shared_ptr exists.
///
/// ── The normative rule (spec §3.3, which imports §3.2's clauses) ────────────
///
/// `ArrowSchema` is already the Arrow C Data Interface, so schema *content*
/// crosses a C boundary for free and no Fletcher schema format is invented. What
/// does not cross for free is the OWNERSHIP: the C Data Interface's `release` is
/// **unique** ownership, while this handle is **shared** and is documented as
/// storable by a callback across threads. So a SharedSchema crossing the seam is
/// an owner-handle pair `{owner, const ArrowSchema*}` obeying §3.2's five
/// clauses — retain/release safe from any thread, release never throwing or
/// re-entering the seam, contents immutable once they cross, an argument
/// borrowed for the call and a callee that keeps it taking its own reference.
///
/// **A boundary releases the OWNER HANDLE. It must never call the Arrow C Data
/// Interface `release` on a shared schema** — that destroys the schema under
/// every other holder, including holders in other languages. This is the one
/// memory-unsafe reading available here, so it is written down rather than
/// implied. `arrow::ImportSchema` consumes what it is given, which is why
/// `fletcher::ImportArrowSchema` (pubsub-arrow) deep-copies first and why it is
/// public: so no caller writes the unsafe conversion.
///
/// No shape change is owed: MakeSharedSchema below already returns the aliasing
/// `SharedSchema(owner, owner->get())` — an owner plus a pointer — so it can
/// already name a schema Fletcher did not allocate. §3.3 owed the written rule,
/// not a reshape.
///
/// `CreateTopic` transfers ownership IN (by-value OwnedSchema); delivery to a
/// subscriber callback BORROWS.
using SharedSchema = std::shared_ptr<const ArrowSchema>;

/// Creates a SharedSchema from an OwnedSchema (move semantics).
/// Returns nullptr if the source schema is empty.
inline SharedSchema MakeSharedSchema(OwnedSchema schema) {
    if (!schema) return nullptr;
    auto owner = std::make_shared<OwnedSchema>(std::move(schema));
    return SharedSchema(owner, owner->get());
}

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_OWNED_SCHEMA_HPP_
