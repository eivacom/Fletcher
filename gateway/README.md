# gateway

Fletcher's WebSocket gateway server. A schema-agnostic byte router that exposes a Fletcher `Driver` over the network via WebSocket so non-C++ clients (TypeScript, browsers, anything else that can speak WS) can subscribe to topics and publish messages.

## What this directory ships

A single executable, `gateway`, built from the sources in `src/`. There is no public C++ API:

- **No installed headers.** All headers live under `src/` and are not part of any export interface.
- **Not a publishable Conan package.** The `conanfile.py` here exists only as a local build driver (dependency graph + CMake toolchain) — it has no `name` / `version`, so `conan create` is not a valid invocation. The gateway exe is distributed via GitHub Releases (`cd.gateway.yml` on `gateway-v*` tag pushes), not as a Conan package.
- **No library target exposed externally.** Sources compile into a tiny internal helper static library (`gateway_codec`, for unit-test linkage only) plus the exe. Neither is installed or consumed from outside this directory.

If you need to integrate with the gateway from another project, the only supported interface is the WebSocket protocol (see [gateway-client-ts](../gateway-client-ts/README.md) for the reference implementation).

## Schema-agnostic by design (with optional schema passthrough)

The gateway knows nothing semantically about topic schemas, and nothing about which topics will exist before clients show up:

- Topics are established implicitly. A client `subscribe` or `publish` creates the topic slot inside the in-process provider on the fly — there is no pre-declaration, no admin endpoint, no startup config that lists topics.
- The gateway never generates a schema itself. It does not inspect row bytes and does not understand their structure.
- **Passthrough only:** when a publisher attaches a `schema` to `create_topic`, the gateway caches it and forwards it to later subscribers via the `subscribed` response (`schema` + base64-encoded `schemaIpc` fields). When no publisher has announced a schema, those fields are simply absent and clients must know the schema another way — typically by generating a `SchemaDescriptor` from a `.proto` via `protoc-gen-fletcher`.

This keeps the gateway as a pure byte router: it forwards row bytes between publishers and subscribers without ever inspecting their structure, while still letting clients that need a schema discover one over the wire.

## Providers

The gateway routes between WebSocket clients and a pub/sub provider chosen with `--provider`:

- **`inprocess`** (default) — an in-process loopback that only connects WebSocket clients on the same gateway process. This is what the [gateway-end-to-end](../integration-tests/gateway-end-to-end/README.md) integration test exercises.
- **`fastdds`** — a [FastDDS](../fastdds-pubsub-provider/README.md)-backed provider that bridges the gateway to any FastDDS app on the same DDS domain (`--domain-id`). The gateway becomes a DDS subscriber/publisher on behalf of its WebSocket clients, so a TypeScript client can subscribe to — or publish to — data flowing over DDS. The end-to-end coverage lives in [integration-tests/gateway-fastdds-ts](../integration-tests/gateway-fastdds-ts/README.md).

Both providers are always compiled into the exe and the released binary; `--provider` selects between them at runtime.

### Configuring the provider

`--provider-config FILE` reads `FILE` and hands its contents to the selected provider as its configuration document. **The format is the provider's, not the gateway's** — the gateway does not parse it, validate it or know what it means; it reads the bytes and passes them on. That is why one flag serves every provider, including ones a later build adds.

- for **`fastdds`**, the document is a [Fast DDS XML QoS profiles document](../fastdds-pubsub-provider/README.md#qos-configuration). Reserved profile names are `fletcher_participant` (mandatory in a non-empty document), `fletcher_writer`, `fletcher_reader`, and a profile named after the `/`-joined topic for a per-topic override. Note that **a supplied profile is that endpoint's whole quality-of-service** — start from the published starting-point block rather than from a bare profile.
- for **`inprocess`**, it is `key=value` lines; the only key is `schema_carriage`.

Without the flag the document is empty and each provider uses its own defaults, which for `fastdds` is Fletcher's profile (`RELIABLE` + `KEEP_ALL` + `TRANSIENT_LOCAL`, with the resource limits and reader-side `data_sharing OFF` that profile carries). An unreadable `FILE` exits 2, as a bad `--provider` does; a document the provider rejects exits 2 with the provider's own message. An **empty or whitespace-only** `FILE` also exits 2, with its own message: passing the flag asks to be configured from that file, and every provider reads an empty document as "my own defaults", so accepting it would start a gateway that applies none of your intent and says nothing. Omit the flag if that is what you want.

## Installing

Pre-built archives are attached to each [`gateway-v*`](https://github.com/eivacom/Fletcher/releases?q=gateway) GitHub Release. Each archive holds the executable, the two [licence files](#licences-in-the-archive) a binary distribution has to carry — `LICENSE` and `THIRD-PARTY-LICENSES.txt` — and, in a shared build, the runtime libraries that belong beside the exe. Pick the asset for your platform:

**Windows** — download and unpack `gateway-windows.zip`:

```powershell
gh release download gateway-v0.1.0-alpha --repo eivacom/Fletcher --pattern gateway-windows.zip
Expand-Archive gateway-windows.zip -DestinationPath gateway
.\gateway\gateway.exe --port 9090
```

**Linux** — download and extract `gateway-linux.tar.gz`. The exec bit is preserved inside the tarball:

```bash
gh release download gateway-v0.1.0-alpha --repo eivacom/Fletcher --pattern gateway-linux.tar.gz
tar -xzf gateway-linux.tar.gz
./gateway --port 9090
```

To build from source instead of using a release binary, see [Building](#building) below.

### Licences in the archive

Both archives carry two licence files next to the executable. Keep both with the binary if you redistribute the gateway.

`LICENSE` is the gateway's own: the exe and the `fletcher-*` libraries inside it are LGPL-3.0-or-later, which asks for its text to accompany the binary. It is a verbatim copy of the [repository LICENSE](../LICENSE).

`THIRD-PARTY-LICENSES.txt` covers everything else linked in. The gateway pulls in a stack of permissively licensed libraries — boost (BSL-1.0), Fast DDS and Fast CDR (Apache-2.0), asio (BSL-1.0), foonathan_memory, tinyxml2 and zlib (Zlib), bzip2 (BSD-style), nlohmann_json (MIT) — and every one of those licences asks that its copyright notice and licence text be reproduced when the binary is redistributed. The default build is static, so those libraries are *inside* `gateway(.exe)` and there is nothing else in the archive for the texts to travel with; the file is how the release satisfies that condition.

**The file is generated, never hand-maintained.** [`deployers/third_party_licenses.py`](deployers/third_party_licenses.py) is a Conan deployer that reads the *resolved* dependency graph of the exact package being released and reproduces each dependency's own `licenses/` texts verbatim, under a header naming the package, its version, its declared licence and its homepage. Bump, add or drop a dependency and the notice follows on the next release with no second edit to remember. It covers the whole host graph, including packages the consumer never compiles against directly — asio reaches the gateway only through Fast DDS, but it is header-only, so its code really is in the binary and its licence really does have to ship.

If any third-party package in the graph contributes no licence text, the deployer raises and the build fails rather than publishing a notice that quietly omits a library.

`ci.gateway.yml` stages both files on both platforms on **every** run, not only on a tag push, so a dependency change the deployer cannot describe fails the pull request that introduces it instead of the release weeks later. On a tag push the same `dist/` is then filled by `runtime_deploy` and archived, and the workflow unpacks the finished archive and fails unless the exe and both licence files are in it — so a change to the archiving step cannot drop one silently either. To produce the notice locally:

```bash
cd gateway
```

```bash
conan install --requires="fletcher-gateway/[*, include_prerelease]" \
  --deployer=deployers/third_party_licenses.py --deployer-folder=dist \
  -c tools.graph:skip_binaries=False \
  -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

That resolves `fletcher-gateway` out of the local cache, so run it after the `conan create` chain in [Building](#building) has put the gateway and its dependencies there.

`-c tools.graph:skip_binaries=False` is not optional. A static application needs none of its dependencies at run time, so Conan marks their binaries `Skip` and leaves them with no package folder — which is exactly where the licence texts live. (It may also fetch a package that was skipped during the build and never downloaded, such as asio.) Without the conf the deployer finds nothing and says so.

## Running

```bash
gateway --port 9090 --bind-address 0.0.0.0
```

### CLI arguments

| Arg | Default | Purpose |
|---|---|---|
| `--port N` | `9090` | TCP port to listen on. |
| `--bind-address ADDR` | `0.0.0.0` | Interface to bind. Use `127.0.0.1` for loopback-only deployments. |
| `--provider TYPE` | `inprocess` | Pub/sub provider: `inprocess` (loopback between WebSocket clients) or `fastdds` (bridge to FastDDS apps on a DDS domain). Both are compiled in; the switch selects at runtime. |
| `--domain-id N` | `0` | DDS domain id for the `fastdds` provider. Ignored by `inprocess`. Always wins over a domain named in the provider document. |
| `--provider-config FILE` | — | Read `FILE` and hand its contents to the selected provider as its configuration document, in **that provider's** own format (Fast DDS XML QoS profiles for `fastdds`, `key=value` lines for `inprocess`). See [Configuring the provider](#configuring-the-provider). |
| `--version` | — | Print `fletcher-gateway <version>` and exit. The version string is read from `gateway/VERSION` at build time. |
| `--help`, `-h` | — | Print usage and exit. |

### Version metadata

`gateway/VERSION` is the single source of truth for the gateway version. CMake reads it at configure time and (a) feeds the numeric `MAJOR.MINOR.PATCH` prefix into `project(VERSION ...)`, (b) embeds the full string (including any pre-release suffix) as a `GATEWAY_VERSION_STRING` compile definition consumed by `--version`, and (c) on Windows generates a `version.rc` resource that the linker bakes into `gateway.exe`. Right-click `gateway.exe` → Properties → Details shows the same `FileVersion` / `ProductVersion` Windows users expect. The `cd.gateway.yml` workflow verifies that the pushed tag matches `gateway/VERSION` before creating the release.

### Process lifecycle

- Prints `READY <port>` on stdout once accepting connections. Launchers (tests, supervisors) can synchronise on that line without polling the socket.
- Reads stdin and exits cleanly on the literal line `stop`. Gives deterministic cross-platform shutdown — POSIX `SIGTERM` semantics differ from Windows.

## Building

Gateway depends on (resolved via Conan):

- `fletcher-core`
- `fletcher-pubsub`
- `fletcher-fastdds-pubsub-provider` (for the `fastdds` provider; pulls in Fast DDS)
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

This brings the `gateway` exe target into your build tree without exposing any of gateway's internal headers or library code. The integration test ([integration-tests/gateway-end-to-end/](../integration-tests/gateway-end-to-end/README.md)) uses this pattern.

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

This is the same path CI takes (`.github/workflows/ci.gateway.yml`) on both Linux and Windows, so the local run matches what gates the pull request.

## WebSocket protocol

The gateway exposes Fletcher's WebSocket protocol — text JSON control frames + binary data frames. See:

- [`gateway-client-ts/src/ws-protocol.ts`](../gateway-client-ts/src/ws-protocol.ts) for the canonical frame builders and parsers (TypeScript).
- [`gateway-client-ts/src/client.ts`](../gateway-client-ts/src/client.ts) for `FletcherClient`, a higher-level wrapper.
- [`integration-tests/gateway-end-to-end/`](../integration-tests/gateway-end-to-end/README.md) for end-to-end coverage of both the protocol and the binary frame layouts.

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

- Per-topic QoS is not configurable from the gateway CLI — the `fastdds` provider uses Fletcher's default QoS profile for every topic.
