# BIND-8 — conformance to its acceptance

Graded at `89f9a34` by the tests-and-acceptance reviewer of `BIND-8-codereview.md`, read-only, against
the acceptance in `plans/BIND-csharp-bindings.md` (### BIND-8) and the tracker row's forcing tests.
CI at `89f9a34`: `ci.pr` run 39248253992, 56/56, and every side workflow green.
**Five of eight criteria MET, one MET by a later ruling, two PARTIAL; forcing tests PARTIAL (Bucket 2)
and MET (no `runtimes/`).**

| # | Criterion | Grade | Evidence |
|---|---|---|---|
| 1 | Text-frame control and binary-frame data protocol; sub-ids parsed from strings | MET | `ControlMessagesTests.ParsesSubscribedWithAnIdBeyondJavaScriptsIntegers` (2^53+1), `BuildUnsubscribeWritesTheIdAsADecimalString`, `AMalformedReplyIsRefusedAsInvalidData` (a numeric `subId` refused); `GatewayFramesTests`; both live lanes. |
| 2 | `FletcherClient` with `SubscribeAsync<T>` / `PublishAsync<T>`, real generics; `CancellationToken` everywhere; no `async void`; no sync-over-async (amended by D-BIND-72) | MET by ruling | D-BIND-108 declined a generic client: it works on `RecordBatch` and the generated pair is typed. Every public async method takes a token (`DisposeAsync` aside); no `async void`, `.Result`, `.Wait()` or `GetResult()` in the package. The bullet's wording is stale (review NIT). |
| 3 | The generated gateway pair: `protoc/tests` cases and a live round trip | MET | Four `CsGateway.*` cases and the golden held by `CsVisitorPair.TheCommittedGoldensAreThePluginsOutput`; `GeneratedGatewayPairTests` (6); `EndToEndTests.TheGeneratedGatewayPairRoundTripsTypedRows` over `inprocess` and `fastdds`. |
| 4 | The one managed-codec exception; `Envelope` here and nowhere else | MET | The only `Envelope` in `dotnet/src` is `Fletcher.GatewayClient/Envelope.cs`. |
| 5 | No native assets, asserted by CI on the packed package | MET | `ci.dotnet` step `Package.GatewayClientHasNoRuntimesFolder` (a `lib/` entry, no `runtimes/`, no dependency on `Eiva.Fletcher` or `Eiva.Fletcher.Interop`), green on CI; mutants caught locally. |
| 6 | Bucket 2 ported: the Part 4 file set | **PARTIAL** | TypeScript 36 mapped (client 6, envelope 5, object-backend 3, wire-encode 8, ws-protocol 14); `test_publish_frame` 9 one for one. `test_schema_codec`: of 21, the original 11 have C# counterparts but 10 of them test `ControlMessages.SchemaToJson`, which no shipped path calls since D-BIND-112 (review B4); the 10 cases BIND-8 added (gateway conflict measurement, base64, `FieldToJson`) have no counterpart and no recorded exclusion. |
| 7 | `schemaIpc` parsed via `Apache.Arrow.Ipc`, metadata preserved | MET (thin) | `ASubscribedReplyCarriesTheSchemaAsBase64Ipc`, `TheDescriptorCarriesEachFieldsNumberFromItsMetadata` over the C++ `.ipc` goldens; nested and schema-level metadata are not asserted (review DEBT). |
| 8 | Cross-language rows: the C# gateway client with C++, TypeScript and the C# native pair, both directions, field by field | **PARTIAL** | All six rows, through the generated gateway pair, compared on the receiving side. PARTIAL: the one retry pools rows of both attempts (review B3), and on the green CI run Linux C++ → C# passed only on its retry (rows 0 and 1 on attempt 1). |
| T | Tracker forcing tests: Bucket 2 green; `Package.GatewayClientHasNoRuntimesFolder` | PARTIAL / MET | As rows 6 and 5. |

**After the review's fixes (2026-10-19, in the tree):** row 6 is met by D-BIND-128's re-mapping:
`test_schema_codec` is recorded as gateway-internal and the client's obligation (its `schemaIpc`
reads back as the same declaration, at every depth) is held over every C++ `.ipc` golden and live
by the cross-language lane. Row 7's "thin" is answered by the same comparison (nested and
schema-level metadata, Timestamp unit and zone). Row 8 is met: each retry attempt must deliver all
four rows by itself, and the rows travel live after a probe row (D-BIND-129). Row 2's stale wording
is annotated in the tracker. Grades become final when the fixes are green on CI.

**Final grades (2026-10-20, on CI at `750d79d`, `ci.pr` 39375700668, 56/56; the fixes green
first at `7e2acca`, `ci.pr` 39281811974): all eight criteria MET (row 2 by ruling, D-BIND-108);
forcing tests MET** (Bucket 2 as re-mapped by D-BIND-128, `Package.GatewayClientHasNoRuntimesFolder`).
Beyond the acceptance, the client gained batched delivery (D-BIND-133, slice 8f) and one write at a
time on its socket (review A1).
