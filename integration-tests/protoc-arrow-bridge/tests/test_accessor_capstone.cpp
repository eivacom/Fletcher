// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// RBA-7 capstone (C++ half of accessor_cpp_and_rust_agree_on_same_batch).
//
// This test and integration-tests/protoc-gen-fletcher-rust/tests/
// accessor_capstone.rs are paired: BOTH read the SAME committed
//   - schema  : integration-tests/accessor-capstone/proto/accessor_capstone.proto
//   - fixture : integration-tests/accessor-capstone/fixtures/
//               accessor_capstone_fixture.json
//   - oracle  : integration-tests/accessor-capstone/fixtures/
//               accessor_capstone_expected.json
// build an in-memory Arrow batch from the fixture (native Arrow builders, not
// IPC), construct the generated CapstoneBatchAccessor, normalize every read into
// the oracle's shape, and assert observed == expected. Because both languages
// compare to the ONE committed oracle, observed_cpp == expected (here) AND
// observed_rust == expected (Rust) transitively proves observed_cpp ==
// observed_rust (D-RBA-8). Expected values are NEVER hand-transcribed into this
// .cpp — they are parsed from the shared JSON.
//
// Covered (every field kind + a representative null in each optional path):
//   scalar non-nullable (id), scalar nullable (label, null row),
//   STRUCT (maybe_child, null row), REPEATED_SCALAR (samples, null element via
//   is_null(j)), REPEATED_STRUCT (children, null element), MAP scalar value
//   (scores, null value via value_is_null(j)), MAP message value (child_by_key,
//   null message value), NESTED_LIST (nested_children, null inner list), plus
//   schema + positional field metadata (absent -> nullptr normalized to {}).

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "accessor_capstone.fletcher.accessor.pb.h"

namespace {

using json = nlohmann::json;
using fletcher_gen::rba::capstone::CapstoneBatchAccessor;
using fletcher_gen::rba::capstone::ChildAccessor;

// Path to the shared capstone fixtures dir, supplied by CMake.
json LoadJson(const std::string& filename) {
    const std::string path = std::string(CAPSTONE_FIXTURES_DIR) + "/" + filename;
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot open capstone fixture: " << path;
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

// ---------------------------------------------------------------------------
// Arrow type helpers — the schema the generated accessor validates against.
// ---------------------------------------------------------------------------

// Child = struct<score:int32 (non-nullable), name:utf8 (non-nullable)>.
std::shared_ptr<arrow::DataType> ChildType() {
    return arrow::struct_({arrow::field("score", arrow::int32(), /*nullable=*/false),
                           arrow::field("name", arrow::utf8(), /*nullable=*/false)});
}

std::shared_ptr<arrow::Buffer> MakeValidityBuffer(const std::vector<bool>& valid) {
    if (valid.empty()) return nullptr;
    arrow::TypedBufferBuilder<bool> bb;
    for (bool v : valid) EXPECT_TRUE(bb.Append(v).ok());
    std::shared_ptr<arrow::Buffer> buf;
    EXPECT_TRUE(bb.Finish(&buf).ok());
    return buf;
}

// Build a Child StructArray from a flat list of {score,name} (null = struct
// null). `j` over the flattened element list; `valid[j]` false -> a null Child.
std::shared_ptr<arrow::StructArray> MakeChildArray(const std::vector<int32_t>& scores,
                                                   const std::vector<std::string>& names,
                                                   const std::vector<bool>& valid) {
    arrow::Int32Builder sb;
    EXPECT_TRUE(sb.AppendValues(scores).ok());
    std::shared_ptr<arrow::Array> score_arr;
    EXPECT_TRUE(sb.Finish(&score_arr).ok());

    arrow::StringBuilder nb;
    for (const auto& n : names) EXPECT_TRUE(nb.Append(n).ok());
    std::shared_ptr<arrow::Array> name_arr;
    EXPECT_TRUE(nb.Finish(&name_arr).ok());

    auto r = arrow::StructArray::Make(
        {score_arr, name_arr},
        {arrow::field("score", arrow::int32(), false), arrow::field("name", arrow::utf8(), false)},
        MakeValidityBuffer(valid));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::StructArray>(*r);
}

// ---------------------------------------------------------------------------
// Build the Arrow columns directly from the shared fixture JSON `rows`.
// ---------------------------------------------------------------------------
struct Columns {
    std::shared_ptr<arrow::Array> id, label, maybe_child, samples, children, scores, child_by_key,
        nested_children;
    arrow::ArrayVector AsVector() const {
        return {id, label, maybe_child, samples, children, scores, child_by_key, nested_children};
    }
};

// id: non-nullable int32.
std::shared_ptr<arrow::Array> BuildId(const json& rows) {
    arrow::Int32Builder b;
    for (const auto& row : rows) EXPECT_TRUE(b.Append(row.at("id").get<int32_t>()).ok());
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(b.Finish(&out).ok());
    return out;
}

// label: nullable utf8 (JSON null -> Arrow null).
std::shared_ptr<arrow::Array> BuildLabel(const json& rows) {
    arrow::StringBuilder b;
    for (const auto& row : rows) {
        const auto& v = row.at("label");
        if (v.is_null())
            EXPECT_TRUE(b.AppendNull().ok());
        else
            EXPECT_TRUE(b.Append(v.get<std::string>()).ok());
    }
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(b.Finish(&out).ok());
    return out;
}

// maybe_child: nullable struct<Child>. A JSON null row -> a null struct element.
std::shared_ptr<arrow::Array> BuildMaybeChild(const json& rows) {
    std::vector<int32_t> scores;
    std::vector<std::string> names;
    std::vector<bool> valid;
    for (const auto& row : rows) {
        const auto& c = row.at("maybe_child");
        if (c.is_null()) {
            scores.push_back(0);
            names.push_back("");
            valid.push_back(false);
        } else {
            scores.push_back(c.at("score").get<int32_t>());
            names.push_back(c.at("name").get<std::string>());
            valid.push_back(true);
        }
    }
    return MakeChildArray(scores, names, valid);
}

// samples: list<int32>. Element JSON null -> a null scalar element.
std::shared_ptr<arrow::Array> BuildSamples(const json& rows) {
    arrow::Int32Builder vb;
    std::vector<int32_t> offsets;
    int32_t off = 0;
    offsets.push_back(off);
    for (const auto& row : rows) {
        for (const auto& s : row.at("samples")) {
            if (s.is_null())
                EXPECT_TRUE(vb.AppendNull().ok());
            else
                EXPECT_TRUE(vb.Append(s.get<int32_t>()).ok());
            ++off;
        }
        offsets.push_back(off);
    }
    std::shared_ptr<arrow::Array> vals;
    EXPECT_TRUE(vb.Finish(&vals).ok());
    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off_arr;
    EXPECT_TRUE(ob.Finish(&off_arr).ok());
    auto r = arrow::ListArray::FromArrays(*off_arr, *vals);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return *r;
}

// children: list<struct<Child>>. Element JSON null -> a null struct element.
std::shared_ptr<arrow::Array> BuildChildren(const json& rows) {
    std::vector<int32_t> scores;
    std::vector<std::string> names;
    std::vector<bool> valid;
    std::vector<int32_t> offsets;
    int32_t off = 0;
    offsets.push_back(off);
    for (const auto& row : rows) {
        for (const auto& c : row.at("children")) {
            if (c.is_null()) {
                scores.push_back(0);
                names.push_back("");
                valid.push_back(false);
            } else {
                scores.push_back(c.at("score").get<int32_t>());
                names.push_back(c.at("name").get<std::string>());
                valid.push_back(true);
            }
            ++off;
        }
        offsets.push_back(off);
    }
    auto child = MakeChildArray(scores, names, valid);
    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off_arr;
    EXPECT_TRUE(ob.Finish(&off_arr).ok());
    auto r = arrow::ListArray::FromArrays(*off_arr, *child);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return *r;
}

// scores: map<utf8,int32>. Value JSON null -> a null scalar map value.
std::shared_ptr<arrow::Array> BuildScores(const json& rows) {
    arrow::StringBuilder kb;
    arrow::Int32Builder vb;
    std::vector<int32_t> offsets;
    int32_t off = 0;
    offsets.push_back(off);
    for (const auto& row : rows) {
        for (const auto& kv : row.at("scores")) {
            EXPECT_TRUE(kb.Append(kv.at("key").get<std::string>()).ok());
            const auto& v = kv.at("value");
            if (v.is_null())
                EXPECT_TRUE(vb.AppendNull().ok());
            else
                EXPECT_TRUE(vb.Append(v.get<int32_t>()).ok());
            ++off;
        }
        offsets.push_back(off);
    }
    std::shared_ptr<arrow::Array> karr, varr;
    EXPECT_TRUE(kb.Finish(&karr).ok());
    EXPECT_TRUE(vb.Finish(&varr).ok());
    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off_arr;
    EXPECT_TRUE(ob.Finish(&off_arr).ok());
    auto r = arrow::MapArray::FromArrays(off_arr, karr, varr);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return *r;
}

// child_by_key: map<utf8,struct<Child>>. Value JSON null -> a null struct value.
std::shared_ptr<arrow::Array> BuildChildByKey(const json& rows) {
    arrow::StringBuilder kb;
    std::vector<int32_t> scores;
    std::vector<std::string> names;
    std::vector<bool> valid;
    std::vector<int32_t> offsets;
    int32_t off = 0;
    offsets.push_back(off);
    for (const auto& row : rows) {
        for (const auto& kv : row.at("child_by_key")) {
            EXPECT_TRUE(kb.Append(kv.at("key").get<std::string>()).ok());
            const auto& v = kv.at("value");
            if (v.is_null()) {
                scores.push_back(0);
                names.push_back("");
                valid.push_back(false);
            } else {
                scores.push_back(v.at("score").get<int32_t>());
                names.push_back(v.at("name").get<std::string>());
                valid.push_back(true);
            }
            ++off;
        }
        offsets.push_back(off);
    }
    std::shared_ptr<arrow::Array> karr;
    EXPECT_TRUE(kb.Finish(&karr).ok());
    auto vals = MakeChildArray(scores, names, valid);
    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off_arr;
    EXPECT_TRUE(ob.Finish(&off_arr).ok());
    auto r = arrow::MapArray::FromArrays(off_arr, karr, vals);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return *r;
}

// nested_children: list<list<struct<Child>>> (depth 2). An inner-list JSON null
// -> a null inner list (row-level validity on the inner list level).
std::shared_ptr<arrow::Array> BuildNestedChildren(const json& rows) {
    // Flatten leaf Child elements.
    std::vector<int32_t> scores;
    std::vector<std::string> names;
    std::vector<bool> leaf_valid;
    // Inner list level: per inner list, an offset and a row-validity flag.
    std::vector<int32_t> inner_offsets;
    std::vector<bool> inner_valid;
    int32_t leaf_off = 0;
    inner_offsets.push_back(leaf_off);
    // Outer list level: per row, the count of inner lists.
    std::vector<int32_t> outer_offsets;
    int32_t inner_idx = 0;
    outer_offsets.push_back(inner_idx);

    for (const auto& row : rows) {
        for (const auto& inner : row.at("nested_children")) {
            if (inner.is_null()) {
                // A null inner list contributes one inner-list slot with no leaf
                // elements and a false validity flag.
                inner_offsets.push_back(leaf_off);
                inner_valid.push_back(false);
            } else {
                for (const auto& c : inner) {
                    scores.push_back(c.at("score").get<int32_t>());
                    names.push_back(c.at("name").get<std::string>());
                    leaf_valid.push_back(true);
                    ++leaf_off;
                }
                inner_offsets.push_back(leaf_off);
                inner_valid.push_back(true);
            }
            ++inner_idx;
        }
        outer_offsets.push_back(inner_idx);
    }

    auto leaf = MakeChildArray(scores, names, leaf_valid);

    // Inner list array (with row-level nulls for the null inner lists).
    arrow::Int32Builder iob;
    EXPECT_TRUE(iob.AppendValues(inner_offsets).ok());
    std::shared_ptr<arrow::Array> inner_off_arr;
    EXPECT_TRUE(iob.Finish(&inner_off_arr).ok());
    auto inner_res = arrow::ListArray::FromArrays(
        *inner_off_arr, *leaf, arrow::default_memory_pool(), MakeValidityBuffer(inner_valid));
    EXPECT_TRUE(inner_res.ok()) << inner_res.status().ToString();

    // Outer list array.
    arrow::Int32Builder oob;
    EXPECT_TRUE(oob.AppendValues(outer_offsets).ok());
    std::shared_ptr<arrow::Array> outer_off_arr;
    EXPECT_TRUE(oob.Finish(&outer_off_arr).ok());
    auto outer_res = arrow::ListArray::FromArrays(*outer_off_arr, **inner_res);
    EXPECT_TRUE(outer_res.ok()) << outer_res.status().ToString();
    return *outer_res;
}

Columns BuildColumns(const json& rows) {
    Columns c;
    c.id = BuildId(rows);
    c.label = BuildLabel(rows);
    c.maybe_child = BuildMaybeChild(rows);
    c.samples = BuildSamples(rows);
    c.children = BuildChildren(rows);
    c.scores = BuildScores(rows);
    c.child_by_key = BuildChildByKey(rows);
    c.nested_children = BuildNestedChildren(rows);
    return c;
}

// Schema, with field types taken from the built columns and schema/field
// metadata attached from the fixture (positional field_metadata index).
std::shared_ptr<arrow::Schema> BuildSchema(const json& fixture, const Columns& c) {
    std::vector<std::shared_ptr<arrow::Field>> fields = {
        arrow::field("id", c.id->type(), /*nullable=*/false),
        arrow::field("label", c.label->type(), /*nullable=*/true),
        arrow::field("maybe_child", c.maybe_child->type(), /*nullable=*/true),
        arrow::field("samples", c.samples->type(), /*nullable=*/false),
        arrow::field("children", c.children->type(), /*nullable=*/false),
        arrow::field("scores", c.scores->type(), /*nullable=*/false),
        arrow::field("child_by_key", c.child_by_key->type(), /*nullable=*/false),
        arrow::field("nested_children", c.nested_children->type(), /*nullable=*/false),
    };
    // Per-field metadata (positional). Absent entries ({}) carry no metadata.
    const auto& fmeta = fixture.at("field_metadata");
    for (size_t i = 0; i < fields.size() && i < fmeta.size(); ++i) {
        const auto& m = fmeta[i];
        if (m.is_object() && !m.empty()) {
            std::vector<std::string> keys, vals;
            for (auto it = m.begin(); it != m.end(); ++it) {
                keys.push_back(it.key());
                vals.push_back(it.value().get<std::string>());
            }
            fields[i] = fields[i]->WithMetadata(arrow::KeyValueMetadata::Make(keys, vals));
        }
    }
    // Schema-level metadata.
    std::vector<std::string> keys, vals;
    for (auto it = fixture.at("schema_metadata").begin(); it != fixture.at("schema_metadata").end();
         ++it) {
        keys.push_back(it.key());
        vals.push_back(it.value().get<std::string>());
    }
    auto schema_md = arrow::KeyValueMetadata::Make(keys, vals);
    return arrow::schema(fields, schema_md);
}

// ---------------------------------------------------------------------------
// Normalize the observed accessor readout into the oracle's JSON shape.
// ---------------------------------------------------------------------------

// C++ nullptr metadata <=> Rust empty map <=> {}. Sorted by key (nlohmann json
// objects are key-sorted), so the comparison is order-independent.
json MetadataToJson(const arrow::KeyValueMetadata* md) {
    json obj = json::object();
    if (md == nullptr) return obj;
    for (int i = 0; i < md->size(); ++i) obj[md->key(i)] = md->value(i);
    return obj;
}

// A Child RowView -> {"score","name"}.
json ChildToJson(const ChildAccessor::RowView& v) {
    json o = json::object();
    o["score"] = v.score();
    o["name"] = std::string(v.name());
    return o;
}

json NormalizeRow(const CapstoneBatchAccessor& a, int64_t row) {
    json o = json::object();

    // scalar non-nullable.
    o["id"] = a.id(row);

    // scalar nullable -> value | null.
    if (auto lbl = a.label(row))
        o["label"] = std::string(*lbl);
    else
        o["label"] = nullptr;

    // STRUCT (nullable 1:1) -> {...} | null.
    if (auto mc = a.maybe_child(row))
        o["maybe_child"] = ChildToJson(*mc);
    else
        o["maybe_child"] = nullptr;

    // REPEATED_SCALAR -> [value|null,...], element null via is_null(j).
    {
        json arr = json::array();
        auto span = a.samples(row);
        for (int64_t j = 0; j < span.size(); ++j) {
            if (span.is_null(j))
                arr.push_back(nullptr);
            else
                arr.push_back(span[j]);
        }
        o["samples"] = arr;
    }

    // REPEATED_STRUCT -> [{...}|null,...], element null via span[j] == nullopt.
    {
        json arr = json::array();
        auto span = a.children(row);
        for (int64_t j = 0; j < span.size(); ++j) {
            if (auto e = span[j])
                arr.push_back(ChildToJson(*e));
            else
                arr.push_back(nullptr);
        }
        o["children"] = arr;
    }

    // MAP scalar value -> [{key,value}], value null via value_is_null(j).
    {
        json arr = json::array();
        auto m = a.scores(row);
        for (int64_t j = 0; j < m.size(); ++j) {
            json kv = json::object();
            kv["key"] = std::string(m.key(j));
            if (m.value_is_null(j))
                kv["value"] = nullptr;
            else
                kv["value"] = m.value(j);
            arr.push_back(kv);
        }
        o["scores"] = arr;
    }

    // MAP message value -> [{key,value}], value null via value(j) == nullopt.
    {
        json arr = json::array();
        auto m = a.child_by_key(row);
        for (int64_t j = 0; j < m.size(); ++j) {
            json kv = json::object();
            kv["key"] = std::string(m.key(j));
            if (auto v = m.value(j))
                kv["value"] = ChildToJson(*v);
            else
                kv["value"] = nullptr;
            arr.push_back(kv);
        }
        o["child_by_key"] = arr;
    }

    // NESTED_LIST depth 2 -> [[{...},...] | null, ...], inner null via
    // span[i] == nullopt.
    {
        json arr = json::array();
        auto outer = a.nested_children(row);
        for (int64_t i = 0; i < outer.size(); ++i) {
            if (auto inner = outer[i]) {
                json inner_arr = json::array();
                for (int64_t j = 0; j < inner->size(); ++j) {
                    if (auto e = (*inner)[j])
                        inner_arr.push_back(ChildToJson(*e));
                    else
                        inner_arr.push_back(nullptr);
                }
                arr.push_back(inner_arr);
            } else {
                arr.push_back(nullptr);
            }
        }
        o["nested_children"] = arr;
    }

    return o;
}

// Produce the full observed readout (schema metadata + positional field metadata
// + per-row normalized values) for `a`.
json NormalizeReadout(const CapstoneBatchAccessor& a, int field_count) {
    json out = json::object();
    out["schema_metadata"] = MetadataToJson(a.schema_metadata());
    json fmeta = json::array();
    for (int i = 0; i < field_count; ++i) fmeta.push_back(MetadataToJson(a.field_metadata(i)));
    out["field_metadata"] = fmeta;
    json rows = json::array();
    for (int64_t r = 0; r < a.num_rows(); ++r) rows.push_back(NormalizeRow(a, r));
    out["rows"] = rows;
    return out;
}

// Strip the documentation-only "_comment" key so it never affects comparison.
json StripComment(json j) {
    if (j.is_object()) j.erase("_comment");
    return j;
}

}  // namespace

TEST(AccessorCapstoneTest, AccessorCppAndRustAgreeOnSameBatch) {
    const json fixture = LoadJson("accessor_capstone_fixture.json");
    const json expected = StripComment(LoadJson("accessor_capstone_expected.json"));
    const json& rows = fixture.at("rows");

    const Columns cols = BuildColumns(rows);
    const auto schema = BuildSchema(fixture, cols);
    auto batch =
        arrow::RecordBatch::Make(schema, static_cast<int64_t>(rows.size()), cols.AsVector());

    auto res = CapstoneBatchAccessor::Make(batch);
    ASSERT_TRUE(res.ok()) << res.status().ToString();
    const auto& a = *res;

    const int kFieldCount = 8;
    const json observed = NormalizeReadout(a, kFieldCount);

    // The single, decisive assertion: the C++ readout equals the ONE shared
    // committed oracle. (Rust asserts the same against the same file.)
    EXPECT_EQ(observed, expected) << "observed:\n"
                                  << observed.dump(2) << "\nexpected:\n"
                                  << expected.dump(2);

    // Explicit null-path spot checks (per the design: not only implied by the
    // snapshot). These read the fixture's documented null placements directly.
    EXPECT_FALSE(a.maybe_child(1).has_value());            // null 1:1 struct row
    EXPECT_FALSE(a.label(1).has_value());                  // null scalar row
    EXPECT_TRUE(a.samples(0).is_null(1));                  // null scalar element
    EXPECT_FALSE(a.children(0)[1].has_value());            // null struct element
    EXPECT_TRUE(a.scores(0).value_is_null(1));             // null scalar map value
    EXPECT_FALSE(a.child_by_key(0).value(1).has_value());  // null map message value
    EXPECT_FALSE(a.nested_children(0)[1].has_value());     // null inner list

    // Explicit metadata-equivalence contract (S1): lock in the C++-nullptr ==
    // Rust-empty-map normalization DIRECTLY, not only via the snapshot. A field
    // with ABSENT metadata reads back as the empty form (C++: nullptr, which
    // MetadataToJson normalizes to {}); a field WITH metadata reads its keys back
    // verbatim. The Rust capstone makes the mirror-image assertion (empty map for
    // the same absent indices). The expected oracle encodes {} for absent indices
    // 1,2,4..7 and the keys for present indices 0,3 (field_metadata order is
    // positional, spec §5).
    //
    // Present fields (read verbatim).
    {
        const arrow::KeyValueMetadata* fm0 = a.field_metadata(0);
        ASSERT_NE(fm0, nullptr) << "field 0 carries metadata in the fixture";
        EXPECT_EQ(fm0->Get("role").ValueOrDie(), "id");
        EXPECT_EQ(MetadataToJson(fm0), expected.at("field_metadata").at(0));

        const arrow::KeyValueMetadata* fm3 = a.field_metadata(3);
        ASSERT_NE(fm3, nullptr) << "field 3 carries metadata in the fixture";
        EXPECT_EQ(fm3->Get("role").ValueOrDie(), "samples");
        EXPECT_EQ(MetadataToJson(fm3), expected.at("field_metadata").at(3));
    }
    // Absent fields: C++ reports nullptr, which normalizes to the empty map {}.
    for (int absent : {1, 2, 4, 5, 6, 7}) {
        EXPECT_EQ(a.field_metadata(absent), nullptr)
            << "field " << absent << " has no metadata -> C++ nullptr (== Rust empty map)";
        EXPECT_EQ(MetadataToJson(a.field_metadata(absent)), json::object())
            << "absent field " << absent << " normalizes to {}";
        EXPECT_EQ(MetadataToJson(a.field_metadata(absent)),
                  expected.at("field_metadata").at(absent));
    }
    // Schema-level metadata reads its keys back verbatim.
    {
        const arrow::KeyValueMetadata* sm = a.schema_metadata();
        ASSERT_NE(sm, nullptr);
        EXPECT_EQ(sm->Get("capstone").ValueOrDie(), "rba-7");
        EXPECT_EQ(sm->Get("owner").ValueOrDie(), "accessor");
    }
}
