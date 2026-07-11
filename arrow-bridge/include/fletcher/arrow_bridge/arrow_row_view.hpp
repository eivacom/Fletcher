// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_ARROW_ROW_VIEW_HPP_
#define FLETCHER_INCLUDE_ARROW_ROW_VIEW_HPP_

#include <arrow/api.h>

#include <cstdint>
#include <memory>

namespace fletcher {

// ---------------------------------------------------------------------------
// ArrowScalarList — zero-copy accessor for repeated scalar fields.
//
// Wraps the typed Arrow array inside a ListScalar and provides element access
// via the array's Value(i) method.  No data is copied.
//
// Template parameters:
//   ValueT — C++ return type (e.g. int32_t, double, std::string_view)
//   ArrayT — Arrow array type (e.g. arrow::Int32Array, arrow::StringArray)
// ---------------------------------------------------------------------------

template <typename ValueT, typename ArrayT>
class ArrowScalarList {
    std::shared_ptr<arrow::Array> array_;

   public:
    ArrowScalarList() = default;
    explicit ArrowScalarList(std::shared_ptr<arrow::Array> array) : array_(std::move(array)) {}

    int64_t size() const { return array_ ? array_->length() : 0; }
    bool empty() const { return size() == 0; }

    ValueT operator[](int64_t i) const { return static_cast<const ArrayT&>(*array_).Value(i); }

    struct Iterator {
        const ArrowScalarList* list;
        int64_t i;
        ValueT operator*() const { return (*list)[i]; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowRowViewList — accessor for repeated message (struct) fields.
//
// Each element is constructed on demand as a ViewT from the StructScalar
// extracted via GetScalar(i).
//
// Template parameters:
//   ViewT — the ArrowRowView class for the nested message.
//           Must be constructible from std::shared_ptr<arrow::Scalar>.
// ---------------------------------------------------------------------------

template <typename ViewT>
class ArrowRowViewList {
    std::shared_ptr<arrow::Array> array_;

   public:
    ArrowRowViewList() = default;
    explicit ArrowRowViewList(std::shared_ptr<arrow::Array> array) : array_(std::move(array)) {}

    int64_t size() const { return array_ ? array_->length() : 0; }
    bool empty() const { return size() == 0; }

    ViewT operator[](int64_t i) const { return ViewT(array_->GetScalar(i).ValueOrDie()); }

    struct Iterator {
        const ArrowRowViewList* list;
        int64_t i;
        ViewT operator*() const { return (*list)[i]; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowNestedList — accessor for List<List<Struct>> fields (depth 2).
//
// Each outer element returns an ArrowRowViewList<ViewT> for the inner list.
// ---------------------------------------------------------------------------

template <typename ViewT>
class ArrowNestedList {
    std::shared_ptr<arrow::Array> array_;

   public:
    ArrowNestedList() = default;
    explicit ArrowNestedList(std::shared_ptr<arrow::Array> array) : array_(std::move(array)) {}

    int64_t size() const { return array_ ? array_->length() : 0; }
    bool empty() const { return size() == 0; }

    ArrowRowViewList<ViewT> operator[](int64_t i) const {
        auto scalar = array_->GetScalar(i).ValueOrDie();
        const auto& ls = static_cast<const arrow::ListScalar&>(*scalar);
        return ArrowRowViewList<ViewT>(ls.value);
    }

    struct Iterator {
        const ArrowNestedList* list;
        int64_t i;
        ArrowRowViewList<ViewT> operator*() const { return (*list)[i]; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowNestedList2 — accessor for List<List<List<Struct>>> fields (depth 3).
//
// Each outer element returns an ArrowNestedList<ViewT> for the middle list.
// ---------------------------------------------------------------------------

template <typename ViewT>
class ArrowNestedList2 {
    std::shared_ptr<arrow::Array> array_;

   public:
    ArrowNestedList2() = default;
    explicit ArrowNestedList2(std::shared_ptr<arrow::Array> array) : array_(std::move(array)) {}

    int64_t size() const { return array_ ? array_->length() : 0; }
    bool empty() const { return size() == 0; }

    ArrowNestedList<ViewT> operator[](int64_t i) const {
        auto scalar = array_->GetScalar(i).ValueOrDie();
        const auto& ls = static_cast<const arrow::ListScalar&>(*scalar);
        return ArrowNestedList<ViewT>(ls.value);
    }

    struct Iterator {
        const ArrowNestedList2* list;
        int64_t i;
        ArrowNestedList<ViewT> operator*() const { return (*list)[i]; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowNestedScalarList — accessor for List<List<Scalar>> fields (depth 2).
//
// GIR-10: the scalar-leaf counterpart of ArrowNestedList. Each outer element
// returns an ArrowScalarList<ValueT, ArrayT> for the inner scalar list.
//
// Template parameters:
//   ValueT — C++ return type of the leaf scalar (e.g. int32_t, double)
//   ArrayT — Arrow array type of the leaf scalar (e.g. arrow::Int32Array)
// ---------------------------------------------------------------------------

template <typename ValueT, typename ArrayT>
class ArrowNestedScalarList {
    std::shared_ptr<arrow::Array> array_;

   public:
    ArrowNestedScalarList() = default;
    explicit ArrowNestedScalarList(std::shared_ptr<arrow::Array> array) : array_(std::move(array)) {}

    int64_t size() const { return array_ ? array_->length() : 0; }
    bool empty() const { return size() == 0; }

    ArrowScalarList<ValueT, ArrayT> operator[](int64_t i) const {
        auto scalar = array_->GetScalar(i).ValueOrDie();
        const auto& ls = static_cast<const arrow::ListScalar&>(*scalar);
        return ArrowScalarList<ValueT, ArrayT>(ls.value);
    }

    struct Iterator {
        const ArrowNestedScalarList* list;
        int64_t i;
        ArrowScalarList<ValueT, ArrayT> operator*() const { return (*list)[i]; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowNestedScalarList2 — accessor for List<List<List<Scalar>>> fields (depth 3).
//
// GIR-10: each outer element returns an ArrowNestedScalarList<ValueT, ArrayT>
// for the middle list.
// ---------------------------------------------------------------------------

template <typename ValueT, typename ArrayT>
class ArrowNestedScalarList2 {
    std::shared_ptr<arrow::Array> array_;

   public:
    ArrowNestedScalarList2() = default;
    explicit ArrowNestedScalarList2(std::shared_ptr<arrow::Array> array)
        : array_(std::move(array)) {}

    int64_t size() const { return array_ ? array_->length() : 0; }
    bool empty() const { return size() == 0; }

    ArrowNestedScalarList<ValueT, ArrayT> operator[](int64_t i) const {
        auto scalar = array_->GetScalar(i).ValueOrDie();
        const auto& ls = static_cast<const arrow::ListScalar&>(*scalar);
        return ArrowNestedScalarList<ValueT, ArrayT>(ls.value);
    }

    struct Iterator {
        const ArrowNestedScalarList2* list;
        int64_t i;
        ArrowNestedScalarList<ValueT, ArrayT> operator*() const { return (*list)[i]; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowScalarMap — accessor for map<K,V> fields where V is a scalar type.
//
// Template parameters:
//   KV, KA — key value type and Arrow array type
//   VV, VA — value value type and Arrow array type
// ---------------------------------------------------------------------------

template <typename KV, typename KA, typename VV, typename VA>
class ArrowScalarMap {
    std::shared_ptr<arrow::Array> keys_;
    std::shared_ptr<arrow::Array> vals_;

   public:
    ArrowScalarMap() = default;
    explicit ArrowScalarMap(const std::shared_ptr<arrow::Array>& struct_array) {
        if (struct_array) {
            const auto& sa = static_cast<const arrow::StructArray&>(*struct_array);
            keys_ = sa.field(0);
            vals_ = sa.field(1);
        }
    }

    int64_t size() const { return keys_ ? keys_->length() : 0; }
    bool empty() const { return size() == 0; }

    KV key(int64_t i) const { return static_cast<const KA&>(*keys_).Value(i); }
    VV value(int64_t i) const { return static_cast<const VA&>(*vals_).Value(i); }

    struct Entry {
        KV key;
        VV value;
    };
    struct Iterator {
        const ArrowScalarMap* map;
        int64_t i;
        Entry operator*() const { return {map->key(i), map->value(i)}; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

// ---------------------------------------------------------------------------
// ArrowRowViewMap — accessor for map<K,V> fields where V is a message type.
//
// Template parameters:
//   KV, KA — key value type and Arrow array type
//   ViewT  — the ArrowRowView class for the value message
// ---------------------------------------------------------------------------

template <typename KV, typename KA, typename ViewT>
class ArrowRowViewMap {
    std::shared_ptr<arrow::Array> keys_;
    std::shared_ptr<arrow::Array> vals_;

   public:
    ArrowRowViewMap() = default;
    explicit ArrowRowViewMap(const std::shared_ptr<arrow::Array>& struct_array) {
        if (struct_array) {
            const auto& sa = static_cast<const arrow::StructArray&>(*struct_array);
            keys_ = sa.field(0);
            vals_ = sa.field(1);
        }
    }

    int64_t size() const { return keys_ ? keys_->length() : 0; }
    bool empty() const { return size() == 0; }

    KV key(int64_t i) const { return static_cast<const KA&>(*keys_).Value(i); }
    ViewT value(int64_t i) const { return ViewT(vals_->GetScalar(i).ValueOrDie()); }

    struct Entry {
        KV key;
        ViewT value;
    };
    struct Iterator {
        const ArrowRowViewMap* map;
        int64_t i;
        Entry operator*() const { return {map->key(i), map->value(i)}; }
        Iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i != o.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_ARROW_ROW_VIEW_HPP_
