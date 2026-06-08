# gateway-fastdds-ts integration test

End-to-end test of the gateway's **FastDDS provider**: a C++ FastDDS
application and a TypeScript `gateway-client-ts` client both pub/sub through
the gateway (started with `--provider fastdds`), in **both** directions.

This is the test that proves the gateway can bridge a real FastDDS app to a
WebSocket/TypeScript client — the pure C++↔C++ (DDS) and TS↔TS (loopback)
paths are already covered by `pubsub-arrow-fastdds` and `gateway-end-to-end`.

## Topology

```
  C++ FastDDS peer  ──DDS(domain 142)──►  gateway --provider fastdds  ──WS──►  TS client
  (fastdds_peer)    ◄──DDS(domain 142)──  (FastDDS DataReader/Writer)  ◄──WS──  (FletcherClient)
```

Both ends use protoc-gen-fletcher generated types from a single
`proto/sensor_reading.proto`, which declares two client-streaming service
methods — one per direction:

| Topic (`SensorFeed.*`) | Publisher | Subscriber | What it proves |
|---|---|---|---|
| `CppToTs` | C++ peer (`SensorFeed_CppToTsPublisher`) | TS client | C++ publish + TS subscribe |
| `TsToCpp` | TS client | C++ peer (`SensorFeed_TsToCppSubscriber`) | TS publish + C++ subscribe |

The C++ peer (`src/fastdds_peer.cpp`) publishes a known set of rows on
`CppToTs` at startup and echoes every `TsToCpp` row it receives to stdout as a
`RECV ...` line. The vitest suite (`test/gateway-fastdds.test.ts`) drives the
TS side and asserts both directions, plus that the publisher-announced schema
propagates the whole chain (FastDDS `__schema` → gateway → WebSocket
`subscribed` response). A fourth case covers **TS subscriber-first**: a TS
client subscribes to a fresh topic through the gateway before any publisher
exists, then a second client declares the topic and publishes — and the first
client receives it (the subscribe path is non-blocking and the late publisher
is matched).

QoS is Fletcher's default profile (`RELIABLE` + `KEEP_ALL` + `TRANSIENT_LOCAL`)
on both sides, so a late-joining reader still receives previously published
rows.

## Layout

```
proto/sensor_reading.proto   message + two service/stream rpcs
src/fastdds_peer.cpp          C++ FastDDS peer (generated Publisher + Subscriber)
test/gateway-fastdds.test.ts  vitest orchestration + assertions
CMakeLists.txt                builds the gateway exe + fastdds_peer; runs codegen
conanfile.py                  resolves deps, writes CMake toolchain
generated-ts/                 protoc-gen-fletcher TS output (gitignored)
```

## Running locally (devcontainer / Linux profile)

First, put the components in the local Conan cache (from the repo root):

```bash
for c in core pubsub protoc fastdds-pubsub-provider; do (cd "$c" && conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release); done
```

Then build the gateway + `fastdds_peer` and generate the TS artefacts:

```bash
cd integration-tests/gateway-fastdds-ts
```

```bash
conan install . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
```

```bash
conan build . -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
```

Install the JS deps:

```bash
npm ci
```

The gateway links Fast DDS as a shared library, so the suite must run with the
Conan runtime environment active (it sets `LD_LIBRARY_PATH`) — otherwise the
spawned gateway fails to load `libfastcdr`:

```bash
source build/Release/generators/conanrun.sh && npm test
```

Gateway and peer discover each other over Fast DDS's shared-memory / localhost
transport, so within a single host (or a single container) the test works out
of the box — no `--network host` needed.

### Overrides

| Env var | Default | Purpose |
|---|---|---|
| `TEST_PORT` | `19093` | Gateway WebSocket port. |
| `DDS_DOMAIN_ID` | `142` | DDS domain shared by the gateway and the peer. |
| `GATEWAY_BIN` | (build tree) | Path to the gateway binary. |
| `FASTDDS_PEER_BIN` | (build tree) | Path to the `fastdds_peer` binary. |
