#include <gtest/gtest.h>

#include <arrow/api.h>

#include "row_codec.hpp"

TEST(FingerprintTest, StableForIdenticalSchemas) {
    auto s1 = arrow::schema({arrow::field("a", arrow::int32(),  false),
                              arrow::field("b", arrow::utf8(),   true)});
    auto s2 = arrow::schema({arrow::field("a", arrow::int32(),  false),
                              arrow::field("b", arrow::utf8(),   true)});
    EXPECT_EQ(fletcher::FingerprintHash(*s1), fletcher::FingerprintHash(*s2));
}

TEST(FingerprintTest, DiffersForDifferentFieldTypes) {
    auto s1 = arrow::schema({arrow::field("v", arrow::int32())});
    auto s2 = arrow::schema({arrow::field("v", arrow::utf8())});
    EXPECT_NE(fletcher::FingerprintHash(*s1), fletcher::FingerprintHash(*s2));
}

TEST(FingerprintTest, DiffersForDifferentNullability) {
    auto s1 = arrow::schema({arrow::field("v", arrow::int32(), false)});
    auto s2 = arrow::schema({arrow::field("v", arrow::int32(), true)});
    EXPECT_NE(fletcher::FingerprintHash(*s1), fletcher::FingerprintHash(*s2));
}

TEST(FingerprintTest, DiffersForDifferentFieldNames) {
    auto s1 = arrow::schema({arrow::field("foo", arrow::float64())});
    auto s2 = arrow::schema({arrow::field("bar", arrow::float64())});
    EXPECT_NE(fletcher::FingerprintHash(*s1), fletcher::FingerprintHash(*s2));
}

TEST(FingerprintTest, IgnoresSchemaLevelMetadata) {
    auto meta = arrow::key_value_metadata({"key"}, {"value"});
    auto s1 = arrow::schema({arrow::field("v", arrow::int32())});
    auto s2 = arrow::schema({arrow::field("v", arrow::int32())}, meta);
    EXPECT_EQ(fletcher::FingerprintHash(*s1), fletcher::FingerprintHash(*s2));
}

TEST(FingerprintTest, DiffersForDifferentNumberOfFields) {
    auto s1 = arrow::schema({arrow::field("a", arrow::int32())});
    auto s2 = arrow::schema({arrow::field("a", arrow::int32()),
                              arrow::field("b", arrow::int32())});
    EXPECT_NE(fletcher::FingerprintHash(*s1), fletcher::FingerprintHash(*s2));
}
