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

    /// Create a deep copy of src.
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
