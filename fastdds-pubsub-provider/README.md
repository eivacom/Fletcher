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
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
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

## Building the package locally

### Windows (MSVC)

**Prerequisites:** Visual Studio 2022 with C++ workload, CMake, Python, Conan 2.

Install the EIVA Conan configuration (remote + profiles) once:

```bat
conan config install https://github.com/eivacom/conan-configuration.git
```

Build and package (Release, no tests):

```bat
conan create . --build=missing -pr:a=Visual-Studio-2022-v143-x64-Release
```

Build, run tests, and package:

```bat
conan create . --build=missing -pr:a=Visual-Studio-2022-v143-x64-Release -o "&:run_tests=True"
```

The built package lands in the local Conan cache (`%USERPROFILE%\.conan2`).

To iterate without the full `conan create` cycle use `conan build` against the source tree:

```bat
conan build . --build=missing -pr:a=Visual-Studio-2022-v143-x64-Debug -o "&:run_tests=True"
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
conan build . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

Build, package, and run tests (equivalent to CI):

```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Release -o "&:run_tests=True"
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

The `fast-dds::fast-dds` link dependency is private to this library;
consumers do not need to depend on Fast DDS directly.

## CI pipeline

The workflow is defined in `.github/workflows/fletcher-fastdds-pubsub-provider.yml` and runs on every
push to `feature/fletcher-fastdds-pubsub-provider`, on every pull request touching `fastdds-pubsub-provider/**`,
and on release tags matching `fastdds-pubsub-provider-v*`.

```
push / pull_request
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
Windows Server Core LTSC 2025            Ubuntu 24.04 x64
Native runner                            Docker container (.devcontainer)
Profile: Visual-Studio-2022-             Profile: Ubuntu22-gcc-12-Release
         v143-x64-Release
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼
                        upload
                  (tag push only)
                  Creates GitHub Release (.tgz assets)
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-server-core-ltsc2025` | `Visual-Studio-2022-v143-x64-Release` | Release |
| `build-linux` | `ubuntu_24.04_x64` (Docker) | `Ubuntu22-gcc-12-Release` | Release |

Both jobs build with `-o "&:run_tests=True"` so the full GTest suite runs as part of every CI build.

### Package handoff

Both platforms produce a separate binary package. Each build job saves its
package to a GitHub Actions artifact; the `upload` job downloads both, restores
them into the Conan cache, and attaches them as GitHub Release assets:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

The `upload` job only runs on a tag push after both `build-windows` and
`build-linux` pass, and verifies that the tag version matches the version in
`conanfile.py` before uploading.

## Runtime requirements

The Fast DDS runtime (discovery server or default multicast discovery) must be reachable at the configured domain ID. On a single machine with no network configuration, the default multicast discovery works out of the box. For multi-host deployments, configure Fast DDS via its XML profile mechanism or a discovery server — see the [Fast DDS documentation](https://fast-dds.docs.eprosima.com/).
