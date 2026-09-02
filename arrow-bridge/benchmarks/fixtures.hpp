// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Row shapes for the Arrow-tier serialization benchmark suite. Each namespace below is one shape:
// a plain-C++ RowValues struct, Schema() (the arrow::Schema), MakeRowValues(i)/ToArrowRow (build a
// row two ways from the same data), WritePositional/ReadPositional (the hand-written
// PositionalWriter/PositionalReader twin — exactly what fletcher-protoc generated code would emit
// for this schema, verified byte-for-byte against fletcher::Codec in the validation pass before the
// benchmarks run).
//
// MakeBatch (used by the Arrow IPC and end-to-end arms) is NOT hand-rolled per shape here: batch
// construction never runs inside a timed loop, so the suite builds N rows via MakeRow and folds
// them into a RecordBatch with BuildBatchScalarPath below (the same MakeBuilder/AppendScalar/Finish
// pattern subscriber_arrow.cpp's BuildBatch uses) rather than seven independent typed-builder
// pipelines. That is measured on purpose by BM_Decode_Batch_ScalarPath_*; here it is only fixture
// setup.
#ifndef FLETCHER_ARROW_BRIDGE_BENCHMARKS_FIXTURES_HPP_
#define FLETCHER_ARROW_BRIDGE_BENCHMARKS_FIXTURES_HPP_

#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fletcher {
namespace benchmarks {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// A deterministic, exactly-`width`-byte string: digits of `i` right-aligned over an 'x' fill.
inline std::string FixedWidthString(int64_t i, size_t width) {
    std::string s(width, 'x');
    std::string suffix = std::to_string(i % 1000000);
    size_t start = width > suffix.size() ? width - suffix.size() : 0;
    for (size_t k = 0; k < suffix.size() && start + k < width; ++k) s[start + k] = suffix[k];
    return s;
}

inline std::vector<uint8_t> FixedWidthBytes(int64_t i, size_t width) {
    std::vector<uint8_t> b(width);
    for (size_t k = 0; k < width; ++k) {
        b[k] = static_cast<uint8_t>((i + static_cast<int64_t>(k) * 7) & 0xFF);
    }
    return b;
}

// Re-folds a dictionary column from per-row (plain value, or null) scalars — verbatim mirror of
// SubscriberArrow::RecordBatchBatcher::BuildDictionaryColumn (subscriber_arrow.cpp:210-238).
inline std::shared_ptr<arrow::Array> BuildDictionaryColumnBench(
    const std::shared_ptr<arrow::DataType>& dict_type, const std::vector<ArrowRow>& rows, int col) {
    const auto& value_type = static_cast<const arrow::DictionaryType&>(*dict_type).value_type();
    auto builder = arrow::MakeBuilder(value_type).ValueOrDie();
    (void)builder->Reserve(static_cast<int64_t>(rows.size()));
    for (const auto& row : rows) {
        const auto& s = row[static_cast<size_t>(col)];
        if (!s || !s->is_valid || !builder->AppendScalar(*s).ok()) (void)builder->AppendNull();
    }
    auto value_array = builder->Finish().ValueOrDie();
    auto encoded = arrow::compute::DictionaryEncode(arrow::Datum(value_array)).ValueOrDie();
    auto array = encoded.make_array();
    if (!array->type()->Equals(*dict_type)) {
        auto casted = arrow::compute::Cast(arrow::Datum(array), dict_type).ValueOrDie();
        array = casted.make_array();
    }
    return array;
}

// Verbatim mirror of SubscriberArrow::RecordBatchBatcher::BuildBatch
// (subscriber_arrow.cpp:177-205): the CURRENT batched-decode path. MakeBuilder per column,
// Reserve(rows), AppendScalar(*row[c]) per row, Finish, RecordBatch::Make. Used both as the timed
// BM_Decode_Batch_ScalarPath_* body and as fixture setup for MakeBatch-style construction elsewhere
// in this file.
inline std::shared_ptr<arrow::RecordBatch> BuildBatchScalarPath(
    const std::shared_ptr<arrow::Schema>& schema, const std::vector<ArrowRow>& rows) {
    const int num_fields = schema->num_fields();
    std::vector<std::shared_ptr<arrow::Array>> columns(static_cast<size_t>(num_fields));
    for (int c = 0; c < num_fields; ++c) {
        const auto& field_type = schema->field(c)->type();
        if (field_type->id() == arrow::Type::DICTIONARY) {
            columns[static_cast<size_t>(c)] = BuildDictionaryColumnBench(field_type, rows, c);
            continue;
        }
        auto builder = arrow::MakeBuilder(field_type).ValueOrDie();
        (void)builder->Reserve(static_cast<int64_t>(rows.size()));
        for (const auto& row : rows) {
            if (!builder->AppendScalar(*row[static_cast<size_t>(c)]).ok())
                (void)builder->AppendNull();
        }
        columns[static_cast<size_t>(c)] = builder->Finish().ValueOrDie();
    }
    return arrow::RecordBatch::Make(schema, static_cast<int64_t>(rows.size()), std::move(columns));
}

// ---------------------------------------------------------------------------
// Scalars10 — bool, int32, int64, uint32, float, double, utf8(16B), timestamp[ns], duration[ns],
// binary(32B), all non-nullable.
// ---------------------------------------------------------------------------

namespace scalars10 {

struct RowValues {
    bool f0;
    int32_t f1;
    int64_t f2;
    uint32_t f3;
    float f4;
    double f5;
    std::string f6;           // 16 bytes
    int64_t f7;               // timestamp[ns]
    int64_t f8;               // duration[ns]
    std::vector<uint8_t> f9;  // 32 bytes
};

inline RowValues MakeRowValues(int64_t i) {
    RowValues v;
    v.f0 = (i % 2) == 0;
    v.f1 = static_cast<int32_t>(i * 3 - 7);
    v.f2 = static_cast<int64_t>(i) * 1000000007LL;
    v.f3 = static_cast<uint32_t>(i) * 2654435761u;
    v.f4 = static_cast<float>(i) * 1.5f;
    v.f5 = static_cast<double>(i) * 2.5;
    v.f6 = FixedWidthString(i, 16);
    v.f7 = 1700000000000000000LL + i;
    v.f8 = 500000000LL + i;
    v.f9 = FixedWidthBytes(i, 32);
    return v;
}

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({
        arrow::field("b", arrow::boolean(), false),
        arrow::field("i32", arrow::int32(), false),
        arrow::field("i64", arrow::int64(), false),
        arrow::field("u32", arrow::uint32(), false),
        arrow::field("f32", arrow::float32(), false),
        arrow::field("f64", arrow::float64(), false),
        arrow::field("s16", arrow::utf8(), false),
        arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO), false),
        arrow::field("dur", arrow::duration(arrow::TimeUnit::NANO), false),
        arrow::field("bin32", arrow::binary(), false),
    });
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    auto dur_type = arrow::duration(arrow::TimeUnit::NANO);
    auto bin_buf = arrow::Buffer::FromString(
        std::string(reinterpret_cast<const char*>(v.f9.data()), v.f9.size()));
    return {
        std::make_shared<arrow::BooleanScalar>(v.f0),
        std::make_shared<arrow::Int32Scalar>(v.f1),
        std::make_shared<arrow::Int64Scalar>(v.f2),
        std::make_shared<arrow::UInt32Scalar>(v.f3),
        std::make_shared<arrow::FloatScalar>(v.f4),
        std::make_shared<arrow::DoubleScalar>(v.f5),
        std::make_shared<arrow::StringScalar>(v.f6),
        std::make_shared<arrow::TimestampScalar>(v.f7, ts_type),
        std::make_shared<arrow::DurationScalar>(v.f8, dur_type),
        std::make_shared<arrow::BinaryScalar>(bin_buf),
    };
}

inline ArrowRow MakeRow(int64_t i) { return ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 10);
    writer.WriteBool(v.f0);
    writer.WriteInt32(v.f1);
    writer.WriteInt64(v.f2);
    writer.WriteUint32(v.f3);
    writer.WriteFloat(v.f4);
    writer.WriteDouble(v.f5);
    writer.WriteString(v.f6);
    writer.WriteTimestamp(v.f7);
    writer.WriteDuration(v.f8);
    writer.WriteBinary(v.f9.data(), v.f9.size());
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 10);
    v.f0 = reader.ReadBool();
    v.f1 = reader.ReadInt32();
    v.f2 = reader.ReadInt64();
    v.f3 = reader.ReadUint32();
    v.f4 = reader.ReadFloat();
    v.f5 = reader.ReadDouble();
    v.f6 = std::string(reader.ReadString());
    v.f7 = reader.ReadTimestamp();
    v.f8 = reader.ReadDuration();
    auto bin = reader.ReadBinary();
    v.f9.assign(bin.first, bin.first + bin.second);
    return v;
}

}  // namespace scalars10

// ---------------------------------------------------------------------------
// Nullable10 — same fields as Scalars10, all nullable; fields 0, 3, 6, 9 (every third) are null in
// every row.
// ---------------------------------------------------------------------------

namespace nullable10 {

using RowValues = scalars10::RowValues;

inline RowValues MakeRowValues(int64_t i) { return scalars10::MakeRowValues(i); }

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({
        arrow::field("b", arrow::boolean(), true),
        arrow::field("i32", arrow::int32(), true),
        arrow::field("i64", arrow::int64(), true),
        arrow::field("u32", arrow::uint32(), true),
        arrow::field("f32", arrow::float32(), true),
        arrow::field("f64", arrow::float64(), true),
        arrow::field("s16", arrow::utf8(), true),
        arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO), true),
        arrow::field("dur", arrow::duration(arrow::TimeUnit::NANO), true),
        arrow::field("bin32", arrow::binary(), true),
    });
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    auto dur_type = arrow::duration(arrow::TimeUnit::NANO);
    return {
        arrow::MakeNullScalar(arrow::boolean()),
        std::make_shared<arrow::Int32Scalar>(v.f1),
        std::make_shared<arrow::Int64Scalar>(v.f2),
        arrow::MakeNullScalar(arrow::uint32()),
        std::make_shared<arrow::FloatScalar>(v.f4),
        std::make_shared<arrow::DoubleScalar>(v.f5),
        arrow::MakeNullScalar(arrow::utf8()),
        std::make_shared<arrow::TimestampScalar>(v.f7, ts_type),
        std::make_shared<arrow::DurationScalar>(v.f8, dur_type),
        arrow::MakeNullScalar(arrow::binary()),
    };
}

// Qualified (not ADL-found) on purpose: RowValues is a type alias for scalars10::RowValues, so
// unqualified ToArrowRow(...) here is ambiguous between this overload and scalars10::ToArrowRow.
inline ArrowRow MakeRow(int64_t i) { return nullable10::ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 10);
    writer.SetNull(0);
    writer.SetNull(3);
    writer.SetNull(6);
    writer.SetNull(9);
    writer.WriteInt32(v.f1);
    writer.WriteInt64(v.f2);
    writer.WriteFloat(v.f4);
    writer.WriteDouble(v.f5);
    writer.WriteTimestamp(v.f7);
    writer.WriteDuration(v.f8);
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 10);
    if (!reader.IsNull(0)) v.f0 = reader.ReadBool();
    v.f1 = reader.ReadInt32();
    v.f2 = reader.ReadInt64();
    if (!reader.IsNull(3)) v.f3 = reader.ReadUint32();
    v.f4 = reader.ReadFloat();
    v.f5 = reader.ReadDouble();
    if (!reader.IsNull(6)) v.f6 = std::string(reader.ReadString());
    v.f7 = reader.ReadTimestamp();
    v.f8 = reader.ReadDuration();
    if (!reader.IsNull(9)) {
        auto bin = reader.ReadBinary();
        v.f9.assign(bin.first, bin.first + bin.second);
    }
    return v;
}

}  // namespace nullable10

// ---------------------------------------------------------------------------
// Cloud — timestamp[ns], list<float>x2667, list<uint32>x2667 (list item field nullable=true, as
// protoc emits).
// ---------------------------------------------------------------------------

namespace cloud {

constexpr int64_t kCount = 2667;

struct RowValues {
    int64_t timestamp;
    std::vector<float> floats;
    std::vector<uint32_t> uints;
};

inline RowValues MakeRowValues(int64_t i) {
    RowValues v;
    v.timestamp = 1700000000000000000LL + i;
    v.floats.resize(static_cast<size_t>(kCount));
    v.uints.resize(static_cast<size_t>(kCount));
    for (int64_t k = 0; k < kCount; ++k) {
        v.floats[static_cast<size_t>(k)] = static_cast<float>((i + k) % 1000) * 0.01f;
        v.uints[static_cast<size_t>(k)] = static_cast<uint32_t>((i + k) % 100000);
    }
    return v;
}

inline std::shared_ptr<arrow::DataType> FloatListType() {
    return arrow::list(arrow::field("item", arrow::float32(), true));
}
inline std::shared_ptr<arrow::DataType> UintListType() {
    return arrow::list(arrow::field("item", arrow::uint32(), true));
}

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({
        arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO), false),
        arrow::field("floats", FloatListType(), false),
        arrow::field("uints", UintListType(), false),
    });
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    arrow::FloatBuilder fb;
    if (!fb.AppendValues(v.floats).ok()) throw std::runtime_error("cloud floats append failed");
    auto float_arr = fb.Finish().ValueOrDie();

    arrow::UInt32Builder ub;
    if (!ub.AppendValues(v.uints).ok()) throw std::runtime_error("cloud uints append failed");
    auto uint_arr = ub.Finish().ValueOrDie();

    return {
        std::make_shared<arrow::TimestampScalar>(v.timestamp,
                                                 arrow::timestamp(arrow::TimeUnit::NANO)),
        std::make_shared<arrow::ListScalar>(float_arr, FloatListType()),
        std::make_shared<arrow::ListScalar>(uint_arr, UintListType()),
    };
}

inline ArrowRow MakeRow(int64_t i) { return ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 3);
    writer.WriteTimestamp(v.timestamp);
    {
        writer.BeginList(static_cast<uint32_t>(v.floats.size()));
        writer.WriteFixedArray(v.floats.data(), v.floats.size());
    }
    {
        writer.BeginList(static_cast<uint32_t>(v.uints.size()));
        writer.WriteFixedArray(v.uints.data(), v.uints.size());
    }
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 3);
    v.timestamp = reader.ReadTimestamp();
    {
        auto hdr = reader.ReadListHeader();
        reader.ReadFixedArrayInto(v.floats, hdr.count);
    }
    {
        auto hdr = reader.ReadListHeader();
        reader.ReadFixedArrayInto(v.uints, hdr.count);
    }
    return v;
}

}  // namespace cloud

// ---------------------------------------------------------------------------
// Pose — timestamp[ns], struct{list<double>x16}, struct{list<double>x6}.
// ---------------------------------------------------------------------------

namespace pose {

struct RowValues {
    int64_t timestamp;
    std::array<double, 16> pose_values;
    std::array<double, 6> velocity_values;
};

inline RowValues MakeRowValues(int64_t i) {
    RowValues v{};
    v.timestamp = 1700000000000000000LL + i;
    for (int k = 0; k < 16; ++k) {
        v.pose_values[static_cast<size_t>(k)] = static_cast<double>(i) + k * 0.25;
    }
    for (int k = 0; k < 6; ++k) {
        v.velocity_values[static_cast<size_t>(k)] = static_cast<double>(k) * 0.5 + i * 0.001;
    }
    return v;
}

inline std::shared_ptr<arrow::DataType> DoubleListType() {
    return arrow::list(arrow::field("item", arrow::float64(), true));
}
inline std::shared_ptr<arrow::DataType> PoseStructType() {
    return arrow::struct_({arrow::field("values", DoubleListType(), false)});
}
inline std::shared_ptr<arrow::DataType> VelocityStructType() {
    return arrow::struct_({arrow::field("values", DoubleListType(), false)});
}

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({
        arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO), false),
        arrow::field("pose", PoseStructType(), false),
        arrow::field("velocity", VelocityStructType(), false),
    });
}

inline std::shared_ptr<arrow::Scalar> MakeListStructScalar(
    const double* data, size_t count, const std::shared_ptr<arrow::DataType>& stype) {
    arrow::DoubleBuilder db;
    for (size_t k = 0; k < count; ++k) {
        if (!db.Append(data[k]).ok()) throw std::runtime_error("pose value append failed");
    }
    auto arr = db.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr, DoubleListType());
    return std::make_shared<arrow::StructScalar>(arrow::ScalarVector{list_scalar}, stype);
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    return {
        std::make_shared<arrow::TimestampScalar>(v.timestamp,
                                                 arrow::timestamp(arrow::TimeUnit::NANO)),
        MakeListStructScalar(v.pose_values.data(), v.pose_values.size(), PoseStructType()),
        MakeListStructScalar(v.velocity_values.data(), v.velocity_values.size(),
                             VelocityStructType()),
    };
}

inline ArrowRow MakeRow(int64_t i) { return ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 3);
    writer.WriteTimestamp(v.timestamp);
    {
        auto s = writer.BeginStruct(1);
        s.BeginList(static_cast<uint32_t>(v.pose_values.size()));
        s.WriteFixedArray(v.pose_values.data(), v.pose_values.size());
    }
    {
        auto s = writer.BeginStruct(1);
        s.BeginList(static_cast<uint32_t>(v.velocity_values.size()));
        s.WriteFixedArray(v.velocity_values.data(), v.velocity_values.size());
    }
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 3);
    v.timestamp = reader.ReadTimestamp();
    {
        auto s = reader.ReadStruct(1);
        auto hdr = s.ReadListHeader();
        s.ReadFixedArray(v.pose_values.data(), hdr.count);
    }
    {
        auto s = reader.ReadStruct(1);
        auto hdr = s.ReadListHeader();
        s.ReadFixedArray(v.velocity_values.data(), hdr.count);
    }
    return v;
}

}  // namespace pose

// ---------------------------------------------------------------------------
// Points — list<struct<x,y,z: double>>x1000.
// ---------------------------------------------------------------------------

namespace points {

constexpr int64_t kCount = 1000;

struct Point {
    double x, y, z;
};

struct RowValues {
    std::vector<Point> points;
};

inline RowValues MakeRowValues(int64_t i) {
    RowValues v;
    v.points.resize(static_cast<size_t>(kCount));
    for (int64_t k = 0; k < kCount; ++k) {
        auto& p = v.points[static_cast<size_t>(k)];
        p.x = static_cast<double>(i) + k * 0.001;
        p.y = static_cast<double>(i) * 0.5 + k * 0.002;
        p.z = static_cast<double>(i) * 0.25 + k * 0.003;
    }
    return v;
}

inline std::shared_ptr<arrow::DataType> PointStructType() {
    return arrow::struct_({
        arrow::field("x", arrow::float64(), false),
        arrow::field("y", arrow::float64(), false),
        arrow::field("z", arrow::float64(), false),
    });
}
inline std::shared_ptr<arrow::DataType> PointsListType() {
    return arrow::list(arrow::field("item", PointStructType(), true));
}

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({arrow::field("points", PointsListType(), false)});
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    auto* pool = arrow::default_memory_pool();
    auto x_b = std::make_shared<arrow::DoubleBuilder>();
    auto y_b = std::make_shared<arrow::DoubleBuilder>();
    auto z_b = std::make_shared<arrow::DoubleBuilder>();
    auto struct_b = std::make_shared<arrow::StructBuilder>(
        PointStructType(), pool, std::vector<std::shared_ptr<arrow::ArrayBuilder>>{x_b, y_b, z_b});
    for (const auto& p : v.points) {
        if (!struct_b->Append().ok()) throw std::runtime_error("points struct append failed");
        if (!x_b->Append(p.x).ok() || !y_b->Append(p.y).ok() || !z_b->Append(p.z).ok())
            throw std::runtime_error("points value append failed");
    }
    auto struct_arr = struct_b->Finish().ValueOrDie();
    return {std::make_shared<arrow::ListScalar>(struct_arr, PointsListType())};
}

inline ArrowRow MakeRow(int64_t i) { return ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 1);
    writer.BeginList(static_cast<uint32_t>(v.points.size()));
    for (const auto& p : v.points) {
        auto s = writer.BeginStruct(3);
        s.WriteDouble(p.x);
        s.WriteDouble(p.y);
        s.WriteDouble(p.z);
    }
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 1);
    auto hdr = reader.ReadListHeader();
    v.points.resize(hdr.count);
    for (uint32_t k = 0; k < hdr.count; ++k) {
        if (hdr.IsElementNull(k)) continue;
        auto s = reader.ReadStruct(3);
        v.points[k].x = s.ReadDouble();
        v.points[k].y = s.ReadDouble();
        v.points[k].z = s.ReadDouble();
    }
    return v;
}

}  // namespace points

// ---------------------------------------------------------------------------
// Nested — list<list<struct<x,y: double>>> (2 outer x 5 inner), map<utf8,double>x8,
// struct{struct{int32, utf8}}.
// ---------------------------------------------------------------------------

namespace nested {

struct Xy {
    double x, y;
};

struct RowValues {
    std::vector<std::vector<Xy>> outer;                       // 2 x 5
    std::vector<std::pair<std::string, double>> map_entries;  // 8
    int32_t inner_i32;
    std::string inner_str;
};

inline RowValues MakeRowValues(int64_t i) {
    RowValues v;
    v.outer.resize(2);
    for (int o = 0; o < 2; ++o) {
        v.outer[static_cast<size_t>(o)].resize(5);
        for (int in = 0; in < 5; ++in) {
            v.outer[static_cast<size_t>(o)][static_cast<size_t>(in)] =
                Xy{static_cast<double>(i) + o * 10 + in * 0.1,
                   static_cast<double>(i) * 0.5 + o + in * 0.2};
        }
    }
    v.map_entries.reserve(8);
    for (int k = 0; k < 8; ++k) {
        v.map_entries.emplace_back("key" + std::to_string(k), static_cast<double>(i) + k * 0.5);
    }
    v.inner_i32 = static_cast<int32_t>(i * 7);
    v.inner_str = FixedWidthString(i, 12);
    return v;
}

inline std::shared_ptr<arrow::DataType> XyType() {
    return arrow::struct_(
        {arrow::field("x", arrow::float64(), false), arrow::field("y", arrow::float64(), false)});
}
inline std::shared_ptr<arrow::DataType> InnerListType() {
    return arrow::list(arrow::field("item", XyType(), true));
}
inline std::shared_ptr<arrow::DataType> OuterListType() {
    return arrow::list(arrow::field("item", InnerListType(), true));
}
inline std::shared_ptr<arrow::DataType> MapType() {
    return arrow::map(arrow::utf8(), arrow::field("value", arrow::float64(), true));
}
inline std::shared_ptr<arrow::DataType> InnerBoxType() {
    return arrow::struct_(
        {arrow::field("i", arrow::int32(), false), arrow::field("s", arrow::utf8(), false)});
}
inline std::shared_ptr<arrow::DataType> BoxType() {
    return arrow::struct_({arrow::field("inner", InnerBoxType(), false)});
}

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({
        arrow::field("nested", OuterListType(), false),
        arrow::field("kv", MapType(), false),
        arrow::field("box", BoxType(), false),
    });
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    auto* pool = arrow::default_memory_pool();

    // The scalar's value is ONE level down from its type: a ListScalar of type list<list<struct>>>
    // holds, as `value`, an array of type list<struct> (length 2 — the outer list's own elements).
    // There is no separate "outer" builder wrapping the whole thing again — that would add a third
    // nesting level meant for a batch of *rows*, not this one row's value.
    auto x_b = std::make_shared<arrow::DoubleBuilder>();
    auto y_b = std::make_shared<arrow::DoubleBuilder>();
    auto struct_b = std::make_shared<arrow::StructBuilder>(
        XyType(), pool, std::vector<std::shared_ptr<arrow::ArrayBuilder>>{x_b, y_b});
    arrow::ListBuilder inner_list_b(pool, struct_b, InnerListType());

    for (const auto& inner_vec : v.outer) {
        if (!inner_list_b.Append().ok()) throw std::runtime_error("nested inner append failed");
        for (const auto& xy : inner_vec) {
            if (!struct_b->Append().ok()) throw std::runtime_error("nested struct append failed");
            if (!x_b->Append(xy.x).ok() || !y_b->Append(xy.y).ok())
                throw std::runtime_error("nested xy append failed");
        }
    }
    auto outer_arr = inner_list_b.Finish().ValueOrDie();
    auto nested_scalar = std::make_shared<arrow::ListScalar>(outer_arr, OuterListType());

    arrow::StringBuilder key_b;
    arrow::DoubleBuilder val_b;
    for (const auto& kv : v.map_entries) {
        if (!key_b.Append(kv.first).ok() || !val_b.Append(kv.second).ok())
            throw std::runtime_error("nested map append failed");
    }
    auto keys = key_b.Finish().ValueOrDie();
    auto vals = val_b.Finish().ValueOrDie();
    auto map_type = MapType();
    const auto& mt = static_cast<const arrow::MapType&>(*map_type);
    auto entries = arrow::StructArray::Make({keys, vals}, mt.value_type()->fields()).ValueOrDie();
    auto map_scalar = std::make_shared<arrow::MapScalar>(entries, map_type);

    auto inner_struct = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{std::make_shared<arrow::Int32Scalar>(v.inner_i32),
                            std::make_shared<arrow::StringScalar>(v.inner_str)},
        InnerBoxType());
    auto outer_struct =
        std::make_shared<arrow::StructScalar>(arrow::ScalarVector{inner_struct}, BoxType());

    return {nested_scalar, map_scalar, outer_struct};
}

inline ArrowRow MakeRow(int64_t i) { return ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 3);
    {
        writer.BeginList(static_cast<uint32_t>(v.outer.size()));
        for (const auto& inner_vec : v.outer) {
            writer.BeginList(static_cast<uint32_t>(inner_vec.size()));
            for (const auto& xy : inner_vec) {
                auto s = writer.BeginStruct(2);
                s.WriteDouble(xy.x);
                s.WriteDouble(xy.y);
            }
        }
    }
    {
        auto mc = writer.BeginMap(static_cast<uint32_t>(v.map_entries.size()));
        for (const auto& kv : v.map_entries) writer.WriteString(kv.first);
        mc.BeginValues();
        for (const auto& kv : v.map_entries) writer.WriteDouble(kv.second);
    }
    {
        auto outer_s = writer.BeginStruct(1);
        auto inner_s = outer_s.BeginStruct(2);
        inner_s.WriteInt32(v.inner_i32);
        inner_s.WriteString(v.inner_str);
    }
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 3);
    {
        auto outer_hdr = reader.ReadListHeader();
        v.outer.resize(outer_hdr.count);
        for (uint32_t o = 0; o < outer_hdr.count; ++o) {
            if (outer_hdr.IsElementNull(o)) continue;
            auto inner_hdr = reader.ReadListHeader();
            v.outer[o].resize(inner_hdr.count);
            for (uint32_t in = 0; in < inner_hdr.count; ++in) {
                if (inner_hdr.IsElementNull(in)) continue;
                auto s = reader.ReadStruct(2);
                v.outer[o][in].x = s.ReadDouble();
                v.outer[o][in].y = s.ReadDouble();
            }
        }
    }
    {
        uint32_t count = reader.ReadMapCount();
        std::vector<std::string> keys(count);
        for (uint32_t k = 0; k < count; ++k) keys[k] = std::string(reader.ReadString());
        const uint8_t* val_bf = reader.ReadMapValueBitfield(count);
        v.map_entries.resize(count);
        for (uint32_t k = 0; k < count; ++k) {
            double val = 0.0;
            bool is_null = ((val_bf[k / 8] >> (k % 8)) & 1u) != 0;
            if (!is_null) val = reader.ReadDouble();
            v.map_entries[k] = {keys[k], val};
        }
    }
    {
        auto outer_s = reader.ReadStruct(1);
        auto inner_s = outer_s.ReadStruct(2);
        v.inner_i32 = inner_s.ReadInt32();
        v.inner_str = std::string(inner_s.ReadString());
    }
    return v;
}

}  // namespace nested

// ---------------------------------------------------------------------------
// Generic — dictionary<int32,utf8> (3 values cycling), fixed_size_list<float,3>, decimal128(10,2),
// dense_union<int32,utf8> (alternating), large_utf8, list<timestamp[ns]>x1000.
// ---------------------------------------------------------------------------

namespace generic {

constexpr int64_t kTsListCount = 1000;

struct RowValues {
    std::string dict_value;
    std::array<float, 3> fsl;
    int64_t decimal_raw;  // fits in the low 64 bits of the decimal128, sign bit 0
    bool union_is_int;
    int32_t union_int;
    std::string union_str;
    std::string large_str;
    std::vector<int64_t> ts_list;
};

inline RowValues MakeRowValues(int64_t i) {
    RowValues v;
    static const char* const kDictValues[3] = {"alpha", "beta", "gamma"};
    v.dict_value = kDictValues[i % 3];
    v.fsl = {static_cast<float>(i), static_cast<float>(i) + 0.5f, static_cast<float>(i) + 1.5f};
    v.decimal_raw = 123456 + i;
    v.union_is_int = (i % 2) == 0;
    v.union_int = static_cast<int32_t>(i * 3);
    v.union_str = FixedWidthString(i, 8);
    v.large_str = "large-utf8-value-" + std::to_string(i);
    v.ts_list.resize(static_cast<size_t>(kTsListCount));
    for (int64_t k = 0; k < kTsListCount; ++k) {
        v.ts_list[static_cast<size_t>(k)] = 1700000000000000000LL + i * 1000 + k;
    }
    return v;
}

inline std::shared_ptr<arrow::DataType> DictType() {
    return arrow::dictionary(arrow::int32(), arrow::utf8());
}
inline std::shared_ptr<arrow::DataType> FslType() {
    return arrow::fixed_size_list(arrow::float32(), 3);
}
inline std::shared_ptr<arrow::DataType> DecType() { return arrow::decimal128(10, 2); }
inline std::shared_ptr<arrow::DataType> UnionType() {
    return arrow::dense_union({arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
                              {0, 1});
}
inline std::shared_ptr<arrow::DataType> TsListType() {
    return arrow::list(arrow::field("item", arrow::timestamp(arrow::TimeUnit::NANO), true));
}

inline std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({
        arrow::field("dict", DictType(), false),
        arrow::field("fsl", FslType(), false),
        arrow::field("dec", DecType(), false),
        arrow::field("u", UnionType(), false),
        arrow::field("big", arrow::large_utf8(), false),
        arrow::field("ts_list", TsListType(), false),
    });
}

inline ArrowRow ToArrowRow(const RowValues& v) {
    auto dict_scalar = std::make_shared<arrow::StringScalar>(v.dict_value);

    arrow::FloatBuilder fb;
    for (float f : v.fsl) {
        if (!fb.Append(f).ok()) throw std::runtime_error("generic fsl append failed");
    }
    auto fsl_arr = fb.Finish().ValueOrDie();
    auto fsl_scalar = std::make_shared<arrow::FixedSizeListScalar>(fsl_arr, FslType());

    auto dec_scalar =
        std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(v.decimal_raw), DecType());

    std::shared_ptr<arrow::Scalar> union_scalar;
    if (v.union_is_int) {
        union_scalar = std::make_shared<arrow::DenseUnionScalar>(
            std::make_shared<arrow::Int32Scalar>(v.union_int), static_cast<int8_t>(0), UnionType());
    } else {
        union_scalar = std::make_shared<arrow::DenseUnionScalar>(
            std::make_shared<arrow::StringScalar>(v.union_str), static_cast<int8_t>(1),
            UnionType());
    }

    auto big_scalar = std::make_shared<arrow::LargeStringScalar>(v.large_str);

    arrow::TimestampBuilder ts_b(arrow::timestamp(arrow::TimeUnit::NANO),
                                 arrow::default_memory_pool());
    for (int64_t ts : v.ts_list) {
        if (!ts_b.Append(ts).ok()) throw std::runtime_error("generic ts_list append failed");
    }
    auto ts_arr = ts_b.Finish().ValueOrDie();
    auto ts_list_scalar = std::make_shared<arrow::ListScalar>(ts_arr, TsListType());

    return {dict_scalar, fsl_scalar, dec_scalar, union_scalar, big_scalar, ts_list_scalar};
}

inline ArrowRow MakeRow(int64_t i) { return ToArrowRow(MakeRowValues(i)); }

inline void WritePositional(const RowValues& v, WriteBuffer& buf) {
    PositionalWriter writer(buf, 6);
    writer.WriteString(v.dict_value);
    {
        // fixed_size_list has no COUNT prefix and PositionalWriter has no dedicated primitive for
        // it: write the elem null bitfield directly (no nulls here, so it stays zeroed) then the
        // payload, matching Codec::EncodeListElements for FIXED_SIZE_LIST.
        writer.buf().AppendZeros((v.fsl.size() + 7) / 8);
        writer.WriteFixedArray(v.fsl.data(), v.fsl.size());
    }
    {
        uint8_t bytes[16];
        arrow::Decimal128(v.decimal_raw).ToBytes(bytes);
        writer.buf().Append(bytes, sizeof(bytes));
    }
    {
        writer.WriteInt8(v.union_is_int ? static_cast<int8_t>(0) : static_cast<int8_t>(1));
        if (v.union_is_int) {
            writer.WriteInt32(v.union_int);
        } else {
            writer.WriteString(v.union_str);
        }
    }
    writer.WriteString(v.large_str);
    {
        writer.BeginList(static_cast<uint32_t>(v.ts_list.size()));
        writer.WriteFixedArray(v.ts_list.data(), v.ts_list.size());
    }
}

inline RowValues ReadPositional(const uint8_t* data, size_t len) {
    RowValues v{};
    PositionalReader reader(data, len, 6);
    v.dict_value = std::string(reader.ReadString());
    {
        // Skip the fixed_size_list elem null bitfield (no dedicated primitive;
        // ReadFixedArray<uint8_t> is a plain byte-count read with no length prefix, which is
        // exactly what's needed here).
        std::vector<uint8_t> discard((v.fsl.size() + 7) / 8);
        reader.ReadFixedArray(discard.data(), discard.size());
        reader.ReadFixedArray(v.fsl.data(), v.fsl.size());
    }
    {
        uint8_t bytes[16];
        reader.ReadFixedArray(bytes, sizeof(bytes));
        std::memcpy(&v.decimal_raw, bytes, sizeof(v.decimal_raw));
    }
    {
        int8_t code = reader.ReadInt8();
        v.union_is_int = (code == 0);
        if (v.union_is_int) {
            v.union_int = reader.ReadInt32();
        } else {
            v.union_str = std::string(reader.ReadString());
        }
    }
    v.large_str = std::string(reader.ReadString());
    {
        auto hdr = reader.ReadListHeader();
        reader.ReadFixedArrayInto(v.ts_list, hdr.count);
    }
    return v;
}

}  // namespace generic

}  // namespace benchmarks
}  // namespace fletcher

#endif  // FLETCHER_ARROW_BRIDGE_BENCHMARKS_FIXTURES_HPP_
