// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_RECORDBATCH_SPANS_HPP_
#define FLETCHER_INCLUDE_RECORDBATCH_SPANS_HPP_

#include <arrow/api.h>

#include <cstdint>
#include <optional>
#include <string_view>

// ---------------------------------------------------------------------------
// RecordBatch accessor span helpers (RBA-4).
//
// These are the small, borrowed window types the generated <Class>Accessor
// getters return for collection fields. They are deliberately independent of
// arrow_row_view.hpp: the older ArrowScalarList / ArrowRowViewList helpers are
// per-row Scalar based and use GetScalar() on some collection paths, whereas the
// RBA-4 accessor model casts every column once in Make() and indexes the cached
// typed arrays directly. No span here ever allocates or calls GetScalar().
//
// BORROWING / LIFETIME CONTRACT (load-bearing — read before using):
//
//   * A span stores RAW pointers to the typed Arrow arrays and/or nested
//     accessors that are OWNED (by value / shared_ptr) by the parent
//     <Class>Accessor that produced it.
//   * A span (and any RowView it yields) is valid ONLY while that specific
//     parent accessor instance is alive. A span MUST NOT outlive its accessor.
//   * Accessor copies are fine: the cached Arrow handles and inner accessors are
//     value members / shared owners, so a copied accessor co-owns the same
//     buffers. A span produced by one accessor instance remains tied to THAT
//     instance, not to copies of it.
//   * `base` is the absolute (flattened, offset-0-origin) index of element 0 of
//     this span inside the cached values array; `len` is the element count.
//     operator[]/is_null index at `base + i` into the same cached array, which
//     is coordinate-consistent with the outer ListArray's value_offset(row).
//
// RBA-4a defines ScalarSpan and StructSpan (repeated scalar / repeated struct).
// The map and nested-list span types are added in RBA-4b.
// ---------------------------------------------------------------------------

namespace fletcher {

// Borrowed window over a repeated-scalar (Arrow list<T>) element range.
//
// Template parameters:
//   V       — the C++ value type the getter returns (e.g. double, int32_t,
//             std::string_view for utf8/binary).
//   ArrayT  — the concrete Arrow array type for the values child
//             (e.g. arrow::DoubleArray, arrow::StringArray).
//   UseGetView — true for utf8/binary (zero-copy GetView), false for
//             numeric/bool/temporal (Value). The generated accessor selects
//             this to match the leaf type; callers never set it.
//
// Scalar element nulls are NOT collapsed into std::optional — callers probe
// is_null(j) explicitly (per the design's "no read-through-null" for the
// element value while keeping the list dense).
template <class V, class ArrayT, bool UseGetView = false>
struct ScalarSpan {
    const ArrayT* values = nullptr;
    int64_t base = 0;
    int64_t len = 0;

    int64_t size() const { return len; }
    bool empty() const { return len == 0; }
    bool is_null(int64_t i) const { return values->IsNull(base + i); }

    V operator[](int64_t i) const {
        if constexpr (UseGetView) {
            return values->GetView(base + i);
        } else {
            return values->Value(base + i);
        }
    }
};

// Borrowed window over a repeated-struct (Arrow list<struct<...>>) element
// range. `values` points at the nested <Inner>Accessor owned by the parent
// accessor; that inner accessor was built over the list's FLATTENED values
// StructArray (offset-0 origin), so element j of row `row` lives at the absolute
// index base + j, where base == outer_list->value_offset(row).
//
// A null struct element yields std::nullopt (element-level B2 no-read-through-
// null), proven via the inner accessor's is_null() against the retained struct
// validity bitmap.
template <class AccT>
struct StructSpan {
    const AccT* values = nullptr;
    int64_t base = 0;
    int64_t len = 0;

    int64_t size() const { return len; }
    bool empty() const { return len == 0; }
    bool is_null(int64_t i) const { return values->is_null(base + i); }

    std::optional<typename AccT::RowView> operator[](int64_t i) const {
        const int64_t r = base + i;
        if (values->is_null(r)) return std::nullopt;
        return values->at(r);
    }
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_RECORDBATCH_SPANS_HPP_
