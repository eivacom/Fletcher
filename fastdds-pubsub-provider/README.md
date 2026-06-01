# FastDDSPubSubProvider

Implements `fletcher::PubSubProvider` using [eProsima Fast DDS](https://fast-dds.docs.eprosima.com/) (RTPS). Transports `EncodedRow` byte buffers over a DDS domain with reliability settings tuned to minimise message loss.

## How it works

A single `FastDDSPubSubProvider` instance manages one DDS `DomainParticipant`, one `Publisher`, and one `Subscriber`. Topics are created on demand via `CreateTopic`. DataWriters and DataReaders are created lazily on the first call to `Publish` and `Subscribe` respectively.

The binary payload sent over the DDS bus is a raw `EncodedRow` (the positional wire format produced by generated code or `Codec::EncodeRow`), wrapped in a minimal CDR-LE framing: a 4-byte encapsulation header followed by a 4-byte length prefix. A custom `TopicDataType` (`RawBytesTopicType`) handles the CDR serialisation without requiring IDL generation as a build step.

### Topic name

The `std::vector<std::string>` topic segments from `PubSubProvider` are joined with `/` to form the DDS topic name. For example, segments `{"integration", "TelemetryFeed", "TelemetryStream"}` become the DDS topic `"integration/TelemetryFeed/TelemetryStream"`.

### QoS configuration

QoS is configured up-front via `FastDDSProviderOptions` at construction time. The Options struct has provider-instance defaults (`default_writer_qos`, `default_reader_qos`) and per-topic overrides (`topic_writer_qos`, `topic_reader_qos`). For a given topic, the per-topic override (if present) wins; otherwise the instance default is applied. There are no runtime setters — configuration is immutable after construction.

#### Fletcher's default QoS profile

If you construct `FastDDSPubSubProvider(FastDDSProviderOptions{})` without touching the QoS fields, both data DataWriter and DataReader get this profile:

| Policy | Setting | Reason |
|---|---|---|
| `reliability` | `RELIABLE_RELIABILITY_QOS` | The middleware retransmits unacknowledged samples; no silent drops. |
| `history` | `KEEP_ALL_HISTORY_QOS` | All samples are retained until every matched reader has acknowledged them. With `RELIABLE`, the writer blocks (rather than dropping) when the history is full. |
| `durability` | `TRANSIENT_LOCAL_DURABILITY_QOS` | Samples published before a subscriber joins are replayed to that subscriber on discovery, so no data is lost during startup races. |

These three policies together implement "at-least-once" delivery within a single DDS domain.

The companion schema channel (`__schema` topic) always uses `RELIABLE` + `KEEP_LAST(depth=1)` + `TRANSIENT_LOCAL`. It is a Fletcher-internal implementation detail and not configurable.

## Usage

```cpp
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
using namespace fletcher;

// Default options — Fletcher's profile on domain 0, 1 MB max payload.
auto provider = std::make_shared<FastDDSPubSubProvider>(FastDDSProviderOptions{});

// Custom options — pick a DDS domain and tune QoS:
FastDDSProviderOptions opts;
opts.domain_id = 7;
opts.max_payload_bytes = 4 * 1024 * 1024;
opts.default_writer_qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
opts.default_writer_qos.history().depth = 10;
auto custom = std::make_shared<FastDDSPubSubProvider>(std::move(opts));
```

The provider is passed to `fletcher::Publisher` / `fletcher::Subscriber` or to generated `<Msg>Publisher` / `<Msg>Subscriber` classes:

```cpp
// Generated from a proto service definition (in fletcher_gen namespace):
using namespace fletcher_gen::integration;

TelemetryFeed_TelemetryStreamPublisher pub(provider);
pub.Publish(Telemetry().set_device_id(1).set_value(98.6));

TelemetryFeed_TelemetryStreamSubscriber sub(provider);
uint64_t sub_id = sub.Subscribe([](Telemetry msg, fletcher::Attachments att) {
    // Called on a Fast DDS internal listener thread.
});
```

Or used directly through the `PubSubProvider` interface:

```cpp
provider->CreateTopic({"my", "topic"}, schema);
provider->Publish({"my", "topic"}, encoded_row);
provider->Subscribe({"my", "topic"}, [](const uint8_t* data, size_t len,
                                          SharedSchema, Attachments) { ... });
provider->Unsubscribe({"my", "topic"});
```

### Per-topic QoS overrides

```cpp
FastDDSProviderOptions opts;
// Override only the "telemetry/high-rate" topic to use KEEP_LAST(5):
auto& wqos = opts.topic_writer_qos["telemetry/high-rate"];
wqos = opts.default_writer_qos;  // start from the default
wqos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
wqos.history().depth = 5;
auto provider = std::make_shared<FastDDSPubSubProvider>(std::move(opts));
```

The key in `topic_writer_qos` / `topic_reader_qos` is the joined topic name (segments joined with `/`).

### Constraints

- `CreateTopic` must be called before `Publish` on the publisher side. Calling it twice for the same topic throws.
- On the subscriber side `Subscribe` can be called without a prior `CreateTopic` — it polls the `__schema` companion DDS topic (up to 5 s) to retrieve the schema published by the publisher.
- Only one subscription per topic per provider instance is supported (one `DataReader` per topic). Call `Unsubscribe` before re-subscribing. Multi-callback fan-out lives in `fletcher::Subscriber` one layer up.
- The subscription callback is invoked from a Fast DDS internal listener thread. Shared state accessed from the callback must be protected externally.
- `FastDDSPubSubProvider` is non-copyable and non-movable (DDS entities cannot be transferred).

## Building the package locally

### Windows (MSVC)

**Prerequisites:** Visual Studio 2022 with C++ workload, CMake, Python, Conan 2.

Conan profiles live in [`../.conan-profiles/`](../.conan-profiles/) in the repo and are referenced by relative path — no separate profile-install step is needed.

Build and package (Release, no tests):

```bat
conan create . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Build, run tests, and package:

```bat
conan create . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

The built package lands in the local Conan cache (`%USERPROFILE%\.conan2`).

To iterate without the full `conan create` cycle use `conan build` against the source tree:

```bat
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

To run the tests separately with CTest after a `conan build` (Visual Studio is a multi-config generator so the config must be specified):

```bat
ctest --test-dir build -C Debug --output-on-failure
```

Add `-V` for full GTest output:

```bat
ctest --test-dir build -C Debug --output-on-failure -V
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory.

If the `build/` folder contains stale artifacts from a previous Windows build, remove it first — `DartConfiguration.tcl` bakes in absolute paths at configure time and will cause CTest to fail when those paths don't match the current platform:

```bash
rm -rf build/
```

Build and run tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Build, package, and run tests (equivalent to CI):

```bash
conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Run tests separately with CTest after a `conan build` (the Linux build lives under `build/<BuildType>`):

```bash
ctest --test-dir build/Debug --output-on-failure
```

Add `-V` for full GTest output:

```bash
ctest --test-dir build/Debug --output-on-failure -V
```

## Consuming the package

### 1. Add to your conanfile.py

```python
def requirements(self):
    self.requires("fletcher-fastdds-pubsub-provider/0.1.0-alpha")
```

Install dependencies:

```bash
conan install . --build=missing -pr:a=<your-profile>
```

### 2. Wire up CMake

```cmake
find_package(fletcher-fastdds-pubsub-provider REQUIRED)

# Fully qualified target name:
target_link_libraries(my_app PRIVATE
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)

# Or the convenience alias injected by the package's build module:
target_link_libraries(my_app PRIVATE fletcher::fastdds-pubsub-provider)
```

The `fast-dds::fast-dds` link dependency is **public** to this library
because `FastDDSProviderOptions` exposes `eprosima::fastdds::dds::DataWriterQos`
and `eprosima::fastdds::dds::DataReaderQos` in its public API. Consumers
get the FastDDS headers transitively and can include them directly when
they need to construct or tune a QoS profile.

## CI pipeline

The build workflow is defined in `.github/workflows/ci.fastdds-pubsub-provider.yml`.
It is `workflow_call`-only — invoked from `ci.pr.yml` for pull requests
touching `fastdds-pubsub-provider/**` and from `cd.fastdds-pubsub-provider.yml`
on `fastdds-pubsub-provider-v*` tag pushes. The matching upload job
lives in `cd.fastdds-pubsub-provider.yml`, not here.

```
ci.pr.yml (PRs) / cd.fastdds-pubsub-provider.yml (tag push)
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
windows-2022                             ubuntu-latest
Native runner                            Docker container (.devcontainer)
Profile: Windows-msvc194-                Profile: Linux-gcc13-
         x86_64-Release                            x86_64-Release
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼ (only on tag push)
                        upload
              (cd.fastdds-pubsub-provider.yml job)
              Creates GitHub Release with
              fletcher-fastdds-pubsub-provider-{windows,linux}-conan-package.tgz
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-2022` | `.conan-profiles/Windows-msvc194-x86_64-Release` | Release |
| `build-linux` | `ubuntu-latest` (Docker) | `.conan-profiles/Linux-gcc13-x86_64-Release` | Release |

Both jobs build with `-o "&:run_tests=True"` so the full GTest suite runs as part of every CI build.

### Package handoff

Both platforms produce a separate binary package. Each build job saves
its package to a GitHub Actions workflow artifact; on a tag push the
`upload` job in `cd.fastdds-pubsub-provider.yml` downloads both and
attaches them as GitHub Release assets:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

The `upload` job only runs from `cd.fastdds-pubsub-provider.yml`
(tag push), and verifies that the tag version matches the version in
`conanfile.py` before creating the release.

## Runtime requirements

The Fast DDS runtime (discovery server or default multicast discovery) must be reachable at the configured domain ID. On a single machine with no network configuration, the default multicast discovery works out of the box. For multi-host deployments, configure Fast DDS via its XML profile mechanism or a discovery server — see the [Fast DDS documentation](https://fast-dds.docs.eprosima.com/).
