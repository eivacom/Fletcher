// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-2's forcing test: the nanoarrow codec writes the bytes `arrow-bridge`'s
// codec writes. Not similar bytes — the same bytes.
//
// ── Why this is the whole safety argument ───────────────────────────────────
// This round puts a SECOND encoder of the positional wire format into the tree.
// One is unavoidable: the Arrow C++ one cannot sit behind a C ABI (it returns
// the row, which is the copy, and takes a vector of shared_ptr<Scalar>, which
// cannot cross P/Invoke). Two encoders is a standing invitation to drift, and
// drift here is not a crash — it is a subscriber decoding a publisher's bytes
// into the wrong fields, silently, on a production bus.
//
// So the two are compared byte for byte over a corpus of schemas, on every CI
// run. A single differing byte fails, and the failure names the fixture and the
// row.
//
// ── Why this test links arrow-bridge and the component does not ─────────────
// `fletcher-c-abi` must never link `arrow-bridge`: that dependency would pull
// Arrow C++ into the shipped per-RID native asset, which is most of the reason
// the codec was written on nanoarrow in the first place. The TEST links it,
// because the oracle has to be the real thing rather than a transcription of it.
// A test-only dependency is not a shipped one, and CI's packed-size check
// measures the packaged shim rather than this binary.
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../src/nanoarrow_codec.hpp"

namespace {

using fletcher::abi::BoundRows;
using fletcher::abi::NanoarrowCodec;

arrow::MemoryPool* Pool() { return arrow::default_memory_pool(); }

/// Finish a builder or fail loudly. A fixture that cannot be built is a broken
/// test rather than a failed comparison, and the two should not look alike.
std::shared_ptr<arrow::Array> Finish(arrow::ArrayBuilder& builder) {
    std::shared_ptr<arrow::Array> array;
    const arrow::Status status = builder.Finish(&array);
    EXPECT_TRUE(status.ok()) << "fixture builder failed: " << status.ToString();
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
Fixture Scalars() {
    arrow::BooleanBuilder flag;
    // One at a time: AppendValues on a BooleanBuilder is ambiguous for a braced
    // list, which reads as a compiler complaint but is really the vector<bool>
    // specialisation showing through.
    EXPECT_TRUE(flag.Append(true).ok());
    EXPECT_TRUE(flag.Append(false).ok());
    EXPECT_TRUE(flag.AppendNull().ok());

    arrow::Int32Builder i32;
    EXPECT_TRUE(i32.AppendValues({1, std::numeric_limits<int32_t>::min()}).ok());
    EXPECT_TRUE(i32.AppendNull().ok());

    arrow::Int64Builder i64;
    EXPECT_TRUE(i64.AppendValues({1, std::numeric_limits<int64_t>::min()}).ok());
    EXPECT_TRUE(i64.AppendNull().ok());

    arrow::UInt32Builder u32;
    EXPECT_TRUE(u32.AppendValues({1, std::numeric_limits<uint32_t>::max()}).ok());
    EXPECT_TRUE(u32.AppendNull().ok());

    arrow::UInt64Builder u64;
    EXPECT_TRUE(u64.AppendValues({1, std::numeric_limits<uint64_t>::max()}).ok());
    EXPECT_TRUE(u64.AppendNull().ok());

    arrow::FloatBuilder f32;
    EXPECT_TRUE(f32.AppendValues({1.5F, std::numeric_limits<float>::max()}).ok());
    EXPECT_TRUE(f32.AppendNull().ok());

    arrow::DoubleBuilder f64;
    EXPECT_TRUE(f64.AppendValues({1.5, std::numeric_limits<double>::max()}).ok());
    EXPECT_TRUE(f64.AppendNull().ok());

    // Non-ASCII on purpose: a boundary that counted characters rather than bytes
    // reads identically to one that counted bytes until a fixture like this.
    arrow::StringBuilder text;
    EXPECT_TRUE(text.Append("bl\xc3\xa5\x62\xc3\xa6r").ok());
    EXPECT_TRUE(text.Append("").ok());  // the classic off-by-one
    EXPECT_TRUE(text.AppendNull().ok());

    arrow::BinaryBuilder blob;
    const uint8_t bytes[] = {0x00, 0xFF, 0x7F};
    EXPECT_TRUE(blob.Append(bytes, 3).ok());
    EXPECT_TRUE(blob.Append(bytes, 0).ok());
    EXPECT_TRUE(blob.AppendNull().ok());

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
Fixture Temporal() {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    auto dur_type = arrow::duration(arrow::TimeUnit::NANO);

    arrow::TimestampBuilder at(ts_type, Pool());
    EXPECT_TRUE(at.AppendValues({0, 1789000000000000000LL}).ok());
    EXPECT_TRUE(at.AppendNull().ok());

    arrow::DurationBuilder took(dur_type, Pool());
    EXPECT_TRUE(took.AppendValues({0, -1}).ok());
    EXPECT_TRUE(took.AppendNull().ok());

    auto schema = arrow::schema({arrow::field("at", ts_type), arrow::field("took", dur_type)});
    return {"temporal", arrow::RecordBatch::Make(schema, 3, {Finish(at), Finish(took)})};
}

/// A nested message: set, present-but-all-null, and null. The middle row is the
/// one that separates "the struct is absent" from "the struct is here and its
/// fields are absent" — two different byte sequences and two different meanings.
Fixture Nested() {
    auto inner =
        arrow::struct_({arrow::field("id", arrow::int32()), arrow::field("label", arrow::utf8())});

    arrow::StructBuilder who(
        inner, Pool(),
        {std::make_shared<arrow::Int32Builder>(), std::make_shared<arrow::StringBuilder>()});
    auto* id = static_cast<arrow::Int32Builder*>(who.field_builder(0));
    auto* label = static_cast<arrow::StringBuilder*>(who.field_builder(1));

    EXPECT_TRUE(who.Append().ok());
    EXPECT_TRUE(id->Append(7).ok());
    EXPECT_TRUE(label->Append("a").ok());

    EXPECT_TRUE(who.Append().ok());
    EXPECT_TRUE(id->AppendNull().ok());
    EXPECT_TRUE(label->AppendNull().ok());

    EXPECT_TRUE(who.AppendNull().ok());

    auto schema = arrow::schema({arrow::field("who", inner)});
    return {"nested", arrow::RecordBatch::Make(schema, 3, {Finish(who)})};
}

/// The three composite constructs the mapping produces: repeated scalar,
/// repeated message, and a map. Each carries a populated row, an EMPTY row and a
/// null row, because empty and null are different bytes and a count prefix is
/// exactly where that goes wrong.
Fixture Composites() {
    arrow::ListBuilder readings(Pool(), std::make_shared<arrow::Int32Builder>());
    auto* reading = static_cast<arrow::Int32Builder*>(readings.value_builder());
    EXPECT_TRUE(readings.Append().ok());
    EXPECT_TRUE(reading->AppendValues({1, 2, 3}).ok());
    EXPECT_TRUE(readings.Append().ok());
    EXPECT_TRUE(readings.AppendNull().ok());

    auto point_type = arrow::struct_({arrow::field("x", arrow::int32())});
    auto point_builder = std::make_shared<arrow::StructBuilder>(
        point_type, Pool(),
        std::vector<std::shared_ptr<arrow::ArrayBuilder>>{std::make_shared<arrow::Int32Builder>()});
    arrow::ListBuilder points(Pool(), point_builder);
    auto* point = static_cast<arrow::StructBuilder*>(points.value_builder());
    auto* x = static_cast<arrow::Int32Builder*>(point->field_builder(0));
    EXPECT_TRUE(points.Append().ok());
    EXPECT_TRUE(point->Append().ok());
    EXPECT_TRUE(x->Append(1).ok());
    EXPECT_TRUE(point->AppendNull().ok());  // a null ELEMENT inside a list
    EXPECT_TRUE(points.Append().ok());
    EXPECT_TRUE(points.AppendNull().ok());

    arrow::MapBuilder tags(Pool(), std::make_shared<arrow::StringBuilder>(),
                           std::make_shared<arrow::Int32Builder>());
    auto* key = static_cast<arrow::StringBuilder*>(tags.key_builder());
    auto* value = static_cast<arrow::Int32Builder*>(tags.item_builder());
    EXPECT_TRUE(tags.Append().ok());
    EXPECT_TRUE(key->Append("a").ok());
    EXPECT_TRUE(value->Append(1).ok());
    EXPECT_TRUE(key->Append("b").ok());
    EXPECT_TRUE(value->AppendNull().ok());  // a null VALUE, which has its own bitfield
    EXPECT_TRUE(tags.Append().ok());
    EXPECT_TRUE(tags.AppendNull().ok());

    auto schema = arrow::schema({
        arrow::field("readings", arrow::list(arrow::int32())),
        arrow::field("points", arrow::list(point_type)),
        arrow::field("tags", arrow::map(arrow::utf8(), arrow::int32())),
    });
    return {"composites",
            arrow::RecordBatch::Make(schema, 3, {Finish(readings), Finish(points), Finish(tags)})};
}

std::vector<Fixture> Corpus() { return {Scalars(), Temporal(), Nested(), Composites()}; }

/// The oracle: `arrow-bridge`'s codec, over the same row.
std::vector<uint8_t> OracleEncode(const arrow::RecordBatch& batch, int64_t row) {
    fletcher::Codec codec(batch.schema());
    fletcher::ArrowRow values;
    values.reserve(static_cast<size_t>(batch.num_columns()));
    for (int c = 0; c < batch.num_columns(); ++c) {
        auto scalar = batch.column(c)->GetScalar(row);
        EXPECT_TRUE(scalar.ok()) << scalar.status().ToString();
        values.push_back(scalar.ValueOrDie());
    }
    return codec.EncodeRow(values);
}

/// The subject: the nanoarrow codec, over the batch exported across the C Data
/// Interface — which is exactly how a binding hands it over.
std::vector<uint8_t> SubjectEncode(const arrow::RecordBatch& batch, int64_t row) {
    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    const arrow::Status exported = arrow::ExportRecordBatch(batch, &c_array, &c_schema);
    EXPECT_TRUE(exported.ok()) << exported.ToString();

    std::vector<uint8_t> bytes;
    {
        NanoarrowCodec codec(c_schema);
        BoundRows rows(codec, c_array);
        fletcher::VectorWriteBuffer buffer;
        codec.EncodeRow(rows, row, buffer);
        bytes = buffer.Finish();
    }

    // The borrow rule, standing guard: the codec never consumed the export, so
    // both structures are still ours to release. Had BoundRows called the
    // array's release callback, this would be a double free.
    if (c_array.release != nullptr) c_array.release(&c_array);
    if (c_schema.release != nullptr) c_schema.release(&c_schema);
    return bytes;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

TEST(NanoarrowCodec, ByteIdenticalToArrowBridge) {
    const std::vector<Fixture> corpus = Corpus();
    ASSERT_FALSE(corpus.empty()) << "the corpus is empty, so this row proves nothing";

    int compared = 0;
    for (const Fixture& fixture : corpus) {
        // A fixture whose builders failed would have zero rows, and the loop
        // below would then compare nothing and pass. The vacuity is the thing to
        // guard: a green row that ran no comparison looks exactly like a green
        // row that ran every one.
        ASSERT_GT(fixture.batch->num_rows(), 0) << "fixture '" << fixture.name << "' built no rows";

        for (int64_t row = 0; row < fixture.batch->num_rows(); ++row) {
            const std::vector<uint8_t> expected = OracleEncode(*fixture.batch, row);
            const std::vector<uint8_t> actual = SubjectEncode(*fixture.batch, row);

            ASSERT_FALSE(expected.empty())
                << "fixture '" << fixture.name << "', row " << row
                << ": the ORACLE produced no bytes, so equality here would mean nothing";
            ++compared;

            // Compared as hex so a failure prints the bytes rather than a
            // container's address, and the first differing nibble is findable.
            ASSERT_EQ(Hex(expected), Hex(actual))
                << "fixture '" << fixture.name << "', row " << row
                << ": the nanoarrow codec and arrow-bridge's codec disagree about the wire "
                   "format. One wire format means these two are never allowed to differ by a "
                   "byte - a difference here is a subscriber decoding a publisher's bytes into "
                   "the wrong fields, silently.";
        }
    }

    // The corpus is the coverage claim, so its size is stated rather than
    // implied: four fixtures of three rows each.
    EXPECT_EQ(compared, 12) << "the corpus changed size; update this count deliberately";
}

/// The borrow rule, as its own row rather than as a side effect of the one above.
///
/// `fl_rows_bind` promises the array is borrowed and never consumed: that is what
/// lets ONE export serve N publishes. If the codec ever called the release
/// callback, the pointer would be nulled and this row says so directly.
TEST(NanoarrowCodec, BindBorrowsTheArrayAndNeverConsumesIt) {
    const Fixture fixture = Scalars();

    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    const arrow::Status exported = arrow::ExportRecordBatch(*fixture.batch, &c_array, &c_schema);
    ASSERT_TRUE(exported.ok()) << exported.ToString();

    {
        NanoarrowCodec codec(c_schema);
        BoundRows rows(codec, c_array);
        fletcher::VectorWriteBuffer buffer;
        for (int64_t row = 0; row < fixture.batch->num_rows(); ++row) {
            codec.EncodeRow(rows, row, buffer);
        }
        EXPECT_NE(c_array.release, nullptr)
            << "the array was released while still bound - one export must serve N publishes";
    }

    EXPECT_NE(c_array.release, nullptr)
        << "unbinding released the caller's array; the ABI promises the caller releases it";
    EXPECT_NE(c_schema.release, nullptr) << "the schema export was consumed; it is deep-copied";

    c_array.release(&c_array);
    c_schema.release(&c_schema);
}

/// Refusals name the field. A schema the wire format cannot carry is refused at
/// OPEN — before a single row is bound — and the message says which field, because
/// "dictionary fields are not supported" without a field name costs its reader an
/// afternoon on a wide schema.
TEST(NanoarrowCodec, RefusesAnUnsupportedTypeNamingTheField) {
    auto schema =
        arrow::schema({arrow::field("id", arrow::int32()),
                       arrow::field("category", arrow::dictionary(arrow::int32(), arrow::utf8()))});
    auto batch = arrow::RecordBatch::Make(
        schema, 0,
        {arrow::MakeArrayOfNull(arrow::int32(), 0).ValueOrDie(),
         arrow::MakeArrayOfNull(arrow::dictionary(arrow::int32(), arrow::utf8()), 0).ValueOrDie()});

    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    ASSERT_TRUE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

    try {
        NanoarrowCodec codec(c_schema);
        ADD_FAILURE() << "a dictionary column was accepted; the wire format does not carry one";
    } catch (const fletcher::PubSubError& e) {
        EXPECT_EQ(e.status(), fletcher::PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("category"), std::string::npos)
            << "the refusal did not name the offending field: " << e.what();
    }

    c_array.release(&c_array);
    c_schema.release(&c_schema);
}

}  // namespace
