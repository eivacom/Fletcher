# gateway-dotnet-crosslang integration test

The cross-language pub/sub matrix's C# rows, in both directions, every row compared field by field
on the receiving side:

- **The C# gateway client's** (BIND-8d, D-BIND-116): the generated C# **gateway** pair exchanges
  rows with a C++ native pair, a C# native pair and a TypeScript gateway client, through one
  gateway over Fast DDS.
- **The C# native pair's** (BIND-9b-4, D-BIND-161): the generated C# **native** pair exchanges rows
  with the C++ native pair over Fast DDS, with the TypeScript client through the gateway, and with
  a second C# native pair in another process. Peer to peer: the test process only drives them.

## Topology

```
  C++ peer (native pair)      ──DDS──┐
  C# native peer (shim)       ──DDS──┤
  second C# native peer       ──DDS──┤
                                     ├──  gateway --provider fastdds  ──WS──  C# gateway client (the test)
                                     │                                ──WS──  TypeScript peer
```

The test process is the C# gateway client; the gateway and the four peers are child processes it
starts once (`CrossLangLane`) and stops after the suite. The second C# native peer
(`--role secondary`) only receives `NativeToNative`, and the first does not, so that topic's rows
always cross a process boundary.

| Test | Topic (`CrossLang.*`) | Publisher | Subscriber, which compares |
|---|---|---|---|
| `CSharpGatewayToCpp` | `CsToCpp` | C# gateway pair | C++ native pair |
| `CppToCSharpGateway` | `CppToCs` | C++ native pair | C# gateway pair |
| `CSharpGatewayToCSharpNative` | `CsToNative` | C# gateway pair | C# native pair |
| `CSharpNativeToCSharpGateway` | `NativeToCs` | C# native pair | C# gateway pair |
| `CSharpGatewayToTypeScript` | `CsToTs` | C# gateway pair | TypeScript client |
| `TypeScriptToCSharpGateway` | `TsToCs` | TypeScript client | C# gateway pair |
| `CSharpNativeToCpp` | `NativeToCpp` | C# native pair | C++ native pair |
| `CppToCSharpNative` | `CppToNative` | C++ native pair | C# native pair |
| `CSharpNativeToTypeScript` | `NativeToTs` | C# native pair, through the gateway | TypeScript client |
| `TypeScriptToCSharpNative` | `TsToNative` | TypeScript client, through the gateway | C# native pair |
| `CSharpNativeToCSharpNative` | `NativeToNative` | C# native pair | second C# native pair |

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

A peer reports each row it receives as `<topic> RECV <seq> OK` or `<topic> RECV <seq> MISMATCH <n>:
<path>: expected ... got ...;` on stdout, and publishes its four rows on the stdin line
`publish <topic>`. Lines and commands name the topic because a peer listens and publishes on more
than one: a late row of an earlier test must never count for the next (D-BIND-161).

## What it found (BIND-8d)

- The TypeScript backend wrote an empty placeholder for a message of another `.proto` file, so
  `crosslang.fletcher.ts` neither type-checked nor ran (D-BIND-118, fixed in the plugin).
- The TypeScript backend described a singular flatten-wrapper field as a list of the WRAPPER, where
  the wire carries a list of its element, so it could not exchange such a row with C++ or C#
  (D-BIND-119, fixed in the plugin).
- The gateway's `subscribed` reply read an uninitialised value for a field with metadata but no
  `field_number`, which failed a subscription with `std::bad_alloc` once the topic's schema was
  already declared (fixed in `gateway/src/schema_codec.cpp`).

## Every publisher declares its own topic

Each publisher declares its topic with the schema's Arrow IPC bytes, the TypeScript peer included:
its generated `CrossRow` carries them (`schemaIpc`, D-BIND-130), so its declaration is a native
publisher's, byte for byte. (Until BIND-8e, TypeScript could announce only the lossy JSON schema,
and the C# client declared the topic for it, D-BIND-117.)

## Live, not replayed

Before the fixture rows, the publisher sends probe rows (`seq` -1, which every receiver reports
as `<topic> RECV -1 UNKNOWN` and nobody compares) until one arrives, so the receiving reader has
matched the writer and the rows travel live rather than through a Fast DDS backlog replay (D-BIND-129). A retry
attempt must deliver all four rows by itself.

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
