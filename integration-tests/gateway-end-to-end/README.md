# Integration test — gateway + gateway-client-ts end-to-end WebSocket

End-to-end test that verifies the C++ `gateway/` (WebSocket server) and the TypeScript `gateway-client-ts/` (`FletcherClient`) agree on every byte and every JSON key across a real WebSocket connection. The two halves have unit tests with their own mocks (`MockWebSocket` on the client, mock pubsub `Driver` on the server) — this is the only place that proves the two halves actually interoperate.

## What it covers

Both directions of the protocol plus the binary frame layouts documented in `gateway/src/gateway.hpp`:

| Test | What it verifies |
|---|---|
| `subscribed response — schema delivery` | The `subscribed` text frame carries both `schema` (SchemaDescriptor JSON, populated from the YAML config's schema) and `schemaIpc` (base64-encoded Arrow IPC bytes). Field types are checked against the wire-type IDs. |
| `client publish ↔ subscription round-trip` | A single `FletcherClient` subscribes to `telemetry`, publishes three distinct rows, and expects all three to come back via the subscription. Exercises both directions of the WebSocket protocol in one test through the gateway's in-process loopback provider. |
| `server -> client MESSAGE frame is [SUB_ID :8 LE][ENVELOPE]` | Subscribes on a raw `WebSocket`, then publishes on the same socket to trigger a loopback delivery; asserts the binary frame layout byte-for-byte. |
| `client -> server PUBLISH frame is [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE]` | Built with `buildPublish` from the TS protocol module; raw bytes asserted against the documented format. |

## Server binary

The test spawns the production `gateway` exe built from `gateway/src/` by `gateway/CMakeLists.txt`. This directory's `CMakeLists.txt` pulls the gateway build tree in via `add_subdirectory("../../gateway")` so the same source compiles whether built via the integration test or directly from `gateway/`. The gateway has no public C++ API (no installed headers, no Conan recipe) — the only supported integration point is the WebSocket protocol.

The vitest test spawns the resulting exe with `child_process.spawn`, waits for it to print `READY <port>` on stdout, drives the WebSocket protocol against it, and shuts it down by writing `stop\n` to its stdin (deterministic cross-platform shutdown — Windows SIGTERM semantics differ from POSIX).

The exe is configured via [`test-config.yml`](test-config.yml), which pre-creates a single `telemetry` topic with the three-field schema (`sensor_id : int32`, `temperature : float64`, `label : utf8`) — enough to exercise the null bitfield, fixed-width, and variable-length encodings in one shot. The CLI args used at spawn time:

| Arg | Value | Purpose |
|---|---|---|
| `--port N` | `19091` | TCP port; high number to minimise collision with system services. |
| `--bind-address ADDR` | `127.0.0.1` | Loopback only. |
| `--config FILE.yml` | `test-config.yml` | Topics + schemas to pre-create. |

The exe is production-grade — no test-specific behaviour baked in. The client-publish ⇄ self-subscription round-trip via the gateway's in-process loopback provider is what wires the "publishes from a client are delivered to subscribers (including the same client) over the WebSocket bus" assertion. A real DDS-backed provider for `gateway` is tracked separately; once it exists this same exe will gain a `--provider TYPE` switch.

## How it runs in CI

The workflow `.github/workflows/integration-test.gateway-end-to-end.yml` triggers on PRs touching `core/`, `pubsub/`, `gateway/`, `gateway-client-ts/`, this directory, or its workflow file. It:

1. Builds the required Fletcher components (`core`, `pubsub`, `protoc`) via `conan create <component>/.` into the local Conan cache.
2. Runs `conan install` + `cmake --preset` + `cmake --build` in this directory to produce `build/Release/gateway_build/gateway`.
3. Runs `npm ci` + `npm test` to execute the vitest suite against the binary.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) for how to open the devcontainer. From inside it, from the repo root:

```bash
conan create core/.   --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create pubsub/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create protoc/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

`protoc` is needed because one of the test cases drives the gateway with the `TelemetrySchema` class that `protoc-gen-fletcher` generates from [`proto/telemetry.proto`](proto/telemetry.proto); the CMake build runs the plugin on every reconfigure.

Then build and run the integration test itself:

```bash
cd integration-tests/gateway-end-to-end
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

```bash
npm ci
```

```bash
npm test
```

The vitest suite spawns `build/Release/gateway_build/gateway` with `--port`, `--bind-address`, and `--config test-config.yml`, waits for `READY <port>`, runs the four scenarios, and tears the gateway down via `stop\n` on stdin.

### Overriding the binary location

The test resolves the `gateway` binary from `build/Release/gateway_build/` by default. To point it at a different build, set:

```bash
GATEWAY_BIN=/path/to/gateway npm test
```
