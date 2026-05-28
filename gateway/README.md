# gateway

Fletcher's WebSocket gateway server. A schema-agnostic byte router that exposes a Fletcher `Driver` over the network via WebSocket so non-C++ clients (TypeScript, browsers, anything else that can speak WS) can subscribe to topics and publish messages.

## What this directory ships

A single executable, `gateway`, built from the sources in `src/`. There is no public C++ API:

- **No installed headers.** All headers live under `src/` and are not part of any export interface.
- **Not a publishable Conan package.** The `conanfile.py` here exists only as a local build driver (dependency graph + CMake toolchain) — it has no `name` / `version`, so `conan create` is not a valid invocation. The gateway exe is distributed via GitHub Releases (`release-gateway.yml` on `gateway-v*` tag pushes), not as a Conan package.
- **No library target exposed externally.** Sources compile into a tiny internal helper static library (`gateway_codec`, for unit-test linkage only) plus the exe. Neither is installed or consumed from outside this directory.

If you need to integrate with the gateway from another project, the only supported interface is the WebSocket protocol (see [gateway-client-ts](../gateway-client-ts/) for the reference implementation).

## Schema-agnostic by design (with optional schema passthrough)

The gateway knows nothing semantically about topic schemas, and nothing about which topics will exist before clients show up:

- Topics are established implicitly. A client `subscribe` or `publish` creates the topic slot inside the in-process provider on the fly — there is no pre-declaration, no admin endpoint, no startup config that lists topics.
- The gateway never generates a schema itself. It does not inspect row bytes and does not understand their structure.
- **Passthrough only:** when a publisher attaches a `schema` to `create_topic`, the gateway caches it and forwards it to later subscribers via the `subscribed` response (`schema` + base64-encoded `schemaIpc` fields). When no publisher has announced a schema, those fields are simply absent and clients must know the schema another way — typically by generating a `SchemaDescriptor` from a `.proto` via `protoc-gen-fletcher`.

This keeps the gateway as a pure byte router: it forwards row bytes between publishers and subscribers without ever inspecting their structure, while still letting clients that need a schema discover one over the wire.

## Provider note (current limitation)

`gateway` currently always uses an in-process loopback provider. There is no DDS-backed provider yet, so the practical use today is the end-to-end integration test ([integration-tests/gateway-end-to-end](../integration-tests/gateway-end-to-end/)). Once a real provider exists this same exe will gain a `--provider TYPE` switch — the rest of the CLI is designed to stay stable.

## Running

```bash
gateway --port 9090 --bind-address 0.0.0.0
```

### CLI arguments

| Arg | Default | Purpose |
|---|---|---|
| `--port N` | `9090` | TCP port to listen on. |
| `--bind-address ADDR` | `0.0.0.0` | Interface to bind. Use `127.0.0.1` for loopback-only deployments. |
| `--help`, `-h` | — | Print usage and exit. |

### Process lifecycle

- Prints `READY <port>` on stdout once accepting connections. Launchers (tests, supervisors) can synchronise on that line without polling the socket.
- Reads stdin and exits cleanly on the literal line `stop`. Gives deterministic cross-platform shutdown — POSIX `SIGTERM` semantics differ from Windows.

## Building

Gateway depends on (resolved via Conan):

- `fletcher-core`
- `fletcher-pubsub`
- `boost` (Beast + Asio, header-only)
- `nlohmann_json`

### Inside the devcontainer

```bash
cd gateway
```

```bash
conan install . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan build . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

`conan build .` runs the recipe's `build()` method, which dispatches the right cmake configure / build invocations for the generator in use. On Linux (single-config Ninja/Make) it resolves to `cmake --preset conan-release && cmake --build --preset conan-release`; on Windows (multi-config Visual Studio) it picks `conan-default` for configure and `conan-release` for build. Letting Conan dispatch avoids the platform-specific preset asymmetry.

The resulting binary is at `build/Release/gateway` (`gateway.exe` on Windows).

### As a build artefact for another CMake project

```cmake
add_subdirectory(path/to/gateway path/to/build/gateway_build)
```

This brings the `gateway` exe target into your build tree without exposing any of gateway's internal headers or library code. The integration test ([integration-tests/gateway-end-to-end/](../integration-tests/gateway-end-to-end/)) uses this pattern.

## Unit tests

`gateway/tests/` holds gtest cases for the pure helpers that handle untrusted input from WebSocket clients — `ParsePublishFrame` (binary frame bounds-checking) and `BuildArrowSchemaFromJson` (JSON schema validation). Everything else in the gateway is covered by the end-to-end integration test against a real `FletcherClient`.

To build and run them in the devcontainer:

```bash
cd gateway
```

```bash
conan install . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

```bash
conan build . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

The `-o "&:run_tests=True"` switch pulls `gtest` into the dependency graph, sets `FLETCHER_BUILD_TESTS=ON` for CMake, and makes the recipe's `build()` invoke `cmake.test()` after the build — so a successful `conan build` means the gtest suite already ran. Omit the switch for a tests-off plain build.

This is the same path CI takes (`.github/workflows/fletcher-gateway.yml`) on both Linux and Windows, so the local run matches what gates the pull request.

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
  {"type":"subscribed","subId":"...","topic":"..."}
  {"type":"topic_created"|"unsubscribed"|"published"|"topics_list"|"error", ...}

binary frames:
  client → server (PUBLISH): [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE :rest]
  server → client (MESSAGE): [SUB_ID :8 LE][ENVELOPE :rest]
```

`ENVELOPE` is Fletcher's `[ROW_LEN :4][ROW_DATA][ATTACH_COUNT :4][attachments...]` envelope format from `core/envelope.hpp`.

## Tracked gaps

- No DDS-backed provider yet — the in-process loopback is the only option. Will gain a `--provider TYPE` selector when a real provider lands.
