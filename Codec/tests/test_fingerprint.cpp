#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"

TEST_CASE("FingerprintHash is stable for identical schemas") {
    auto s1 = arrow::schema({arrow::field("a", arrow::int32(),  false),
                              arrow::field("b", arrow::utf8(),   true)});
    auto s2 = arrow::schema({arrow::field("a", arrow::int32(),  false),
                              arrow::field("b", arrow::utf8(),   true)});
    CHECK(fletcher::FingerprintHash(*s1) == fletcher::FingerprintHash(*s2));
}

TEST_CASE("FingerprintHash differs for different field types") {
    auto s1 = arrow::schema({arrow::field("v", arrow::int32())});
    auto s2 = arrow::schema({arrow::field("v", arrow::utf8())});
    CHECK(fletcher::FingerprintHash(*s1) != fletcher::FingerprintHash(*s2));
}

TEST_CASE("FingerprintHash differs for different nullability") {
    auto s1 = arrow::schema({arrow::field("v", arrow::int32(), false)});
    auto s2 = arrow::schema({arrow::field("v", arrow::int32(), true)});
    CHECK(fletcher::FingerprintHash(*s1) != fletcher::FingerprintHash(*s2));
}

TEST_CASE("FingerprintHash differs for different field names") {
    auto s1 = arrow::schema({arrow::field("foo", arrow::float64())});
    auto s2 = arrow::schema({arrow::field("bar", arrow::float64())});
    CHECK(fletcher::FingerprintHash(*s1) != fletcher::FingerprintHash(*s2));
}

TEST_CASE("FingerprintHash ignores schema-level metadata") {
    auto meta = arrow::key_value_metadata({"key"}, {"value"});
    auto s1 = arrow::schema({arrow::field("v", arrow::int32())});
    auto s2 = arrow::schema({arrow::field("v", arrow::int32())}, meta);
    CHECK(fletcher::FingerprintHash(*s1) == fletcher::FingerprintHash(*s2));
}

TEST_CASE("FingerprintHash differs for different number of fields") {
    auto s1 = arrow::schema({arrow::field("a", arrow::int32())});
    auto s2 = arrow::schema({arrow::field("a", arrow::int32()),
                              arrow::field("b", arrow::int32())});
    CHECK(fletcher::FingerprintHash(*s1) != fletcher::FingerprintHash(*s2));
}
