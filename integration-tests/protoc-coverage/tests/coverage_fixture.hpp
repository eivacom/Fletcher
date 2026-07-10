// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-1 shared coverage fixture. Centralizes construction of one populated
// CompositeCoverage row (with empty + non-empty containers and set + unset
// optionals) so both the harness test and the accessor test build the SAME
// row, and GIR-2's parity oracle can reuse the exact fixture without
// re-transcribing values.
//
// Enum values are plain int32 literals: GIR-1 exercises the current
// enum-as-int32 lowering (no typed C++ enum symbols are emitted; GIR-9 adds
// those against this same fixture). These are NOT an IR type table — they are
// literal expected values for the current generated getters.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coverage.fletcher.pb.h"

namespace coverage_fixture {

namespace gen = fletcher_gen::integration::coverage;

// enum-as-int32 expected values (mirror the proto enum numbers).
inline constexpr int32_t kTopLevelStatusOk = 1;
inline constexpr int32_t kTopLevelStatusWarn = 2;
inline constexpr int32_t kInnerStatusActive = 1;

// Representative scalar expected values.
inline constexpr int32_t kInt32 = -42;
inline constexpr int64_t kInt64 = 9'000'000'000LL;
inline constexpr int32_t kOptInt32 = 7;
inline constexpr int32_t kWrappedInt32 = 123;
inline constexpr int64_t kTimestampNs = 1'700'000'000'000'000'000LL;
inline constexpr int64_t kDurationNs = 5'000'000'000LL;

inline const std::string& StringValue() {
    static const std::string v = "coverage";
    return v;
}
inline const std::string& BytesValue() {
    static const std::string v("\x01\x02\x03", 3);
    return v;
}

inline gen::ScalarCoverage MakeScalars() {
    gen::ScalarCoverage s;
    s.set_bool_value(true)
        .set_int32_value(kInt32)
        .set_int64_value(kInt64)
        .set_uint32_value(7u)
        .set_uint64_value(8ull)
        .set_sint32_value(-9)
        .set_sint64_value(-10)
        .set_fixed32_value(11u)
        .set_fixed64_value(12ull)
        .set_sfixed32_value(-13)
        .set_sfixed64_value(-14)
        .set_float_value(1.5f)
        .set_double_value(2.5)
        .set_string_value(StringValue())
        .set_bytes_value(BytesValue())
        // optional: set optional_int32; leave optional_bool/string/bytes UNSET.
        .set_optional_int32(kOptInt32)
        // WKT wrappers: set the int32 wrapper; leave the rest unset (nullable).
        .set_wrapped_int32(kWrappedInt32)
        // WKT timestamp / duration (nanoseconds).
        .set_timestamp_value(kTimestampNs)
        .set_duration_value(kDurationNs)
        // enums (int32-lowered): package-scope + nested.
        .set_status(kTopLevelStatusWarn)
        .set_nested_status(kInnerStatusActive);
    return s;
}

inline gen::Leaf MakeLeaf(int32_t id, std::string_view label, int32_t status) {
    gen::Leaf l;
    l.set_id(id).set_label(label).set_status(status);
    return l;
}

// One broad row touching every construct that stays in coverage.proto for
// GIR-1: scalars/WKT/enums, nested + optional structs, repeated scalar/struct
// (incl. an empty container), scalar + message maps, message-level and
// field-level flatten, and struct-leaf nested lists (depth 2 and 3).
inline gen::CompositeCoverage MakeComposite() {
    gen::CompositeCoverage c;
    c.set_scalars(MakeScalars());
    c.set_optional_scalars(MakeScalars());  // set optional message
    // optional_branch intentionally left UNSET -> null after decode.

    gen::Branch b;
    b.set_leaf(MakeLeaf(1, "root", kTopLevelStatusOk));
    b.set_optional_leaf(MakeLeaf(2, "opt", kTopLevelStatusWarn));
    b.set_leaves({MakeLeaf(3, "a", kTopLevelStatusOk), MakeLeaf(4, "b", kTopLevelStatusWarn)});
    c.set_branch(b);

    c.set_repeated_scalar({10, 20, 30});
    c.set_repeated_string({"x", "y"});
    c.set_repeated_bytes({});  // empty container
    c.set_repeated_struct({MakeLeaf(5, "s0", kTopLevelStatusOk), MakeLeaf(6, "s1", kTopLevelStatusWarn)});

    c.set_map_scalar({{"a", 1}, {"b", 2}});
    c.set_map_struct({{"k", MakeLeaf(7, "mk", kTopLevelStatusWarn)}});

    // Message-level flatten (StructListWrapper single repeated field -> list<struct>).
    c.set_flattened_struct_list({MakeLeaf(8, "f0", kTopLevelStatusOk)});
    // Struct-leaf nested lists: depth 2 (one non-empty inner list + one empty).
    c.set_nested_struct_lists({{MakeLeaf(9, "n0", kTopLevelStatusOk)}, {}});
    // Struct-leaf nested lists: depth 3.
    c.set_depth3_struct_lists({{{MakeLeaf(10, "d0", kTopLevelStatusWarn)}}});
    // Optional flatten wrapper (set).
    c.set_optional_flattened_struct_list({MakeLeaf(11, "of", kTopLevelStatusOk)});

    // Multi-field message flatten -> struct<x,y> (flatten ignored for >1 field).
    gen::FlattenedPoint p;
    p.set_x(1.25).set_y(2.5);
    c.set_message_flattened_point(p);

    // Field-level flatten: FieldFlattenedPosition inlines x,y directly.
    gen::FieldFlattenedPosition fp;
    fp.set_x(3.75).set_y(4.5);
    c.set_field_flattened_position(fp);
    return c;
}

}  // namespace coverage_fixture
