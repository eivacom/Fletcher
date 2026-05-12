# Integration test — schema transport edge cases

End-to-end test covering the timing-sensitive corners of Fletcher's `/__schema` companion-topic mechanism. The per-component unit tests cover the happy path; this test covers the cases where publishers and subscribers don't line up in the obvious order, but the documented QoS (RELIABLE + TRANSIENT_LOCAL + KEEP_LAST(1) on `/__schema`, RELIABLE + TRANSIENT_LOCAL + KEEP_ALL on the data topic) is supposed to bridge the gap.

Implements US #17021 (Azure DevOps) / GitHub issue [#31](https://github.com/eivacom/Fletcher/issues/31).

## What it covers

Each test runs against a real `FastDDSPubSubProvider` on its own DDS domain (no mocks, no in-process shortcuts) so the actual DDS discovery + durability machinery is exercised.

| Test | What it verifies |
|---|---|
| `SubscriberJoiningAfterPublishStillGetsSchemaAndData` | Publisher creates the topic and publishes a row *before* the subscriber connects. TRANSIENT_LOCAL on `/__schema` delivers the schema to the late joiner; TRANSIENT_LOCAL on the data topic delivers the retained row. |
| `SubscribeWaitsUntilPublisherCreatesTopic` | Subscriber calls `Subscribe` *before* the publisher has created the topic. Subscribe internally polls `/__schema` for up to 5 s and must return successfully once `CreateTopic` finally fires. |
| `NewPublisherCanResumeSchemaForLateSubscribers` | Original publisher creates the topic, publishes a row, then is destroyed (provider + DataWriter + TRANSIENT_LOCAL history gone). A new publisher takes over on the same DDS domain with the same topic + schema. A subscriber joining after the takeover gets the schema from the new publisher and receives its row. |

## Cross-provider scenarios

Out of scope for this PR — the cross-provider versions of these scenarios depend on the XRCE Agent infrastructure tracked in US #17019 and overlap with that test plan.

## How it runs in CI

The workflow `.github/workflows/integration-test.schema-transport.yml` triggers on PRs touching `core/**`, `pubsub/**`, `pubsub-arrow/**`, `arrow-bridge/**`, `fastdds-pubsub-provider/**`, or this directory. It:

1. Builds each component locally via `conan create <component>/.`, putting the branch's in-flight versions in the Conan cache.
2. `conan install` + `cmake build` produces the `integration_tests` gtest binary.
3. `ctest --output-on-failure` runs the suite. The docker container uses `--network host` so DDS multicast discovery works.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). The `postCreateCommand` runs `conan config install` automatically — no `conan-eiva` login is needed because the integration test builds every component from this branch's source into the local Conan cache, and `conan install` resolves against that cache.

### Build the components from this branch into the local cache

```bash
cd /workspaces/Fletcher
```

```bash
conan create core/.                    --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create arrow-bridge/.            --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create pubsub/.                  --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create pubsub-arrow/.            --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create fastdds-pubsub-provider/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

### Build the integration test

```bash
cd /workspaces/Fletcher/integration-tests/schema-transport
```

```bash
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
cmake --preset conan-release
```

```bash
cmake --build --preset conan-release
```

### Run the tests

```bash
ctest --preset conan-release --output-on-failure
```

### Iterating after a component change

Re-run only the `conan create` for the component you touched, then redo `conan install` + `cmake --build` for the integration test. Conan picks up the latest version in the local cache automatically.
