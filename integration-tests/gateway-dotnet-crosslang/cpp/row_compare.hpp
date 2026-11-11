// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C++ side's field-by-field comparison of a received CompositeCoverage with the fixture row
// it was built from (BIND-8d). Every field of every message CompositeCoverage reaches is named,
// so a difference reads as `row.branch.leaves[1].label: expected "b" got "x"`, not as a byte
// offset. The C# (RowCompare.cs) and TypeScript (compare.ts) peers compare the same fields.
#ifndef FLETCHER_CROSSLANG_ROW_COMPARE_HPP_
#define FLETCHER_CROSSLANG_ROW_COMPARE_HPP_

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "coverage_fixture.hpp"

namespace crosslang {

namespace gen = fletcher_gen::integration::coverage;

/// The fixture rows the lane sends, by `seq`: protoc-coverage's four CompositeCoverage rows.
inline constexpr int32_t kRowCount = 4;

inline gen::CompositeCoverage FixtureRow(int32_t seq) {
    switch (seq) {
        case 0:
            return coverage_fixture::MakeComposite();
        case 1:
            return coverage_fixture::MakeCompositeWithAlternateNullsAndEmpties();
        case 2:
            return coverage_fixture::MakeCompositeWithMapsNonSorted();
        default:
            return coverage_fixture::MakeCompositeDefaulted();
    }
}

class Diff {
   public:
    const std::vector<std::string>& differences() const { return out_; }

    template <typename T>
    void Eq(const std::string& path, const T& expected, const T& actual) {
        if (!(expected == actual)) Add(path, Show(expected), Show(actual));
    }

    void Bytes(const std::string& path, std::string_view expected, std::string_view actual) {
        if (expected != actual) Add(path, Hex(expected), Hex(actual));
    }

    template <typename T>
    void Opt(const std::string& path, const std::optional<T>& expected,
             const std::optional<T>& actual) {
        if (expected.has_value() != actual.has_value()) {
            Add(path, expected ? Show(*expected) : "null", actual ? Show(*actual) : "null");
        } else if (expected && !(*expected == *actual)) {
            Add(path, Show(*expected), Show(*actual));
        }
    }

    void OptBytes(const std::string& path, const std::optional<std::string_view>& expected,
                  const std::optional<std::string_view>& actual) {
        if (expected.has_value() != actual.has_value()) {
            Add(path, expected ? Hex(*expected) : "null", actual ? Hex(*actual) : "null");
        } else if (expected) {
            Bytes(path, *expected, *actual);
        }
    }

    // Both present or both absent; `compare` runs on a present pair.
    template <typename T, typename F>
    void Ptr(const std::string& path, const T* expected, const T* actual, F compare) {
        if ((expected == nullptr) != (actual == nullptr)) {
            Add(path, expected ? "present" : "null", actual ? "present" : "null");
        } else if (expected) {
            compare(path, *expected, *actual);
        }
    }

    template <typename T, typename F>
    void List(const std::string& path, const std::vector<T>& expected, const std::vector<T>& actual,
              F compare) {
        if (expected.size() != actual.size()) {
            Add(path + ".length", std::to_string(expected.size()), std::to_string(actual.size()));
            return;
        }
        for (size_t i = 0; i < expected.size(); ++i)
            compare(path + "[" + std::to_string(i) + "]", expected[i], actual[i]);
    }

   private:
    void Add(const std::string& path, const std::string& expected, const std::string& actual) {
        out_.push_back(path + ": expected " + expected + " got " + actual);
    }

    template <typename T>
    static std::string Show(const T& v) {
        std::ostringstream s;
        if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>) {
            s << '"' << v << '"';
        } else if constexpr (std::is_same_v<T, bool>) {
            s << (v ? "true" : "false");
        } else if constexpr (std::is_floating_point_v<T>) {
            s << std::setprecision(17) << v;
        } else {
            s << +v;
        }
        return s.str();
    }

    static std::string Hex(std::string_view bytes) {
        std::ostringstream s;
        s << "0x";
        for (unsigned char c : bytes) s << std::hex << std::setw(2) << std::setfill('0') << +c;
        return s.str();
    }

    std::vector<std::string> out_;
};

inline void CompareLeaf(Diff& d, const std::string& p, const gen::Leaf& e, const gen::Leaf& a) {
    d.Eq(p + ".id", e.id(), a.id());
    d.Eq(p + ".label", e.label(), a.label());
    d.Eq(p + ".status", e.status(), a.status());
}

inline void CompareBranch(Diff& d, const std::string& p, const gen::Branch& e,
                          const gen::Branch& a) {
    auto leaf = [&d](const std::string& q, const gen::Leaf& x, const gen::Leaf& y) {
        CompareLeaf(d, q, x, y);
    };
    leaf(p + ".leaf", e.leaf(), a.leaf());
    d.Ptr(p + ".optional_leaf", e.optional_leaf(), a.optional_leaf(), leaf);
    d.List(p + ".leaves", e.leaves(), a.leaves(), leaf);
}

inline void CompareScalars(Diff& d, const std::string& p, const gen::ScalarCoverage& e,
                           const gen::ScalarCoverage& a) {
    d.Eq(p + ".bool_value", e.bool_value(), a.bool_value());
    d.Eq(p + ".int32_value", e.int32_value(), a.int32_value());
    d.Eq(p + ".int64_value", e.int64_value(), a.int64_value());
    d.Eq(p + ".uint32_value", e.uint32_value(), a.uint32_value());
    d.Eq(p + ".uint64_value", e.uint64_value(), a.uint64_value());
    d.Eq(p + ".sint32_value", e.sint32_value(), a.sint32_value());
    d.Eq(p + ".sint64_value", e.sint64_value(), a.sint64_value());
    d.Eq(p + ".fixed32_value", e.fixed32_value(), a.fixed32_value());
    d.Eq(p + ".fixed64_value", e.fixed64_value(), a.fixed64_value());
    d.Eq(p + ".sfixed32_value", e.sfixed32_value(), a.sfixed32_value());
    d.Eq(p + ".sfixed64_value", e.sfixed64_value(), a.sfixed64_value());
    d.Eq(p + ".float_value", e.float_value(), a.float_value());
    d.Eq(p + ".double_value", e.double_value(), a.double_value());
    d.Eq(p + ".string_value", e.string_value(), a.string_value());
    d.Bytes(p + ".bytes_value", e.bytes_value(), a.bytes_value());
    d.Opt(p + ".optional_bool", e.optional_bool(), a.optional_bool());
    d.Opt(p + ".optional_int32", e.optional_int32(), a.optional_int32());
    d.Opt(p + ".optional_string", e.optional_string(), a.optional_string());
    d.OptBytes(p + ".optional_bytes", e.optional_bytes(), a.optional_bytes());
    d.Opt(p + ".wrapped_bool", e.wrapped_bool(), a.wrapped_bool());
    d.Opt(p + ".wrapped_int32", e.wrapped_int32(), a.wrapped_int32());
    d.Opt(p + ".wrapped_int64", e.wrapped_int64(), a.wrapped_int64());
    d.Opt(p + ".wrapped_uint32", e.wrapped_uint32(), a.wrapped_uint32());
    d.Opt(p + ".wrapped_uint64", e.wrapped_uint64(), a.wrapped_uint64());
    d.Opt(p + ".wrapped_float", e.wrapped_float(), a.wrapped_float());
    d.Opt(p + ".wrapped_double", e.wrapped_double(), a.wrapped_double());
    d.Opt(p + ".wrapped_string", e.wrapped_string(), a.wrapped_string());
    d.OptBytes(p + ".wrapped_bytes", e.wrapped_bytes(), a.wrapped_bytes());
    d.Eq(p + ".timestamp_value", e.timestamp_value(), a.timestamp_value());
    d.Eq(p + ".duration_value", e.duration_value(), a.duration_value());
    d.Eq(p + ".status", e.status(), a.status());
    d.Eq(p + ".nested_status", e.nested_status(), a.nested_status());
}

inline std::vector<std::string> CompareComposite(const gen::CompositeCoverage& e,
                                                 const gen::CompositeCoverage& a) {
    Diff d;
    const std::string p = "row";
    auto scalars = [&d](const std::string& q, const gen::ScalarCoverage& x,
                        const gen::ScalarCoverage& y) { CompareScalars(d, q, x, y); };
    auto branch = [&d](const std::string& q, const gen::Branch& x, const gen::Branch& y) {
        CompareBranch(d, q, x, y);
    };
    auto leaf = [&d](const std::string& q, const gen::Leaf& x, const gen::Leaf& y) {
        CompareLeaf(d, q, x, y);
    };
    auto leaves = [&d, &leaf](const std::string& q, const std::vector<gen::Leaf>& x,
                              const std::vector<gen::Leaf>& y) { d.List(q, x, y, leaf); };
    auto text = [&d](const std::string& q, const std::string& x, const std::string& y) {
        d.Eq(q, std::string_view(x), std::string_view(y));
    };
    auto bytes = [&d](const std::string& q, const std::string& x, const std::string& y) {
        d.Bytes(q, x, y);
    };
    auto int32 = [&d](const std::string& q, int32_t x, int32_t y) { d.Eq(q, x, y); };

    scalars(p + ".scalars", e.scalars(), a.scalars());
    d.Ptr(p + ".optional_scalars", e.optional_scalars(), a.optional_scalars(), scalars);
    branch(p + ".branch", e.branch(), a.branch());
    d.Ptr(p + ".optional_branch", e.optional_branch(), a.optional_branch(), branch);
    d.List(p + ".repeated_scalar", e.repeated_scalar(), a.repeated_scalar(), int32);
    d.List(p + ".repeated_string", e.repeated_string(), a.repeated_string(), text);
    d.List(p + ".repeated_bytes", e.repeated_bytes(), a.repeated_bytes(), bytes);
    leaves(p + ".repeated_struct", e.repeated_struct(), a.repeated_struct());
    // Maps in entry order: a map's order is part of what crosses (the maps-non-sorted row).
    d.List(p + ".map_scalar", e.map_scalar(), a.map_scalar(),
           [&](const std::string& q, const auto& x, const auto& y) {
               text(q + ".key", x.first, y.first);
               int32(q + ".value", x.second, y.second);
           });
    d.List(p + ".map_struct", e.map_struct(), a.map_struct(),
           [&](const std::string& q, const auto& x, const auto& y) {
               text(q + ".key", x.first, y.first);
               leaf(q + ".value", x.second, y.second);
           });
    leaves(p + ".flattened_struct_list", e.flattened_struct_list(), a.flattened_struct_list());
    d.List(p + ".nested_struct_lists", e.nested_struct_lists(), a.nested_struct_lists(), leaves);
    d.List(p + ".depth3_struct_lists", e.depth3_struct_lists(), a.depth3_struct_lists(),
           [&](const std::string& q, const auto& x, const auto& y) { d.List(q, x, y, leaves); });
    leaves(p + ".optional_flattened_struct_list", e.optional_flattened_struct_list(),
           a.optional_flattened_struct_list());
    d.Eq(p + ".message_flattened_point.x", e.message_flattened_point().x(),
         a.message_flattened_point().x());
    d.Eq(p + ".message_flattened_point.y", e.message_flattened_point().y(),
         a.message_flattened_point().y());
    d.Eq(p + ".field_flattened_position.x", e.field_flattened_position().x(),
         a.field_flattened_position().x());
    d.Eq(p + ".field_flattened_position.y", e.field_flattened_position().y(),
         a.field_flattened_position().y());
    return d.differences();
}

/// The line a peer prints for a received row: `RECV <seq> OK`, `RECV <seq> MISMATCH <n>: <diffs>`
/// or `RECV <seq> UNKNOWN` for a `seq` the fixture does not have.
inline std::string ReportRow(int32_t seq, const gen::CompositeCoverage& actual) {
    if (seq < 0 || seq >= kRowCount) return "RECV " + std::to_string(seq) + " UNKNOWN";
    const std::vector<std::string> diffs = CompareComposite(FixtureRow(seq), actual);
    if (diffs.empty()) return "RECV " + std::to_string(seq) + " OK";
    std::string line =
        "RECV " + std::to_string(seq) + " MISMATCH " + std::to_string(diffs.size()) + ":";
    for (const auto& diff : diffs) line += " " + diff + ";";
    return line;
}

}  // namespace crosslang

#endif  // FLETCHER_CROSSLANG_ROW_COMPARE_HPP_
