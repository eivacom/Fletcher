# FastDDSPubSubProvider

Implements `fletcher::PubSub` using [eProsima Fast DDS](https://fast-dds.docs.eprosima.com/) (RTPS). Transports `EncodedRow` byte buffers over a DDS domain with reliability settings tuned to minimise message loss.

## How it works

A single `FastDDSPubSubProvider` instance manages one DDS `DomainParticipant`, one `Publisher`, and one `Subscriber`. Topics are created on demand via `CreateTopic`. DataWriters and DataReaders are created lazily on the first call to `Publish` and `Subscribe` respectively.

The binary payload sent over the DDS bus is a raw `EncodedRow` (the positional wire format produced by generated code or `Codec::EncodeRow`), wrapped in a minimal CDR-LE framing: a 4-byte encapsulation header followed by a 4-byte length prefix. A custom `TopicDataType` (`RawBytesTopicType`) handles the CDR serialisation without requiring IDL generation as a build step.

### Topic name

The `std::vector<std::string>` topic segments from `PubSub` are joined with `/` to form the DDS topic name. For example, segments `{"integration", "TelemetryFeed", "TelemetryStream"}` become the DDS topic `"integration/TelemetryFeed/TelemetryStream"`.

### QoS settings (both DataWriter and DataReader)

| Policy | Setting | Reason |
|---|---|---|
| `reliability` | `RELIABLE_RELIABILITY_QOS` | The middleware retransmits unacknowledged samples; no silent drops. |
| `history` | `KEEP_ALL_HISTORY_QOS` | All samples are retained until every matched reader has acknowledged them. With `RELIABLE`, the writer blocks (rather than dropping) when the history is full. |
| `durability` | `TRANSIENT_LOCAL_DURABILITY_QOS` | Samples published before a subscriber joins are replayed to that subscriber on discovery, so no data is lost during startup races. |

These three policies together implement "at-least-once" delivery within a single DDS domain.

## Usage

```cpp
#include <fast_dds_pubsub_provider.hpp>
using namespace fletcher;

// Create a provider on DDS domain 0 (default).
// max_payload_bytes bounds the full DDS payload: CDR framing + row bytes + attachments.
auto provider = std::make_shared<FastDDSPubSubProvider>(
    /*domain_id=*/0,
    /*max_payload_bytes=*/1024 * 1024);  // 1 MB
```

The provider is then passed to generated `Publisher` and `Subscriber` classes:

```cpp
// Generated from a proto service definition (in fletcher_gen namespace):
using namespace fletcher_gen::integration;

TelemetryFeed_TelemetryStreamPublisher pub(provider);
pub.Publish(Telemetry().set_device_id(1).set_value(98.6));

TelemetryFeed_TelemetryStreamSubscriber sub(provider);
sub.Subscribe([](Telemetry msg, fletcher::Attachments att) {
    // Called on a Fast DDS internal listener thread.
});
```

Or used directly through the `PubSub` interface:

```cpp
provider->CreateTopic({"my", "topic"}, schema);
provider->Publish({"my", "topic"}, encoded_row);
provider->Subscribe({"my", "topic"}, [](const EncodedRow& row) { ... });
provider->Unsubscribe({"my", "topic"});
```

### Constraints

- `CreateTopic` must be called before `Publish` on the publisher side. Calling it twice for the same topic throws.
- On the subscriber side `Subscribe` can be called without a prior `CreateTopic` — it polls the `__schema` companion DDS topic (up to 5 s) to retrieve the schema published by the publisher.
- Only one subscription per topic per provider instance is supported (one `DataReader` per topic). Call `Unsubscribe` before re-subscribing.
- The subscription callback is invoked from a Fast DDS internal listener thread. Shared state accessed from the callback must be protected externally.
- `FastDDSPubSubProvider` is non-copyable and non-movable (DDS entities cannot be transferred).

## Building

This library is distributed as a Conan package: `eiva-fletcher-fastdds-pubsub-provider`.

### Consuming via Conan

Add the package to your `conanfile.py`:

```python
def requirements(self):
    self.requires("eiva-fletcher-fastdds-pubsub-provider/0.1.0-alpha")
```

Then install and generate:

```
conan install . --build=missing
```

### Linking in your own CMake target

```cmake
find_package(eiva-fletcher-fastdds-pubsub-provider REQUIRED)

# Use the fully qualified target name:
target_link_libraries(my_app PRIVATE eiva-fletcher-fastdds-pubsub-provider::eiva-fletcher-fastdds-pubsub-provider)

# Or the convenience alias injected by the package's build module:
target_link_libraries(my_app PRIVATE fletcher::fastdds-pubsub-provider)
```

The `fast-dds::fast-dds` link dependency is private to this library; consumers do not need to depend on Fast DDS directly.

### Building the package itself

```
conan create . --build=missing -pr:a=<your-profile>

# With tests:
conan create . --build=missing -pr:a=<your-profile> -o "&:run_tests=True"
```

## Runtime requirements

The Fast DDS runtime (discovery server or default multicast discovery) must be reachable at the configured domain ID. On a single machine with no network configuration, the default multicast discovery works out of the box. For multi-host deployments, configure Fast DDS via its XML profile mechanism or a discovery server — see the [Fast DDS documentation](https://fast-dds.docs.eprosima.com/).
