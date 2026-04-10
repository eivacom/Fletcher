#ifndef FLETCHER_INCLUDE_PUBSUB_OWNED_SCHEMA_HPP_
#define FLETCHER_INCLUDE_PUBSUB_OWNED_SCHEMA_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace fletcher {

/// RAII wrapper for ArrowSchema (calls ArrowSchemaRelease on destruction).
class OwnedSchema {
 public:
    OwnedSchema() noexcept { std::memset(&schema_, 0, sizeof(schema_)); }

    ~OwnedSchema() {
        if (schema_.release)
            schema_.release(&schema_);
    }

    OwnedSchema(OwnedSchema&& other) noexcept : schema_(other.schema_) {
        std::memset(&other.schema_, 0, sizeof(other.schema_));
    }

    OwnedSchema& operator=(OwnedSchema&& other) noexcept {
        if (this != &other) {
            if (schema_.release)
                schema_.release(&schema_);
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
    static OwnedSchema DeepCopy(const ArrowSchema* src) {
        OwnedSchema copy;
        ArrowSchemaDeepCopy(src, copy.get());
        return copy;
    }

 private:
    ArrowSchema schema_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_OWNED_SCHEMA_HPP_
