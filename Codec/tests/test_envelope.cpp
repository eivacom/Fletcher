#include <catch2/catch_all.hpp>

#include "envelope.hpp"

using namespace fletcher;

// ---------------------------------------------------------------------------
// SerializeEnvelope / DeserializeEnvelope roundtrips
// ---------------------------------------------------------------------------

TEST_CASE("Envelope: roundtrip with no attachments") {
    Envelope env;
    env.row = {0x01, 0x02, 0x03, 0x04};

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    CHECK(restored.row == env.row);
    CHECK(restored.attachments.empty());
}

TEST_CASE("Envelope: roundtrip with one attachment") {
    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});

    Envelope env;
    env.row = {0xAA, 0xBB};
    env.attachments["image"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    CHECK(restored.row == env.row);
    REQUIRE(restored.attachments.size() == 1);
    REQUIRE(restored.attachments.count("image") == 1);
    CHECK(*restored.attachments.at("image") == *blob);
}

TEST_CASE("Envelope: roundtrip with multiple attachments") {
    auto blob_a = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x01, 0x02});
    auto blob_b = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x03, 0x04, 0x05});
    auto blob_c = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{});

    Envelope env;
    env.row = {0xFF};
    env.attachments["a"] = blob_a;
    env.attachments["b"] = blob_b;
    env.attachments["empty"] = blob_c;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    CHECK(restored.row == env.row);
    REQUIRE(restored.attachments.size() == 3);
    CHECK(*restored.attachments.at("a") == *blob_a);
    CHECK(*restored.attachments.at("b") == *blob_b);
    CHECK(restored.attachments.at("empty")->empty());
}

TEST_CASE("Envelope: roundtrip with large blob (>1 MB)") {
    std::vector<uint8_t> big(1'100'000, 0x42);
    auto blob = std::make_shared<const std::vector<uint8_t>>(std::move(big));

    Envelope env;
    env.row = {0x00};
    env.attachments["big"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    CHECK(restored.row == env.row);
    REQUIRE(restored.attachments.count("big") == 1);
    CHECK(restored.attachments.at("big")->size() == 1'100'000);
    CHECK(restored.attachments.at("big")->front() == 0x42);
}

TEST_CASE("Envelope: empty row with attachments") {
    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x01});

    Envelope env;
    // row is empty
    env.attachments["data"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    CHECK(restored.row.empty());
    REQUIRE(restored.attachments.size() == 1);
    CHECK(*restored.attachments.at("data") == *blob);
}

TEST_CASE("Envelope: completely empty envelope") {
    Envelope env;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    CHECK(restored.row.empty());
    CHECK(restored.attachments.empty());
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST_CASE("DeserializeEnvelope: throws on truncated buffer") {
    std::vector<uint8_t> tiny = {0x01, 0x02};
    CHECK_THROWS_AS(DeserializeEnvelope(tiny), std::invalid_argument);
}

TEST_CASE("DeserializeEnvelope: throws on truncated row data") {
    // Claim row_len=100 but only provide 4 bytes.
    std::vector<uint8_t> buf = {0x64, 0x00, 0x00, 0x00,  // row_len = 100
                                 0x01, 0x02, 0x03, 0x04}; // only 4 bytes
    CHECK_THROWS_AS(DeserializeEnvelope(buf), std::invalid_argument);
}

TEST_CASE("DeserializeEnvelope: throws on truncated attachment key") {
    // Valid row (len=1, data=0xFF), attach_count=1, key_len=100 but no key data.
    std::vector<uint8_t> buf = {
        0x01, 0x00, 0x00, 0x00,  // row_len = 1
        0xFF,                     // row data
        0x01, 0x00, 0x00, 0x00,  // attach_count = 1
        0x64, 0x00, 0x00, 0x00}; // key_len = 100 (truncated)
    CHECK_THROWS_AS(DeserializeEnvelope(buf), std::invalid_argument);
}
