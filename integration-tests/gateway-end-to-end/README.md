# Integration test — gateway + gateway-client-ts end-to-end WebSocket

End-to-end test that verifies the C++ `gateway/` (WebSocket server) and the TypeScript `gateway-client-ts/` (`FletcherClient`) agree on every byte and every JSON key across a real WebSocket connection. The two halves have unit tests with their own mocks (`MockWebSocket` on the client, mock pubsub `Driver` on the server) — this is the only place that proves the two halves actually interoperate.

## What it covers

Both directions of the protocol plus the binary frame layout that the wire-level documentation in `gateway/include/web_gateway/web_gateway.hpp` describes:

| Test | What it verifies |
|---|---|
| `subscribed response — schema delivery` | The `subscribed` text frame carries both `schema` (SchemaDescriptor JSON, populated from the server-side Arrow schema) and `schemaIpc` (base64-encoded Arrow IPC bytes). Field types are checked against the wire-type IDs. |
| `publish-from-server -> client delivery` | The server publishes a heartbeat row every 50 ms via its in-process provider. A `FletcherClient` subscribes and receives the rows. Decoded values match the server's encoder. |
| `client publish -> server delivery` | The client subscribes to `"echo"` and then publishes a row to the same topic. Because the in-process provider routes `Publish()` to the registered callback, the row comes back over the WebSocket — proving the client→server path. |
| `server -> client MESSAGE frame is [SUB_ID :8 LE][ENVELOPE]` | Raw binary inspection at the WebSocket layer asserts the exact byte layout. |
| `client -> server PUBLISH frame is [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE]` | Built with `buildPublish` from the TS protocol module; raw bytes asserted against the documented format. |

## Server binary lives in `gateway/tests/`

The fixture exe is `gateway-test-server`, built from `gateway/tests/server_main.cpp` by `gateway/CMakeLists.txt`. This directory's `CMakeLists.txt` pulls the gateway build tree in via `add_subdirectory("../../gateway")`, so the same source produces the exe regardless of whether it is built via the integration test or directly from `gateway/`.

The vitest test spawns the resulting exe with `child_process.spawn`, waits for it to print `READY <port>` on stdout, drives the WebSocket protocol against it, and shuts it down by writing `stop\n` to its stdin (deterministic cross-platform shutdown — Windows SIGTERM semantics differ from POSIX).

The exe takes three CLI args:

| Arg | Default | Purpose |
|---|---|---|
| `--port N` | `9091` | TCP port on the bind address. |
| `--heartbeat-ms N` | `100` | Milliseconds between heartbeat publishes (0 disables). |
| `--bind-address ADDR` | `127.0.0.1` | Bind address; loopback by default. |

It pre-creates two topics (`heartbeat` for the server-publish path, `echo` for the client-publish path) with a three-field schema (`sensor_id : int32`, `temperature : float64`, `label : utf8`) — enough to exercise null bitfield, fixed-width, and variable-length encodings in one shot.

`gateway-test-server` is **not** a production binary. `gateway/` has no real DDS-backed provider yet; once one exists this fixture should be replaced by a real `gateway` binary that takes a provider configuration. Tracked as gap.

## How it runs in CI

The workflow `.github/workflows/integration-test.gateway-end-to-end.yml` triggers on PRs touching `core/`, `pubsub/`, `gateway/`, `gateway-client-ts/`, this directory, or its workflow file. It:

1. Builds the required Fletcher components (`core`, `pubsub`) via `conan create <component>/.` into the local Conan cache.
2. Runs `conan install` + `cmake --preset` + `cmake --build` in this directory to produce `build/Release/test_server`.
3. Runs `npm ci` + `npm test` to execute the vitest suite against the binary.

## Running locally

See the repo root's [Development environment](../../README.md#development-environment) for how to open the devcontainer. From inside it, from the repo root:

```bash
conan create core/.   --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

```bash
conan create pubsub/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

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

The vitest suite spawns `build/Release/test_server`, waits for `READY <port>`, runs the four scenarios, and tears the server down via `stop\n` on stdin.

### Overriding the binary location

The test resolves `test_server` from `build/Release/` by default. To point it at a different build, set:

```bash
TEST_SERVER_BIN=/path/to/test_server npm test
```
