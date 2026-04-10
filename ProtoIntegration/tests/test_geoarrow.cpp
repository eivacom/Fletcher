#include <catch2/catch_test_macros.hpp>
#include <arrow/api.h>
#include <row_codec.hpp>

#include "geoarrow_test.fletcher.pb.h"

// Helper: decode an encoded row using the given schema.
static fletcher::ArrowRow GeoRoundTrip(
    const fletcher::EncodedRow& encoded,
    std::shared_ptr<arrow::Schema> schema) {
    fletcher::RowCodec codec(std::move(schema));
    return codec.DecodeRow(encoded);
}

// =============================================================================
// Schema structure
// =============================================================================

TEST_CASE("GeoArrow: VehicleTrack schema structure") {
    auto schema = integration::VehicleTrackArrowRowSchema();
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
    auto schema = integration::VehicleTrackArrowRowSchema();
    auto meta = schema->field(1)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.point");
}

TEST_CASE("GeoArrow: extension metadata on LineString field") {
    auto schema = integration::VehicleTrackArrowRowSchema();
    auto meta = schema->field(2)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.linestring");
}

TEST_CASE("GeoArrow: extension metadata on Box field") {
    auto schema = integration::VehicleTrackArrowRowSchema();
    auto meta = schema->field(3)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.box");
}

TEST_CASE("GeoArrow: extension metadata on PointZ field") {
    auto schema = integration::VehicleTrackArrowRowSchema();
    auto meta = schema->field(4)->metadata();
    REQUIRE(meta != nullptr);
    auto result = meta->Get("ARROW:extension:name");
    REQUIRE(result.ok());
    CHECK(*result == "geoarrow.point");
}

TEST_CASE("GeoArrow: extension metadata on MultiPoint field") {
    auto schema = integration::VehicleTrackArrowRowSchema();
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
    auto schema = integration::VehicleTrackArrowRowSchema();
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(1)->type());
    REQUIRE(point_type->num_fields() == 2);
    CHECK(point_type->field(0)->name() == "x");
    CHECK(point_type->field(0)->type()->id() == arrow::Type::DOUBLE);
    CHECK(point_type->field(1)->name() == "y");
    CHECK(point_type->field(1)->type()->id() == arrow::Type::DOUBLE);
}

TEST_CASE("GeoArrow: PointZ struct has x, y, z children") {
    auto schema = integration::VehicleTrackArrowRowSchema();
    auto point_type = std::static_pointer_cast<arrow::StructType>(
        schema->field(4)->type());
    REQUIRE(point_type->num_fields() == 3);
    CHECK(point_type->field(0)->name() == "x");
    CHECK(point_type->field(1)->name() == "y");
    CHECK(point_type->field(2)->name() == "z");
}

TEST_CASE("GeoArrow: Box struct has xmin, ymin, xmax, ymax children") {
    auto schema = integration::VehicleTrackArrowRowSchema();
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
// Byte-identical verification: EncodeTo vs RowCodec
// =============================================================================

TEST_CASE("GeoArrow: EncodeTo byte-identical for VehicleTrack") {
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

    // Path A: EncodeTo (direct)
    auto encoded_direct = row.Encode();

    // Path B: ToScalars → RowCodec
    fletcher::RowCodec codec(integration::VehicleTrackArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(row.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}
