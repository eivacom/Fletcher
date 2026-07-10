// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-2 (Phase 3b) parity oracle. Pins the generated edge wire contract before
// the IR rewrite (GIR-3..7) begins. For every active message shape in
// coverage.proto this asserts, per fixture:
//
//   1. row.Encode()                       == committed golden bytes
//   2. Codec.EncodeRow(ToArrowRow(row))   == committed golden bytes
//   3. row.Encode()                       == Codec.EncodeRow(ToArrowRow(row))
//
// so a coordinated drift where BOTH encode paths reorder/rewrite identically is
// still caught against the historical golden anchor. It then proves decode by
// value: encode -> Codec.DecodeRow -> generated View -> field-by-field equality,
// and the same for decoding the committed golden bytes directly.
//
// The golden .bin files under golden/ are the wire-contract baseline. They are
// generated FROM the current generator's Encode() output (the contract being
// pinned) via the gated ParityOracle.RegenerateGoldens mode; a byte change for
// an already-supported input is a stop-and-ask per locked decision #2.
//
// coverage_future.proto is deliberately NOT gated here: its scalar-leaf flatten
// and scalar nested-list cases are not yet faithfully generated on every
// surface. They rejoin this oracle in GIR-10.
//
// Decode lifetime: Codec::DecodeRow may hand back scalars that borrow the source
// buffer zero-copy, so every decode binds its byte buffer to a NAMED local that
// outlives the View. No temporaries are decoded.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fletcher/arrow_bridge/codec.hpp>

#include "coverage.fletcher.arrow.pb.h"
#include "coverage.fletcher.pb.h"
#include "coverage_fixture.hpp"
#include "coverage_test_helpers.hpp"

namespace gen = fletcher_gen::integration::coverage;
namespace fx = coverage_fixture;

namespace {

std::filesystem::path GoldenDir() { return std::filesystem::path(PARITY_GOLDEN_DIR_PATH); }

// ---------------------------------------------------------------------------
// Field-by-field equality between a generated edge message (the expected value)
// and a generated Arrow View (the decoded value). Defined callee-before-caller
// because these are found by ordinary lookup from the templates below (they are
// not in an associated namespace of the gen:: view types, so ADL cannot reach
// them at instantiation).
// ---------------------------------------------------------------------------

void ExpectEquals(const gen::Leaf& e, const gen::LeafView& a) {
    EXPECT_EQ(a.id(), e.id());
    EXPECT_EQ(a.label(), e.label());
    EXPECT_EQ(a.status(), e.status());
}

void ExpectEquals(const gen::NestedEnums& e, const gen::NestedEnumsView& a) {
    EXPECT_EQ(a.state(), e.state());
}

void ExpectEquals(const gen::FlattenedPoint& e, const gen::FlattenedPointView& a) {
    EXPECT_DOUBLE_EQ(a.x(), e.x());
    EXPECT_DOUBLE_EQ(a.y(), e.y());
}

void ExpectEquals(const gen::FieldFlattenedPosition& e, const gen::FieldFlattenedPositionView& a) {
    EXPECT_DOUBLE_EQ(a.x(), e.x());
    EXPECT_DOUBLE_EQ(a.y(), e.y());
}

void ExpectEquals(const gen::ServiceReply& e, const gen::ServiceReplyView& a) {
    EXPECT_EQ(a.accepted(), e.accepted());
    EXPECT_EQ(a.message(), e.message());
}

void ExpectEquals(const gen::ScalarCoverage& e, const gen::ScalarCoverageView& a) {
    EXPECT_EQ(a.bool_value(), e.bool_value());
    EXPECT_EQ(a.int32_value(), e.int32_value());
    EXPECT_EQ(a.int64_value(), e.int64_value());
    EXPECT_EQ(a.uint32_value(), e.uint32_value());
    EXPECT_EQ(a.uint64_value(), e.uint64_value());
    EXPECT_EQ(a.sint32_value(), e.sint32_value());
    EXPECT_EQ(a.sint64_value(), e.sint64_value());
    EXPECT_EQ(a.fixed32_value(), e.fixed32_value());
    EXPECT_EQ(a.fixed64_value(), e.fixed64_value());
    EXPECT_EQ(a.sfixed32_value(), e.sfixed32_value());
    EXPECT_EQ(a.sfixed64_value(), e.sfixed64_value());
    EXPECT_FLOAT_EQ(a.float_value(), e.float_value());
    EXPECT_DOUBLE_EQ(a.double_value(), e.double_value());
    EXPECT_EQ(a.string_value(), e.string_value());
    EXPECT_EQ(a.bytes_value(), e.bytes_value());
    // optional primitives (set + unset states both asserted, including null).
    EXPECT_EQ(a.optional_bool(), e.optional_bool());
    EXPECT_EQ(a.optional_int32(), e.optional_int32());
    EXPECT_EQ(a.optional_string(), e.optional_string());
    EXPECT_EQ(a.optional_bytes(), e.optional_bytes());
    // WKT wrappers (set + unset states both asserted, including null).
    EXPECT_EQ(a.wrapped_bool(), e.wrapped_bool());
    EXPECT_EQ(a.wrapped_int32(), e.wrapped_int32());
    EXPECT_EQ(a.wrapped_int64(), e.wrapped_int64());
    EXPECT_EQ(a.wrapped_uint32(), e.wrapped_uint32());
    EXPECT_EQ(a.wrapped_uint64(), e.wrapped_uint64());
    ASSERT_EQ(a.wrapped_float().has_value(), e.wrapped_float().has_value());
    if (e.wrapped_float().has_value()) EXPECT_FLOAT_EQ(*a.wrapped_float(), *e.wrapped_float());
    ASSERT_EQ(a.wrapped_double().has_value(), e.wrapped_double().has_value());
    if (e.wrapped_double().has_value()) EXPECT_DOUBLE_EQ(*a.wrapped_double(), *e.wrapped_double());
    EXPECT_EQ(a.wrapped_string(), e.wrapped_string());
    EXPECT_EQ(a.wrapped_bytes(), e.wrapped_bytes());
    EXPECT_EQ(a.timestamp_value(), e.timestamp_value());
    EXPECT_EQ(a.duration_value(), e.duration_value());
    EXPECT_EQ(a.status(), e.status());
    EXPECT_EQ(a.nested_status(), e.nested_status());
}

void ExpectEquals(const gen::Branch& e, const gen::BranchView& a) {
    ExpectEquals(e.leaf(), a.leaf());
    if (e.optional_leaf() != nullptr) {
        ASSERT_TRUE(a.optional_leaf().has_value());
        ExpectEquals(*e.optional_leaf(), *a.optional_leaf());
    } else {
        EXPECT_FALSE(a.optional_leaf().has_value());
    }
    ASSERT_EQ(a.leaves().size(), static_cast<int64_t>(e.leaves().size()));
    for (int64_t i = 0; i < static_cast<int64_t>(e.leaves().size()); ++i)
        ExpectEquals(e.leaves()[static_cast<size_t>(i)], a.leaves()[i]);
}

void ExpectEquals(const gen::CompositeCoverage& e, const gen::CompositeCoverageView& a) {
    ExpectEquals(e.scalars(), a.scalars());

    if (e.optional_scalars() != nullptr) {
        ASSERT_TRUE(a.optional_scalars().has_value());
        ExpectEquals(*e.optional_scalars(), *a.optional_scalars());
    } else {
        EXPECT_FALSE(a.optional_scalars().has_value());
    }

    ExpectEquals(e.branch(), a.branch());

    if (e.optional_branch() != nullptr) {
        ASSERT_TRUE(a.optional_branch().has_value());
        ExpectEquals(*e.optional_branch(), *a.optional_branch());
    } else {
        EXPECT_FALSE(a.optional_branch().has_value());
    }

    // repeated scalar / string / bytes: size + order + element values.
    {
        const auto rs = a.repeated_scalar();
        ASSERT_EQ(rs.size(), static_cast<int64_t>(e.repeated_scalar().size()));
        for (int64_t i = 0; i < rs.size(); ++i)
            EXPECT_EQ(rs[i], e.repeated_scalar()[static_cast<size_t>(i)]);
    }
    {
        const auto rs = a.repeated_string();
        ASSERT_EQ(rs.size(), static_cast<int64_t>(e.repeated_string().size()));
        for (int64_t i = 0; i < rs.size(); ++i)
            EXPECT_EQ(rs[i], e.repeated_string()[static_cast<size_t>(i)]);
    }
    {
        const auto rb = a.repeated_bytes();
        ASSERT_EQ(rb.size(), static_cast<int64_t>(e.repeated_bytes().size()));
        for (int64_t i = 0; i < rb.size(); ++i)
            EXPECT_EQ(rb[i], e.repeated_bytes()[static_cast<size_t>(i)]);
    }
    {
        const auto rst = a.repeated_struct();
        ASSERT_EQ(rst.size(), static_cast<int64_t>(e.repeated_struct().size()));
        for (int64_t i = 0; i < rst.size(); ++i)
            ExpectEquals(e.repeated_struct()[static_cast<size_t>(i)], rst[i]);
    }

    // map<string,int32>: key-based (the generated API does not contract order).
    {
        const auto ms = a.map_scalar();
        std::map<std::string, int32_t> em, am;
        for (const auto& [k, v] : e.map_scalar()) em[k] = v;
        for (int64_t i = 0; i < ms.size(); ++i) am[std::string(ms.key(i))] = ms.value(i);
        EXPECT_EQ(am, em);
    }
    // map<string,Leaf>: key-based key set + per-key Leaf field equality.
    {
        const auto ms = a.map_struct();
        std::map<std::string, int64_t> aidx;
        for (int64_t i = 0; i < ms.size(); ++i) aidx[std::string(ms.key(i))] = i;
        ASSERT_EQ(aidx.size(), e.map_struct().size());
        for (const auto& [k, leaf] : e.map_struct()) {
            auto it = aidx.find(k);
            ASSERT_NE(it, aidx.end()) << "missing map_struct key " << k;
            ExpectEquals(leaf, ms.value(it->second));
        }
    }

    // message-level flatten struct-list + optional flatten struct-list.
    {
        const auto fsl = a.flattened_struct_list();
        ASSERT_EQ(fsl.size(), static_cast<int64_t>(e.flattened_struct_list().size()));
        for (int64_t i = 0; i < fsl.size(); ++i)
            ExpectEquals(e.flattened_struct_list()[static_cast<size_t>(i)], fsl[i]);
    }
    {
        const auto ofsl = a.optional_flattened_struct_list();
        ASSERT_EQ(ofsl.size(), static_cast<int64_t>(e.optional_flattened_struct_list().size()));
        for (int64_t i = 0; i < ofsl.size(); ++i)
            ExpectEquals(e.optional_flattened_struct_list()[static_cast<size_t>(i)], ofsl[i]);
    }

    // struct-leaf nested lists: depth 2.
    {
        const auto nsl = a.nested_struct_lists();
        ASSERT_EQ(nsl.size(), static_cast<int64_t>(e.nested_struct_lists().size()));
        for (int64_t i = 0; i < nsl.size(); ++i) {
            const auto inner = nsl[i];
            ASSERT_EQ(inner.size(),
                      static_cast<int64_t>(e.nested_struct_lists()[static_cast<size_t>(i)].size()));
            for (int64_t j = 0; j < inner.size(); ++j)
                ExpectEquals(e.nested_struct_lists()[static_cast<size_t>(i)][static_cast<size_t>(j)],
                             inner[j]);
        }
    }
    // struct-leaf nested lists: depth 3.
    {
        const auto d3 = a.depth3_struct_lists();
        ASSERT_EQ(d3.size(), static_cast<int64_t>(e.depth3_struct_lists().size()));
        for (int64_t i = 0; i < d3.size(); ++i) {
            const auto mid = d3[i];
            ASSERT_EQ(mid.size(),
                      static_cast<int64_t>(e.depth3_struct_lists()[static_cast<size_t>(i)].size()));
            for (int64_t j = 0; j < mid.size(); ++j) {
                const auto inner = mid[j];
                ASSERT_EQ(inner.size(), static_cast<int64_t>(
                              e.depth3_struct_lists()[static_cast<size_t>(i)][static_cast<size_t>(j)]
                                  .size()));
                for (int64_t k = 0; k < inner.size(); ++k)
                    ExpectEquals(e.depth3_struct_lists()[static_cast<size_t>(i)]
                                                        [static_cast<size_t>(j)]
                                                        [static_cast<size_t>(k)],
                                 inner[k]);
            }
        }
    }

    ExpectEquals(e.message_flattened_point(), a.message_flattened_point());
    ExpectEquals(e.field_flattened_position(), a.field_flattened_position());
}

void ExpectEquals(const gen::ServiceRequest& e, const gen::ServiceRequestView& a) {
    ExpectEquals(e.payload(), a.payload());
}

// ---------------------------------------------------------------------------
// The three oracle assertions, as reusable templates. ToArrowRow(row) is found
// by ADL (it is emitted into the gen:: namespace).
// ---------------------------------------------------------------------------

template <typename Row, typename SchemaFn>
void ExpectEncodeMatchesGoldenAndEncodeRow(const Row& row, SchemaFn schema_fn,
                                           const std::filesystem::path& golden_path) {
    const std::vector<uint8_t> golden_bytes = coverage_test::ReadFileBytes(golden_path);
    ASSERT_FALSE(golden_bytes.empty()) << golden_path;

    // (1) generated edge Encode() pinned to the historical golden.
    const fletcher::EncodedRow generated_bytes = row.Encode();
    EXPECT_EQ(generated_bytes, golden_bytes) << "generated Encode() != golden: " << golden_path;

    // (2) runtime codec EncodeRow(ToArrowRow(row)) pinned to the same golden —
    //     catches a coordinated reorder both encode paths would share.
    auto schema = coverage_test::ImportNano(schema_fn());
    ASSERT_NE(schema, nullptr);
    fletcher::Codec codec(std::move(schema));
    const fletcher::ArrowRow arrow_row = ToArrowRow(row);
    const fletcher::EncodedRow codec_bytes = codec.EncodeRow(arrow_row);
    EXPECT_EQ(codec_bytes, golden_bytes) << "Codec::EncodeRow() != golden: " << golden_path;

    // (3) both independent encode paths agree (the GIR-3..7 invariant).
    EXPECT_EQ(generated_bytes, codec_bytes)
        << "generated Encode() != Codec::EncodeRow(): " << golden_path;
}

template <typename View, typename Row, typename SchemaFn>
void ExpectRoundTripEquals(const Row& row, SchemaFn schema_fn) {
    // Named local outlives the decode + View (zero-copy borrow discipline).
    const fletcher::EncodedRow encoded_bytes = row.Encode();
    auto schema = coverage_test::ImportNano(schema_fn());
    ASSERT_NE(schema, nullptr);
    fletcher::Codec codec(std::move(schema));
    fletcher::ArrowRow decoded = codec.DecodeRow(encoded_bytes.data(), encoded_bytes.size());
    View view(std::move(decoded));
    ExpectEquals(row, view);
}

template <typename View, typename Expected, typename SchemaFn>
void ExpectGoldenDecodesTo(const std::filesystem::path& golden_path, const Expected& expected,
                           SchemaFn schema_fn) {
    const std::vector<uint8_t> golden_bytes = coverage_test::ReadFileBytes(golden_path);
    ASSERT_FALSE(golden_bytes.empty()) << golden_path;
    auto schema = coverage_test::ImportNano(schema_fn());
    ASSERT_NE(schema, nullptr);
    fletcher::Codec codec(std::move(schema));
    fletcher::ArrowRow decoded = codec.DecodeRow(golden_bytes.data(), golden_bytes.size());
    View view(std::move(decoded));
    ExpectEquals(expected, view);
}

// One entry per active message + fixture variant. Each drives all three oracle
// checks against its committed golden.
template <typename View, typename Row, typename SchemaFn>
void CheckFixture(const Row& row, SchemaFn schema_fn, const char* golden_name) {
    const auto golden = GoldenDir() / golden_name;
    SCOPED_TRACE(golden_name);
    ExpectEncodeMatchesGoldenAndEncodeRow(row, schema_fn, golden);
    ExpectRoundTripEquals<View>(row, schema_fn);
    ExpectGoldenDecodesTo<View>(golden, row, schema_fn);
}

}  // namespace

TEST(ParityOracle, EncodeEqualsEncodeRowAndRoundTrips) {
    CheckFixture<gen::ScalarCoverageView>(fx::MakeScalars(), gen::ScalarCoverageSchema,
                                          "coverage.ScalarCoverage.v1.bin");
    CheckFixture<gen::ScalarCoverageView>(fx::MakeScalarsAllSet(), gen::ScalarCoverageSchema,
                                          "coverage.ScalarCoverage.all-set.v1.bin");
    CheckFixture<gen::CompositeCoverageView>(fx::MakeComposite(), gen::CompositeCoverageSchema,
                                             "coverage.CompositeCoverage.v1.bin");
    CheckFixture<gen::CompositeCoverageView>(fx::MakeCompositeWithAlternateNullsAndEmpties(),
                                             gen::CompositeCoverageSchema,
                                             "coverage.CompositeCoverage.alternate-null-empty.v1.bin");
    CheckFixture<gen::CompositeCoverageView>(fx::MakeCompositeWithMapsNonSorted(),
                                             gen::CompositeCoverageSchema,
                                             "coverage.CompositeCoverage.maps-non-sorted.v1.bin");
    CheckFixture<gen::BranchView>(fx::MakeBranch(), gen::BranchSchema, "coverage.Branch.v1.bin");
    CheckFixture<gen::LeafView>(fx::MakeLeaf(42, "leaf", fx::kTopLevelStatusError),
                                gen::LeafSchema, "coverage.Leaf.v1.bin");
    CheckFixture<gen::NestedEnumsView>(fx::MakeNestedEnums(), gen::NestedEnumsSchema,
                                       "coverage.NestedEnums.v1.bin");
    CheckFixture<gen::FlattenedPointView>(fx::MakeFlattenedPoint(), gen::FlattenedPointSchema,
                                          "coverage.FlattenedPoint.v1.bin");
    CheckFixture<gen::FieldFlattenedPositionView>(fx::MakeFieldFlattenedPosition(),
                                                  gen::FieldFlattenedPositionSchema,
                                                  "coverage.FieldFlattenedPosition.v1.bin");
    CheckFixture<gen::ServiceRequestView>(fx::MakeServiceRequest(), gen::ServiceRequestSchema,
                                          "coverage.ServiceRequest.v1.bin");
    CheckFixture<gen::ServiceReplyView>(fx::MakeServiceReply(), gen::ServiceReplySchema,
                                        "coverage.ServiceReply.v1.bin");
}

// Gated golden (re)generation. NOT part of the normal contract: it is SKIPPED
// unless FLETCHER_REGEN_PARITY_GOLDENS is set in the environment. The bytes it
// writes are the CURRENT generator's Encode() output — that IS the wire
// contract being pinned — so any resulting diff under golden/*.bin must be
// reviewed as wire-contract churn (stop-and-ask for an already-supported input).
//
//   FLETCHER_REGEN_PARITY_GOLDENS=1 \
//     ./coverage_parity_oracle_tests --gtest_filter=ParityOracle.RegenerateGoldens
TEST(ParityOracle, RegenerateGoldens) {
    const char* flag = std::getenv("FLETCHER_REGEN_PARITY_GOLDENS");
    if (flag == nullptr || std::string(flag).empty())
        GTEST_SKIP() << "set FLETCHER_REGEN_PARITY_GOLDENS=1 to (re)write golden/*.bin";

    std::filesystem::create_directories(GoldenDir());
    auto write = [](const char* name, const fletcher::EncodedRow& bytes) {
        const auto path = GoldenDir() / name;
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.good()) << path;
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(out.good()) << path;
    };

    write("coverage.ScalarCoverage.v1.bin", fx::MakeScalars().Encode());
    write("coverage.ScalarCoverage.all-set.v1.bin", fx::MakeScalarsAllSet().Encode());
    write("coverage.CompositeCoverage.v1.bin", fx::MakeComposite().Encode());
    write("coverage.CompositeCoverage.alternate-null-empty.v1.bin",
          fx::MakeCompositeWithAlternateNullsAndEmpties().Encode());
    write("coverage.CompositeCoverage.maps-non-sorted.v1.bin",
          fx::MakeCompositeWithMapsNonSorted().Encode());
    write("coverage.Branch.v1.bin", fx::MakeBranch().Encode());
    write("coverage.Leaf.v1.bin", fx::MakeLeaf(42, "leaf", fx::kTopLevelStatusError).Encode());
    write("coverage.NestedEnums.v1.bin", fx::MakeNestedEnums().Encode());
    write("coverage.FlattenedPoint.v1.bin", fx::MakeFlattenedPoint().Encode());
    write("coverage.FieldFlattenedPosition.v1.bin", fx::MakeFieldFlattenedPosition().Encode());
    write("coverage.ServiceRequest.v1.bin", fx::MakeServiceRequest().Encode());
    write("coverage.ServiceReply.v1.bin", fx::MakeServiceReply().Encode());
}
