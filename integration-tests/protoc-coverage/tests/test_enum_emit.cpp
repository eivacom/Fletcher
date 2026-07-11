// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-9 forcing test: EnumEmit.GeneratedEnumSymbolsRoundTrip (#75).
//
// Proves the GIR-backed C++ generator emits, for every proto enum:
//   * a scoped `enum class <Name> : int32_t { ... }` at the correct scope
//     (package-namespace for top-level enums, nested inside the owning generated
//     message class for nested enums), including a STANDALONE unreferenced enum;
//   * TYPED field accessors (getters + fluent setters) ADDED alongside the raw
//     int32 accessors, on both the edge row class AND the Arrow view class;
//   * an enum-owner ordering edge in TopologicalVisit so a message that
//     references a nested enum whose OWNER is declared later (EnumOwner, with no
//     TYPE_MESSAGE edge) still compiles;
// while the wire stays byte-identical between the raw-int32 and typed setter
// paths (storage remains int32; no wire change).
//
// Existing coverage.proto enum fields (ScalarCoverage.status / .nested_status,
// Leaf.status, NestedEnums.state) are exercised too: they gain typed accessors
// additively, reusing the existing TopLevelStatus / NestedEnums::InnerStatus
// declarations (no re-declaration).

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coverage.fletcher.pb.h"
#include "enum_coverage.fletcher.arrow.pb.h"
#include "enum_coverage.fletcher.pb.h"

namespace gen = fletcher_gen::integration::coverage;

TEST(EnumEmit, GeneratedEnumSymbolsRoundTrip) {
    // ---- enum classes emitted at the right scope --------------------------
    // Top-level (package-namespace) enum, imported from coverage.proto.
    static_assert(static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK) == 1);
    // Nested enum inside its owning generated message class.
    static_assert(static_cast<int32_t>(gen::EnumOwner::InnerStatus::INNER_STATUS_ACTIVE) == 1);
    // STANDALONE, intentionally-unreferenced enum still emitted.
    static_assert(static_cast<int32_t>(gen::StandaloneStatus::STANDALONE_STATUS_PRESENT) == 1);

    // ---- mutable row: typed getters + fluent typed setters ----------------
    gen::EnumCoverage msg;

    msg.set_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(msg.status(), static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK));
    EXPECT_EQ(msg.status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);

    msg.set_nullable_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);
    ASSERT_TRUE(msg.nullable_status_typed().has_value());
    EXPECT_EQ(*msg.nullable_status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    msg.set_nested_status(gen::EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);
    EXPECT_EQ(msg.nested_status_typed(), gen::EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);

    // Repeated + map raw setters take int32 containers (no typed repeated/map
    // setters this round); typed GETTERS cast the value side.
    msg.set_statuses(std::vector<int32_t>{
        static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK),
        static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN),
    });
    const auto typed_statuses = msg.statuses_typed();
    ASSERT_EQ(typed_statuses.size(), 2u);
    EXPECT_EQ(typed_statuses[0], gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(typed_statuses[1], gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    msg.set_status_by_name(std::vector<std::pair<std::string, int32_t>>{
        {"ok", static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK)},
        {"warn", static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN)},
    });
    const auto typed_map = msg.status_by_name_typed();
    ASSERT_EQ(typed_map.size(), 2u);
    EXPECT_EQ(typed_map[0].second, gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(typed_map[1].second, gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    // ---- immutable Arrow view: typed getters (edge AND view) --------------
    const gen::EnumCoverageView view(ToArrowRow(msg));

    EXPECT_EQ(view.status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);

    ASSERT_TRUE(view.nullable_status_typed().has_value());
    EXPECT_EQ(*view.nullable_status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    EXPECT_EQ(view.nested_status_typed(), gen::EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);

    const auto view_statuses = view.statuses_typed();
    ASSERT_EQ(view_statuses.size(), 2u);
    EXPECT_EQ(view_statuses[0], gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(view_statuses[1], gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    const auto view_typed_map = view.status_by_name_typed();
    ASSERT_EQ(view_typed_map.size(), 2u);
    EXPECT_EQ(view_typed_map[0].second, gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(view_typed_map[1].second, gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    // ---- wire byte identity: raw-int32 path == typed path -----------------
    gen::EnumCoverage raw_msg;
    raw_msg.set_status(1);
    raw_msg.set_nullable_status(2);
    raw_msg.set_nested_status(1);
    raw_msg.set_statuses(std::vector<int32_t>{1, 2});
    raw_msg.set_status_by_name(std::vector<std::pair<std::string, int32_t>>{{"ok", 1}, {"warn", 2}});

    gen::EnumCoverage typed_msg;
    typed_msg.set_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    typed_msg.set_nullable_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);
    typed_msg.set_nested_status(gen::EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);
    typed_msg.set_statuses(std::vector<int32_t>{
        static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK),
        static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN),
    });
    typed_msg.set_status_by_name(std::vector<std::pair<std::string, int32_t>>{
        {"ok", static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK)},
        {"warn", static_cast<int32_t>(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN)},
    });

    EXPECT_EQ(raw_msg.Encode(), typed_msg.Encode());

    // ---- existing coverage.proto fields gain typed accessors additively ---
    gen::ScalarCoverage scalars;
    scalars.set_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(scalars.status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    scalars.set_nested_status(gen::NestedEnums::InnerStatus::INNER_STATUS_ACTIVE);
    EXPECT_EQ(scalars.nested_status_typed(), gen::NestedEnums::InnerStatus::INNER_STATUS_ACTIVE);

    gen::Leaf leaf;
    leaf.set_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);
    EXPECT_EQ(leaf.status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_WARN);

    gen::NestedEnums nested;
    nested.set_state(gen::NestedEnums::InnerStatus::INNER_STATUS_DISABLED);
    EXPECT_EQ(nested.state_typed(), gen::NestedEnums::InnerStatus::INNER_STATUS_DISABLED);

    // ---- 4a: nested enum whose OWNER is NOT emittable -> raw int32 only ----
    // FlattenedEnumOwner (flatten wrapper) and RecursiveEnumOwner (recursive)
    // are never generated, so their nested enum classes have no C++ home. The
    // referencing messages must expose ONLY the raw int32 accessor; the mere
    // fact that these generated headers COMPILE is the gate assertion (without
    // the emittability gate they would name undeclared enum classes). No
    // `wrapped_typed()` / `rec_typed()` is emitted.
    gen::FlattenedEnumRef flat_ref;
    flat_ref.set_wrapped(1);
    EXPECT_EQ(flat_ref.wrapped(), 1);

    gen::RecursiveEnumRef rec_ref;
    rec_ref.set_rec(1);
    EXPECT_EQ(rec_ref.rec(), 1);

    // ---- P2-1: field-flattened IMPORTED enum field ------------------------
    // FieldFlattenedEnum inlines ImportedEnumCarrier's fields, including the
    // imported enum TopLevelStatus; the inlined field still gets a typed
    // accessor and its defining-file header must be visible.
    gen::FieldFlattenedEnum ff;
    ff.set_carrier_status(gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    ff.set_carrier_amount(7);
    EXPECT_EQ(ff.carrier_status_typed(), gen::TopLevelStatus::TOP_LEVEL_STATUS_OK);
    EXPECT_EQ(ff.carrier_amount(), 7);
}
