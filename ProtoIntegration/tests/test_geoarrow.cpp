#include <catch2/catch_test_macros.hpp>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <positional_codec.hpp>
#include <crs_utils.hpp>
#include <pubsub/owned_schema.hpp>

#include "geoarrow_test.fletcher.pb.h"

// Helper: import an OwnedSchema (nanoarrow) to shared_ptr<arrow::Schema>.
static std::shared_ptr<arrow::Schema> ImportNano(fletcher::OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    REQUIRE(result.ok());
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
    fletcher::PositionalCodec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

// =============================================================================
// Schema structure
// =============================================================================

TEST_CASE("GeoArrow: VehicleTrack schema structure") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    REQUIRE(schema->num_fields() == 6);

    // vehicle_id — plain string
    CHECK(schema->field(0)->name() == "vehicle_id");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRING);

    // last_position — struct with extension metadata
    CHECK(schema->field(1)->name() == "last_position");
    CHECK(schema->field(1)->type()->id() == arrow::Type::STRUCT);
    CHECK_FALSE(schema->field(1)->nullable());

    // route — list (collapsed from LineString)
    CHECK(schema->field(2)->name() == "route");
    CHECK(schema->field(2)->type()->id() == arrow::Type::LIST);

    // bounding_box — struct with extension metadata
    CHECK(schema->field(3)->name() == "bounding_box");
    CHECK(schema->field(3)->type()->id() == arrow::Type::STRUCT);

    // altitude_point — nullable struct (optional PointZ)
    CHECK(schema->field(4)->name() == "altitude_point");
    CHECK(schema->field(4)->type()->id() == arrow::Type::STRUCT);
    CHECK(schema->field(4)->nullable());

    // waypoints — list (collapsed from MultiPoint)
    CHECK(schema->field(5)->name() == "waypoints");
    CHECK(schema->field(5)->type()->id() == arrow::Type::LIST);
}

// =============================================================================
// Extension metadata on schema fields
// =============================================================================

TEST_CASE("GeoArrow: extension metadata on Point field") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(1)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.point");
}

TEST_CASE("GeoArrow: extension metadata on LineString field") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(2)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.linestring");
}

TEST_CASE("GeoArrow: extension metadata on Box field") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(3)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.box");
}

TEST_CASE("GeoArrow: extension metadata on PointZ field") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(4)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.point");
}

TEST_CASE("GeoArrow: extension metadata on MultiPoint field") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto meta = schema->field(5)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.multipoint");
}

// =============================================================================
// Point struct children match GeoArrow spec
// =============================================================================

TEST_CASE("GeoArrow: Point struct has x, y children") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(1)->type());
    REQUIRE(point_type->num_fields() == 2);
    CHECK(point_type->field(0)->name() == "x");
    CHECK(point_type->field(0)->type()->id() == arrow::Type::DOUBLE);
    CHECK(point_type->field(1)->name() == "y");
    CHECK(point_type->field(1)->type()->id() == arrow::Type::DOUBLE);
}

TEST_CASE("GeoArrow: PointZ struct has x, y, z children") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(4)->type());
    REQUIRE(point_type->num_fields() == 3);
    CHECK(point_type->field(0)->name() == "x");
    CHECK(point_type->field(1)->name() == "y");
    CHECK(point_type->field(2)->name() == "z");
}

TEST_CASE("GeoArrow: Box struct has xmin, ymin, xmax, ymax children") {
    auto schema = ImportNano(integration::VehicleTrackArrowRowSchema());
    auto box_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(3)->type());
    REQUIRE(box_type->num_fields() == 4);
    CHECK(box_type->field(0)->name() == "xmin");
    CHECK(box_type->field(1)->name() == "ymin");
    CHECK(box_type->field(2)->name() == "xmax");
    CHECK(box_type->field(3)->name() == "ymax");
}

// =============================================================================
// Round-trip encode/decode
// =============================================================================

TEST_CASE("GeoArrow: Point round-trip") {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow pt;
    pt.set_x(37.7749).set_y(-122.4194);

    row.set_vehicle_id("v-1").set_last_position(pt);

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    REQUIRE(scalars.size() == 6);

    // last_position is a struct scalar
    auto* pos = dynamic_cast<arrow::StructScalar*>(scalars[1].get());
    REQUIRE(pos != nullptr);
    CHECK(pos->is_valid);
}

TEST_CASE("GeoArrow: LineString round-trip (collapsed to list)") {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow p1, p2, p3;
    p1.set_x(0.0).set_y(0.0);
    p2.set_x(1.0).set_y(1.0);
    p3.set_x(2.0).set_y(2.0);

    row.set_vehicle_id("v-2").set_route({p1, p2, p3});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    REQUIRE(scalars.size() == 6);

    // route is a list of structs
    auto* route = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    REQUIRE(route != nullptr);
    CHECK(route->value->length() == 3);
}

TEST_CASE("GeoArrow: Box round-trip") {
    integration::VehicleTrackArrowRow row;
    geoarrow::BoxArrowRow box;
    box.set_xmin(-10.0).set_ymin(-20.0).set_xmax(10.0).set_ymax(20.0);

    row.set_vehicle_id("v-3").set_bounding_box(box);

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    REQUIRE(scalars.size() == 6);

    auto* bb = dynamic_cast<arrow::StructScalar*>(scalars[3].get());
    REQUIRE(bb != nullptr);
    CHECK(bb->is_valid);
}

TEST_CASE("GeoArrow: optional PointZ null when not set") {
    integration::VehicleTrackArrowRow row;
    row.set_vehicle_id("v-4");

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    REQUIRE(scalars.size() == 6);
    CHECK_FALSE(scalars[4]->is_valid);  // altitude_point not set
}

TEST_CASE("GeoArrow: optional PointZ valid when set") {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointZArrowRow pz;
    pz.set_x(37.7749).set_y(-122.4194).set_z(100.0);

    row.set_vehicle_id("v-5").set_altitude_point(pz);

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    REQUIRE(scalars.size() == 6);
    CHECK(scalars[4]->is_valid);
}

TEST_CASE("GeoArrow: MultiPoint round-trip") {
    integration::VehicleTrackArrowRow row;
    geoarrow::PointArrowRow w1, w2;
    w1.set_x(10.0).set_y(20.0);
    w2.set_x(30.0).set_y(40.0);

    row.set_vehicle_id("v-6").set_waypoints({w1, w2});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::VehicleTrackArrowRowSchema());
    REQUIRE(scalars.size() == 6);

    auto* wp = dynamic_cast<arrow::ListScalar*>(scalars[5].get());
    REQUIRE(wp != nullptr);
    CHECK(wp->value->length() == 2);
}

// =============================================================================
// Native roundtrip (no Arrow dependency needed)
// =============================================================================

TEST_CASE("GeoArrow: native roundtrip - VehicleTrack") {
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
    CHECK(decoded.vehicle_id() == "v-test");
    CHECK(decoded.last_position().x() == 37.7749);
    CHECK(decoded.last_position().y() == -122.4194);
    REQUIRE(decoded.route().size() == 2);
    CHECK(decoded.route()[0].x() == 0.0);
    CHECK(decoded.bounding_box().xmin() == -10.0);
    CHECK(decoded.bounding_box().ymax() == 20.0);
    REQUIRE(decoded.altitude_point() != nullptr);
    CHECK(decoded.altitude_point()->z() == 100.0);
    REQUIRE(decoded.waypoints().size() == 2);
}

// =============================================================================
// CRS utilities
// =============================================================================

TEST_CASE("CRS: EpsgToProjJson returns PROJJSON for 4326") {
    auto pj = fletcher::EpsgToProjJson(4326);
    REQUIRE_FALSE(pj.empty());
    CHECK(pj.find("\"EPSG\"") != std::string::npos);
    CHECK(pj.find("4326") != std::string::npos);
    CHECK(pj.find("WGS 84") != std::string::npos);
}

TEST_CASE("CRS: EpsgToProjJson returns PROJJSON for 3857") {
    auto pj = fletcher::EpsgToProjJson(3857);
    REQUIRE_FALSE(pj.empty());
    CHECK(pj.find("3857") != std::string::npos);
    CHECK(pj.find("Pseudo-Mercator") != std::string::npos);
}

TEST_CASE("CRS: EpsgToProjJson returns empty for unknown code") {
    CHECK(fletcher::EpsgToProjJson(99999).empty());
}

TEST_CASE("CRS: ResolveCrs passes through raw PROJJSON") {
    std::string projjson = R"({"type":"GeographicCRS"})";
    CHECK(fletcher::ResolveCrs(projjson) == projjson);
}

TEST_CASE("CRS: ResolveCrs resolves EPSG codes") {
    auto resolved = fletcher::ResolveCrs("EPSG:4326");
    CHECK_FALSE(resolved.empty());
    CHECK(resolved.find("WGS 84") != std::string::npos);
}

TEST_CASE("CRS: ResolveCrs returns empty for unknown format") {
    CHECK(fletcher::ResolveCrs("something").empty());
}

TEST_CASE("CRS: BuildExtensionMetadata empty CRS") {
    CHECK(fletcher::BuildExtensionMetadata("") == "{}");
}

TEST_CASE("CRS: BuildExtensionMetadata with PROJJSON") {
    std::string pj = R"({"type":"GeographicCRS"})";
    auto meta = fletcher::BuildExtensionMetadata(pj);
    CHECK(meta == R"({"crs":{"type":"GeographicCRS"}})");
}

// =============================================================================
// Phase 2: Polygon, MultiLineString, MultiPolygon
// =============================================================================

// -- Schema structure ---------------------------------------------------------

TEST_CASE("GeoArrow: LandParcel schema structure") {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    REQUIRE(schema->num_fields() == 5);

    CHECK(schema->field(0)->name() == "parcel_id");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRING);

    // boundary — Polygon → List<List<Struct>>
    CHECK(schema->field(1)->name() == "boundary");
    CHECK(schema->field(1)->type()->id() == arrow::Type::LIST);
    auto inner1 = std::static_pointer_cast<arrow::ListType>(schema->field(1)->type());
    CHECK(inner1->value_type()->id() == arrow::Type::LIST);
    auto inner2 = std::static_pointer_cast<arrow::ListType>(inner1->value_type());
    CHECK(inner2->value_type()->id() == arrow::Type::STRUCT);

    // access_roads — MultiLineString → List<List<Struct>>
    CHECK(schema->field(2)->name() == "access_roads");
    CHECK(schema->field(2)->type()->id() == arrow::Type::LIST);

    // zones — MultiPolygon → List<List<List<Struct>>>
    CHECK(schema->field(3)->name() == "zones");
    CHECK(schema->field(3)->type()->id() == arrow::Type::LIST);
    auto z1 = std::static_pointer_cast<arrow::ListType>(schema->field(3)->type());
    CHECK(z1->value_type()->id() == arrow::Type::LIST);
    auto z2 = std::static_pointer_cast<arrow::ListType>(z1->value_type());
    CHECK(z2->value_type()->id() == arrow::Type::LIST);
    auto z3 = std::static_pointer_cast<arrow::ListType>(z2->value_type());
    CHECK(z3->value_type()->id() == arrow::Type::STRUCT);

    // boundary_3d — optional PolygonZ → nullable
    CHECK(schema->field(4)->name() == "boundary_3d");
    CHECK(schema->field(4)->nullable());
}

// -- Extension metadata -------------------------------------------------------

TEST_CASE("GeoArrow: extension metadata on Polygon field") {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(1)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.polygon");
}

TEST_CASE("GeoArrow: extension metadata on MultiLineString field") {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(2)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.multilinestring");
}

TEST_CASE("GeoArrow: extension metadata on MultiPolygon field") {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(3)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.multipolygon");
}

TEST_CASE("GeoArrow: extension metadata on PolygonZ field") {
    auto schema = ImportNano(integration::LandParcelArrowRowSchema());
    auto meta = schema->field(4)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.polygon");
}

// -- Round-trip encode/decode -------------------------------------------------

TEST_CASE("GeoArrow: Polygon round-trip") {
    integration::LandParcelArrowRow row;

    geoarrow::PointArrowRow p1, p2, p3, p4;
    p1.set_x(0).set_y(0);
    p2.set_x(1).set_y(0);
    p3.set_x(0).set_y(1);
    p4.set_x(0).set_y(0);

    row.set_parcel_id("p-1").set_boundary({{p1, p2, p3, p4}});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    REQUIRE(scalars.size() == 5);

    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    REQUIRE(outer != nullptr);
    CHECK(outer->value->length() == 1);  // one ring

    // Decode back to a LandParcelArrowRow and verify values.
    integration::LandParcelArrowRow decoded(row.Encode());
    REQUIRE(decoded.boundary().size() == 1);
    REQUIRE(decoded.boundary()[0].size() == 4);
    CHECK(decoded.boundary()[0][0].x() == 0.0);
    CHECK(decoded.boundary()[0][0].y() == 0.0);
}

TEST_CASE("GeoArrow: MultiLineString round-trip") {
    integration::LandParcelArrowRow row;

    geoarrow::PointArrowRow a1, a2, b1, b2, b3;
    a1.set_x(0).set_y(0); a2.set_x(1).set_y(1);
    b1.set_x(2).set_y(2); b2.set_x(3).set_y(3); b3.set_x(4).set_y(4);

    row.set_parcel_id("p-2").set_access_roads({{a1, a2}, {b1, b2, b3}});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    REQUIRE(outer != nullptr);
    CHECK(outer->value->length() == 2);

    integration::LandParcelArrowRow decoded(row.Encode());
    REQUIRE(decoded.access_roads().size() == 2);
    CHECK(decoded.access_roads()[0].size() == 2);
    CHECK(decoded.access_roads()[1].size() == 3);
}

TEST_CASE("GeoArrow: MultiPolygon round-trip") {
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
    REQUIRE(outer != nullptr);
    CHECK(outer->value->length() == 2);

    integration::LandParcelArrowRow decoded(row.Encode());
    REQUIRE(decoded.zones().size() == 2);
    REQUIRE(decoded.zones()[0].size() == 1);
    REQUIRE(decoded.zones()[0][0].size() == 4);
}

TEST_CASE("GeoArrow: optional PolygonZ null when not set") {
    integration::LandParcelArrowRow row;
    row.set_parcel_id("p-4");
    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    REQUIRE(scalars.size() == 5);
    CHECK_FALSE(scalars[4]->is_valid);
}

TEST_CASE("GeoArrow: optional PolygonZ valid when set") {
    integration::LandParcelArrowRow row;

    geoarrow::PointZArrowRow p1, p2, p3, p4;
    p1.set_x(0).set_y(0).set_z(10);
    p2.set_x(1).set_y(0).set_z(20);
    p3.set_x(0).set_y(1).set_z(30);
    p4.set_x(0).set_y(0).set_z(10);

    row.set_parcel_id("p-5").set_boundary_3d({{p1, p2, p3, p4}});

    auto scalars = GeoRoundTrip(row.Encode(),
        integration::LandParcelArrowRowSchema());
    CHECK(scalars[4]->is_valid);
}

// =============================================================================
// CRS compile-time and runtime tests are temporarily disabled.
// The nanoarrow schema generator does not yet support CRS parameters.
// These tests will be re-enabled once CRS resolution is ported to nanoarrow.
// =============================================================================
