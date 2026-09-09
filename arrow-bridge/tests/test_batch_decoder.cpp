// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/arrow_bridge/batch_decoder.hpp>
#include <fletcher/arrow_bridge/codec.hpp>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helper: encode every row with Codec::EncodeRow, Append it into a fresh
// BatchDecoder, Finish, and check every (row, column) scalar against the
// Codec::DecodeRow oracle for that same row's bytes. This is the differential
// test the whole suite leans on: BatchDecoder must decode exactly what
// Codec::DecodeRow decodes, just into columns instead of an ArrowRow.
// ---------------------------------------------------------------------------

namespace {

void RunShapeTest(const std::shared_ptr<arrow::Schema>& schema,
                  const std::vector<fletcher::ArrowRow>& rows) {
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);

    std::vector<fletcher::ArrowRow> oracle;
    oracle.reserve(rows.size());
    for (const auto& row : rows) {
        auto bytes = codec.EncodeRow(row);
        decoder.Append(bytes.data(), bytes.size());
        oracle.push_back(codec.DecodeRow(bytes));
    }
    EXPECT_EQ(decoder.num_rows(), static_cast<int64_t>(rows.size()));

    auto batch = decoder.Finish();
    ASSERT_TRUE(batch->ValidateFull().ok()) << batch->ValidateFull().ToString();
    EXPECT_TRUE(batch->schema()->Equals(*schema));
    ASSERT_EQ(batch->num_rows(), static_cast<int64_t>(rows.size()));

    for (size_t r = 0; r < rows.size(); ++r) {
        for (int c = 0; c < batch->num_columns(); ++c) {
            auto got = batch->column(c)->GetScalar(static_cast<int64_t>(r)).ValueOrDie();
            // Codec::DecodeRow yields a dictionary field's plain value (see codec.hpp);
            // BatchDecoder instead hands back a real dictionary column, so resolve the index before
            // comparing.
            if (got->type->id() == arrow::Type::DICTIONARY)
                got = static_cast<const arrow::DictionaryScalar&>(*got)
                          .GetEncodedValue()
                          .ValueOrDie();
            EXPECT_TRUE(got->Equals(*oracle[r][static_cast<size_t>(c)]))
                << "row " << r << " col " << c << ": got " << got->ToString() << ", want "
                << oracle[r][static_cast<size_t>(c)]->ToString();
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Every shape Step 1's oracle tests exercise, decoded through BatchDecoder
//    instead of Codec::DecodeRow.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, MatchesDecodeRowForEveryShape) {
    // --- Scalars (no nulls). ---
    {
        auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
        auto dur_type = arrow::duration(arrow::TimeUnit::NANO);
        auto schema = arrow::schema({
            arrow::field("b", arrow::boolean(), true),
            arrow::field("i32", arrow::int32(), true),
            arrow::field("i64", arrow::int64(), true),
            arrow::field("u32", arrow::uint32(), true),
            arrow::field("f32", arrow::float32(), true),
            arrow::field("f64", arrow::float64(), true),
            arrow::field("s", arrow::utf8(), true),
            arrow::field("ts", ts_type, true),
            arrow::field("dur", dur_type, true),
            arrow::field("bin", arrow::binary(), true),
        });
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            std::string bin(4, static_cast<char>(0));
            for (int b = 0; b < 4; ++b)
                bin[static_cast<size_t>(b)] = static_cast<char>((i + b) & 0xFF);
            rows.push_back({
                std::make_shared<arrow::BooleanScalar>(i % 2 == 0),
                std::make_shared<arrow::Int32Scalar>(i * 7 - 3),
                std::make_shared<arrow::Int64Scalar>(static_cast<int64_t>(i) * 1'000'000'000LL),
                std::make_shared<arrow::UInt32Scalar>(static_cast<uint32_t>(i) * 42u),
                std::make_shared<arrow::FloatScalar>(static_cast<float>(i) * 1.5f),
                std::make_shared<arrow::DoubleScalar>(static_cast<double>(i) * 2.25),
                std::make_shared<arrow::StringScalar>("row" + std::to_string(i)),
                std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(i) * 123456789LL,
                                                         ts_type),
                std::make_shared<arrow::DurationScalar>(static_cast<int64_t>(i) * 99LL, dur_type),
                std::make_shared<arrow::BinaryScalar>(arrow::Buffer::FromString(bin)),
            });
        }
        RunShapeTest(schema, rows);

        // --- Nullable: the same schema, one field nulled per row in turn. ---
        std::vector<fletcher::ArrowRow> nullable_rows = rows;
        for (size_t i = 0; i < nullable_rows.size(); ++i) {
            size_t field = i % nullable_rows[i].size();
            nullable_rows[i][field] = arrow::MakeNullScalar(nullable_rows[i][field]->type);
        }
        RunShapeTest(schema, nullable_rows);
    }

    // --- Cloud: timestamp + list<float32> + list<uint32>, varying point counts. ---
    {
        auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
        auto schema = arrow::schema({
            arrow::field("t", ts_type, true),
            arrow::field("xs", arrow::list(arrow::float32()), true),
            arrow::field("ys", arrow::list(arrow::uint32()), true),
        });
        std::vector<fletcher::ArrowRow> rows;
        const int counts[5] = {0, 1, 10, 2667, 5};
        for (int i = 0; i < 5; ++i) {
            arrow::FloatBuilder xb;
            arrow::UInt32Builder yb;
            for (int j = 0; j < counts[i]; ++j) {
                ASSERT_TRUE(xb.Append(static_cast<float>(j) * 0.5f).ok());
                ASSERT_TRUE(yb.Append(static_cast<uint32_t>(j)).ok());
            }
            auto xs_arr = xb.Finish().ValueOrDie();
            auto ys_arr = yb.Finish().ValueOrDie();
            rows.push_back({
                std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(i) * 111LL, ts_type),
                std::make_shared<arrow::ListScalar>(xs_arr, schema->field(1)->type()),
                std::make_shared<arrow::ListScalar>(ys_arr, schema->field(2)->type()),
            });
        }
        RunShapeTest(schema, rows);
    }

    // --- Pose: struct{list<double>} x2. ---
    {
        auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
        auto matrix_type = arrow::struct_({arrow::field("m", arrow::list(arrow::float64()), true)});
        auto cov_type = arrow::struct_({arrow::field("c", arrow::list(arrow::float64()), true)});
        auto schema = arrow::schema({
            arrow::field("t", ts_type, true),
            arrow::field("orientation", matrix_type, true),
            arrow::field("covariance", cov_type, true),
        });
        auto matrix_list_type =
            static_cast<const arrow::StructType&>(*matrix_type).field(0)->type();
        auto cov_list_type = static_cast<const arrow::StructType&>(*cov_type).field(0)->type();
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            std::vector<double> matrix(16), cov(6);
            for (int k = 0; k < 16; ++k) matrix[static_cast<size_t>(k)] = k * 1.5 + i;
            for (int k = 0; k < 6; ++k) cov[static_cast<size_t>(k)] = k + 0.25 + i;
            arrow::DoubleBuilder mb, cb;
            ASSERT_TRUE(mb.AppendValues(matrix).ok());
            ASSERT_TRUE(cb.AppendValues(cov).ok());
            auto orientation = std::make_shared<arrow::StructScalar>(
                arrow::ScalarVector{std::make_shared<arrow::ListScalar>(mb.Finish().ValueOrDie(),
                                                                        matrix_list_type)},
                matrix_type);
            auto covariance = std::make_shared<arrow::StructScalar>(
                arrow::ScalarVector{
                    std::make_shared<arrow::ListScalar>(cb.Finish().ValueOrDie(), cov_list_type)},
                cov_type);
            rows.push_back({
                std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(i) * 55LL, ts_type),
                orientation,
                covariance,
            });
        }
        RunShapeTest(schema, rows);
    }

    // --- Points: list<struct<x,y,z:double>>, varying counts incl. empty. ---
    {
        auto point_type = arrow::struct_({
            arrow::field("x", arrow::float64(), true),
            arrow::field("y", arrow::float64(), true),
            arrow::field("z", arrow::float64(), true),
        });
        auto list_type = arrow::list(point_type);
        auto schema = arrow::schema({arrow::field("points", list_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        const int counts[5] = {0, 1, 3, 5, 2};
        for (int i = 0; i < 5; ++i) {
            arrow::DoubleBuilder xb, yb, zb;
            for (int j = 0; j < counts[i]; ++j) {
                ASSERT_TRUE(xb.Append(j + i).ok());
                ASSERT_TRUE(yb.Append(-(j + i)).ok());
                ASSERT_TRUE(zb.Append(j * 0.5).ok());
            }
            auto structs =
                arrow::StructArray::Make(
                    {xb.Finish().ValueOrDie(), yb.Finish().ValueOrDie(), zb.Finish().ValueOrDie()},
                    point_type->fields())
                    .ValueOrDie();
            rows.push_back({std::make_shared<arrow::ListScalar>(structs, list_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- Nested: list<list<struct<x,y>>>, varying outer/inner counts incl. empty. ---
    {
        auto xy_type = arrow::struct_(
            {arrow::field("x", arrow::float64(), true), arrow::field("y", arrow::float64(), true)});
        auto inner_list_type = arrow::list(xy_type);
        auto outer_list_type = arrow::list(inner_list_type);
        auto schema = arrow::schema({arrow::field("nested", outer_list_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            arrow::DoubleBuilder xb, yb;
            std::vector<int32_t> offsets = {0};
            int32_t total = 0;
            for (int o = 0; o < i; ++o) {
                for (int j = 0; j <= i; ++j) {
                    ASSERT_TRUE(xb.Append(o * 10 + j).ok());
                    ASSERT_TRUE(yb.Append(-(o * 10 + j)).ok());
                    ++total;
                }
                offsets.push_back(total);
            }
            auto structs =
                arrow::StructArray::Make({xb.Finish().ValueOrDie(), yb.Finish().ValueOrDie()},
                                         xy_type->fields())
                    .ValueOrDie();
            arrow::Int32Builder offsets_b;
            ASSERT_TRUE(offsets_b.AppendValues(offsets).ok());
            auto offsets_arr = offsets_b.Finish().ValueOrDie();
            auto inner_list_arr = arrow::ListArray::FromArrays(*offsets_arr, *structs).ValueOrDie();
            rows.push_back({std::make_shared<arrow::ListScalar>(inner_list_arr, outer_list_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- Map<utf8, int32>, varying entry counts incl. empty. ---
    {
        auto map_type = arrow::map(arrow::utf8(), arrow::int32());
        auto schema = arrow::schema({arrow::field("m", map_type, true)});
        const auto& mt = static_cast<const arrow::MapType&>(*map_type);
        std::vector<fletcher::ArrowRow> rows;
        const int counts[5] = {0, 1, 3, 2, 4};
        for (int i = 0; i < 5; ++i) {
            arrow::StringBuilder kb;
            arrow::Int32Builder vb;
            for (int j = 0; j < counts[i]; ++j) {
                ASSERT_TRUE(kb.Append("k" + std::to_string(i) + "_" + std::to_string(j)).ok());
                ASSERT_TRUE(vb.Append(i * 100 + j).ok());
            }
            auto entries =
                arrow::StructArray::Make({kb.Finish().ValueOrDie(), vb.Finish().ValueOrDie()},
                                         mt.value_type()->fields())
                    .ValueOrDie();
            rows.push_back({std::make_shared<arrow::MapScalar>(entries, map_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- struct{struct{int32,utf8}}. ---
    {
        auto leaf_type = arrow::struct_(
            {arrow::field("a", arrow::int32(), true), arrow::field("b", arrow::utf8(), true)});
        auto outer_type = arrow::struct_({arrow::field("inner", leaf_type, true)});
        auto schema = arrow::schema({arrow::field("s", outer_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            auto leaf = std::make_shared<arrow::StructScalar>(
                arrow::ScalarVector{
                    std::make_shared<arrow::Int32Scalar>(i),
                    std::make_shared<arrow::StringScalar>("leaf" + std::to_string(i))},
                leaf_type);
            rows.push_back(
                {std::make_shared<arrow::StructScalar>(arrow::ScalarVector{leaf}, outer_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- fixed_size_list<float32, 3>. ---
    {
        auto fsl_type = arrow::fixed_size_list(arrow::float32(), 3);
        auto schema = arrow::schema({arrow::field("v", fsl_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            arrow::FloatBuilder fb;
            ASSERT_TRUE(fb.AppendValues({static_cast<float>(i), static_cast<float>(i) * 2,
                                         static_cast<float>(i) * 3})
                            .ok());
            rows.push_back(
                {std::make_shared<arrow::FixedSizeListScalar>(fb.Finish().ValueOrDie(), fsl_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- decimal128. ---
    {
        auto dec_type = arrow::decimal128(10, 2);
        auto schema = arrow::schema({arrow::field("d", dec_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i)
            rows.push_back({std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(i * 12345),
                                                                      dec_type)});
        RunShapeTest(schema, rows);
    }

    // --- Intervals: months, day_time, month_day_nano. ---
    {
        auto schema = arrow::schema({
            arrow::field("months", arrow::month_interval(), true),
            arrow::field("daytime", arrow::day_time_interval(), true),
            arrow::field("mdn", arrow::month_day_nano_interval(), true),
        });
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            rows.push_back({
                std::make_shared<arrow::MonthIntervalScalar>(i),
                std::make_shared<arrow::DayTimeIntervalScalar>(
                    arrow::DayTimeIntervalType::DayMilliseconds{i, i * 100}),
                std::make_shared<arrow::MonthDayNanoIntervalScalar>(
                    arrow::MonthDayNanoIntervalType::MonthDayNanos{i, i * 2,
                                                                   static_cast<int64_t>(i) * 1000}),
            });
        }
        RunShapeTest(schema, rows);
    }

    // --- Dense union, alternating variants. ---
    {
        auto union_type = arrow::dense_union(
            {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
        auto schema = arrow::schema({arrow::field("u", union_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            if (i % 2 == 0)
                rows.push_back({std::make_shared<arrow::DenseUnionScalar>(
                    std::make_shared<arrow::Int32Scalar>(i), 0, union_type)});
            else
                rows.push_back({std::make_shared<arrow::DenseUnionScalar>(
                    std::make_shared<arrow::StringScalar>("u" + std::to_string(i)), 1,
                    union_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- Sparse union, alternating variants. ---
    {
        auto union_type = arrow::sparse_union(
            {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
        auto schema = arrow::schema({arrow::field("u", union_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i) {
            if (i % 2 == 0)
                rows.push_back({arrow::SparseUnionScalar::FromValue(
                    std::make_shared<arrow::Int32Scalar>(i), 0, union_type)});
            else
                rows.push_back({arrow::SparseUnionScalar::FromValue(
                    std::make_shared<arrow::StringScalar>("u" + std::to_string(i)), 1,
                    union_type)});
        }
        RunShapeTest(schema, rows);
    }

    // --- Top-level dictionary<int32, utf8>. ---
    {
        auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        auto schema = arrow::schema({arrow::field("v", dict_type, true)});
        std::vector<fletcher::ArrowRow> rows;
        for (const auto& v : {"alpha", "beta", "alpha", "gamma", "beta"})
            rows.push_back({std::make_shared<arrow::StringScalar>(v)});
        RunShapeTest(schema, rows);
    }

    // --- large_utf8. ---
    {
        auto schema = arrow::schema({arrow::field("v", arrow::large_utf8(), true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int i = 0; i < 5; ++i)
            rows.push_back(
                {std::make_shared<arrow::LargeStringScalar>("large-" + std::to_string(i))});
        RunShapeTest(schema, rows);
    }

    // --- list<timestamp>. ---
    {
        auto ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
        auto schema = arrow::schema({arrow::field("v", arrow::list(ts_type), true)});
        std::vector<fletcher::ArrowRow> rows;
        const int counts[5] = {0, 1, 4, 2, 3};
        for (int i = 0; i < 5; ++i) {
            arrow::TimestampBuilder tb(ts_type, arrow::default_memory_pool());
            for (int j = 0; j < counts[i]; ++j)
                ASSERT_TRUE(tb.Append(static_cast<int64_t>(i) * 1000 + j).ok());
            rows.push_back({std::make_shared<arrow::ListScalar>(tb.Finish().ValueOrDie(),
                                                                schema->field(0)->type())});
        }
        RunShapeTest(schema, rows);
    }
}

// ---------------------------------------------------------------------------
// 2. A malformed row leaves the decoder's row count (and, on Finish, the
//    builders) exactly as they were before the failed Append.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, MalformedRowLeavesBuildersUntouched) {
    auto list_type = arrow::list(arrow::utf8());
    auto schema = arrow::schema(
        {arrow::field("l", list_type, true), arrow::field("n", arrow::int32(), true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);

    arrow::StringBuilder sb;
    ASSERT_TRUE(sb.Append("a").ok());
    ASSERT_TRUE(sb.Append("bb").ok());
    fletcher::ArrowRow good_row = {
        std::make_shared<arrow::ListScalar>(sb.Finish().ValueOrDie(), list_type),
        std::make_shared<arrow::Int32Scalar>(5),
    };
    auto good_bytes = codec.EncodeRow(good_row);

    decoder.Append(good_bytes.data(), good_bytes.size());
    EXPECT_EQ(decoder.num_rows(), 1);

    // Truncated in the middle of the second string's payload.
    std::vector<uint8_t> truncated(good_bytes.begin(), good_bytes.end() - 1);
    EXPECT_THROW(decoder.Append(truncated.data(), truncated.size()), std::invalid_argument);
    EXPECT_EQ(decoder.num_rows(), 1);

    decoder.Append(good_bytes.data(), good_bytes.size());
    EXPECT_EQ(decoder.num_rows(), 2);

    auto batch = decoder.Finish();
    ASSERT_TRUE(batch->ValidateFull().ok());
    ASSERT_EQ(batch->num_rows(), 2);
    auto oracle = codec.DecodeRow(good_bytes);
    for (int64_t row = 0; row < 2; ++row) {
        for (int c = 0; c < batch->num_columns(); ++c) {
            auto got = batch->column(c)->GetScalar(row).ValueOrDie();
            EXPECT_TRUE(got->Equals(*oracle[static_cast<size_t>(c)]));
        }
    }
}

TEST(BatchDecoderTest, MalformedListCountLeavesStateUnchanged) {
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::int32()), true)});
    fletcher::BatchDecoder decoder(schema);
    const std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(decoder.Append(buf.data(), buf.size()), std::invalid_argument);
    EXPECT_EQ(decoder.num_rows(), 0);
}

TEST(BatchDecoderTest, MalformedMapCountLeavesStateUnchanged) {
    auto schema =
        arrow::schema({arrow::field("v", arrow::map(arrow::utf8(), arrow::int32()), true)});
    fletcher::BatchDecoder decoder(schema);
    const std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(decoder.Append(buf.data(), buf.size()), std::invalid_argument);
    EXPECT_EQ(decoder.num_rows(), 0);
}

TEST(BatchDecoderTest, MalformedUnionCodeLeavesStateUnchanged) {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type, true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);
    auto bytes = codec.EncodeRow({std::make_shared<arrow::DenseUnionScalar>(
        std::make_shared<arrow::Int32Scalar>(1), 0, union_type)});
    ASSERT_GT(bytes.size(), 1u);
    bytes[1] = 100;  // byte 0 is the row bitfield, byte 1 is the union type_code
    EXPECT_THROW(decoder.Append(bytes.data(), bytes.size()), std::invalid_argument);
    EXPECT_EQ(decoder.num_rows(), 0);
}

TEST(BatchDecoderTest, TrailingByteLeavesStateUnchanged) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32(), true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);
    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(7)});
    row.push_back(0xAB);
    EXPECT_THROW(decoder.Append(row.data(), row.size()), std::invalid_argument);
    EXPECT_EQ(decoder.num_rows(), 0);
}

TEST(BatchDecoderTest, ListPayloadShorterThanCountLeavesStateUnchanged) {
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::float32()), true)});
    fletcher::BatchDecoder decoder(schema);
    // COUNT says 1000, an all-valid element bitfield follows, but only 10 floats' worth of payload
    // (40 bytes) are actually present.
    std::vector<uint8_t> buf = {0x00};  // row bitfield: field present
    const uint32_t count = 1000;
    const auto* count_bytes = reinterpret_cast<const uint8_t*>(&count);
    buf.insert(buf.end(), count_bytes, count_bytes + sizeof(count));
    buf.resize(buf.size() + (static_cast<size_t>(count) + 7) / 8,
               0);                                   // element null bitfield: all valid
    buf.resize(buf.size() + 10 * sizeof(float), 0);  // only 10 floats present
    EXPECT_THROW(decoder.Append(buf.data(), buf.size()), std::invalid_argument);
    EXPECT_EQ(decoder.num_rows(), 0);
}

// ---------------------------------------------------------------------------
// 3. Empty and all-null lists.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, EmptyAndAllNullLists) {
    auto list_type = arrow::list(arrow::int32());
    auto schema = arrow::schema({arrow::field("v", list_type, true)});
    std::vector<fletcher::ArrowRow> rows;

    {
        arrow::Int32Builder b;
        rows.push_back({std::make_shared<arrow::ListScalar>(b.Finish().ValueOrDie(), list_type)});
    }
    {
        arrow::Int32Builder b;
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(b.AppendNull().ok());
        rows.push_back({std::make_shared<arrow::ListScalar>(b.Finish().ValueOrDie(), list_type)});
    }
    {
        arrow::Int32Builder b;
        ASSERT_TRUE(b.Append(1).ok());
        ASSERT_TRUE(b.AppendNull().ok());
        ASSERT_TRUE(b.Append(3).ok());
        rows.push_back({std::make_shared<arrow::ListScalar>(b.Finish().ValueOrDie(), list_type)});
    }
    {
        arrow::Int32Builder b;
        for (int i = 0; i < 20; ++i) ASSERT_TRUE(b.AppendNull().ok());
        rows.push_back({std::make_shared<arrow::ListScalar>(b.Finish().ValueOrDie(), list_type)});
    }

    RunShapeTest(schema, rows);
}

// ---------------------------------------------------------------------------
// 4. A null slot at every nesting level BatchDecoder handles.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, NullSlotsAtEveryLevel) {
    auto struct_ab = arrow::struct_(
        {arrow::field("a", arrow::int32(), true), arrow::field("b", arrow::utf8(), true)});
    auto list_of_struct = arrow::list(struct_ab);
    auto struct_with_list =
        arrow::struct_({arrow::field("inner", arrow::list(arrow::int32()), true)});
    auto map_type = arrow::map(arrow::utf8(), arrow::int32());
    auto fsl_type = arrow::fixed_size_list(arrow::int32(), 3);
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto top_struct_type = arrow::struct_({arrow::field("x", arrow::int32(), true)});

    auto schema = arrow::schema({
        arrow::field("list_of_struct", list_of_struct, true),
        arrow::field("struct_with_list", struct_with_list, true),
        arrow::field("m", map_type, true),
        arrow::field("fsl", fsl_type, true),
        arrow::field("u", union_type, true),
        arrow::field("top_struct", top_struct_type, true),
    });

    // Null struct element inside a list: [struct(1,"x"), null, struct(2,"y")].
    arrow::StructBuilder sb(
        struct_ab, arrow::default_memory_pool(),
        std::vector<std::shared_ptr<arrow::ArrayBuilder>>{
            std::make_shared<arrow::Int32Builder>(), std::make_shared<arrow::StringBuilder>()});
    auto* ab = static_cast<arrow::Int32Builder*>(sb.field_builder(0));
    auto* bb = static_cast<arrow::StringBuilder*>(sb.field_builder(1));
    ASSERT_TRUE(sb.Append().ok());
    ASSERT_TRUE(ab->Append(1).ok());
    ASSERT_TRUE(bb->Append("x").ok());
    ASSERT_TRUE(sb.AppendNull().ok());
    ASSERT_TRUE(sb.Append().ok());
    ASSERT_TRUE(ab->Append(2).ok());
    ASSERT_TRUE(bb->Append("y").ok());
    std::shared_ptr<arrow::Array> struct_arr;
    ASSERT_TRUE(sb.Finish(&struct_arr).ok());
    auto list_scalar = std::make_shared<arrow::ListScalar>(struct_arr, list_of_struct);

    // Null list inside a struct.
    auto struct_with_null_list = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{arrow::MakeNullScalar(arrow::list(arrow::int32()))}, struct_with_list);

    // Null map item.
    arrow::StringBuilder kb;
    arrow::Int32Builder vb;
    ASSERT_TRUE(kb.Append("k0").ok());
    ASSERT_TRUE(vb.AppendNull().ok());
    ASSERT_TRUE(kb.Append("k1").ok());
    ASSERT_TRUE(vb.Append(9).ok());
    const auto& mt = static_cast<const arrow::MapType&>(*map_type);
    auto entries = arrow::StructArray::Make({kb.Finish().ValueOrDie(), vb.Finish().ValueOrDie()},
                                            mt.value_type()->fields())
                       .ValueOrDie();
    auto map_scalar = std::make_shared<arrow::MapScalar>(entries, map_type);

    arrow::Int32Builder fb;
    ASSERT_TRUE(fb.AppendValues({10, 20, 30}).ok());
    auto fsl_scalar =
        std::make_shared<arrow::FixedSizeListScalar>(fb.Finish().ValueOrDie(), fsl_type);

    auto union_scalar = std::make_shared<arrow::DenseUnionScalar>(
        std::make_shared<arrow::Int32Scalar>(42), 0, union_type);

    auto top_struct_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{std::make_shared<arrow::Int32Scalar>(1)}, top_struct_type);

    fletcher::ArrowRow row0 = {list_scalar, struct_with_null_list, map_scalar,
                               fsl_scalar,  union_scalar,          top_struct_scalar};

    // Row 1: null fixed_size_list, null union slot, and null top-level struct.
    fletcher::ArrowRow row1 = {
        list_scalar,
        struct_with_null_list,
        map_scalar,
        arrow::MakeNullScalar(fsl_type),
        arrow::MakeNullScalar(union_type),
        arrow::MakeNullScalar(top_struct_type),
    };

    RunShapeTest(schema, {row0, row1});
}

// ---------------------------------------------------------------------------
// 5. Degenerate schemas: a zero-field struct, and Finish() with no rows.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, ZeroFieldStruct) {
    auto empty_struct_type = arrow::struct_({});
    auto schema = arrow::schema(
        {arrow::field("s", empty_struct_type, true), arrow::field("n", arrow::int32(), true)});
    fletcher::ArrowRow row = {
        std::make_shared<arrow::StructScalar>(arrow::ScalarVector{}, empty_struct_type),
        std::make_shared<arrow::Int32Scalar>(3),
    };
    RunShapeTest(schema, {row});
}

TEST(BatchDecoderTest, ZeroRowFinishIsValid) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32(), true)});
    fletcher::BatchDecoder decoder(schema);

    auto batch = decoder.Finish();
    ASSERT_TRUE(batch->ValidateFull().ok());
    EXPECT_EQ(batch->num_rows(), 0);

    fletcher::Codec codec(schema);
    auto bytes = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(9)});
    decoder.Append(bytes.data(), bytes.size());
    EXPECT_EQ(decoder.num_rows(), 1);
    auto batch2 = decoder.Finish();
    ASSERT_TRUE(batch2->ValidateFull().ok());
    EXPECT_EQ(batch2->num_rows(), 1);
}

// ---------------------------------------------------------------------------
// 6. Finish() resets the decoder for reuse.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, ReuseAfterFinish) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32(), true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);

    for (int i = 0; i < 3; ++i) {
        auto bytes = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(i)});
        decoder.Append(bytes.data(), bytes.size());
    }
    auto batch1 = decoder.Finish();
    ASSERT_TRUE(batch1->ValidateFull().ok());
    EXPECT_EQ(batch1->num_rows(), 3);

    for (int i = 10; i < 12; ++i) {
        auto bytes = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(i)});
        decoder.Append(bytes.data(), bytes.size());
    }
    auto batch2 = decoder.Finish();
    ASSERT_TRUE(batch2->ValidateFull().ok());
    ASSERT_EQ(batch2->num_rows(), 2);
    auto col = std::static_pointer_cast<arrow::Int32Array>(batch2->column(0));
    EXPECT_EQ(col->Value(0), 10);
    EXPECT_EQ(col->Value(1), 11);
}

// ---------------------------------------------------------------------------
// 7. Timestamp timezone metadata survives Finish().
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, TimestampTimezonePreserved) {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO, "UTC");
    auto schema = arrow::schema({arrow::field("t", ts_type, true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);
    auto bytes = codec.EncodeRow({std::make_shared<arrow::TimestampScalar>(123456789LL, ts_type)});
    decoder.Append(bytes.data(), bytes.size());
    auto batch = decoder.Finish();
    ASSERT_TRUE(batch->ValidateFull().ok());
    EXPECT_TRUE(batch->column(0)->type()->Equals(*ts_type));
}

// ---------------------------------------------------------------------------
// 8. A sparse union keeps every child the same length; inactive slots are
//    null.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, SparseUnionInactiveChildrenAreNull) {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type, true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);

    for (int i = 0; i < 4; ++i) {
        std::shared_ptr<arrow::Scalar> scalar =
            (i % 2 == 0) ? arrow::SparseUnionScalar::FromValue(
                               std::make_shared<arrow::Int32Scalar>(i), 0, union_type)
                         : arrow::SparseUnionScalar::FromValue(
                               std::make_shared<arrow::StringScalar>("s" + std::to_string(i)), 1,
                               union_type);
        auto bytes = codec.EncodeRow({scalar});
        decoder.Append(bytes.data(), bytes.size());
    }
    auto batch = decoder.Finish();
    ASSERT_TRUE(batch->ValidateFull().ok());
    auto union_arr = std::static_pointer_cast<arrow::SparseUnionArray>(batch->column(0));
    ASSERT_EQ(union_arr->length(), 4);
    auto i_child = union_arr->field(0);
    auto s_child = union_arr->field(1);
    ASSERT_EQ(i_child->length(), 4);
    ASSERT_EQ(s_child->length(), 4);
    for (int i = 0; i < 4; ++i) {
        if (i % 2 == 0) {
            EXPECT_FALSE(i_child->IsNull(i));
            EXPECT_TRUE(s_child->IsNull(i));
        } else {
            EXPECT_TRUE(i_child->IsNull(i));
            EXPECT_FALSE(s_child->IsNull(i));
        }
    }
}

// ---------------------------------------------------------------------------
// 9. Top-level dictionary fields are re-folded on Finish().
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, TopLevelDictionaryRefolded) {
    for (const auto& index_type : {arrow::int8(), arrow::int16(), arrow::int32(), arrow::int64()}) {
        auto dict_type = arrow::dictionary(index_type, arrow::utf8(), /*ordered=*/true);
        auto schema = arrow::schema({arrow::field("v", dict_type, true)});
        fletcher::Codec codec(schema);
        fletcher::BatchDecoder decoder(schema);

        for (const auto& v : {"a", "b", "a", "c"}) {
            auto bytes = codec.EncodeRow({std::make_shared<arrow::StringScalar>(v)});
            decoder.Append(bytes.data(), bytes.size());
        }
        auto null_bytes = codec.EncodeRow({arrow::MakeNullScalar(arrow::utf8())});
        decoder.Append(null_bytes.data(), null_bytes.size());

        auto batch = decoder.Finish();
        ASSERT_TRUE(batch->ValidateFull().ok());
        EXPECT_TRUE(batch->column(0)->type()->Equals(*dict_type)) << dict_type->ToString();
        ASSERT_EQ(batch->column(0)->length(), 5);
        EXPECT_TRUE(batch->column(0)->IsNull(4));
        for (int i = 0; i < 4; ++i) EXPECT_FALSE(batch->column(0)->IsNull(i));
    }

    // All-null column.
    {
        auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        auto schema = arrow::schema({arrow::field("v", dict_type, true)});
        fletcher::Codec codec(schema);
        fletcher::BatchDecoder decoder(schema);
        for (int i = 0; i < 3; ++i) {
            auto bytes = codec.EncodeRow({arrow::MakeNullScalar(arrow::utf8())});
            decoder.Append(bytes.data(), bytes.size());
        }
        auto batch = decoder.Finish();
        ASSERT_TRUE(batch->ValidateFull().ok());
        ASSERT_EQ(batch->column(0)->length(), 3);
        for (int i = 0; i < 3; ++i) EXPECT_TRUE(batch->column(0)->IsNull(i));
    }

    // Zero rows: DictionaryEncode on an empty values array must not misbehave.
    {
        auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        auto schema = arrow::schema({arrow::field("v", dict_type, true)});
        fletcher::BatchDecoder decoder(schema);
        auto batch = decoder.Finish();
        ASSERT_TRUE(batch->ValidateFull().ok());
        EXPECT_EQ(batch->num_rows(), 0);
        EXPECT_TRUE(batch->column(0)->type()->Equals(*dict_type));
    }

    // A second batch after Finish() re-folds again.
    {
        auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        auto schema = arrow::schema({arrow::field("v", dict_type, true)});
        fletcher::Codec codec(schema);
        fletcher::BatchDecoder decoder(schema);
        auto bytes1 = codec.EncodeRow({std::make_shared<arrow::StringScalar>("x")});
        decoder.Append(bytes1.data(), bytes1.size());
        auto batch1 = decoder.Finish();
        ASSERT_TRUE(batch1->ValidateFull().ok());

        auto bytes2 = codec.EncodeRow({std::make_shared<arrow::StringScalar>("y")});
        decoder.Append(bytes2.data(), bytes2.size());
        auto batch2 = decoder.Finish();
        ASSERT_TRUE(batch2->ValidateFull().ok());
        ASSERT_EQ(batch2->num_rows(), 1);
        EXPECT_TRUE(batch2->column(0)->type()->Equals(*dict_type));
    }
}

// ---------------------------------------------------------------------------
// 10. Schemas BatchDecoder cannot build are rejected at construction.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, NestedDictionaryRejectedAtConstruction) {
    auto nested = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({arrow::field("v", arrow::list(nested), true)});
    EXPECT_THROW(fletcher::BatchDecoder{schema}, std::invalid_argument);
}

TEST(BatchDecoderTest, NullTypeRejectedAtConstruction) {
    auto schema = arrow::schema({arrow::field("v", arrow::null(), true)});
    EXPECT_THROW(fletcher::BatchDecoder{schema}, std::invalid_argument);
}

TEST(BatchDecoderTest, DictionaryOfHalfFloatRejectedAtConstruction) {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::float16());
    auto schema = arrow::schema({arrow::field("v", dict_type, true)});
    EXPECT_THROW(fletcher::BatchDecoder{schema}, std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 11. A capacity-exceeding row throws and leaves the batch built so far intact.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, CapacityExceededLeavesBuildersUntouched) {
    auto schema = arrow::schema({arrow::field("v", arrow::binary(), true)});
    fletcher::Codec codec(schema);
    fletcher::BatchDecoder decoder(schema);

    const std::string big(static_cast<size_t>(8) << 20,
                          'x');  // 8 MiB: ~256 rows trips the 2^31-1 budget
    auto row_bytes =
        codec.EncodeRow({std::make_shared<arrow::BinaryScalar>(arrow::Buffer::FromString(big))});

    int64_t appended = 0;
    bool threw = false;
    for (int i = 0; i < 300; ++i) {
        try {
            decoder.Append(row_bytes.data(), row_bytes.size());
            ++appended;
        } catch (const fletcher::BatchCapacityExceeded&) {
            threw = true;
            break;
        }
    }
    ASSERT_TRUE(threw);
    EXPECT_EQ(decoder.num_rows(), appended);

    auto batch = decoder.Finish();
    ASSERT_TRUE(batch->ValidateFull().ok());
    EXPECT_EQ(batch->num_rows(), appended);

    // Finish() reset the decoder: the same row Appends fine again.
    decoder.Append(row_bytes.data(), row_bytes.size());
    EXPECT_EQ(decoder.num_rows(), 1);
}

// ---------------------------------------------------------------------------
// 12. Every fixed-width run type AppendRun knows about, through BatchDecoder.
// ---------------------------------------------------------------------------

TEST(BatchDecoderTest, EveryFixedWidthRunTypeThroughAppend) {
    constexpr int kCount = 300;
    constexpr int kNullIndex = 57;

    auto check = [](const std::shared_ptr<arrow::DataType>& elem_type,
                    const std::function<std::shared_ptr<arrow::Scalar>(int)>& gen) {
        auto schema = arrow::schema({arrow::field("v", arrow::list(elem_type), true)});
        std::vector<fletcher::ArrowRow> rows;
        for (int null_index : {-1, kNullIndex}) {
            auto builder = arrow::MakeBuilder(elem_type).ValueOrDie();
            for (int i = 0; i < kCount; ++i) {
                if (i == null_index) {
                    ASSERT_TRUE(builder->AppendNull().ok());
                } else {
                    ASSERT_TRUE(builder->AppendScalar(*gen(i)).ok());
                }
            }
            std::shared_ptr<arrow::Array> arr;
            ASSERT_TRUE(builder->Finish(&arr).ok());
            rows.push_back({std::make_shared<arrow::ListScalar>(arr, arrow::list(elem_type))});
        }
        RunShapeTest(schema, rows);
    };

    check(arrow::boolean(),
          [](int i) { return std::make_shared<arrow::BooleanScalar>(i % 2 == 0); });
    check(arrow::int8(),
          [](int i) { return std::make_shared<arrow::Int8Scalar>(static_cast<int8_t>(i)); });
    check(arrow::int16(),
          [](int i) { return std::make_shared<arrow::Int16Scalar>(static_cast<int16_t>(i)); });
    check(arrow::int32(), [](int i) { return std::make_shared<arrow::Int32Scalar>(i); });
    check(arrow::int64(), [](int i) {
        return std::make_shared<arrow::Int64Scalar>(static_cast<int64_t>(i) * 1'000'000LL);
    });
    check(arrow::uint8(),
          [](int i) { return std::make_shared<arrow::UInt8Scalar>(static_cast<uint8_t>(i)); });
    check(arrow::uint16(),
          [](int i) { return std::make_shared<arrow::UInt16Scalar>(static_cast<uint16_t>(i)); });
    check(arrow::uint32(),
          [](int i) { return std::make_shared<arrow::UInt32Scalar>(static_cast<uint32_t>(i)); });
    check(arrow::uint64(),
          [](int i) { return std::make_shared<arrow::UInt64Scalar>(static_cast<uint64_t>(i)); });
    check(arrow::float16(),
          [](int i) { return std::make_shared<arrow::HalfFloatScalar>(static_cast<uint16_t>(i)); });
    check(arrow::float32(),
          [](int i) { return std::make_shared<arrow::FloatScalar>(static_cast<float>(i) * 0.5f); });
    check(arrow::float64(), [](int i) {
        return std::make_shared<arrow::DoubleScalar>(static_cast<double>(i) * 0.25);
    });
    check(arrow::date32(), [](int i) { return std::make_shared<arrow::Date32Scalar>(i); });
    check(arrow::date64(), [](int i) {
        return std::make_shared<arrow::Date64Scalar>(static_cast<int64_t>(i) * 86'400'000LL);
    });
    {
        auto type = arrow::time32(arrow::TimeUnit::MILLI);
        check(type, [type](int i) { return std::make_shared<arrow::Time32Scalar>(i, type); });
    }
    {
        auto type = arrow::time64(arrow::TimeUnit::MICRO);
        check(type, [type](int i) {
            return std::make_shared<arrow::Time64Scalar>(static_cast<int64_t>(i), type);
        });
    }
    {
        auto type = arrow::timestamp(arrow::TimeUnit::NANO);
        check(type, [type](int i) {
            return std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(i), type);
        });
    }
    {
        auto type = arrow::duration(arrow::TimeUnit::MILLI);
        check(type, [type](int i) {
            return std::make_shared<arrow::DurationScalar>(static_cast<int64_t>(i), type);
        });
    }
    check(arrow::month_interval(),
          [](int i) { return std::make_shared<arrow::MonthIntervalScalar>(i); });
    check(arrow::day_time_interval(), [](int i) {
        return std::make_shared<arrow::DayTimeIntervalScalar>(
            arrow::DayTimeIntervalType::DayMilliseconds{i, i * 2});
    });
    check(arrow::month_day_nano_interval(), [](int i) {
        return std::make_shared<arrow::MonthDayNanoIntervalScalar>(
            arrow::MonthDayNanoIntervalType::MonthDayNanos{i, i, static_cast<int64_t>(i) * 1000});
    });
    {
        auto type = arrow::decimal128(18, 4);
        check(type, [type](int i) {
            return std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(i * 12345), type);
        });
    }
    {
        auto type = arrow::decimal256(30, 6);
        check(type, [type](int i) {
            return std::make_shared<arrow::Decimal256Scalar>(
                arrow::Decimal256(static_cast<int64_t>(i) * 6789), type);
        });
    }
    {
        auto type = arrow::fixed_size_binary(6);
        check(type, [type](int i) {
            std::string s(6, static_cast<char>(0));
            for (int b = 0; b < 6; ++b)
                s[static_cast<size_t>(b)] = static_cast<char>((i + b) & 0xFF);
            return std::make_shared<arrow::FixedSizeBinaryScalar>(arrow::Buffer::FromString(s),
                                                                  type);
        });
    }
}
