// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// RBA-2 forcing test — the first real generated C++ <Class>Accessor.
//
// Builds an arrow::RecordBatch for the flat scalar fixture (accessor_scalar.proto)
// and proves:
//   - every generated getter reads the right per-row value (incl. a nullable
//     field whose null yields std::nullopt);
//   - a type-mismatched batch -> failed arrow::Result with a precise message;
//   - a name-mismatched but type-compatible batch -> success (name tolerance);
//   - a proto-non-nullable scalar column carrying a runtime null -> failed Result
//     (the null_count()==0 gate, applied to actual nulls, not validity presence);
//   - a non-nullable column with NO validity buffer is accepted;
//   - wrong column count -> error; Make never throws;
//   - the accessor keeps its data alive via cached handles after the caller
//     drops every other reference to the batch;
//   - Make(StructArray) reads an equivalent StructArray identically.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "accessor_scalar.fletcher.accessor.pb.h"

namespace {

using fletcher_gen::integration::ScalarRowAccessor;

// Per-row fixture values (3 rows). Row 1 has the nullable fields set to null.
constexpr int64_t kNumRows = 3;

template <typename Builder, typename T>
std::shared_ptr<arrow::Array> BuildNonNull(const std::vector<T>& values) {
    Builder b;
    EXPECT_TRUE(b.AppendValues(values).ok());
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(b.Finish(&out).ok());
    return out;
}

// Timestamp / duration need an explicit type passed to the builder.
std::shared_ptr<arrow::Array> BuildInt64Typed(const std::shared_ptr<arrow::DataType>& type,
                                              const std::vector<int64_t>& values) {
    std::unique_ptr<arrow::ArrayBuilder> base;
    EXPECT_TRUE(arrow::MakeBuilder(arrow::default_memory_pool(), type, &base).ok());
    if (type->id() == arrow::Type::TIMESTAMP) {
        EXPECT_TRUE(static_cast<arrow::TimestampBuilder*>(base.get())->AppendValues(values).ok());
    } else {
        EXPECT_TRUE(static_cast<arrow::DurationBuilder*>(base.get())->AppendValues(values).ok());
    }
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(base->Finish(&out).ok());
    return out;
}

// Build the per-column arrays matching the ScalarRow schema field order:
//   0 b_flag (bool), 1 i32, 2 i64, 3 u32, 4 u64, 5 f32, 6 f64, 7 text (utf8),
//   8 blob (binary), 9 occurred_at (ts ns), 10 elapsed (dur ns),
//   11 opt_i32 (nullable int32), 12 opt_text (nullable utf8),
//   13 opt_i64 (nullable int64).
struct Columns {
    std::shared_ptr<arrow::Array> b_flag, i32, i64, u32, u64, f32, f64, text, blob, occurred_at,
        elapsed, opt_i32, opt_text, opt_i64;

    arrow::ArrayVector AsVector() const {
        return {b_flag,  i32,         i64,     u32,      u64,     f32,      f64,
                text,    blob,        occurred_at, elapsed, opt_i32, opt_text, opt_i64};
    }
};

Columns MakeFixtureColumns() {
    Columns c;
    c.b_flag = BuildNonNull<arrow::BooleanBuilder, bool>({true, false, true});
    c.i32 = BuildNonNull<arrow::Int32Builder, int32_t>({-1, 0, 2147483647});
    c.i64 = BuildNonNull<arrow::Int64Builder, int64_t>({-9, 0, 9223372036854775807LL});
    c.u32 = BuildNonNull<arrow::UInt32Builder, uint32_t>({0u, 7u, 4294967295u});
    c.u64 = BuildNonNull<arrow::UInt64Builder, uint64_t>({0ull, 8ull, 18446744073709551615ull});
    c.f32 = BuildNonNull<arrow::FloatBuilder, float>({1.5f, -2.5f, 3.25f});
    c.f64 = BuildNonNull<arrow::DoubleBuilder, double>({10.5, -20.25, 30.125});

    {
        arrow::StringBuilder sb;
        EXPECT_TRUE(sb.AppendValues({"alpha", "beta", "gamma"}).ok());
        EXPECT_TRUE(sb.Finish(&c.text).ok());
    }
    {
        arrow::BinaryBuilder bb;
        EXPECT_TRUE(bb.Append("\x01\x02", 2).ok());
        EXPECT_TRUE(bb.Append("", 0).ok());
        EXPECT_TRUE(bb.Append("\xff", 1).ok());
        EXPECT_TRUE(bb.Finish(&c.blob).ok());
    }

    c.occurred_at = BuildInt64Typed(arrow::timestamp(arrow::TimeUnit::NANO),
                                    {100, 200, 300});
    c.elapsed = BuildInt64Typed(arrow::duration(arrow::TimeUnit::NANO), {5, 6, 7});

    // Nullable columns: row index 1 is null.
    {
        arrow::Int32Builder ib;
        EXPECT_TRUE(ib.Append(11).ok());
        EXPECT_TRUE(ib.AppendNull().ok());
        EXPECT_TRUE(ib.Append(33).ok());
        EXPECT_TRUE(ib.Finish(&c.opt_i32).ok());
    }
    {
        arrow::StringBuilder sb;
        EXPECT_TRUE(sb.Append("present").ok());
        EXPECT_TRUE(sb.AppendNull().ok());
        EXPECT_TRUE(sb.Append("here").ok());
        EXPECT_TRUE(sb.Finish(&c.opt_text).ok());
    }
    {
        arrow::Int64Builder ib;
        EXPECT_TRUE(ib.Append(111).ok());
        EXPECT_TRUE(ib.AppendNull().ok());
        EXPECT_TRUE(ib.Append(333).ok());
        EXPECT_TRUE(ib.Finish(&c.opt_i64).ok());
    }
    return c;
}

// The schema field order/types/nullability matching ScalarRow.
std::shared_ptr<arrow::Schema> MakeFixtureSchema() {
    return arrow::schema({
        arrow::field("b_flag", arrow::boolean(), /*nullable=*/false),
        arrow::field("i32", arrow::int32(), false),
        arrow::field("i64", arrow::int64(), false),
        arrow::field("u32", arrow::uint32(), false),
        arrow::field("u64", arrow::uint64(), false),
        arrow::field("f32", arrow::float32(), false),
        arrow::field("f64", arrow::float64(), false),
        arrow::field("text", arrow::utf8(), false),
        arrow::field("blob", arrow::binary(), false),
        arrow::field("occurred_at", arrow::timestamp(arrow::TimeUnit::NANO), false),
        arrow::field("elapsed", arrow::duration(arrow::TimeUnit::NANO), false),
        arrow::field("opt_i32", arrow::int32(), true),
        arrow::field("opt_text", arrow::utf8(), true),
        arrow::field("opt_i64", arrow::int64(), true),
    });
}

std::shared_ptr<arrow::RecordBatch> MakeFixtureBatch() {
    auto cols = MakeFixtureColumns().AsVector();
    return arrow::RecordBatch::Make(MakeFixtureSchema(), kNumRows, cols);
}

void CheckAllGetters(const ScalarRowAccessor& a) {
    ASSERT_EQ(a.num_rows(), kNumRows);

    EXPECT_EQ(a.b_flag(0), true);
    EXPECT_EQ(a.b_flag(1), false);
    EXPECT_EQ(a.b_flag(2), true);

    EXPECT_EQ(a.i32(0), -1);
    EXPECT_EQ(a.i32(2), 2147483647);
    EXPECT_EQ(a.i64(2), 9223372036854775807LL);
    EXPECT_EQ(a.u32(2), 4294967295u);
    EXPECT_EQ(a.u64(2), 18446744073709551615ull);
    EXPECT_FLOAT_EQ(a.f32(0), 1.5f);
    EXPECT_DOUBLE_EQ(a.f64(2), 30.125);

    EXPECT_EQ(a.text(0), std::string_view("alpha"));
    EXPECT_EQ(a.text(2), std::string_view("gamma"));
    EXPECT_EQ(a.blob(0), std::string_view("\x01\x02", 2));
    EXPECT_EQ(a.blob(1), std::string_view("", 0));
    EXPECT_EQ(a.blob(2), std::string_view("\xff", 1));

    EXPECT_EQ(a.occurred_at(1), 200);
    EXPECT_EQ(a.elapsed(2), 7);

    // Nullable fields: row 0/2 set, row 1 null -> nullopt.
    ASSERT_TRUE(a.opt_i32(0).has_value());
    EXPECT_EQ(*a.opt_i32(0), 11);
    EXPECT_FALSE(a.opt_i32(1).has_value());
    ASSERT_TRUE(a.opt_i32(2).has_value());
    EXPECT_EQ(*a.opt_i32(2), 33);

    ASSERT_TRUE(a.opt_text(0).has_value());
    EXPECT_EQ(*a.opt_text(0), std::string_view("present"));
    EXPECT_FALSE(a.opt_text(1).has_value());

    ASSERT_TRUE(a.opt_i64(2).has_value());
    EXPECT_EQ(*a.opt_i64(2), 333);
    EXPECT_FALSE(a.opt_i64(1).has_value());
}

}  // namespace

TEST(AccessorTest, ScalarColumnsReadAndValidatePositionally) {
    // (1) Happy path: every getter returns the right per-row value.
    {
        auto batch = MakeFixtureBatch();
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        CheckAllGetters(*r);
    }

    // (2a) Type-mismatched batch (int32 column carrying a double) -> failed
    //      Result naming class, column index, field, expected and actual types.
    {
        auto cols = MakeFixtureColumns();
        // Replace i32 (column 1) with a double array -> type mismatch.
        cols.i32 = BuildNonNull<arrow::DoubleBuilder, double>({1.0, 2.0, 3.0});
        auto fields = MakeFixtureSchema()->fields();
        auto batch = arrow::RecordBatch::Make(arrow::schema(fields), kNumRows, cols.AsVector());
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_FALSE(r.ok());
        const std::string msg = r.status().message();
        EXPECT_NE(msg.find("column 1"), std::string::npos) << msg;
        EXPECT_NE(msg.find("i32"), std::string::npos) << msg;
        EXPECT_NE(msg.find("int32"), std::string::npos) << msg;
        EXPECT_NE(msg.find("double"), std::string::npos) << msg;
    }

    // (2b) A second, distinct mismatch pair on a different column/type family:
    //      a utf8 column (text, column 7) carrying an int64 array. Earlier columns
    //      remain valid so the failure pins precisely to column 7.
    {
        auto cols = MakeFixtureColumns();
        cols.text = BuildNonNull<arrow::Int64Builder, int64_t>({1, 2, 3});
        auto batch = arrow::RecordBatch::Make(MakeFixtureSchema(), kNumRows, cols.AsVector());
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_FALSE(r.ok());
        const std::string msg = r.status().message();
        EXPECT_NE(msg.find("column 7"), std::string::npos) << msg;
        EXPECT_NE(msg.find("text"), std::string::npos) << msg;
        // arrow::utf8()->ToString() renders as "string"; the actual type is int64.
        EXPECT_NE(msg.find("string"), std::string::npos) << msg;
        EXPECT_NE(msg.find("int64"), std::string::npos) << msg;
    }

    // (3) Name-mismatched but type-compatible batch -> success (name tolerance).
    {
        auto cols = MakeFixtureColumns().AsVector();
        std::vector<std::shared_ptr<arrow::Field>> renamed;
        auto base = MakeFixtureSchema()->fields();
        for (size_t i = 0; i < base.size(); ++i) {
            renamed.push_back(arrow::field("renamed_" + std::to_string(i), base[i]->type(),
                                           base[i]->nullable()));
        }
        auto batch = arrow::RecordBatch::Make(arrow::schema(renamed), kNumRows, cols);
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        CheckAllGetters(*r);
    }

    // (4) proto-non-nullable scalar column carrying a runtime null -> failed Result.
    {
        auto cols = MakeFixtureColumns();
        arrow::Int32Builder ib;
        ASSERT_TRUE(ib.Append(1).ok());
        ASSERT_TRUE(ib.AppendNull().ok());  // runtime null in a non-nullable column
        ASSERT_TRUE(ib.Append(3).ok());
        ASSERT_TRUE(ib.Finish(&cols.i32).ok());
        auto batch =
            arrow::RecordBatch::Make(MakeFixtureSchema(), kNumRows, cols.AsVector());
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_FALSE(r.ok());
        const std::string msg = r.status().message();
        EXPECT_NE(msg.find("non-nullable"), std::string::npos) << msg;
        EXPECT_NE(msg.find("i32"), std::string::npos) << msg;
    }

    // (5a) Non-nullable column with NO validity buffer is accepted (default builds
    //      above already have no validity buffer); assert null_count==0 columns pass.
    {
        auto batch = MakeFixtureBatch();
        ASSERT_EQ(batch->column(1)->null_count(), 0);
        ASSERT_EQ(batch->column(1)->data()->buffers[0], nullptr)
            << "fixture's non-nullable int32 should have no validity buffer";
        auto r = ScalarRowAccessor::Make(batch);
        EXPECT_TRUE(r.ok()) << r.status().ToString();
    }

    // (5b) The gate is null_count()==0, NOT "has a validity buffer". A type-correct
    //      column mapped to a non-nullable proto field that DOES carry a validity
    //      buffer but whose actual null_count is 0 must be accepted. We build an
    //      int32 column with a null, then Slice past the null so the slice has a
    //      validity buffer present yet null_count()==0.
    {
        auto cols = MakeFixtureColumns();
        std::shared_ptr<arrow::Array> with_null;
        {
            arrow::Int32Builder ib;
            ASSERT_TRUE(ib.AppendNull().ok());  // index 0 null -> allocates validity buffer
            ASSERT_TRUE(ib.Append(10).ok());
            ASSERT_TRUE(ib.Append(20).ok());
            ASSERT_TRUE(ib.Append(30).ok());
            ASSERT_TRUE(ib.Finish(&with_null).ok());
        }
        // Slice off the leading null: 3 rows, all valid, validity buffer retained.
        cols.i32 = with_null->Slice(1, kNumRows);
        ASSERT_NE(cols.i32->data()->buffers[0], nullptr)
            << "sliced column should still carry a validity buffer";
        ASSERT_EQ(cols.i32->null_count(), 0)
            << "sliced window has no actual nulls";
        auto batch = arrow::RecordBatch::Make(MakeFixtureSchema(), kNumRows, cols.AsVector());
        auto r = ScalarRowAccessor::Make(batch);
        EXPECT_TRUE(r.ok()) << r.status().ToString();
    }

    // (6) Wrong column count -> error.
    {
        auto cols = MakeFixtureColumns().AsVector();
        cols.pop_back();  // drop the last column
        auto fields = MakeFixtureSchema()->fields();
        fields.pop_back();
        auto batch = arrow::RecordBatch::Make(arrow::schema(fields), kNumRows, cols);
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_FALSE(r.ok());
        const std::string msg = r.status().message();
        EXPECT_NE(msg.find("columns"), std::string::npos) << msg;
    }

    // (7) Make never throws on null source pointers.
    {
        std::shared_ptr<arrow::RecordBatch> null_batch;
        auto r = ScalarRowAccessor::Make(null_batch);
        EXPECT_FALSE(r.ok());

        std::shared_ptr<arrow::StructArray> null_struct;
        auto rs = ScalarRowAccessor::Make(null_struct);
        EXPECT_FALSE(rs.ok());
    }
}

TEST(AccessorTest, ScalarAccessorKeepsDataAliveAfterBatchDropped) {
    std::optional<ScalarRowAccessor> acc;
    {
        auto batch = MakeFixtureBatch();
        auto r = ScalarRowAccessor::Make(batch);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        acc.emplace(std::move(*r));
        // Drop the local batch reference; the accessor's cached handles must keep
        // the buffers alive.
        batch.reset();
    }
    ASSERT_TRUE(acc.has_value());
    CheckAllGetters(*acc);
}

TEST(AccessorTest, ScalarAccessorFromStructArrayReadsIdentically) {
    auto cols = MakeFixtureColumns().AsVector();
    auto schema = MakeFixtureSchema();

    // Build an equivalent StructArray from the same children/fields.
    auto struct_res = arrow::StructArray::Make(cols, schema->fields());
    ASSERT_TRUE(struct_res.ok()) << struct_res.status().ToString();
    std::shared_ptr<arrow::StructArray> sa =
        std::static_pointer_cast<arrow::StructArray>(*struct_res);

    auto r = ScalarRowAccessor::Make(sa);
    ASSERT_TRUE(r.ok()) << r.status().ToString();
    CheckAllGetters(*r);

    // A sliced StructArray must read the windowed rows correctly.
    auto sliced = std::static_pointer_cast<arrow::StructArray>(sa->Slice(1, 2));
    auto rs = ScalarRowAccessor::Make(sliced);
    ASSERT_TRUE(rs.ok()) << rs.status().ToString();
    EXPECT_EQ(rs->num_rows(), 2);
    EXPECT_EQ(rs->i32(0), 0);            // original row 1
    EXPECT_EQ(rs->i32(1), 2147483647);  // original row 2
    EXPECT_FALSE(rs->opt_i32(0).has_value());  // original row 1 was null
}
