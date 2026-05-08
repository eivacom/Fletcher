// Vector emitter for the cross-language byte-compat test.
//
// For each scenario we encode a known telemetry row via the protoc-generated
// row class and write a single JSON line to stdout. The vitest test side
// owns the expected input values for each scenario name and uses them to
// check that:
//   1. the TS codec decodes the bytes to those input values, and
//   2. the TS codec re-encodes the same input to byte-identical bytes.
//
// Scenario names must agree with the `scenarios` map in byte-compat.test.ts.
// Adding a new scenario is a two-file change: define the row + name here,
// add the same name + expected input on the TS side.
//
// Output format (one scenario per line):
//   {"name":"<scenario>","encoded":"<base64>"}

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
    out.reserve(((data.size() + 2) / 3) * 4);

    // Process input in 3-byte chunks → 4 base64 characters. Bounded by
    // chunk so no signed/unsigned width concerns even for arbitrarily
    // long inputs.
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t v = (static_cast<uint32_t>(data[i])     << 16)
                   | (static_cast<uint32_t>(data[i + 1]) <<  8)
                   |  static_cast<uint32_t>(data[i + 2]);
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >>  6) & 0x3F]);
        out.push_back(alphabet[ v        & 0x3F]);
        i += 3;
    }
    const size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t v = (static_cast<uint32_t>(data[i])     << 16)
                   | (static_cast<uint32_t>(data[i + 1]) <<  8);
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >>  6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

template <class Row>
void Emit(const char* name, const Row& row) {
    fletcher::EncodedRow encoded = row.Encode();
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    std::cout << R"({"name":")" << name
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
        Emit("basic", row);
    }

    {
        Telemetry row;
        row.set_sensor_id(0)
           .set_temperature(0.0)
           .set_label("")
           .set_valid(false)
           .set_readings({});
        Emit("zero", row);
    }

    {
        Telemetry row;
        row.set_sensor_id(-7)
           .set_temperature(-12.75)
           .set_label("alpha")
           .set_valid(true)
           .set_readings({-1, 0, 1});
        Emit("negative", row);
    }

    return 0;
}
