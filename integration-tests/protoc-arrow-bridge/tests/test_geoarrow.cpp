// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// geoarrow_test.proto — VehicleTrack, LandParcel, GeoWithCrs.
// Verifies GeoArrow geometry types (Point, LineString, Polygon, etc.)
// produce structs/lists with the correct extension metadata, and that
// CRS utilities resolve EPSG codes to PROJJSON.

#include "geoarrow_test.fletcher.pb.h"

#include <arrow_bridge/codec.hpp>
#include <arrow_bridge/crs_utils.hpp>
#include <pubsub/owned_schema.hpp>

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace fletcher;

namespace {

std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

ArrowRow GeoRoundTrip(EncodedRow encoded, OwnedSchema nano_schema) {
    static EncodedRow kept_alive;
    kept_alive = std::move(encoded);
    auto schema = ImportNano(std::move(nano_schema));
    if (!schema) {
        ADD_FAILURE() << "GeoRoundTrip: ImportNano failed";
        return {};
    }
    Codec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

}  // namespace

// =============================================================================
// Schema structure
// =============================================================================

TEST(GeoArrowTest, VehicleTrackSchemaStructure) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(schema->num_fields(), 6);

    EXPECT_EQ(schema->field(0)->name(), "vehicle_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);

    EXPECT_EQ(schema->field(1)->name(), "last_position");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::STRUCT);
    EXPECT_FALSE(schema->field(1)->nullable());

    EXPECT_EQ(schema->field(2)->name(), "route");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::LIST);

    EXPECT_EQ(schema->field(3)->name(), "bounding_box");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::STRUCT);

    EXPECT_EQ(schema->field(4)->name(), "altitude_point");
    EXPECT_EQ(schema->field(4)->type()->id(), arrow::Type::STRUCT);
    EXPECT_TRUE(schema->field(4)->nullable());

    EXPECT_EQ(schema->field(5)->name(), "waypoints");
    EXPECT_EQ(schema->field(5)->type()->id(), arrow::Type::LIST);
}

// =============================================================================
// Extension metadata on schema fields
// =============================================================================

TEST(GeoArrowTest, ExtensionMetadataOnPointField) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto meta = schema->field(1)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.point");
}

TEST(GeoArrowTest, ExtensionMetadataOnLineStringField) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto meta = schema->field(2)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.linestring");
}

TEST(GeoArrowTest, ExtensionMetadataOnBoxField) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto meta = schema->field(3)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.box");
}

TEST(GeoArrowTest, ExtensionMetadataOnPointZField) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto meta = schema->field(4)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.point");
}

TEST(GeoArrowTest, ExtensionMetadataOnMultiPointField) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto meta = schema->field(5)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.multipoint");
}

// =============================================================================
// Point struct children match GeoArrow spec
// =============================================================================

TEST(GeoArrowTest, PointStructHasXYChildren) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(1)->type());
    ASSERT_EQ(point_type->num_fields(), 2);
    EXPECT_EQ(point_type->field(0)->name(), "x");
    EXPECT_EQ(point_type->field(0)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_EQ(point_type->field(1)->name(), "y");
    EXPECT_EQ(point_type->field(1)->type()->id(), arrow::Type::DOUBLE);
}

TEST(GeoArrowTest, PointZStructHasXYZChildren) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(4)->type());
    ASSERT_EQ(point_type->num_fields(), 3);
    EXPECT_EQ(point_type->field(0)->name(), "x");
    EXPECT_EQ(point_type->field(1)->name(), "y");
    EXPECT_EQ(point_type->field(2)->name(), "z");
}

TEST(GeoArrowTest, BoxStructHasXminYminXmaxYmaxChildren) {
    auto schema = ImportNano(fletcher_gen::integration::VehicleTrackSchema());
    auto box_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(3)->type());
    ASSERT_EQ(box_type->num_fields(), 4);
    EXPECT_EQ(box_type->field(0)->name(), "xmin");
    EXPECT_EQ(box_type->field(1)->name(), "ymin");
    EXPECT_EQ(box_type->field(2)->name(), "xmax");
    EXPECT_EQ(box_type->field(3)->name(), "ymax");
}

// =============================================================================
// Round-trip encode/decode (Phase 1)
// =============================================================================

TEST(GeoArrowTest, PointRoundTrip) {
    fletcher_gen::integration::VehicleTrack row;
    fletcher_gen::geoarrow::Point pt;
    pt.set_x(37.7749).set_y(-122.4194);

    row.set_vehicle_id("v-1").set_last_position(pt);

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(scalars.size(), 6u);

    auto* pos = dynamic_cast<arrow::StructScalar*>(scalars[1].get());
    ASSERT_NE(pos, nullptr);
    EXPECT_TRUE(pos->is_valid);
}

TEST(GeoArrowTest, LineStringRoundTripCollapsedToList) {
    fletcher_gen::integration::VehicleTrack row;
    fletcher_gen::geoarrow::Point p1, p2, p3;
    p1.set_x(0.0).set_y(0.0);
    p2.set_x(1.0).set_y(1.0);
    p3.set_x(2.0).set_y(2.0);

    row.set_vehicle_id("v-2").set_route({p1, p2, p3});

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(scalars.size(), 6u);

    auto* route = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(route->value->length(), 3);
}

TEST(GeoArrowTest, BoxRoundTrip) {
    fletcher_gen::integration::VehicleTrack row;
    fletcher_gen::geoarrow::Box box;
    box.set_xmin(-10.0).set_ymin(-20.0).set_xmax(10.0).set_ymax(20.0);

    row.set_vehicle_id("v-3").set_bounding_box(box);

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(scalars.size(), 6u);

    auto* bb = dynamic_cast<arrow::StructScalar*>(scalars[3].get());
    ASSERT_NE(bb, nullptr);
    EXPECT_TRUE(bb->is_valid);
}

TEST(GeoArrowTest, OptionalPointZNullWhenNotSet) {
    fletcher_gen::integration::VehicleTrack row;
    row.set_vehicle_id("v-4");

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(scalars.size(), 6u);
    EXPECT_FALSE(scalars[4]->is_valid);
}

TEST(GeoArrowTest, OptionalPointZValidWhenSet) {
    fletcher_gen::integration::VehicleTrack row;
    fletcher_gen::geoarrow::PointZ pz;
    pz.set_x(37.7749).set_y(-122.4194).set_z(100.0);

    row.set_vehicle_id("v-5").set_altitude_point(pz);

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(scalars.size(), 6u);
    EXPECT_TRUE(scalars[4]->is_valid);
}

TEST(GeoArrowTest, MultiPointRoundTrip) {
    fletcher_gen::integration::VehicleTrack row;
    fletcher_gen::geoarrow::Point w1, w2;
    w1.set_x(10.0).set_y(20.0);
    w2.set_x(30.0).set_y(40.0);

    row.set_vehicle_id("v-6").set_waypoints({w1, w2});

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::VehicleTrackSchema());
    ASSERT_EQ(scalars.size(), 6u);

    auto* wp = dynamic_cast<arrow::ListScalar*>(scalars[5].get());
    ASSERT_NE(wp, nullptr);
    EXPECT_EQ(wp->value->length(), 2);
}

// =============================================================================
// Native roundtrip (no Arrow dependency needed)
// =============================================================================

TEST(GeoArrowTest, NativeRoundtripVehicleTrack) {
    fletcher_gen::integration::VehicleTrack row;
    fletcher_gen::geoarrow::Point pt;
    pt.set_x(37.7749).set_y(-122.4194);

    fletcher_gen::geoarrow::Point p1, p2;
    p1.set_x(0.0).set_y(0.0);
    p2.set_x(1.0).set_y(1.0);

    fletcher_gen::geoarrow::Box box;
    box.set_xmin(-10.0).set_ymin(-20.0).set_xmax(10.0).set_ymax(20.0);

    fletcher_gen::geoarrow::PointZ pz;
    pz.set_x(37.7749).set_y(-122.4194).set_z(100.0);

    row.set_vehicle_id("v-test")
       .set_last_position(pt)
       .set_route({p1, p2})
       .set_bounding_box(box)
       .set_altitude_point(pz)
       .set_waypoints({p1, p2});

    fletcher_gen::integration::VehicleTrack decoded(row.Encode());
    EXPECT_EQ(decoded.vehicle_id(), "v-test");
    EXPECT_DOUBLE_EQ(decoded.last_position().x(), 37.7749);
    EXPECT_DOUBLE_EQ(decoded.last_position().y(), -122.4194);
    ASSERT_EQ(decoded.route().size(), 2u);
    EXPECT_DOUBLE_EQ(decoded.route()[0].x(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.bounding_box().xmin(), -10.0);
    EXPECT_DOUBLE_EQ(decoded.bounding_box().ymax(), 20.0);
    ASSERT_NE(decoded.altitude_point(), nullptr);
    EXPECT_DOUBLE_EQ(decoded.altitude_point()->z(), 100.0);
    ASSERT_EQ(decoded.waypoints().size(), 2u);
}

// =============================================================================
// CRS utilities
// =============================================================================

TEST(GeoArrowCrsTest, EpsgToProjJsonReturnsProjectJsonFor4326) {
    auto pj = EpsgToProjJson(4326);
    ASSERT_FALSE(pj.empty());
    EXPECT_NE(pj.find("\"EPSG\""), std::string::npos);
    EXPECT_NE(pj.find("4326"), std::string::npos);
    EXPECT_NE(pj.find("WGS 84"), std::string::npos);
}

TEST(GeoArrowCrsTest, EpsgToProjJsonReturnsProjectJsonFor3857) {
    auto pj = EpsgToProjJson(3857);
    ASSERT_FALSE(pj.empty());
    EXPECT_NE(pj.find("3857"), std::string::npos);
    EXPECT_NE(pj.find("Pseudo-Mercator"), std::string::npos);
}

TEST(GeoArrowCrsTest, EpsgToProjJsonReturnsEmptyForUnknownCode) {
    EXPECT_TRUE(EpsgToProjJson(99999).empty());
}

TEST(GeoArrowCrsTest, ResolveCrsPassesThroughRawProjectJson) {
    std::string projjson = R"({"type":"GeographicCRS"})";
    EXPECT_EQ(ResolveCrs(projjson), projjson);
}

TEST(GeoArrowCrsTest, ResolveCrsResolvesEpsgCodes) {
    auto resolved = ResolveCrs("EPSG:4326");
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("WGS 84"), std::string::npos);
}

TEST(GeoArrowCrsTest, ResolveCrsReturnsEmptyForUnknownFormat) {
    EXPECT_TRUE(ResolveCrs("something").empty());
}

TEST(GeoArrowCrsTest, BuildExtensionMetadataEmptyCrs) {
    EXPECT_EQ(BuildExtensionMetadata(""), "{}");
}

TEST(GeoArrowCrsTest, BuildExtensionMetadataWithProjectJson) {
    std::string pj = R"({"type":"GeographicCRS"})";
    auto meta = BuildExtensionMetadata(pj);
    EXPECT_EQ(meta, R"({"crs":{"type":"GeographicCRS"}})");
}

// =============================================================================
// Phase 2: Polygon, MultiLineString, MultiPolygon
// =============================================================================

TEST(GeoArrowPhase2Test, LandParcelSchemaStructure) {
    auto schema = ImportNano(fletcher_gen::integration::LandParcelSchema());
    ASSERT_EQ(schema->num_fields(), 5);

    EXPECT_EQ(schema->field(0)->name(), "parcel_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);

    EXPECT_EQ(schema->field(1)->name(), "boundary");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::LIST);
    auto inner1 = std::static_pointer_cast<arrow::ListType>(schema->field(1)->type());
    EXPECT_EQ(inner1->value_type()->id(), arrow::Type::LIST);
    auto inner2 = std::static_pointer_cast<arrow::ListType>(inner1->value_type());
    EXPECT_EQ(inner2->value_type()->id(), arrow::Type::STRUCT);

    EXPECT_EQ(schema->field(2)->name(), "access_roads");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::LIST);

    EXPECT_EQ(schema->field(3)->name(), "zones");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::LIST);
    auto z1 = std::static_pointer_cast<arrow::ListType>(schema->field(3)->type());
    EXPECT_EQ(z1->value_type()->id(), arrow::Type::LIST);
    auto z2 = std::static_pointer_cast<arrow::ListType>(z1->value_type());
    EXPECT_EQ(z2->value_type()->id(), arrow::Type::LIST);
    auto z3 = std::static_pointer_cast<arrow::ListType>(z2->value_type());
    EXPECT_EQ(z3->value_type()->id(), arrow::Type::STRUCT);

    EXPECT_EQ(schema->field(4)->name(), "boundary_3d");
    EXPECT_TRUE(schema->field(4)->nullable());
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnPolygonField) {
    auto schema = ImportNano(fletcher_gen::integration::LandParcelSchema());
    auto meta = schema->field(1)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.polygon");
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnMultiLineStringField) {
    auto schema = ImportNano(fletcher_gen::integration::LandParcelSchema());
    auto meta = schema->field(2)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.multilinestring");
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnMultiPolygonField) {
    auto schema = ImportNano(fletcher_gen::integration::LandParcelSchema());
    auto meta = schema->field(3)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.multipolygon");
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnPolygonZField) {
    auto schema = ImportNano(fletcher_gen::integration::LandParcelSchema());
    auto meta = schema->field(4)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.polygon");
}

TEST(GeoArrowPhase2Test, PolygonRoundTrip) {
    fletcher_gen::integration::LandParcel row;

    fletcher_gen::geoarrow::Point p1, p2, p3, p4;
    p1.set_x(0).set_y(0);
    p2.set_x(1).set_y(0);
    p3.set_x(0).set_y(1);
    p4.set_x(0).set_y(0);

    row.set_parcel_id("p-1").set_boundary({{p1, p2, p3, p4}});

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::LandParcelSchema());
    ASSERT_EQ(scalars.size(), 5u);

    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 1);

    fletcher_gen::integration::LandParcel decoded(row.Encode());
    ASSERT_EQ(decoded.boundary().size(), 1u);
    ASSERT_EQ(decoded.boundary()[0].size(), 4u);
    EXPECT_DOUBLE_EQ(decoded.boundary()[0][0].x(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.boundary()[0][0].y(), 0.0);
}

TEST(GeoArrowPhase2Test, MultiLineStringRoundTrip) {
    fletcher_gen::integration::LandParcel row;

    fletcher_gen::geoarrow::Point a1, a2, b1, b2, b3;
    a1.set_x(0).set_y(0); a2.set_x(1).set_y(1);
    b1.set_x(2).set_y(2); b2.set_x(3).set_y(3); b3.set_x(4).set_y(4);

    row.set_parcel_id("p-2").set_access_roads({{a1, a2}, {b1, b2, b3}});

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::LandParcelSchema());
    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 2);

    fletcher_gen::integration::LandParcel decoded(row.Encode());
    ASSERT_EQ(decoded.access_roads().size(), 2u);
    EXPECT_EQ(decoded.access_roads()[0].size(), 2u);
    EXPECT_EQ(decoded.access_roads()[1].size(), 3u);
}

TEST(GeoArrowPhase2Test, MultiPolygonRoundTrip) {
    fletcher_gen::integration::LandParcel row;

    fletcher_gen::geoarrow::Point p1, p2, p3, p4;
    p1.set_x(0).set_y(0); p2.set_x(1).set_y(0);
    p3.set_x(0).set_y(1); p4.set_x(0).set_y(0);

    fletcher_gen::geoarrow::Point q1, q2, q3, q4;
    q1.set_x(10).set_y(10); q2.set_x(11).set_y(10);
    q3.set_x(10).set_y(11); q4.set_x(10).set_y(10);

    row.set_parcel_id("p-3").set_zones({
        {{p1, p2, p3, p4}},
        {{q1, q2, q3, q4}}
    });

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::LandParcelSchema());
    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[3].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 2);

    fletcher_gen::integration::LandParcel decoded(row.Encode());
    ASSERT_EQ(decoded.zones().size(), 2u);
    ASSERT_EQ(decoded.zones()[0].size(), 1u);
    ASSERT_EQ(decoded.zones()[0][0].size(), 4u);
}

TEST(GeoArrowPhase2Test, OptionalPolygonZNullWhenNotSet) {
    fletcher_gen::integration::LandParcel row;
    row.set_parcel_id("p-4");
    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::LandParcelSchema());
    ASSERT_EQ(scalars.size(), 5u);
    EXPECT_FALSE(scalars[4]->is_valid);
}

TEST(GeoArrowPhase2Test, OptionalPolygonZValidWhenSet) {
    fletcher_gen::integration::LandParcel row;

    fletcher_gen::geoarrow::PointZ p1, p2, p3, p4;
    p1.set_x(0).set_y(0).set_z(10);
    p2.set_x(1).set_y(0).set_z(20);
    p3.set_x(0).set_y(1).set_z(30);
    p4.set_x(0).set_y(0).set_z(10);

    row.set_parcel_id("p-5").set_boundary_3d({{p1, p2, p3, p4}});

    auto scalars = GeoRoundTrip(row.Encode(),
        fletcher_gen::integration::LandParcelSchema());
    EXPECT_TRUE(scalars[4]->is_valid);
}
