# Integration test — fastdds-pubsub-provider <-> xrcedds-pubsub-provider interop

End-to-end test that verifies an `fletcher-xrcedds-pubsub-provider` instance and an `fletcher-fastdds-pubsub-provider` instance can exchange data via a `MicroXRCEAgent`. This is the deployment scenario from Fletcher's architecture: an MCU speaks XRCE-DDS over UDP to an Agent, which bridges into the full DDS network where a vessel workstation runs FastDDS.

## Why this test exists

Neither provider's unit tests exercise the bridged path:

- `fastdds-pubsub-provider` unit tests run two `FastDDSPubSubProvider` instances against each other on the full DDS bus — no XRCE in the picture.
- `xrcedds-pubsub-provider` unit tests deliberately *do not* require a running Agent (they exercise config, encoding, and constructor-throws-without-Agent behaviour).
- A wire-format byte-equality unit test (`XrceProviderTest::EnvelopeXrceUsesSameWireFormatAsFastDds`) proves the two providers' envelope encodings agree on bytes, but only at the encoder level — not over the wire.

This is the only test that proves the Agent bridge actually delivers Fletcher's envelopes intact and that the `/__schema` companion topic survives translation.

## What it covers

Both directions of the bridge on a single shared DDS domain (different topic names + different XRCE session keys keep the two tests isolated):

| Test | Direction | What it verifies |
|---|---|---|
| `XrcePublishReachesFastDDSSubscriber` | XRCE → Agent → FastDDS | Topic naming matches across providers; envelope bytes survive XRCE→DDS translation; schema reaches the FastDDS subscriber via `/__schema`. |
| `FastDDSPublishReachesXrceSubscriber` | FastDDS → Agent → XRCE | Symmetric to above. Exercises `XrceDDSPubSubProvider::Subscribe` — a structurally different code path from FastDDS's subscribe (global `on_topic` callback demultiplexed by reader object id). |

## Self-contained: the Agent is built and managed by the test

`MicroXRCEAgent` is built from source by this directory's `CMakeLists.txt` as an `ExternalProject` (with `UAGENT_SUPERBUILD=ON` so the Agent fetches its own fast-dds / fast-cdr / asio / tinyxml2 / micro-cdr in isolation from the Conan deps the test itself uses). The resulting binary path is injected as `MICRO_XRCE_AGENT_PATH` into the test binary.

A gtest `Environment` fixture spawns the Agent (`fork`+`execv` on Linux, `CreateProcess` on Windows) before any test runs and kills it on tear-down. The fixture polls until the Agent's UDP port is reachable, with a 15-second deadline so it tolerates slow start-up under CI load.

No `MicroXRCEAgent` install, no Docker sidecar, no manual `&`-in-another-terminal step. `cmake build && ctest` is the whole workflow on every platform.

The first build pulls in eProsima's full dependency tree (fast-dds + fast-cdr + asio + tinyxml2 + micro-cdr + the Agent itself) and takes ~10–15 minutes. Subsequent rebuilds reuse the CMake build directory's cache.

## How it runs in CI

The workflow `.github/workflows/ci.integration-test.fastdds-xrce-interop.yml` triggers on PRs touching `core/**`, `arrow-bridge/**`, `pubsub/**`, `pubsub-arrow/**`, `fastdds-pubsub-provider/**`, `xrcedds-pubsub-provider/**`, or this directory. It builds each Fletcher component, then builds and runs the test inside the devcontainer image. The Agent is built and torn down by the test fixture — the workflow does nothing special for it.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Conan profiles live in [`.conan-profiles/`](../../.conan-profiles) and are referenced by relative path — no profile-install step is needed. The integration test builds every component from this branch's source into the local Conan cache, and the `conan build .` step resolves against that cache.

### Build the components from this branch into the local cache

From the repo root inside the devcontainer (Linux) or a Windows shell (substitute the Windows profile):

```bash
conan create core/.                     --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create arrow-bridge/.             --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create pubsub/.                   --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create pubsub-arrow/.             --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create fastdds-pubsub-provider/.  --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create xrcedds-pubsub-provider/.  --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

### Build and run the integration test (and the Agent)

```bash
cd integration-tests/fastdds-xrce-interop
```

```bash
conan build . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
```

`conan build .` configures, builds the test binary (this is where the Agent gets compiled — a one-time cost on a clean build dir), and runs the suite via ctest. Conan drives the configure/build/test preset that matches the active generator; the test binary spawns the Agent itself, so nothing else needs to be running.

### Iterating after a component change

Re-run only the `conan create` for the component you touched, then redo `conan build .` for the integration test. The Agent ExternalProject is incremental — it doesn't rebuild unless you wipe the build directory.
