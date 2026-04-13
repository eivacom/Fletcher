#include <catch2/catch_all.hpp>

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
    REQUIRE(decoded.size() == 1);
    return decoded[0];
}

// Build a finished Array from a builder, asserting success.
template <typename Builder>
std::shared_ptr<arrow::Array> Finish(Builder& b) {
    auto result = b.Finish();
    REQUIRE(result.ok());
    return *result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Struct
// ---------------------------------------------------------------------------

TEST_CASE("Struct roundtrip") {
    auto type = arrow::struct_({arrow::field("x", arrow::int32()),
                                 arrow::field("y", arrow::utf8())});
    arrow::StructScalar::ValueType children = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
    };
    auto scalar = std::make_shared<arrow::StructScalar>(children, type);
    auto decoded = Roundtrip(type, scalar);
    CHECK(decoded->Equals(*scalar));
}

TEST_CASE("Nested struct roundtrip") {
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
    CHECK(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// List / LargeList
// ---------------------------------------------------------------------------

TEST_CASE("List<int32> roundtrip") {
    arrow::Int32Builder builder;
    REQUIRE(builder.Append(10).ok());
    REQUIRE(builder.Append(20).ok());
    REQUIRE(builder.Append(30).ok());
    auto arr    = Finish(builder);
    auto type   = arrow::list(arrow::int32());
    auto scalar = std::make_shared<arrow::ListScalar>(arr);
    auto decoded = Roundtrip(type, scalar);
    CHECK(decoded->Equals(*scalar));
}

TEST_CASE("LargeList<utf8> roundtrip") {
    arrow::StringBuilder builder;
    REQUIRE(builder.Append("alpha").ok());
    REQUIRE(builder.Append("beta").ok());
    auto arr    = Finish(builder);
    auto type   = arrow::large_list(arrow::utf8());
    auto scalar = std::make_shared<arrow::LargeListScalar>(arr);
    auto decoded = Roundtrip(type, scalar);
    CHECK(decoded->Equals(*scalar));
}

TEST_CASE("Empty list roundtrip") {
    arrow::Int32Builder builder;
    auto arr    = Finish(builder);
    auto type   = arrow::list(arrow::int32());
    auto scalar = std::make_shared<arrow::ListScalar>(arr);
    auto decoded = Roundtrip(type, scalar);
    CHECK(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// FixedSizeList
// ---------------------------------------------------------------------------

TEST_CASE("FixedSizeList<float32, 3> roundtrip") {
    arrow::FloatBuilder builder;
    REQUIRE(builder.Append(1.0f).ok());
    REQUIRE(builder.Append(2.0f).ok());
    REQUIRE(builder.Append(3.0f).ok());
    auto arr    = Finish(builder);
    auto type   = arrow::fixed_size_list(arrow::float32(), 3);
    auto scalar = std::make_shared<arrow::FixedSizeListScalar>(arr, type);
    auto decoded = Roundtrip(type, scalar);
    CHECK(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

TEST_CASE("Map<utf8, int32> roundtrip") {
    auto key_builder = std::make_shared<arrow::StringBuilder>();
    auto val_builder = std::make_shared<arrow::Int32Builder>();
    arrow::MapBuilder map_builder(arrow::default_memory_pool(),
                                   key_builder, val_builder);

    REQUIRE(map_builder.Append().ok());      // begin one map entry (list of pairs)
    REQUIRE(key_builder->Append("foo").ok());
    REQUIRE(val_builder->Append(1).ok());
    REQUIRE(key_builder->Append("bar").ok());
    REQUIRE(val_builder->Append(-2).ok());

    auto arr_result = map_builder.Finish();
    REQUIRE(arr_result.ok());
    auto arr = std::static_pointer_cast<arrow::MapArray>(*arr_result);

    auto scalar_result = arr->GetScalar(0);
    REQUIRE(scalar_result.ok());
    auto scalar = *scalar_result;

    auto type   = arrow::map(arrow::utf8(), arrow::int32());
    auto decoded = Roundtrip(type, scalar);
    CHECK(decoded->Equals(*scalar));
}

// ---------------------------------------------------------------------------
// Union
// ---------------------------------------------------------------------------

TEST_CASE("SparseUnion roundtrip -active first child") {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    arrow::SparseUnionScalar::ValueType children = {
        std::make_shared<arrow::Int32Scalar>(99),
        arrow::MakeNullScalar(arrow::utf8()),
    };
    auto scalar  = std::make_shared<arrow::SparseUnionScalar>(children, /*type_code=*/0, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    CHECK(decoded->Equals(*scalar));
}

TEST_CASE("SparseUnion roundtrip -active second child") {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    arrow::SparseUnionScalar::ValueType children = {
        arrow::MakeNullScalar(arrow::int32()),
        std::make_shared<arrow::StringScalar>("active"),
    };
    auto scalar  = std::make_shared<arrow::SparseUnionScalar>(children, /*type_code=*/1, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    CHECK(decoded->Equals(*scalar));
}

TEST_CASE("DenseUnion roundtrip -active first child") {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    auto scalar  = std::make_shared<arrow::DenseUnionScalar>(
                       std::make_shared<arrow::Int32Scalar>(77), /*type_code=*/0, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    CHECK(decoded->Equals(*scalar));
}

TEST_CASE("DenseUnion roundtrip -active second child") {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())},
        {0, 1});

    auto scalar  = std::make_shared<arrow::DenseUnionScalar>(
                       std::make_shared<arrow::StringScalar>("dense"), /*type_code=*/1, union_type);
    auto decoded = Roundtrip(union_type, scalar);
    CHECK(decoded->Equals(*scalar));
}
