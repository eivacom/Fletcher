#include <gtest/gtest.h>

#include <arrow/api.h>

#include "row_codec.hpp"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<arrow::Scalar> Roundtrip(
    const std::shared_ptr<arrow::DataType>& type,
    const std::shared_ptr<arrow::Scalar>&   in)
{
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    fletcher::RowCodec codec(schema);
    auto row     = codec.EncodeRow({in});
    auto decoded = codec.DecodeRow(row);
    if (decoded.size() != 1) { ADD_FAILURE() << "Expected decoded.size() == 1"; return nullptr; }
    return decoded[0];
}

// Build a finished Array from a builder, asserting success.
template <typename Builder>
std::shared_ptr<arrow::Array> Finish(Builder& b) {
    auto result = b.Finish();
    if (!result.ok()) { ADD_FAILURE() << "Finish() failed"; return nullptr; }
    return *result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Struct
// ---------------------------------------------------------------------------

TEST(RecursiveTest, StructRoundtrip) {
    auto type = arrow::struct_({arrow::field("x", arrow::int32()),
                                 arrow::field("y", arrow::utf8())});
    arrow::StructScalar::ValueType children = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
    };
    auto scalar = std::make_shared<arrow::StructScalar>(children, type);
    auto decoded = Roundtrip(type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

TEST(RecursiveTest, NestedStructRoundtrip) {
    auto inner_type = arrow::struct_({arrow::field("z", arrow::float64())});
    auto outer_type = arrow::struct_({arrow::field("inner", inner_type),
                                       arrow::field("flag",  arrow::boolean())});
    arrow::StructScalar::ValueType inner_children = {
        std::make_shared<arrow::DoubleScalar>(3.14),
    };
    arrow::StructScalar::ValueType outer_children = {
        std::make_shared<arrow::StructScalar>(inner_children, inner_type),
        std::make_shared<arrow::BooleanScalar>(true),
    };
    auto scalar = std::make_shared<arrow::StructScalar>(outer_children, outer_type);
    auto decoded = Roundtrip(outer_type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// List / LargeList
// ---------------------------------------------------------------------------

TEST(RecursiveTest, ListInt32Roundtrip) {
    arrow::Int32Builder builder;
    ASSERT_TRUE(builder.Append(10).ok());
    ASSERT_TRUE(builder.Append(20).ok());
    ASSERT_TRUE(builder.Append(30).ok());
    auto arr    = Finish(builder);
    auto type   = arrow::list(arrow::int32());
    auto scalar = std::make_shared<arrow::ListScalar>(arr);
    auto decoded = Roundtrip(type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

TEST(RecursiveTest, LargeListUtf8Roundtrip) {
    arrow::StringBuilder builder;
    ASSERT_TRUE(builder.Append("alpha").ok());
    ASSERT_TRUE(builder.Append("beta").ok());
    auto arr    = Finish(builder);
    auto type   = arrow::large_list(arrow::utf8());
    auto scalar = std::make_shared<arrow::LargeListScalar>(arr);
    auto decoded = Roundtrip(type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

TEST(RecursiveTest, EmptyListRoundtrip) {
    arrow::Int32Builder builder;
    auto arr    = Finish(builder);
    auto type   = arrow::list(arrow::int32());
    auto scalar = std::make_shared<arrow::ListScalar>(arr);
    auto decoded = Roundtrip(type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// FixedSizeList
// ---------------------------------------------------------------------------

TEST(RecursiveTest, FixedSizeListFloat32Roundtrip) {
    arrow::FloatBuilder builder;
    ASSERT_TRUE(builder.Append(1.0f).ok());
    ASSERT_TRUE(builder.Append(2.0f).ok());
    ASSERT_TRUE(builder.Append(3.0f).ok());
    auto arr    = Finish(builder);
    auto type   = arrow::fixed_size_list(arrow::float32(), 3);
    auto scalar = std::make_shared<arrow::FixedSizeListScalar>(arr, type);
    auto decoded = Roundtrip(type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

TEST(RecursiveTest, MapUtf8Int32Roundtrip) {
    auto key_builder = std::make_shared<arrow::StringBuilder>();
    auto val_builder = std::make_shared<arrow::Int32Builder>();
    arrow::MapBuilder map_builder(arrow::default_memory_pool(),
                                   key_builder, val_builder);

    ASSERT_TRUE(map_builder.Append().ok());      // begin one map entry (list of pairs)
    ASSERT_TRUE(key_builder->Append("foo").ok());
    ASSERT_TRUE(val_builder->Append(1).ok());
    ASSERT_TRUE(key_builder->Append("bar").ok());
    ASSERT_TRUE(val_builder->Append(-2).ok());

    auto arr_result = map_builder.Finish();
    ASSERT_TRUE(arr_result.ok());
    auto arr = std::static_pointer_cast<arrow::MapArray>(*arr_result);

    auto scalar_result = arr->GetScalar(0);
    ASSERT_TRUE(scalar_result.ok());
    auto scalar = *scalar_result;

    auto type   = arrow::map(arrow::utf8(), arrow::int32());
    auto decoded = Roundtrip(type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// Union
// ---------------------------------------------------------------------------

TEST(RecursiveTest, SparseUnionActiveFirstChild) {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    arrow::SparseUnionScalar::ValueType children = {
        std::make_shared<arrow::Int32Scalar>(99),
        arrow::MakeNullScalar(arrow::utf8()),
    };
    auto scalar  = std::make_shared<arrow::SparseUnionScalar>(children, /*type_code=*/0, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

TEST(RecursiveTest, SparseUnionActiveSecondChild) {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    arrow::SparseUnionScalar::ValueType children = {
        arrow::MakeNullScalar(arrow::int32()),
        std::make_shared<arrow::StringScalar>("active"),
    };
    auto scalar  = std::make_shared<arrow::SparseUnionScalar>(children, /*type_code=*/1, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

TEST(RecursiveTest, DenseUnionActiveFirstChild) {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    auto scalar  = std::make_shared<arrow::DenseUnionScalar>(
                       std::make_shared<arrow::Int32Scalar>(77), /*type_code=*/0, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}

TEST(RecursiveTest, DenseUnionActiveSecondChild) {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    auto scalar  = std::make_shared<arrow::DenseUnionScalar>(
                       std::make_shared<arrow::StringScalar>("dense"), /*type_code=*/1, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    EXPECT_TRUE(decoded->Equals(*scalar));
}
