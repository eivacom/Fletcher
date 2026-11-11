# Integration test: the C# gateway client against a live gateway

`Eiva.Fletcher.GatewayClient`'s `FletcherClient` driving the real `gateway` exe over a real WebSocket, once over the `inprocess` provider and once over `fastdds` (BIND-8b). It is the C# counterpart of [gateway-end-to-end](../gateway-end-to-end/README.md), which does the same for the TypeScript client.

## What it covers

Per provider, with a generated message ([`proto/fix.proto`](proto/fix.proto): a nested message, a Timestamp, a repeated field and a map):

- **Round trip:** rows published by one client arrive at another's subscription and read back field for field through the generated `FromArrow`.
- **`create_topic`'s `schemaIpc`** ([D-BIND-112](../../plans/BIND-locked-decisions.md)): a second client declaring the same topic with the same generated schema is accepted, a different schema is refused as a conflict, and a subscriber that brings no schema decodes with the one the gateway hands back. The schema is written by Apache.Arrow in C# and read by the gateway's nanoarrow, so this is the check that the two agree.
- **Attachments** travel with their row; **a refusal** (a topic with an empty segment) fails the call that caused it and the connection goes on; **unsubscribing** stops delivery.

The client's own behaviour (one request in flight, cancellation, the subscribe races, handler faults) is tested without a gateway in `dotnet/tests/Fletcher.GatewayClient.Tests`.

## How it runs

1. `conan build .` builds the gateway exe from this branch (`CMakeLists.txt` brings in `gateway/` with `add_subdirectory`; core, pubsub and fastdds-pubsub-provider must be in the Conan cache first).
2. `dotnet test` in `dotnet/GatewayDotnetEndToEnd` generates the model from `proto/` with the fletcher-protoc plugin, named by `FLETCHER_PROTOC_PLUGIN`, and protoc, named by `PROTOC` (both from the Conan cache, as in [protoc-dotnet](../protoc-dotnet/README.md)). It spawns the gateway (found under `build/`, or named by `GATEWAY_BIN`) on port 19191 for `inprocess` and 19194 for `fastdds` (domain 152, or `DDS_DOMAIN_ID`), waits for `READY`, and stops it with `stop` on stdin.

CI: [`ci.integration-test.gateway-dotnet-end-to-end.yml`](../../.github/workflows/ci.integration-test.gateway-dotnet-end-to-end.yml), Linux in the devcontainer and Windows natively.

Over Fast DDS the publisher and the subscriber share one gateway process, so delivery stays inside it; the Fast DDS 3.4 data-sharing defect that [gateway-fastdds-ts](../gateway-fastdds-ts/README.md) guards needs two processes and does not apply here.
