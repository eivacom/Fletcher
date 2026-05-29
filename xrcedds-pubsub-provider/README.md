# xrcedds-pubsub-provider

Implements `fletcher::PubSub` using [eProsima Micro XRCE-DDS Client](https://micro-xrce-dds.docs.eprosima.com/) (v2.4.x). Transports `EncodedRow` byte buffers between a constrained client and an XRCE-DDS Agent over UDP, TCP, or serial.

## How it works

`XrceDDSPubSubProvider` connects to a running **XRCE-DDS Agent** at construction time. A background thread calls `uxr_run_session_time` continuously to pump the session. All XRCE-DDS entities (participant, topic, publisher/subscriber, data writer/reader) are created on the Agent when `CreateTopic` or `Subscribe` is called.

Incoming messages arrive through a single global `on_topic` callback demultiplexed by reader object ID to the correct per-topic subscriber callback.

### Wire format

The binary payload is a serialized `Envelope`:

```
[ROW_LEN:4 LE][ROW_DATA:ROW_LEN][ATTACH_COUNT:4 LE][attachments...]
```

This format is shared with the legacy FastDDS provider so payloads are wire-compatible between provider implementations.

### Topic name

Topic segments are joined with `/`. Segments `{"integration", "TelemetryFeed", "TelemetryStream"}` produce the DDS topic name `"integration/TelemetryFeed/TelemetryStream"`.

### QoS

| Entity | Reliability | Durability | History |
|---|---|---|---|
| Data writer / reader | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_ALL` depth 16 |
| Schema writer / reader | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_LAST` depth 1 |

### Schema discovery

`CreateTopic` publishes serialized schema bytes to a companion `<topic>/__schema` DDS topic. When `Subscribe` is called before `CreateTopic` (subscriber-side), it polls the `__schema` topic for up to 5 seconds to retrieve the schema.

## Usage

```cpp
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
using namespace fletcher;

// Connect to an XRCE-DDS Agent on localhost:2018 (defaults).
// Throws std::runtime_error if the Agent is not reachable.
XrceDDSPubSubProvider provider;

// Or with explicit configuration:
XrceConfig cfg;
cfg.transport    = XrceTransport::kUdp;
cfg.agent_ip     = "192.168.1.10";
cfg.agent_port   = 2018;
cfg.max_payload  = 4096;
auto provider2 = XrceDDSPubSubProvider(cfg);
```

### `XrceConfig` fields

| Field | Default | Description |
|---|---|---|
| `transport` | `kUdp` | Transport: `kUdp`, `kTcp`, or `kSerial` (serial not yet implemented) |
| `agent_ip` | `"127.0.0.1"` | XRCE-DDS Agent IP address |
| `agent_port` | `2018` | XRCE-DDS Agent port |
| `serial_device` | `""` | Serial device path (only when `transport == kSerial`) |
| `serial_baudrate` | `115200` | Serial baud rate |
| `max_payload` | `512` | Maximum payload size in bytes |
| `stream_history` | `4` | Reliable stream history depth (must be a power of 2) |
| `run_loop_ms` | `10` | Milliseconds per `uxr_run_session_time` call in the run-loop thread |
| `session_key` | `0xAABBCCDD` | XRCE session key — must be unique per client on the same Agent |
| `connect_timeout_ms` | `3000` | Timeout for the initial session creation handshake; lower this in tests |

### PubSub interface

```cpp
provider.CreateTopic({"my", "topic"}, schema);
provider.Publish({"my", "topic"}, encoder);
auto result = provider.Subscribe({"my", "topic"}, [](const uint8_t* data, size_t len,
                                                      SharedSchema, Attachments) { ... });
provider.Unsubscribe({"my", "topic"});
```

### Constraints

- `CreateTopic` must be called before `Publish`. Calling it twice on the same topic throws.
- `Subscribe` can be called without a prior `CreateTopic` — it polls the `__schema` companion topic for up to 5 seconds.
- Only one subscription per topic per provider instance. Call `Unsubscribe` before re-subscribing.
- The subscription callback is invoked from the background run-loop thread. Shared state accessed from the callback must be protected externally.
- `XrceDDSPubSubProvider` is non-copyable and non-movable.
- Serial transport (`kSerial`) throws `std::runtime_error` — not yet implemented.

## Runtime requirement

A running [MicroXRCEAgent](https://micro-xrce-dds.docs.eprosima.com/en/latest/agent.html) is required at the configured IP and port. Start one with:

```bash
MicroXRCEAgent udp4 -p 2018
```

The unit tests do **not** require an Agent — the test that exercises missing-Agent behaviour expects the constructor to throw and passes without one.

> **Test duration note:** `ConstructorThrowsWithoutAgent` and `SerialTransportNotImplemented` each take ~1 second to complete. This is the floor imposed by the XRCE-DDS client's internal per-attempt UDP timeout — there is no public API to shorten it further without patching the client library. Both tests set `connect_timeout_ms = 200` which maps to 0 retries (a single attempt), so the combined overhead is ~2 seconds per test run.

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

Run tests with CTest after a `conan build` (Visual Studio is a multi-config generator so `-C` is required):

```bat
ctest --test-dir build -C Debug --output-on-failure
```

Add `-V` for full GTest output:

```bat
ctest --test-dir build -C Debug --output-on-failure -V
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory.

If the `build/` folder contains stale artifacts from a previous Windows build, remove it first — `DartConfiguration.tcl` bakes in absolute paths at configure time and will cause CTest to fail:

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

Run tests with CTest after a `conan build` (the Linux build lives under `build/<BuildType>`):

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
    self.requires("fletcher-xrcedds-pubsub-provider/0.1.0-alpha")
```

Install dependencies:

```bash
conan install . --build=missing -pr:a=<your-profile>
```

### 2. Wire up CMake

```cmake
find_package(fletcher-xrcedds-pubsub-provider REQUIRED)

# Fully qualified target name:
target_link_libraries(my_app PRIVATE
    fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

# Or the convenience alias injected by the package's build module:
target_link_libraries(my_app PRIVATE fletcher::xrcedds-pubsub-provider)
```

Micro XRCE-DDS Client and Micro-CDR are built from source as part of this package via CMake `FetchContent`; consumers do not need to declare them separately.

## CI pipeline

The build workflow is defined in `.github/workflows/ci.xrcedds-pubsub-provider.yml`.
It is `workflow_call`-only — invoked from `ci.pr.yml` for pull requests
touching `xrcedds-pubsub-provider/**` and from `cd.xrcedds-pubsub-provider.yml`
on `xrcedds-pubsub-provider-v*` tag pushes. The matching upload job
lives in `cd.xrcedds-pubsub-provider.yml`, not here.

```
ci.pr.yml (PRs) / cd.xrcedds-pubsub-provider.yml (tag push)
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
              (cd.xrcedds-pubsub-provider.yml job)
              Creates GitHub Release with
              fletcher-xrcedds-pubsub-provider-{windows,linux}-conan-package.tgz
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
`upload` job in `cd.xrcedds-pubsub-provider.yml` downloads both and
attaches them as GitHub Release assets:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

The `upload` job only runs from `cd.xrcedds-pubsub-provider.yml`
(tag push), and verifies that the tag version matches the version in
`conanfile.py` before creating the release.
