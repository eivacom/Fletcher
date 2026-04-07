# ProtoIntegration

Integration test project that exercises the `protoc-gen-fletcher` plugin end-to-end. It compiles a set of `.proto` files spanning the full range of supported proto constructs, generates Arrow row code from them, and runs Catch2 tests against the generated classes.

## Purpose

This project exists to catch regressions in the plugin and the codec together. Unit tests in `ProtoPlugin/tests` and `Codec/tests` test the components in isolation; this project verifies that the complete pipeline — proto → code generator → generated class → RowCodec → decoded scalars — produces correct results.

## Proto files

| File | What it tests |
|---|---|
| `proto/simple.proto` | All scalar types (bool, int32, int64, uint32, uint64, float, double, string, bytes, enum), non-nullable and proto3 `optional` |
| `proto/temporal.proto` | `google.protobuf.Timestamp`, `google.protobuf.Duration`, `*Value` wrapper types |
| `proto/nested.proto` | Single-level and multi-level nested messages (`struct<struct<...>>`) |
| `proto/collections.proto` | `repeated` scalar and `repeated` message (`list<T>`, `list<struct<...>>`) |
| `proto/maps.proto` | `map<string, double>` and `map<string, int64>` |
| `proto/complex.proto` | Combination of WKTs, lists, maps, and `optional` fields in a single message |
| `proto/pubsub.proto` | A service definition; tests generated `Publisher` and `Subscriber` classes |

## Generated output

The plugin emits one file per proto:

```
build/generated/simple.fletcher.pb.h
build/generated/temporal.fletcher.pb.h
...
build/generated/pubsub.fletcher.pb.h
```

These are generated at build time via `add_custom_command` in `CMakeLists.txt`. They are not committed to source control.

## Using generated classes

```cpp
#include "simple.fletcher.pb.h"   // generated from simple.proto

// Build and encode a row:
SimpleArrowRow row;
row.set_device_id(42)
   .set_value(3.14)
   .set_label("sensor-A");

fletcher::EncodedRow encoded = row.Encode();

// Decode back to scalars:
fletcher::RowCodec codec(SimpleArrowRow::Schema());
auto scalars = codec.DecodeRow(encoded);
```

### Pub/sub usage

```cpp
#include "pubsub.fletcher.pb.h"   // generated from pubsub.proto
#include <fast_dds_pubsub_provider.hpp>   // or any other PubSubProvider

auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>();

// Publisher
TelemetryFeed_TelemetryStreamPublisher pub(provider);
pub.Publish(TelemetryArrowRow()
    .set_device_id(1)
    .set_value(98.6)
    .set_timestamp(now_ns())
    .set_metric_name("temperature"));

// Subscriber
TelemetryFeed_TelemetryStreamSubscriber sub(
    provider,
    [](int32_t device_id, double value, int64_t ts, std::string_view metric) {
        // Decoded fields delivered here from the DDS thread.
    });
// Unsubscribes automatically when sub goes out of scope.
```

## Running the tests

The tests are built as part of the root project build:

```
cmake --build build/build --config Release
ctest --test-dir build/build -C Release -R fletcher_proto_integration
```

Or run the executable directly:

```
build/build/ProtoIntegration/Release/fletcher_proto_integration_tests.exe
```

## Dependencies

- `Catch2` — test framework
- `row_codec` — codec used directly and indirectly through generated classes
- `pubsub_provider.hpp` — for `MockPubSubProvider` in pub/sub tests
- Generated headers — produced by the custom build command at CMake build time
