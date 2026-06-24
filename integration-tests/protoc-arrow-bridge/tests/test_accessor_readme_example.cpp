// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// RBA-7 docs guard: compile-checks the C++ accessor usage snippet documented in
// protoc/README.md ("RecordBatch accessors" subsection). This test MIRRORS that
// README snippet (it is not auto-extracted): if the README API shape goes stale
// — wrong factory name, wrong getter shape, wrong include — this test stops
// compiling. Keep the snippet below in sync with the README example.
//
// The README example is written against the existing simple scalar fixture
// (accessor_scalar.proto -> ScalarRowAccessor), already generated for this
// harness, so no new generation input is needed.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

// --- BEGIN protoc/README.md "RecordBatch accessors" snippet (mirror) ---
// Generate the header with --fletcher_opt=accessor, then:
#include "accessor_scalar.fletcher.accessor.pb.h"

using fletcher_gen::integration::ScalarRowAccessor;
// --- END snippet include/using ---

namespace {

std::shared_ptr<arrow::RecordBatch> MakeReadmeBatch() {
    arrow::Int32Builder i32;
    (void)i32.AppendValues({1, 2, 3});
    std::shared_ptr<arrow::Array> i32_arr;
    (void)i32.Finish(&i32_arr);

    arrow::Int32Builder opt;
    (void)opt.Append(11);
    (void)opt.AppendNull();
    (void)opt.Append(33);
    std::shared_ptr<arrow::Array> opt_arr;
    (void)opt.Finish(&opt_arr);

    // ScalarRow has many fields; the README snippet only reads i32 + opt_i32, so
    // here we build a minimal-but-valid batch matching ScalarRow's schema.
    arrow::BooleanBuilder bb;
    (void)bb.AppendValues(std::vector<bool>{true, false, true});
    std::shared_ptr<arrow::Array> b_arr;
    (void)bb.Finish(&b_arr);
    auto i64 = [] {
        arrow::Int64Builder b;
        (void)b.AppendValues({1, 2, 3});
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto u32 = [] {
        arrow::UInt32Builder b;
        (void)b.AppendValues({1u, 2u, 3u});
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto u64 = [] {
        arrow::UInt64Builder b;
        (void)b.AppendValues({1ull, 2ull, 3ull});
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto f32 = [] {
        arrow::FloatBuilder b;
        (void)b.AppendValues({1.0f, 2.0f, 3.0f});
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto f64 = [] {
        arrow::DoubleBuilder b;
        (void)b.AppendValues({1.0, 2.0, 3.0});
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto text = [] {
        arrow::StringBuilder b;
        (void)b.AppendValues({"a", "b", "c"});
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto blob = [] {
        arrow::BinaryBuilder b;
        (void)b.Append("x", 1);
        (void)b.Append("y", 1);
        (void)b.Append("z", 1);
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto ts = [] {
        std::unique_ptr<arrow::ArrayBuilder> base;
        (void)arrow::MakeBuilder(arrow::default_memory_pool(),
                                 arrow::timestamp(arrow::TimeUnit::NANO), &base);
        (void)static_cast<arrow::TimestampBuilder*>(base.get())->AppendValues({1, 2, 3});
        std::shared_ptr<arrow::Array> a;
        (void)base->Finish(&a);
        return a;
    };
    auto dur = [] {
        std::unique_ptr<arrow::ArrayBuilder> base;
        (void)arrow::MakeBuilder(arrow::default_memory_pool(),
                                 arrow::duration(arrow::TimeUnit::NANO), &base);
        (void)static_cast<arrow::DurationBuilder*>(base.get())->AppendValues({1, 2, 3});
        std::shared_ptr<arrow::Array> a;
        (void)base->Finish(&a);
        return a;
    };
    auto opt_text = [] {
        arrow::StringBuilder b;
        (void)b.Append("p");
        (void)b.AppendNull();
        (void)b.Append("r");
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };
    auto opt_i64 = [] {
        arrow::Int64Builder b;
        (void)b.Append(1);
        (void)b.AppendNull();
        (void)b.Append(3);
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    };

    auto schema = arrow::schema({
        arrow::field("b_flag", arrow::boolean(), false),
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
    return arrow::RecordBatch::Make(
        schema, 3,
        {b_arr, i32_arr, i64(), u32(), u64(), f32(), f64(), text(), blob(), ts(), dur(), opt_arr,
         opt_text(), opt_i64()});
}

}  // namespace

TEST(AccessorReadmeExampleTest, DocumentedCppSnippetCompilesAndReads) {
    std::shared_ptr<arrow::RecordBatch> batch = MakeReadmeBatch();

    // --- BEGIN protoc/README.md "RecordBatch accessors" snippet (mirror) ---
    // Construct the accessor from a RecordBatch (read-only, validated once).
    auto result = ScalarRowAccessor::Make(batch);
    if (!result.ok()) { /* handle result.status() */
    }
    const ScalarRowAccessor& accessor = *result;

    // Read a non-nullable scalar by row, and an optional scalar (None on null).
    int32_t first_id = accessor.i32(0);
    std::optional<int32_t> maybe = accessor.opt_i32(1);  // std::nullopt if null
    // --- END snippet ---

    EXPECT_EQ(first_id, 1);
    EXPECT_FALSE(maybe.has_value());
    EXPECT_TRUE(accessor.opt_i32(0).has_value());
    EXPECT_EQ(*accessor.opt_i32(0), 11);
}
