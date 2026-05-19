# gateway

Fletcher's WebSocket gateway server. Distributed as a single executable that exposes a Fletcher `Driver` over the network via WebSocket so non-C++ clients (TypeScript, browsers, anything else that can speak WS) can subscribe to topics and publish messages.

## What this directory ships

A single executable, `gateway`, built from the sources in `src/`. There is no public C++ API:

- **No installed headers.** All headers live under `src/` and are not part of any export interface.
- **No Conan package.** The directory has no `conanfile.py`. You either build the exe standalone or pull `gateway/CMakeLists.txt` into a parent project via `add_subdirectory()`.
- **No library target.** Sources compile straight into the executable.

If you need to integrate with the gateway from another project, the only supported interface is the WebSocket protocol (see [gateway-client-ts](../gateway-client-ts/) for the reference implementation).

## Provider note (current limitation)

`gateway` currently always uses an in-process loopback provider. There is no DDS-backed provider yet, so the practical use today is the end-to-end integration test ([integration-tests/gateway-end-to-end](../integration-tests/gateway-end-to-end/)). Once a real provider exists this same exe will gain a `--provider TYPE` switch — the rest of the CLI is designed to stay stable.

## Running

```bash
gateway --port 9090 --bind-address 0.0.0.0 --config gateway.yml
```

### CLI arguments

| Arg | Default | Purpose |
|---|---|---|
| `--port N` | `9090` | TCP port to listen on. |
| `--bind-address ADDR` | `0.0.0.0` | Interface to bind. Use `127.0.0.1` for loopback-only deployments. |
| `--config FILE.yml` | _(none)_ | YAML file pre-declaring topics and their schemas (see below). Without it, the gateway starts with no topics; clients are expected to create them via the WebSocket protocol once that capability exists. |
| `--help`, `-h` | — | Print usage and exit. |

### Process lifecycle

- Prints `READY <port>` on stdout once accepting connections. Launchers (tests, supervisors) can synchronise on that line without polling the socket.
- Reads stdin and exits cleanly on the literal line `stop`. Gives deterministic cross-platform shutdown — POSIX `SIGTERM` semantics differ from Windows.

## Config file format (`--config FILE.yml`)

The YAML file pre-declares topics and their schemas at startup. Example:

```yaml
topics:
  - name: telemetry
    fields:
      - name: sensor_id
        type: int32
      - name: temperature
        type: float64
      - name: label
        type: utf8

  - name: status/heartbeat
    fields:
      - name: tick
        type: uint64
      - name: alive
        type: bool
```

Topic names may contain `/` to nest them — they're split internally into the segment vector that `Driver::CreateTopic` expects.

### Supported field types

| Config string | Maps to |
|---|---|
| `bool`, `boolean` | `NANOARROW_TYPE_BOOL` |
| `int8`, `int16`, `int32`, `int64` | signed integer types |
| `uint8`, `uint16`, `uint32`, `uint64` | unsigned integer types |
| `float32`, `float` | `NANOARROW_TYPE_FLOAT` (32-bit) |
| `float64`, `double` | `NANOARROW_TYPE_DOUBLE` (64-bit) |
| `utf8`, `string` | `NANOARROW_TYPE_STRING` |
| `binary` | `NANOARROW_TYPE_BINARY` |

Nested structs and lists are not yet supported in the config format; once a use case lands, the parser will be extended.

## Building

Gateway depends on (resolved via Conan):

- `eiva-fletcher-core`
- `eiva-fletcher-pubsub`
- `boost` (Beast + Asio, header-only)
- `nlohmann_json`
- `yaml-cpp`

### Inside the devcontainer

```bash
cd gateway
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

The resulting binary is at `build/Release/gateway` (`gateway.exe` on Windows).

### As a build artefact for another CMake project

```cmake
add_subdirectory(path/to/gateway path/to/build/gateway_build)
```

This brings the `gateway` exe target into your build tree without exposing any of gateway's internal headers or library code. The integration test ([integration-tests/gateway-end-to-end/](../integration-tests/gateway-end-to-end/)) uses this pattern.

## WebSocket protocol

The gateway exposes Fletcher's WebSocket protocol — text JSON control frames + binary data frames. See:

- [`gateway-client-ts/src/ws-protocol.ts`](../gateway-client-ts/src/ws-protocol.ts) for the canonical frame builders and parsers (TypeScript).
- [`gateway-client-ts/src/client.ts`](../gateway-client-ts/src/client.ts) for `FletcherClient`, a higher-level wrapper.
- [`integration-tests/gateway-end-to-end/`](../integration-tests/gateway-end-to-end/) for end-to-end coverage of both the protocol and the binary frame layouts.

Frame layouts at a glance:

```
text frames (client → server):
  {"action":"create_topic","topic":"foo"}
  {"action":"subscribe","topic":"foo"}
  {"action":"unsubscribe","subId":"<bigint string>"}
  {"action":"list_topics"}

text frames (server → client):
  {"type":"subscribed","subId":"...","topic":"...","schema":{...},"schemaIpc":"<base64>"}
  {"type":"topic_created"|"unsubscribed"|"published"|"topics_list"|"error", ...}

binary frames:
  client → server (PUBLISH): [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE :rest]
  server → client (MESSAGE): [SUB_ID :8 LE][ENVELOPE :rest]
```

`ENVELOPE` is Fletcher's `[ROW_LEN :4][ROW_DATA][ATTACH_COUNT :4][attachments...]` envelope format from `core/envelope.hpp`.

## Tracked gaps

- Gateway has no `tests/` (unit tests in the gtest sense). Tracked separately as **DevOps US 17240 / GitHub #45**.
- No DDS-backed provider yet — the in-process loopback is the only option. Will gain a `--provider TYPE` selector when a real provider lands.
