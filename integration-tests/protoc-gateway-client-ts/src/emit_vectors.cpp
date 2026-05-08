// Vector emitter for the cross-language byte-compat test.
//
// For each scenario we encode a known telemetry row via the protoc-generated
// row class and write a single JSON line to stdout. The vitest test then
// spawns this binary, parses the lines, and asserts that:
//   1. the TS codec decodes the bytes to the original input values, and
//   2. the TS codec re-encodes the same input to byte-identical bytes.
//
// Output format (one scenario per line):
//   {"name":"<scenario>","input":{<field>:<value>,...},"encoded":"<base64>"}

#include "telemetry.fletcher.pb.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string Base64Encode(const std::vector<uint8_t>& data) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int bits = -6;
    for (uint8_t c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(alphabet[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        out.push_back(alphabet[((val << 8) >> (bits + 8)) & 0x3F]);
    }
    while (out.size() % 4 != 0) {
        out.push_back('=');
    }
    return out;
}

template <class Row>
void Emit(const char* name, const std::string& input_json, const Row& row) {
    fletcher::EncodedRow encoded = row.Encode();
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    std::cout << R"({"name":")" << name
              << R"(","input":)" << input_json
              << R"(","encoded":")" << Base64Encode(bytes) << "\"}\n";
}

}  // namespace

int main() {
    using namespace fletcher_gen::integration;

    {
        Telemetry row;
        row.set_sensor_id(42)
           .set_temperature(23.5)
           .set_label("intake")
           .set_valid(true)
           .set_readings({100, 200, 300});
        Emit("basic",
             R"({"sensor_id":42,"temperature":23.5,"label":"intake","valid":true,"readings":[100,200,300]})",
             row);
    }

    {
        Telemetry row;
        row.set_sensor_id(0)
           .set_temperature(0.0)
           .set_label("")
           .set_valid(false)
           .set_readings({});
        Emit("zero",
             R"({"sensor_id":0,"temperature":0,"label":"","valid":false,"readings":[]})",
             row);
    }

    {
        Telemetry row;
        row.set_sensor_id(-7)
           .set_temperature(-12.75)
           .set_label("alpha")
           .set_valid(true)
           .set_readings({-1, 0, 1});
        Emit("negative",
             R"({"sensor_id":-7,"temperature":-12.75,"label":"alpha","valid":true,"readings":[-1,0,1]})",
             row);
    }

    return 0;
}
