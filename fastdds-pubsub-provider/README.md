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

### Delivery guarantees

The provider upholds the `PubSubProvider::SubscribeCallback` contract:

- **Schema before data.** The subscription callback is never invoked with a null schema. Because `Subscribe` is non-blocking and may run before any publisher exists, a data sample can arrive before the topic schema does (the schema travels on the separate `__schema` channel). The provider holds such samples until the schema is known, then delivers them.
- **Per-writer order.** Samples from a single writer reach the callback in the order they were published. DDS delivers a single writer's samples to a `DataReader` in order under `RELIABLE` QoS; the provider preserves that order all the way to the callback — **including across the schema handoff**, where the buffered pre-schema backlog is delivered before, and never interleaved with, samples that arrive live afterwards.

Both guarantees are enforced by routing every sample (buffered backlog and live) through a single ordered FIFO that is drained by one thread at a time (`internal::OrderedDelivery`). The schema handoff is the one moment two threads are active — the schema-listener thread that resolves the schema and flushes the backlog, and the data-reader thread delivering live samples — so a sample offered while a drain is in progress is appended behind the in-flight backlog rather than delivered inline, which is what keeps the two from interleaving.

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

Set a different writer QoS per topic when you publish to several topics that each need their own profile. The map key is the joined topic string (segments joined with `/`); any topic not present in the map falls back to `default_writer_qos` / `default_reader_qos`.

```cpp
FastDDSProviderOptions opts;

// "telemetry/high-rate": shallow history, drop old samples.
auto& fast = opts.topic_writer_qos["telemetry/high-rate"];
fast = opts.default_writer_qos;
fast.history().kind    = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
fast.history().depth   = 5;
fast.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;

// "config/snapshot": keep everything, durable for late subscribers.
auto& cfg = opts.topic_writer_qos["config/snapshot"];
cfg = opts.default_writer_qos;
cfg.history().kind    = eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS;
cfg.durability().kind = eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;

// "ops/log": fire-and-forget, no reliability overhead.
auto& log = opts.topic_writer_qos["ops/log"];
log = opts.default_writer_qos;
log.reliability().kind = eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;

auto provider = std::make_shared<FastDDSPubSubProvider>(std::move(opts));
Publisher publisher(provider);
publisher.CreateTopic({"telemetry", "high-rate"}, schema_a);  // uses 'fast'
publisher.CreateTopic({"config", "snapshot"},   schema_b);    // uses 'cfg'
publisher.CreateTopic({"ops", "log"},           schema_c);    // uses 'log'
publisher.CreateTopic({"misc", "events"},       schema_d);    // uses default_writer_qos
```

`topic_reader_qos` works the same way on the subscriber side.

### Constraints

- `CreateTopic` must be called before `Publish` on the publisher side. The conflict check is **per topic** (keyed by the topic name): re-declaring _the same topic_ with an identical schema is idempotent (so several publishers may share one topic), while re-declaring it with a _different_ schema throws (a conflict). Distinct topics are independent — two different topics may carry the **same** schema (identical schemas can describe different data); that is never a conflict.
- On the subscriber side `Subscribe` can be called without a prior `CreateTopic` and is **non-blocking** — it never waits for a publisher. The schema arrives asynchronously over the `__schema` companion DDS topic; `Subscribe` returns a `std::shared_future<SharedSchema>` that resolves when the schema is known, and the provider buffers incoming data until then so the callback is never invoked with a null schema. (This is the subscriber-first contract — subscribe before any publisher exists.) Per-writer order is preserved across this handoff — see [Delivery guarantees](#delivery-guarantees).
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
    self.requires("fletcher-fastdds-pubsub-provider/0.4.0-alpha")
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

The `fastdds` link dependency is **public** to this library
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
