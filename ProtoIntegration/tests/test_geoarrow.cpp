#include <gtest/gtest.h>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <positional_codec.hpp>
#include <crs_utils.hpp>
#include <pubsub/owned_schema.hpp>

#include "geoarrow_test.fletcher.pb.h"

// Helper: import an OwnedSchema (nanoarrow) to shared_ptr<arrow::Schema>.
static std::shared_ptr<arrow::Schema> ImportNano(fletcher::OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) { ADD_FAILURE() << "ImportSchema failed"; return nullptr; }
    return *result;
}

// Helper: decode an encoded row using the given schema.
// The encoded data is kept alive in a static to prevent dangling Buffer
// pointers — DecodeScalarFromReader creates non-owning Buffers into the
// input data.
static fletcher::ArrowRow GeoRoundTrip(
    fletcher::EncodedRow encoded,
    fletcher::OwnedSchema nano_schema) {
    static fletcher::EncodedRow kept_alive;
    kept_alive = std::move(encoded);
    auto schema = ImportNano(std::move(nano_schema));
    if (!schema) { ADD_FAILURE() << "GeoRoundTrip: ImportNano failed"; return {}; }
    fletcher::PositionalCodec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

// =============================================================================
// Schema structure
// =============================================================================

TEST(GeoArrowTest, VehicleTrackSchemaStructure) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(schema->num_fields(), 6);

    // vehicle_id — plain string
    EXPECT_EQ(schema->field(0)->name(), "vehicle_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);

    // last_position — struct with extension metadata
    EXPECT_EQ(schema->field(1)->name(), "last_position");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::STRUCT);
    EXPECT_FALSE(schema->field(1)->nullable());

    // route — list (collapsed from LineString)
    EXPECT_EQ(schema->field(2)->name(), "route");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::LIST);

    // bounding_box — struct with extension metadata
    EXPECT_EQ(schema->field(3)->name(), "bounding_box");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::STRUCT);

    // altitude_point — nullable struct (optional PointZ)
    EXPECT_EQ(schema->field(4)->name(), "altitude_point");
    EXPECT_EQ(schema->field(4)->type()->id(), arrow::Type::STRUCT);
    EXPECT_TRUE(schema->field(4)->nullable());

    // waypoints — list (collapsed from MultiPoint)
    EXPECT_EQ(schema->field(5)->name(), "waypoints");
    EXPECT_EQ(schema->field(5)->type()->id(), arrow::Type::LIST);
}

// =============================================================================
// Extension metadata on schema fields
// =============================================================================

TEST(GeoArrowTest, ExtensionMetadataOnPointField) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(1)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.point");
}

TEST(GeoArrowTest, ExtensionMetadataOnLineStringField) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(2)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.linestring");
}

TEST(GeoArrowTest, ExtensionMetadataOnBoxField) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(3)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.box");
}

TEST(GeoArrowTest, ExtensionMetadataOnPointZField) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(4)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.point");
}

TEST(GeoArrowTest, ExtensionMetadataOnMultiPointField) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
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
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(1)->type());
    ASSERT_EQ(point_type->num_fields(), 2);
    EXPECT_EQ(point_type->field(0)->name(), "x");
    EXPECT_EQ(point_type->field(0)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_EQ(point_type->field(1)->name(), "y");
    EXPECT_EQ(point_type->field(1)->type()->id(), arrow::Type::DOUBLE);
}

TEST(GeoArrowTest, PointZStructHasXYZChildren) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(4)->type());
    ASSERT_EQ(point_type->num_fields(), 3);
    EXPECT_EQ(point_type->field(0)->name(), "x");
    EXPECT_EQ(point_type->field(1)->name(), "y");
    EXPECT_EQ(point_type->field(2)->name(), "z");
}

TEST(GeoArrowTest, BoxStructHasXminYminXmaxYmaxChildren) {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto box_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(3)->type());
    ASSERT_EQ(box_type->num_fields(), 4);
    EXPECT_EQ(box_type->field(0)->name(), "xmin");
    EXPECT_EQ(box_type->field(1)->name(), "ymin");
    EXPECT_EQ(box_type->field(2)->name(), "xmax");
    EXPECT_EQ(box_type->field(3)->name(), "ymax");
}

// =============================================================================
// Round-trip encode/decode
// =============================================================================

TEST(GeoArrowTest, PointRoundTrip) {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow pt;
    pt.set_x(37.7749).set_y(-122.4194);

    row.set_vehicle_id("v-1").set_last_position(pt);

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(scalars.size(), 6);

    // last_position is a struct scalar
    auto* pos = dynamic_cast<arrow::StructScalar*>(scalars[1].get());
    ASSERT_NE(pos, nullptr);
    EXPECT_TRUE(pos->is_valid);
}

TEST(GeoArrowTest, LineStringRoundTripCollapsedToList) {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow p1, p2, p3;
    p1.set_x(0.0).set_y(0.0);
    p2.set_x(1.0).set_y(1.0);
    p3.set_x(2.0).set_y(2.0);

    row.set_vehicle_id("v-2").set_route({p1, p2, p3});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(scalars.size(), 6);

    // route is a list of structs
    auto* route = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(route->value->length(), 3);
}

TEST(GeoArrowTest, BoxRoundTrip) {
    integration::VehicleTrackArrowRow row;
    geoarrow::BoxArrowRow box;
    box.set_xmin(-10.0).set_ymin(-20.0).set_xmax(10.0).set_ymax(20.0);

    row.set_vehicle_id("v-3").set_bounding_box(box);

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(scalars.size(), 6);

    auto* bb = dynamic_cast<arrow::StructScalar*>(scalars[3].get());
    ASSERT_NE(bb, nullptr);
    EXPECT_TRUE(bb->is_valid);
}

TEST(GeoArrowTest, OptionalPointZNullWhenNotSet) {
    integration::VehicleTrackArrowRow row;
    row.set_vehicle_id("v-4");

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(scalars.size(), 6);
    EXPECT_FALSE(scalars[4]->is_valid);  // altitude_point not set
}

TEST(GeoArrowTest, OptionalPointZValidWhenSet) {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointZArrowRow pz;
    pz.set_x(37.7749).set_y(-122.4194).set_z(100.0);

    row.set_vehicle_id("v-5").set_altitude_point(pz);

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(scalars.size(), 6);
    EXPECT_TRUE(scalars[4]->is_valid);
}

TEST(GeoArrowTest, MultiPointRoundTrip) {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow w1, w2;
    w1.set_x(10.0).set_y(20.0);
    w2.set_x(30.0).set_y(40.0);

    row.set_vehicle_id("v-6").set_waypoints({w1, w2});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    ASSERT_EQ(scalars.size(), 6);

    auto* wp = dynamic_cast<arrow::ListScalar*>(scalars[5].get());
    ASSERT_NE(wp, nullptr);
    EXPECT_EQ(wp->value->length(), 2);
}

// =============================================================================
// Native roundtrip (no Arrow dependency needed)
// =============================================================================

TEST(GeoArrowTest, NativeRoundtripVehicleTrack) {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow pt;
    pt.set_x(37.7749).set_y(-122.4194);

    geoarrow::PointArrowRow p1, p2;
    p1.set_x(0.0).set_y(0.0);
    p2.set_x(1.0).set_y(1.0);

    geoarrow::BoxArrowRow box;
    box.set_xmin(-10.0).set_ymin(-20.0).set_xmax(10.0).set_ymax(20.0);

    geoarrow::PointZArrowRow pz;
    pz.set_x(37.7749).set_y(-122.4194).set_z(100.0);

    row.set_vehicle_id("v-test")
       .set_last_position(pt)
       .set_route({p1, p2})
       .set_bounding_box(box)
       .set_altitude_point(pz)
       .set_waypoints({p1, p2});

    integration::VehicleTrackArrowRow decoded(row.Encode());
    EXPECT_EQ(decoded.vehicle_id(), "v-test");
    EXPECT_DOUBLE_EQ(decoded.last_position().x(), 37.7749);
    EXPECT_DOUBLE_EQ(decoded.last_position().y(), -122.4194);
    ASSERT_EQ(decoded.route().size(), 2);
    EXPECT_DOUBLE_EQ(decoded.route()[0].x(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.bounding_box().xmin(), -10.0);
    EXPECT_DOUBLE_EQ(decoded.bounding_box().ymax(), 20.0);
    ASSERT_NE(decoded.altitude_point(), nullptr);
    EXPECT_DOUBLE_EQ(decoded.altitude_point()->z(), 100.0);
    ASSERT_EQ(decoded.waypoints().size(), 2);
}

// =============================================================================
// CRS utilities
// =============================================================================

TEST(GeoArrowCrsTest, EpsgToProjJsonReturnsProjectJsonFor4326) {
    auto pj = fletcher::EpsgToProjJson(4326);
    ASSERT_FALSE(pj.empty());
    EXPECT_NE(pj.find("\"EPSG\""), std::string::npos);
    EXPECT_NE(pj.find("4326"), std::string::npos);
    EXPECT_NE(pj.find("WGS 84"), std::string::npos);
}

TEST(GeoArrowCrsTest, EpsgToProjJsonReturnsProjectJsonFor3857) {
    auto pj = fletcher::EpsgToProjJson(3857);
    ASSERT_FALSE(pj.empty());
    EXPECT_NE(pj.find("3857"), std::string::npos);
    EXPECT_NE(pj.find("Pseudo-Mercator"), std::string::npos);
}

TEST(GeoArrowCrsTest, EpsgToProjJsonReturnsEmptyForUnknownCode) {
    EXPECT_TRUE(fletcher::EpsgToProjJson(99999).empty());
}

TEST(GeoArrowCrsTest, ResolveCrsPassesThroughRawProjectJson) {
    std::string projjson = R"({"type":"GeographicCRS"})";
    EXPECT_EQ(fletcher::ResolveCrs(projjson), projjson);
}

TEST(GeoArrowCrsTest, ResolveCrsResolvesEpsgCodes) {
    auto resolved = fletcher::ResolveCrs("EPSG:4326");
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("WGS 84"), std::string::npos);
}

TEST(GeoArrowCrsTest, ResolveCrsReturnsEmptyForUnknownFormat) {
    EXPECT_TRUE(fletcher::ResolveCrs("something").empty());
}

TEST(GeoArrowCrsTest, BuildExtensionMetadataEmptyCrs) {
    EXPECT_EQ(fletcher::BuildExtensionMetadata(""), "{}");
}

TEST(GeoArrowCrsTest, BuildExtensionMetadataWithProjectJson) {
    std::string pj = R"({"type":"GeographicCRS"})";
    auto meta = fletcher::BuildExtensionMetadata(pj);
    EXPECT_EQ(meta, R"({"crs":{"type":"GeographicCRS"}})");
}

// =============================================================================
// Phase 2: Polygon, MultiLineString, MultiPolygon
// =============================================================================

// -- Schema structure ---------------------------------------------------------

TEST(GeoArrowPhase2Test, LandParcelSchemaStructure) {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    ASSERT_EQ(schema->num_fields(), 5);

    EXPECT_EQ(schema->field(0)->name(), "parcel_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);

    // boundary — Polygon → List<List<Struct>>
    EXPECT_EQ(schema->field(1)->name(), "boundary");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::LIST);
    auto inner1 = std::static_pointer_cast<arrow::ListType>(schema->field(1)->type());
    EXPECT_EQ(inner1->value_type()->id(), arrow::Type::LIST);
    auto inner2 = std::static_pointer_cast<arrow::ListType>(inner1->value_type());
    EXPECT_EQ(inner2->value_type()->id(), arrow::Type::STRUCT);

    // access_roads — MultiLineString → List<List<Struct>>
    EXPECT_EQ(schema->field(2)->name(), "access_roads");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::LIST);

    // zones — MultiPolygon → List<List<List<Struct>>>
    EXPECT_EQ(schema->field(3)->name(), "zones");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::LIST);
    auto z1 = std::static_pointer_cast<arrow::ListType>(schema->field(3)->type());
    EXPECT_EQ(z1->value_type()->id(), arrow::Type::LIST);
    auto z2 = std::static_pointer_cast<arrow::ListType>(z1->value_type());
    EXPECT_EQ(z2->value_type()->id(), arrow::Type::LIST);
    auto z3 = std::static_pointer_cast<arrow::ListType>(z2->value_type());
    EXPECT_EQ(z3->value_type()->id(), arrow::Type::STRUCT);

    // boundary_3d — optional PolygonZ → nullable
    EXPECT_EQ(schema->field(4)->name(), "boundary_3d");
    EXPECT_TRUE(schema->field(4)->nullable());
}

// -- Extension metadata -------------------------------------------------------

TEST(GeoArrowPhase2Test, ExtensionMetadataOnPolygonField) {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(1)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.polygon");
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnMultiLineStringField) {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(2)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.multilinestring");
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnMultiPolygonField) {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(3)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.multipolygon");
}

TEST(GeoArrowPhase2Test, ExtensionMetadataOnPolygonZField) {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(4)->metadata();
    ASSERT_NE(meta, nullptr);
    auto result = meta->Get("ARROW:extension:name");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, "geoarrow.polygon");
}

// -- Round-trip encode/decode -------------------------------------------------

TEST(GeoArrowPhase2Test, PolygonRoundTrip) {
    integration::LandParcelArrowRow row;

    geoarrow::PointArrowRow p1, p2, p3, p4;
    p1.set_x(0).set_y(0);
    p2.set_x(1).set_y(0);
    p3.set_x(0).set_y(1);
    p4.set_x(0).set_y(0);

    row.set_parcel_id("p-1").set_boundary({{p1, p2, p3, p4}});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    ASSERT_EQ(scalars.size(), 5);

    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 1);  // one ring

    // Decode back to a LandParcelArrowRow and verify values.
    integration::LandParcelArrowRow decoded(row.Encode());
    ASSERT_EQ(decoded.boundary().size(), 1);
    ASSERT_EQ(decoded.boundary()[0].size(), 4);
    EXPECT_DOUBLE_EQ(decoded.boundary()[0][0].x(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.boundary()[0][0].y(), 0.0);
}

TEST(GeoArrowPhase2Test, MultiLineStringRoundTrip) {
    integration::LandParcelArrowRow row;

    geoarrow::PointArrowRow a1, a2, b1, b2, b3;
    a1.set_x(0).set_y(0); a2.set_x(1).set_y(1);
    b1.set_x(2).set_y(2); b2.set_x(3).set_y(3); b3.set_x(4).set_y(4);

    row.set_parcel_id("p-2").set_access_roads({{a1, a2}, {b1, b2, b3}});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 2);

    integration::LandParcelArrowRow decoded(row.Encode());
    ASSERT_EQ(decoded.access_roads().size(), 2);
    EXPECT_EQ(decoded.access_roads()[0].size(), 2);
    EXPECT_EQ(decoded.access_roads()[1].size(), 3);
}

TEST(GeoArrowPhase2Test, MultiPolygonRoundTrip) {
    integration::LandParcelArrowRow row;

    geoarrow::PointArrowRow p1, p2, p3, p4;
    p1.set_x(0).set_y(0); p2.set_x(1).set_y(0);
    p3.set_x(0).set_y(1); p4.set_x(0).set_y(0);

    geoarrow::PointArrowRow q1, q2, q3, q4;
    q1.set_x(10).set_y(10); q2.set_x(11).set_y(10);
    q3.set_x(10).set_y(11); q4.set_x(10).set_y(10);

    row.set_parcel_id("p-3").set_zones({
        {{p1, p2, p3, p4}},
        {{q1, q2, q3, q4}}
    });

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[3].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 2);

    integration::LandParcelArrowRow decoded(row.Encode());
    ASSERT_EQ(decoded.zones().size(), 2);
    ASSERT_EQ(decoded.zones()[0].size(), 1);
    ASSERT_EQ(decoded.zones()[0][0].size(), 4);
}

TEST(GeoArrowPhase2Test, OptionalPolygonZNullWhenNotSet) {
    integration::LandParcelArrowRow row;
    row.set_parcel_id("p-4");
    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    ASSERT_EQ(scalars.size(), 5);
    EXPECT_FALSE(scalars[4]->is_valid);
}

TEST(GeoArrowPhase2Test, OptionalPolygonZValidWhenSet) {
    integration::LandParcelArrowRow row;

    geoarrow::PointZArrowRow p1, p2, p3, p4;
    p1.set_x(0).set_y(0).set_z(10);
    p2.set_x(1).set_y(0).set_z(20);
    p3.set_x(0).set_y(1).set_z(30);
    p4.set_x(0).set_y(0).set_z(10);

    row.set_parcel_id("p-5").set_boundary_3d({{p1, p2, p3, p4}});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    EXPECT_TRUE(scalars[4]->is_valid);
}

// =============================================================================
// CRS compile-time and runtime tests are temporarily disabled.
// The nanoarrow schema generator does not yet support CRS parameters.
// These tests will be re-enabled once CRS resolution is ported to nanoarrow.
// =============================================================================
