# gateway-dotnet-crosslang integration test

The C# gateway client's rows of the cross-language pub/sub matrix (BIND-8d, D-BIND-116): the
generated C# **gateway** pair exchanges rows, in both directions, with a C++ native pair, a C#
native pair and a TypeScript gateway client, through one gateway over Fast DDS. Every row is
compared field by field on the receiving side.

## Topology

```
  C++ peer (native pair)  ──DDS──┐
                                 ├──  gateway --provider fastdds  ──WS──  C# gateway client (the test)
  C# native peer (shim)   ──DDS──┘                                ──WS──  TypeScript peer
```

The test process is the C# gateway client; the gateway and the three peers are child
processes it starts once (`CrossLangLane`) and stops after the suite.

| Test | Topic (`CrossLang.*`) | Publisher | Subscriber, which compares |
|---|---|---|---|
| `CSharpGatewayToCpp` | `CsToCpp` | C# gateway pair | C++ native pair |
| `CppToCSharpGateway` | `CppToCs` | C++ native pair | C# gateway pair |
| `CSharpGatewayToCSharpNative` | `CsToNative` | C# gateway pair | C# native pair |
| `CSharpNativeToCSharpGateway` | `NativeToCs` | C# native pair | C# gateway pair |
| `CSharpGatewayToTypeScript` | `CsToTs` | C# gateway pair | TypeScript client |
| `TypeScriptToCSharpGateway` | `TsToCs` | TypeScript client | C# gateway pair |

## One `.proto`, one fixture

`proto/crosslang.proto` streams `CrossRow { int32 seq; CompositeCoverage row; }`, where
`CompositeCoverage` is `protoc-coverage`'s message that reaches every mapped shape (every scalar
kind, optional and wrapped scalars, Timestamp and Duration, enums, nested and optional messages,
lists of scalars, bytes and structs, maps of scalars and structs in entry order, flattened lists to
depth 3, flattened messages). A local wrapper, because every backend skips a pub/sub method whose
message is declared in another file. Each direction sends the four `CompositeCoverage` fixture rows
the protoc-coverage goldens pin, numbered by `seq`; each receiver compares a row with its OWN
transcription of the fixture row `seq` names:

| Language | Fixture | Comparison |
|---|---|---|
| C++ | `protoc-coverage/tests/coverage_fixture.hpp` | `cpp/row_compare.hpp` |
| C# (both) | `dotnet/tests/Shared/CoverageFixture.cs` | `dotnet/Shared/RowCompare.cs` |
| TypeScript | `ts/src/fixture.ts` | `ts/src/compare.ts` |

A peer reports each row it receives as `RECV <seq> OK` or `RECV <seq> MISMATCH <n>: <path>: expected
... got ...;` on stdout, and publishes its four rows on the stdin line `publish`.

## What it found (BIND-8d)

- The TypeScript backend wrote an empty placeholder for a message of another `.proto` file, so
  `crosslang.fletcher.ts` neither type-checked nor ran (D-BIND-118, fixed in the plugin).
- The TypeScript backend described a singular flatten-wrapper field as a list of the WRAPPER, where
  the wire carries a list of its element, so it could not exchange such a row with C++ or C#
  (D-BIND-119, fixed in the plugin).
- The gateway's `subscribed` reply read an uninitialised value for a field with metadata but no
  `field_number`, which failed a subscription with `std::bad_alloc` once the topic's schema was
  already declared (fixed in `gateway/src/schema_codec.cpp`).

## Not yet: TypeScript declaring the topic

The TypeScript client can announce only the JSON `schema`, which cannot carry `CrossRow`'s nested
and Timestamp fields, so in `TypeScriptToCSharpGateway` the C# client declares the topic with
`schemaIpc` and the TypeScript peer publishes on it undeclared (D-BIND-117). BIND-T gives
TypeScript `schemaIpc`; then the peer declares its own topic.

## Fast DDS

The gateway and the native peers load one profile document, the built-in one with both
`<durability>` lines changed to `TRANSIENT_LOCAL`, so a row written before a cross-process match
still arrives (as `gateway-fastdds-ts` does). The Fast DDS exchanges share that lane's exposure to
the Fast DDS 3.4 data-sharing defect (`fastdds-pubsub-provider/README.md`, "Zero-copy: the plain
sample"): as ruled 2026-10-16, a missed delivery is named and retried once.

## Running it

Needs `core`, `pubsub`, `protoc`, `fastdds-pubsub-provider`, `xrcedds-pubsub-provider` and `c-abi`
in the Conan cache (`conan create` each), Node, and the .NET SDK.

```bash
conan build . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
(cd ts && npm ci)
cd dotnet/CrossLang
FLETCHER_PROTOC_PLUGIN=<fletcher-protoc> PROTOC=<protoc> dotnet test -c Release -p:FletcherNativeShim=<libfletcher-c-abi.so>
```

`GATEWAY_BIN`, `CPP_PEER_BIN` and `NATIVE_PEER_DLL` override where the binaries are found;
`DDS_DOMAIN_ID` (default 153) isolates the DDS domain. CI: `ci.integration-test.gateway-dotnet-crosslang.yml`
(Linux in the devcontainer, Windows).
