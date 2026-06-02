# Integration test — pubsub-arrow + fastdds-pubsub-provider

End-to-end test that verifies the `PubSubArrow` Arrow C++ adapter actually works when driven on top of a real `FastDDSPubSubProvider`:

- `fletcher-pubsub-arrow`'s `PubSubArrow` wraps a `PubSub` provider, accepting `ArrowRow` values + `arrow::Schema` and translating to/from the nanoarrow wire format.
- `fletcher-fastdds-pubsub-provider`'s `FastDDSPubSubProvider` is the production DDS transport.

Each component has unit tests against a mock counterpart. This test is the proof that the seam between them holds — that `PubSubArrow.Publish(row)` on one side actually causes `PubSubArrow.Subscribe(callback)` on the other side to fire with a matching `ArrowRow`, including correct schema delivery via the companion `__schema` DDS topic.

## What it covers

For each test case:

- A `PubSubArrow` publisher and a `PubSubArrow` subscriber are constructed in the same process but on **separate `DomainParticipant` instances** (same DDS domain), so DDS discovery + RELIABLE + TRANSIENT_LOCAL is actually exercised — not in-process pointer passing.
- The publisher creates the topic with an `arrow::Schema` and publishes one or more `ArrowRow` instances.
- The subscriber's `Subscribe()` returns a schema; the test asserts it `Equals` the one the publisher created.
- The callback receives decoded `ArrowRow` values; the test asserts each row's scalar values match what was published.

Current scenarios:

| Test | What it verifies |
|---|---|
| `SchemaAndRowDeliveredAcrossDdsBoundary` | One row, three scalar fields (int32, double, utf8). Schema equality + decoded values. |
| `MultipleRowsDeliveredInOrder` | Five rows published back-to-back, all received in order by the subscriber. |

## How it runs in CI

The workflow `.github/workflows/ci.integration-test.pubsub-arrow-fastdds.yml` triggers when any of `core/**`, `arrow-bridge/**`, `pubsub/**`, `pubsub-arrow/**`, `fastdds-pubsub-provider/**`, or this directory changes on a PR. It:

1. Builds each component locally via `conan create <component>/.`, putting the branch's in-flight versions in the Conan cache.
2. `conan install` + `cmake build` produces the `integration_tests` gtest binary.
3. `ctest --output-on-failure` runs the suite. The docker container uses `--network host` so DDS multicast discovery works.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Conan profiles live in [`.conan-profiles/`](../../.conan-profiles) and are referenced by relative path — no profile-install step is needed. The integration test builds every component from this branch's source into the local Conan cache, and `conan install` resolves against that cache.

### Build the components from this branch into the local cache

```bash
cd /workspaces/Fletcher
```

```bash
conan create core/.                    --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create arrow-bridge/.            --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create pubsub/.                  --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create pubsub-arrow/.            --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan create fastdds-pubsub-provider/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
```

### Build and run the integration test

```bash
cd /workspaces/Fletcher/integration-tests/pubsub-arrow-fastdds
```

```bash
conan build . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
```

`conan build` runs the conanfile's `build()` method which executes configure + cmake build + ctest in one go.

### Iterating after a component change

Re-run only the `conan create` for the component you touched, then re-run `conan build .` for the integration test. CMake handles incremental rebuilds; Conan picks up the latest component version from the local cache automatically.
