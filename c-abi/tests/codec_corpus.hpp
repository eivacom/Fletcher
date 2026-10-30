// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The codec's fixture corpus — ONE definition, two consumers.
//
// These five fixtures are what `NanoarrowCodec.ByteIdenticalToArrowBridge`
// compares the two encoders over, and D-BIND-35 ruled them the right corpus for
// that job: they are Arrow arrays covering every arm of both switches, which is
// the shape the codec actually encodes. `integration-tests/binding-abi-conformance`
// needs exactly the same scenarios with the values C++ produces, so it includes
// this header rather than growing a corpus of its own — a second copy would drift,
// and a corpus that drifts from the one the byte-identity test uses is worse than
// no corpus, because both would keep passing.
//
// Header-only and gtest-free on purpose: one consumer is a gtest binary and the
// other is a plain executable, so the fixtures report a failure by THROWING.
// A fixture that cannot be built must not quietly become an empty batch — an
// empty batch encodes to nothing and compares equal to nothing.
#ifndef FLETCHER_C_ABI_TESTS_CODEC_CORPUS_HPP_
#define FLETCHER_C_ABI_TESTS_CODEC_CORPUS_HPP_

#include <arrow/api.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fletcher::abi::corpus {

/// Fail loudly rather than build a half-fixture.
inline void Require(const arrow::Status& status) {
    if (!status.ok()) {
        throw std::runtime_error("codec corpus: fixture builder failed: " + status.ToString());
    }
}

inline arrow::MemoryPool* Pool() { return arrow::default_memory_pool(); }

/// Finish a builder or fail loudly. A fixture that cannot be built is a broken
/// test rather than a failed comparison, and the two should not look alike.
inline std::shared_ptr<arrow::Array> Finish(arrow::ArrayBuilder& builder) {
    std::shared_ptr<arrow::Array> array;
    Require(builder.Finish(&array));
    return array;
}

/// A fixture: a name and the rows to encode, with their schema.
struct Fixture {
    std::string name;
    std::shared_ptr<arrow::RecordBatch> batch;
};

/// The mapping's scalars, each in three rows: a plain value, the extreme where a
/// wrong cast or a wrong width would show, and a null.
///
/// Nulls are half the point of the corpus. A null takes a different path through
/// both encoders — a bit in a bitfield, and no payload at all — so a corpus of
/// all-set rows would compare the two encoders on half their code.
inline Fixture Scalars() {
    arrow::BooleanBuilder flag;
    // One at a time: AppendValues on a BooleanBuilder is ambiguous for a braced
    // list, which reads as a compiler complaint but is really the vector<bool>
    // specialisation showing through.
    Require(flag.Append(true));
    Require(flag.Append(false));
    Require(flag.AppendNull());

    arrow::Int32Builder i32;
    Require(i32.AppendValues({1, std::numeric_limits<int32_t>::min()}));
    Require(i32.AppendNull());

    arrow::Int64Builder i64;
    Require(i64.AppendValues({1, std::numeric_limits<int64_t>::min()}));
    Require(i64.AppendNull());

    arrow::UInt32Builder u32;
    Require(u32.AppendValues({1, std::numeric_limits<uint32_t>::max()}));
    Require(u32.AppendNull());

    arrow::UInt64Builder u64;
    Require(u64.AppendValues({1, std::numeric_limits<uint64_t>::max()}));
    Require(u64.AppendNull());

    arrow::FloatBuilder f32;
    Require(f32.AppendValues({1.5F, std::numeric_limits<float>::max()}));
    Require(f32.AppendNull());

    arrow::DoubleBuilder f64;
    Require(f64.AppendValues({1.5, std::numeric_limits<double>::max()}));
    Require(f64.AppendNull());

    // Non-ASCII on purpose: a boundary that counted characters rather than bytes
    // reads identically to one that counted bytes until a fixture like this.
    arrow::StringBuilder text;
    Require(text.Append("bl\xc3\xa5\x62\xc3\xa6r"));
    Require(text.Append(""));  // the classic off-by-one
    Require(text.AppendNull());

    arrow::BinaryBuilder blob;
    const uint8_t bytes[] = {0x00, 0xFF, 0x7F};
    Require(blob.Append(bytes, 3));
    Require(blob.Append(bytes, 0));
    Require(blob.AppendNull());

    auto schema = arrow::schema({
        arrow::field("flag", arrow::boolean()),
        arrow::field("i32", arrow::int32()),
        arrow::field("i64", arrow::int64()),
        arrow::field("u32", arrow::uint32()),
        arrow::field("u64", arrow::uint64()),
        arrow::field("f32", arrow::float32()),
        arrow::field("f64", arrow::float64()),
        arrow::field("text", arrow::utf8()),
        arrow::field("blob", arrow::binary()),
    });
    return {"scalars", arrow::RecordBatch::Make(
                           schema, 3,
                           {Finish(flag), Finish(i32), Finish(i64), Finish(u32), Finish(u64),
                            Finish(f32), Finish(f64), Finish(text), Finish(blob)})};
}

/// The flattened well-known types: google.protobuf.Timestamp and .Duration both
/// map to a nanosecond int64 with a temporal type on top.
inline Fixture Temporal() {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    auto dur_type = arrow::duration(arrow::TimeUnit::NANO);

    arrow::TimestampBuilder at(ts_type, Pool());
    Require(at.AppendValues({0, 1789000000000000000LL}));
    Require(at.AppendNull());

    arrow::DurationBuilder took(dur_type, Pool());
    Require(took.AppendValues({0, -1}));
    Require(took.AppendNull());

    auto schema = arrow::schema({arrow::field("at", ts_type), arrow::field("took", dur_type)});
    return {"temporal", arrow::RecordBatch::Make(schema, 3, {Finish(at), Finish(took)})};
}

/// A nested message: set, present-but-all-null, and null. The middle row is the
/// one that separates "the struct is absent" from "the struct is here and its
/// fields are absent" — two different byte sequences and two different meanings.
inline Fixture Nested() {
    auto inner =
        arrow::struct_({arrow::field("id", arrow::int32()), arrow::field("label", arrow::utf8())});

    arrow::StructBuilder who(
        inner, Pool(),
        {std::make_shared<arrow::Int32Builder>(), std::make_shared<arrow::StringBuilder>()});
    auto* id = static_cast<arrow::Int32Builder*>(who.field_builder(0));
    auto* label = static_cast<arrow::StringBuilder*>(who.field_builder(1));

    Require(who.Append());
    Require(id->Append(7));
    Require(label->Append("a"));

    Require(who.Append());
    Require(id->AppendNull());
    Require(label->AppendNull());

    Require(who.AppendNull());

    auto schema = arrow::schema({arrow::field("who", inner)});
    return {"nested", arrow::RecordBatch::Make(schema, 3, {Finish(who)})};
}

/// The three composite constructs the mapping produces: repeated scalar,
/// repeated message, and a map. Each carries a populated row, an EMPTY row and a
/// null row, because empty and null are different bytes and a count prefix is
/// exactly where that goes wrong.
inline Fixture Composites() {
    arrow::ListBuilder readings(Pool(), std::make_shared<arrow::Int32Builder>());
    auto* reading = static_cast<arrow::Int32Builder*>(readings.value_builder());
    Require(readings.Append());
    Require(reading->AppendValues({1, 2, 3}));
    Require(readings.Append());
    Require(readings.AppendNull());

    auto point_type = arrow::struct_({arrow::field("x", arrow::int32())});
    auto point_builder = std::make_shared<arrow::StructBuilder>(
        point_type, Pool(),
        std::vector<std::shared_ptr<arrow::ArrayBuilder>>{std::make_shared<arrow::Int32Builder>()});
    arrow::ListBuilder points(Pool(), point_builder);
    auto* point = static_cast<arrow::StructBuilder*>(points.value_builder());
    auto* x = static_cast<arrow::Int32Builder*>(point->field_builder(0));
    Require(points.Append());
    Require(point->Append());
    Require(x->Append(1));
    Require(point->AppendNull());  // a null ELEMENT inside a list
    Require(points.Append());
    Require(points.AppendNull());

    arrow::MapBuilder tags(Pool(), std::make_shared<arrow::StringBuilder>(),
                           std::make_shared<arrow::Int32Builder>());
    auto* key = static_cast<arrow::StringBuilder*>(tags.key_builder());
    auto* value = static_cast<arrow::Int32Builder*>(tags.item_builder());
    Require(tags.Append());
    Require(key->Append("a"));
    Require(value->Append(1));
    Require(key->Append("b"));
    Require(value->AppendNull());  // a null VALUE, which has its own bitfield
    Require(tags.Append());
    Require(tags.AppendNull());

    auto schema = arrow::schema({
        arrow::field("readings", arrow::list(arrow::int32())),
        arrow::field("points", arrow::list(point_type)),
        arrow::field("tags", arrow::map(arrow::utf8(), arrow::int32())),
    });
    return {"composites",
            arrow::RecordBatch::Make(schema, 3, {Finish(readings), Finish(points), Finish(tags)})};
}

/// The fixed-size list: the one composite whose element count lives in the
/// SCHEMA rather than on the wire, so its framing has no COUNT prefix at all.
///
/// It earns its own fixture because that asymmetry is the easy thing to get
/// wrong in both directions at once - an encoder that writes the count and a
/// decoder that reads it agree with each other and disagree with the format,
/// and only the oracle notices. Rows: populated, one with a null ELEMENT, and a
/// null list.
inline Fixture FixedSize() {
    arrow::FixedSizeListBuilder corner(Pool(), std::make_shared<arrow::Int32Builder>(), 3);
    auto* value = static_cast<arrow::Int32Builder*>(corner.value_builder());

    Require(corner.Append());
    Require(value->AppendValues({1, 2, 3}));

    Require(corner.Append());
    Require(value->Append(4));
    Require(value->AppendNull());
    Require(value->Append(6));

    Require(corner.AppendNull());

    auto schema =
        arrow::schema({arrow::field("corner", arrow::fixed_size_list(arrow::int32(), 3))});
    return {"fixed_size", arrow::RecordBatch::Make(schema, 3, {Finish(corner)})};
}

inline std::vector<Fixture> Corpus() {
    return {Scalars(), Temporal(), Nested(), Composites(), FixedSize()};
}

}  // namespace fletcher::abi::corpus

#endif  // FLETCHER_C_ABI_TESTS_CODEC_CORPUS_HPP_
