// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/arrow_bridge/batch_decoder.hpp"

#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "fletcher/arrow_bridge/detail/arrow_result.hpp"
#include "row_reader.hpp"

namespace fletcher {
namespace {

using detail::AllNull;
using detail::AllValid;
using detail::AppendRun;
using detail::BitfieldBytes;
using detail::FixedWidth;
using detail::ReadNullBit;
using detail::ThrowIfNotOk;
using detail::ValueOrThrow;

// One node per schema position, mirroring the builder tree 1:1 so the wire walk below never has to
// re-derive Arrow type information mid-row.
struct Node {
    arrow::Type::type id = arrow::Type::NA;
    std::shared_ptr<arrow::DataType> type;
    arrow::ArrayBuilder* builder = nullptr;  // top level: owned by Impl::columns; nested: owned by
                                             // the parent builder
    int32_t byte_width = 0;  // detail::FixedWidth(*type); > 0 marks a fixed-width scalar
    int32_t list_size = 0;   // FIXED_SIZE_LIST
    std::array<int8_t, 128> code_to_child{};  // unions: type code -> child index, -1 unknown
    std::vector<Node>
        children;         // struct fields | [list value] | [map key, map item] | union children
    int64_t pending = 0;  // Skip pass scratch, see SkipSink
};

// Build the node tree over an existing builder tree. `type`/`builder` already agree by construction
// (BuildNode is only ever called with a type/builder pair that came from the same MakeBuilder
// call), so the trailing Equals guard exists purely to catch a future Arrow builder-tree change,
// not bad input.
void BuildNode(Node& node, const std::shared_ptr<arrow::DataType>& type,
               arrow::ArrayBuilder* builder, const std::string& field_name) {
    switch (type->id()) {
        case arrow::Type::NA:
        case arrow::Type::EXTENSION:
        case arrow::Type::DECIMAL32:
        case arrow::Type::DECIMAL64:
        case arrow::Type::RUN_END_ENCODED:
        case arrow::Type::LIST_VIEW:
        case arrow::Type::LARGE_LIST_VIEW:
        case arrow::Type::DICTIONARY:
            throw std::invalid_argument("BatchDecoder: field '" + field_name +
                                        "' has unsupported type " + type->ToString());
        case arrow::Type::STRUCT: {
            const auto& stype = static_cast<const arrow::StructType&>(*type);
            auto& sb = static_cast<arrow::StructBuilder&>(*builder);
            node.children.resize(static_cast<size_t>(stype.num_fields()));
            for (int i = 0; i < stype.num_fields(); ++i)
                BuildNode(node.children[static_cast<size_t>(i)], stype.field(i)->type(),
                          sb.field_builder(i), stype.field(i)->name());
            break;
        }
        case arrow::Type::LIST: {
            const auto& ltype = static_cast<const arrow::ListType&>(*type);
            auto& lb = static_cast<arrow::ListBuilder&>(*builder);
            node.children.resize(1);
            BuildNode(node.children[0], ltype.value_type(), lb.value_builder(), field_name);
            break;
        }
        case arrow::Type::LARGE_LIST: {
            const auto& ltype = static_cast<const arrow::LargeListType&>(*type);
            auto& lb = static_cast<arrow::LargeListBuilder&>(*builder);
            node.children.resize(1);
            BuildNode(node.children[0], ltype.value_type(), lb.value_builder(), field_name);
            break;
        }
        case arrow::Type::FIXED_SIZE_LIST: {
            const auto& ltype = static_cast<const arrow::FixedSizeListType&>(*type);
            auto& lb = static_cast<arrow::FixedSizeListBuilder&>(*builder);
            node.list_size = ltype.list_size();
            node.children.resize(1);
            BuildNode(node.children[0], ltype.value_type(), lb.value_builder(), field_name);
            break;
        }
        case arrow::Type::MAP: {
            const auto& mtype = static_cast<const arrow::MapType&>(*type);
            auto& mb = static_cast<arrow::MapBuilder&>(*builder);
            node.children.resize(2);
            BuildNode(node.children[0], mtype.key_type(), mb.key_builder(), field_name);
            BuildNode(node.children[1], mtype.item_type(), mb.item_builder(), field_name);
            break;
        }
        case arrow::Type::SPARSE_UNION:
        case arrow::Type::DENSE_UNION: {
            const auto& utype = static_cast<const arrow::UnionType&>(*type);
            node.children.resize(static_cast<size_t>(utype.num_fields()));
            for (int i = 0; i < utype.num_fields(); ++i)
                BuildNode(node.children[static_cast<size_t>(i)], utype.field(i)->type(),
                          builder->child(i), utype.field(i)->name());
            node.code_to_child.fill(-1);
            const auto& ids = utype.child_ids();
            for (size_t c = 0; c < ids.size() && c < node.code_to_child.size(); ++c)
                node.code_to_child[c] = static_cast<int8_t>(ids[c]);
            break;
        }
        default:
            // Scalars, incl. binary/large binary/views: no children, no wire structure beyond the
            // payload consumed in AppendSink::Scalar / SkipSink::Scalar.
            node.byte_width = FixedWidth(*type);
            break;
    }
    node.id = type->id();
    node.type = type;
    node.builder = builder;
    if (!builder->type()->Equals(*type))
        throw std::runtime_error("BatchDecoder: builder type " + builder->type()->ToString() +
                                 " != " + type->ToString());
}

// Types whose Arrow offsets are 32-bit and can therefore overflow: gathered once at construction so
// Append() doesn't have to re-walk the tree looking for them on every row.
void CollectBudgeted(Node& node, std::vector<Node*>& out) {
    switch (node.id) {
        case arrow::Type::STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LIST:
        case arrow::Type::MAP:
        case arrow::Type::DENSE_UNION:
            out.push_back(&node);
            break;
        default:
            break;
    }
    for (Node& child : node.children) CollectBudgeted(child, out);
}

// ---------------------------------------------------------------------------
// The traversal — one template, two sinks (SkipSink validates and computes byte-budget deltas
// without touching a single builder; AppendSink replays the identical walk into the builders and
// cannot fail because Skip already proved the row well-formed and within budget).
// ---------------------------------------------------------------------------

template <class Sink>
void WalkValue(Node& node, detail::Reader& r, Sink& sink);

template <class Sink>
void WalkElements(Node& node, detail::Reader& r, Sink& sink, int64_t count) {
    const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(count));
    Node& elem = node.children[0];
    sink.ListBegin(node, count);
    if (count == 0) return;
    if (elem.byte_width > 0 && AllValid(bitfield, count)) {
        // Divide, never multiply: a wrapped count * width would pass ReadBytes' own check.
        if (static_cast<uint64_t>(count) > r.remaining() / static_cast<size_t>(elem.byte_width))
            throw std::invalid_argument("BatchDecoder: list payload exceeds remaining buffer");
        sink.Run(elem,
                 r.ReadBytes(static_cast<size_t>(count) * static_cast<size_t>(elem.byte_width)),
                 count);
        return;
    }
    if (AllNull(bitfield, count)) {
        sink.Nulls(elem, count);
        return;
    }
    for (int64_t i = 0; i < count; ++i) {
        if (ReadNullBit(bitfield, static_cast<int>(i)))
            sink.Null(elem);
        else
            WalkValue(elem, r, sink);
    }
}

template <class Sink>
void WalkValue(Node& node, detail::Reader& r, Sink& sink) {
    switch (node.id) {
        case arrow::Type::STRUCT: {
            const int n = static_cast<int>(node.children.size());
            const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(n));
            sink.StructBegin(node);
            for (int i = 0; i < n; ++i)
                if (ReadNullBit(bitfield, i))
                    sink.Null(node.children[static_cast<size_t>(i)]);
                else
                    WalkValue(node.children[static_cast<size_t>(i)], r, sink);
            return;
        }
        case arrow::Type::LIST:
        case arrow::Type::LARGE_LIST: {
            const uint32_t count = r.Read<uint32_t>();
            // Elements can be null and carry no payload, so the bitfield is the only safe lower
            // bound (same rule as Codec).
            if (BitfieldBytes(count) > r.remaining())
                throw std::invalid_argument(
                    "BatchDecoder: list element count exceeds remaining buffer");
            WalkElements(node, r, sink, count);
            return;
        }
        case arrow::Type::FIXED_SIZE_LIST:
            WalkElements(node, r, sink, node.list_size);  // no count on the wire
            return;
        case arrow::Type::MAP: {
            const uint32_t count = r.Read<uint32_t>();
            if (count > r.remaining())  // keys are non-null: at least one byte each
                throw std::invalid_argument(
                    "BatchDecoder: map entry count exceeds remaining buffer");
            sink.MapBegin(node, count);
            for (uint32_t i = 0; i < count; ++i) WalkValue(node.children[0], r, sink);
            const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(count));
            for (uint32_t i = 0; i < count; ++i)
                if (ReadNullBit(bitfield, static_cast<int>(i)))
                    sink.Null(node.children[1]);
                else
                    WalkValue(node.children[1], r, sink);
            return;
        }
        case arrow::Type::SPARSE_UNION:
        case arrow::Type::DENSE_UNION: {
            const int8_t code = r.Read<int8_t>();
            const int child = code < 0 ? -1 : node.code_to_child[static_cast<size_t>(code)];
            if (child < 0) throw std::invalid_argument("BatchDecoder: unknown union type_code");
            sink.UnionBegin(node, code, child);
            WalkValue(node.children[static_cast<size_t>(child)], r, sink);
            return;
        }
        default:
            sink.Scalar(node, r);  // the sink consumes the bytes
            return;
    }
}

template <class Sink>
void WalkRow(std::vector<Node>& roots, detail::Reader& r, Sink& sink) {
    const int n = static_cast<int>(roots.size());
    const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(n));
    for (int i = 0; i < n; ++i)
        if (ReadNullBit(bitfield, i))
            sink.Null(roots[static_cast<size_t>(i)]);
        else
            WalkValue(roots[static_cast<size_t>(i)], r, sink);
    sink.RowEnd(r);  // Skip: trailing-bytes check (top level only). Append: no-op.
}

// ---------------------------------------------------------------------------
// SkipSink — validation only, no builder writes, no allocation.
// ---------------------------------------------------------------------------

struct SkipSink {
    void Scalar(Node& node, detail::Reader& r) {
        if (node.byte_width > 0) {
            r.ReadBytes(static_cast<size_t>(node.byte_width));
            return;
        }
        const uint32_t len = r.Read<uint32_t>();
        r.ReadBytes(len);
        if (node.id == arrow::Type::STRING || node.id == arrow::Type::BINARY) {
            node.pending += len;
            const int64_t have =
                static_cast<arrow::BaseBinaryBuilder<arrow::BinaryType>&>(*node.builder)
                    .value_data_length();
            if (have + node.pending > std::numeric_limits<int32_t>::max())
                throw BatchCapacityExceeded(
                    "BatchDecoder: utf8/binary column would exceed 2^31-1 bytes");
        }
    }
    void Run(Node&, const uint8_t*, int64_t) {}
    void Null(Node&) {}
    void Nulls(Node&, int64_t) {}
    void StructBegin(Node&) {}
    void ListBegin(Node& node, int64_t count) {
        if (node.id != arrow::Type::LIST) return;  // LargeList has 64-bit offsets, FSL none
        node.pending += count;
        if (node.children[0].builder->length() + node.pending > std::numeric_limits<int32_t>::max())
            throw BatchCapacityExceeded("BatchDecoder: list column would exceed 2^31-1 elements");
    }
    void MapBegin(Node& node, int64_t count) {
        node.pending += count;
        if (node.children[1].builder->length() + node.pending > std::numeric_limits<int32_t>::max())
            throw BatchCapacityExceeded("BatchDecoder: map column would exceed 2^31-1 entries");
    }
    void UnionBegin(Node& node, int8_t, int child) {
        if (node.id != arrow::Type::DENSE_UNION) return;
        node.pending += 1;  // one value offset into that child
        if (node.children[static_cast<size_t>(child)].builder->length() + node.pending >
            std::numeric_limits<int32_t>::max())
            throw BatchCapacityExceeded(
                "BatchDecoder: dense union child would exceed 2^31-1 elements");
    }
    void RowEnd(const detail::Reader& r) {
        if (r.pos != r.size)
            throw std::invalid_argument("BatchDecoder: buffer not fully consumed (" +
                                        std::to_string(r.remaining()) +
                                        " trailing byte(s)); does not match schema");
    }
};

// ---------------------------------------------------------------------------
// AppendSink — cannot fail on validated input (Skip already proved the row well-formed and within
// every 32-bit budget), so every Status here is an internal invariant, not a bad-input condition.
// ---------------------------------------------------------------------------

template <typename ArrowType>
void AppendFixed(arrow::ArrayBuilder& b, detail::Reader& r) {
    using CType = typename ArrowType::c_type;
    CType v;
    std::memcpy(&v, r.ReadBytes(sizeof(CType)), sizeof(CType));
    ThrowIfNotOk(static_cast<arrow::NumericBuilder<ArrowType>&>(b).Append(v),
                 "BatchDecoder: Append fixed-width");
}

struct AppendSink {
    void Scalar(Node& node, detail::Reader& r) {
        arrow::ArrayBuilder& b = *node.builder;
        switch (node.id) {
            case arrow::Type::BOOL:
                ThrowIfNotOk(static_cast<arrow::BooleanBuilder&>(b).Append(r.Read<uint8_t>() != 0),
                             "BatchDecoder: Append bool");
                return;
            case arrow::Type::INT8:
                AppendFixed<arrow::Int8Type>(b, r);
                return;
            case arrow::Type::INT16:
                AppendFixed<arrow::Int16Type>(b, r);
                return;
            case arrow::Type::INT32:
                AppendFixed<arrow::Int32Type>(b, r);
                return;
            case arrow::Type::INT64:
                AppendFixed<arrow::Int64Type>(b, r);
                return;
            case arrow::Type::UINT8:
                AppendFixed<arrow::UInt8Type>(b, r);
                return;
            case arrow::Type::UINT16:
                AppendFixed<arrow::UInt16Type>(b, r);
                return;
            case arrow::Type::UINT32:
                AppendFixed<arrow::UInt32Type>(b, r);
                return;
            case arrow::Type::UINT64:
                AppendFixed<arrow::UInt64Type>(b, r);
                return;
            case arrow::Type::HALF_FLOAT:
                AppendFixed<arrow::HalfFloatType>(b, r);
                return;
            case arrow::Type::FLOAT:
                AppendFixed<arrow::FloatType>(b, r);
                return;
            case arrow::Type::DOUBLE:
                AppendFixed<arrow::DoubleType>(b, r);
                return;
            case arrow::Type::DATE32:
                AppendFixed<arrow::Date32Type>(b, r);
                return;
            case arrow::Type::DATE64:
                AppendFixed<arrow::Date64Type>(b, r);
                return;
            case arrow::Type::TIME32:
                AppendFixed<arrow::Time32Type>(b, r);
                return;
            case arrow::Type::TIME64:
                AppendFixed<arrow::Time64Type>(b, r);
                return;
            case arrow::Type::TIMESTAMP:
                AppendFixed<arrow::TimestampType>(b, r);
                return;
            case arrow::Type::DURATION:
                AppendFixed<arrow::DurationType>(b, r);
                return;
            case arrow::Type::INTERVAL_MONTHS:
                AppendFixed<arrow::MonthIntervalType>(b, r);
                return;
            case arrow::Type::INTERVAL_DAY_TIME:
                AppendFixed<arrow::DayTimeIntervalType>(b, r);
                return;
            case arrow::Type::INTERVAL_MONTH_DAY_NANO:
                AppendFixed<arrow::MonthDayNanoIntervalType>(b, r);
                return;
            case arrow::Type::DECIMAL128:
            case arrow::Type::DECIMAL256:
            case arrow::Type::FIXED_SIZE_BINARY:
                ThrowIfNotOk(static_cast<arrow::FixedSizeBinaryBuilder&>(b).Append(
                                 r.ReadBytes(static_cast<size_t>(node.byte_width))),
                             "BatchDecoder: Append fixed-size binary");
                return;
            case arrow::Type::STRING:
            case arrow::Type::BINARY: {
                const uint32_t len = r.Read<uint32_t>();
                ThrowIfNotOk(static_cast<arrow::BinaryBuilder&>(b).Append(
                                 r.ReadBytes(len), static_cast<int32_t>(len)),
                             "BatchDecoder: Append binary");
                return;
            }
            case arrow::Type::LARGE_STRING:
            case arrow::Type::LARGE_BINARY: {
                const uint32_t len = r.Read<uint32_t>();
                ThrowIfNotOk(static_cast<arrow::LargeBinaryBuilder&>(b).Append(
                                 r.ReadBytes(len), static_cast<int64_t>(len)),
                             "BatchDecoder: Append large binary");
                return;
            }
            case arrow::Type::STRING_VIEW:
            case arrow::Type::BINARY_VIEW: {
                const uint32_t len = r.Read<uint32_t>();
                ThrowIfNotOk(static_cast<arrow::BinaryViewBuilder&>(b).Append(
                                 r.ReadBytes(len), static_cast<int64_t>(len)),
                             "BatchDecoder: Append binary view");
                return;
            }
            default:
                throw std::runtime_error("BatchDecoder: unreachable scalar type " +
                                         node.type->ToString());  // rejected at construction
        }
    }
    void Run(Node& elem, const uint8_t* bytes, int64_t count) {
        if (!AppendRun(*elem.builder, elem.id, bytes, count))
            throw std::runtime_error("BatchDecoder: no run path for " +
                                     elem.type->ToString());  // byte_width > 0 implies one exists
    }
    void Null(Node& node) { ThrowIfNotOk(node.builder->AppendNull(), "BatchDecoder: AppendNull"); }
    void Nulls(Node& elem, int64_t n) {
        ThrowIfNotOk(elem.builder->AppendNulls(n), "BatchDecoder: AppendNulls");
    }
    void StructBegin(Node& node) {
        ThrowIfNotOk(static_cast<arrow::StructBuilder&>(*node.builder).Append(true),
                     "BatchDecoder: StructBuilder::Append");
    }
    void ListBegin(Node& node, int64_t) {
        switch (node.id) {
            case arrow::Type::LIST:
                ThrowIfNotOk(static_cast<arrow::ListBuilder&>(*node.builder).Append(true),
                             "BatchDecoder: ListBuilder::Append");
                return;
            case arrow::Type::LARGE_LIST:
                ThrowIfNotOk(static_cast<arrow::LargeListBuilder&>(*node.builder).Append(true),
                             "BatchDecoder: LargeListBuilder::Append");
                return;
            case arrow::Type::FIXED_SIZE_LIST:
                ThrowIfNotOk(static_cast<arrow::FixedSizeListBuilder&>(*node.builder).Append(),
                             "BatchDecoder: FixedSizeListBuilder::Append");
                return;
            default:
                throw std::runtime_error("BatchDecoder: unreachable list type");
        }
    }
    void MapBegin(Node& node, int64_t) {
        ThrowIfNotOk(static_cast<arrow::MapBuilder&>(*node.builder).Append(),
                     "BatchDecoder: MapBuilder::Append");
    }
    void UnionBegin(Node& node, int8_t code, int child) {
        if (node.id == arrow::Type::DENSE_UNION) {
            ThrowIfNotOk(static_cast<arrow::DenseUnionBuilder&>(*node.builder).Append(code),
                         "BatchDecoder: DenseUnionBuilder::Append");
            return;
        }
        ThrowIfNotOk(static_cast<arrow::SparseUnionBuilder&>(*node.builder).Append(code),
                     "BatchDecoder: SparseUnionBuilder::Append");
        // A sparse union keeps every child the same length: the inactive children get a null each.
        for (size_t k = 0; k < node.children.size(); ++k)
            if (static_cast<int>(k) != child)
                ThrowIfNotOk(node.children[k].builder->AppendNull(),
                             "BatchDecoder: sparse union AppendNull");
    }
    void RowEnd(const detail::Reader&) {}
};

}  // namespace

// ---------------------------------------------------------------------------
// BatchDecoder
// ---------------------------------------------------------------------------

struct BatchDecoder::Impl {
    std::shared_ptr<arrow::Schema> schema;
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> columns;  // one per field
    std::vector<Node> roots;                                    // one per field
    std::vector<std::shared_ptr<arrow::DataType>> dict_types;  // per field: the declared dictionary
                                                               // type, or null
    std::vector<Node*>
        budgeted;  // nodes with 32-bit offsets: STRING, BINARY, LIST, MAP, DENSE_UNION
    int64_t rows = 0;
};

namespace {

bool IsNestedType(arrow::Type::type id) {
    switch (id) {
        case arrow::Type::STRUCT:
        case arrow::Type::LIST:
        case arrow::Type::LARGE_LIST:
        case arrow::Type::FIXED_SIZE_LIST:
        case arrow::Type::MAP:
        case arrow::Type::SPARSE_UNION:
        case arrow::Type::DENSE_UNION:
        case arrow::Type::DICTIONARY:
            return true;
        default:
            return false;
    }
}

}  // namespace

BatchDecoder::BatchDecoder(std::shared_ptr<arrow::Schema> schema) : impl_(new Impl) {
    impl_->schema = schema;
    const int n = schema->num_fields();
    impl_->columns.resize(static_cast<size_t>(n));
    impl_->roots.resize(static_cast<size_t>(n));
    impl_->dict_types.resize(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        const auto& field = schema->field(i);
        std::shared_ptr<arrow::DataType> build_type = field->type();
        if (build_type->id() == arrow::Type::DICTIONARY) {
            const auto& value_type =
                static_cast<const arrow::DictionaryType&>(*build_type).value_type();
            if (IsNestedType(value_type->id()) || value_type->id() == arrow::Type::HALF_FLOAT)
                throw std::invalid_argument("BatchDecoder: field '" + field->name() +
                                            "' has unsupported dictionary value type " +
                                            value_type->ToString());
            impl_->dict_types[static_cast<size_t>(i)] = build_type;
            build_type = value_type;
        }
        impl_->columns[static_cast<size_t>(i)] =
            ValueOrThrow(arrow::MakeBuilder(build_type), "BatchDecoder: MakeBuilder");
        BuildNode(impl_->roots[static_cast<size_t>(i)], build_type,
                  impl_->columns[static_cast<size_t>(i)].get(), field->name());
    }
    for (Node& root : impl_->roots) CollectBudgeted(root, impl_->budgeted);
}

BatchDecoder::~BatchDecoder() = default;
BatchDecoder::BatchDecoder(BatchDecoder&&) noexcept = default;
BatchDecoder& BatchDecoder::operator=(BatchDecoder&&) noexcept = default;

void BatchDecoder::Append(const uint8_t* data, size_t len) {
    for (Node* n : impl_->budgeted) n->pending = 0;
    {
        // Nothing is appended until the whole row is proven well-formed and within every 32-bit
        // offset budget: a validation pass over a throwaway Reader, then a second pass that can no
        // longer fail.
        detail::Reader r{data, len};
        SkipSink skip;
        WalkRow(impl_->roots, r, skip);
    }
    {
        detail::Reader r{data, len};
        AppendSink append;
        WalkRow(impl_->roots, r, append);
    }
    ++impl_->rows;
}

void BatchDecoder::Reserve(int64_t rows) {
    for (auto& c : impl_->columns) ThrowIfNotOk(c->Reserve(rows), "BatchDecoder: Reserve");
}

int64_t BatchDecoder::num_rows() const noexcept { return impl_->rows; }

std::shared_ptr<arrow::RecordBatch> BatchDecoder::Finish() {
    std::vector<std::shared_ptr<arrow::Array>> columns(impl_->columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
        auto values = ValueOrThrow(impl_->columns[i]->Finish(), "BatchDecoder: Finish");
        if (const auto& dict_type = impl_->dict_types[i]) {
            // The wire carries plain values (see codec.hpp); re-fold them into the declared
            // dictionary type here, on the way out, rather than on every row in.
            auto encoded = ValueOrThrow(arrow::compute::DictionaryEncode(arrow::Datum(values)),
                                        "BatchDecoder: DictionaryEncode");
            auto array = encoded.make_array();
            if (!array->type()->Equals(*dict_type))
                array = ValueOrThrow(arrow::compute::Cast(arrow::Datum(array), dict_type),
                                     "BatchDecoder: Cast")
                            .make_array();
            values = array;
        }
        columns[i] = std::move(values);
    }
    auto batch = arrow::RecordBatch::Make(impl_->schema, impl_->rows, std::move(columns));
    impl_->rows = 0;
    return batch;
}

}  // namespace fletcher
