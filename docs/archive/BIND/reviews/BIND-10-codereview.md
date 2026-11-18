# BIND-10 — code review, all slices

**Subject:** what BIND-10 landed: the rulings (D-BIND-191 to 195), one field path in every codec
(10-1), the root README and CONTRIBUTING (10-2), the component READMEs (10-3), the specs and TD-009
(10-4), the architecture docs with the neutral wording (10-5). Commits `64c9031..8efa3a8`.

**Reviewed at:** `8efa3a8`. CI: `0450081` green 61/61 (the last push); `d219b58..8efa3a8` are local.

**Outcome: 7 BLOCKERs (all shipped text), 3 questions for a ruling, DEBT, NITs.** BIND-10 does not
close until the blockers are fixed and the questions ruled.

> **CLOSED 2026-10-30, on CI at `5a2a107`** (`ci.pr` 41062852098, 61/61 on the first attempt,
> every side workflow green). All three questions ruled (D-BIND-196 to 198); all seven blockers
> fixed, with L-D1 to L-D3 and three nits (`39c57e6`); the archive moved (`5a2a107`). Carried as
> known: T-D2 (narrowed) and T-N2.

## How this review was done, and its limit

The method of BIND-4 to BIND-T. Three reviewers with **fresh context** each took one area: (M) the
code and the machinery (10-1's codecs and tests, the CI checks that read docs, the snippets against
the APIs, every link and anchor); (L) what ships and what it claims (every changed document, read
against the tree, and the 2026-10-30 neutral-wording rule across `docs/` and every README); (T)
tests and acceptance (graded in `BIND-10-conformance.md`, with the cross-language matrix cell by
cell). None had seen the session that wrote the work; they read the rulings, the tracker and the
code and ran nothing (M ran a link checker over the 13 changed files: every target and anchor
resolves). Every BLOCKER and question below was then checked against the tree by the session
before it was recorded. IDs keep the reviewer's letter; a finding two reviewers made carries both.

## BLOCKER

**B1 (L-B1) — "`Eiva.Fletcher.Protoc` runs the generator on every build" is false.**
`README.md:218`, `docs/data-flow-diagrams.md:168` and `docs/component-diagram.md:247` say so. The
package sets two properties (`protoc/nuget/msbuild/Eiva.Fletcher.Protoc.props`: `FletcherProtocPlugin`,
`FletcherProtoInclude`) and its targets only `chmod` the plugin and add `options.proto` to
`@(Protobuf)`; `protoc/README.md:243` says "The project writes its own protoc call, with protoc from
its side (`Google.Protobuf.Tools`)". A reader who adds the three packages gets no generated code.

**B2 (T-B1, M, L-B2) — the C++ → C++ matrix cell names a lane with no C++ peer.** `README.md:110`
names `pubsub-transport-conformance`, which is "One body of C#, every transport"
(its `README.md:3`). C++ → C++ across a real transport is `pubsub-arrow-fastdds`
(`PubSubArrowFastDdsTest.*`, Linux and Windows), as `gateway-fastdds-ts/README.md:8-9` says.

**B3 (M, L-N1) — `--fletcher_opt=csharp_gateway` and `csharp_accessor` given alone.** `README.md:152`,
`:153` and `:234`. The plugin refuses both without `csharp` (`protoc/src/generator.cpp:1880-1899`);
a gateway-only application needs `csharp,csharp_model_only,csharp_gateway`, as
`dotnet/README.package.md:104` says.

**B4 (M, L-B4) — TD-009 gives the wrong cost for the path it names.** `docs/technology-decisions.md:147`:
"A single-row publish builds a one-row Arrow array … (the fused per-row path at 1.95× its time …)".
1.95× is D-BIND-49's M1, a row already in an Arrow batch; the one-row-array path is M2, 3.8 µs and
19.6× (`BIND-locked-decisions.md:1465-1471`), as the generated `Publish(row)` doc says
(`csharp_backend_visitor.cpp:689`).

**B5 (T-B2; left by 10-5) — "CMake 3.15+".** `README.md:282`, `docs/architecture-overview.md:313`;
`xrcedds-pubsub-provider/CMakeLists.txt:4` requires 3.20, and `README.md:301` builds it. BIND-9b's B3,
in the root README and the overview.

**B6 (L-B3) — the build-from-source test command fails as written.** `README.md:323` and
`docs/architecture-overview.md:353` run `dotnet test Fletcher.slnx … -p:FletcherNativeShim=…`; the
solution holds `Fletcher.CopyOracle.Tests`, which FAILS without `-p:FletcherProbeShim` (its csproj
`:27-35`, `dotnet/README.md:82-86`), and the `conan create c-abi/.` before it builds no probe.

**B7 (T-N3, L-B5) — the neutral-wording rule, missed.** `integration-tests/fastdds-xrce-interop/README.md:3`
("a vessel workstation runs FastDDS"); `fastdds-pubsub-provider/benchmarks/README.md:13` and
`transform_batch.hpp:7` ("NaviSuite's `TransformWithVelocity`").

**Fixed 2026-10-30.** B1: the root README, the data-flow doc and the component diagram's legend say
the package brings the plugin and its path, and the project runs protoc (`Google.Protobuf.Tools`)
in a target of its own; the README's ".NET packages" says so beside the `dotnet add` lines. B2: the
cell names `pubsub-arrow-fastdds`. B3: the generated-file rows and the gateway sentence give
`csharp,csharp_model_only,csharp_gateway` and `csharp,csharp_accessor`. B4: TD-009 gives 19.6× for
a row converted to a one-row batch, 1.95× for a row already bound, 1.23× per batch row. B5: CMake
3.20+, with the reason. B6: both build blocks create `copy-probe`, build c-abi `with_probe_shim`
and pass `FletcherProbeShim`. B7: "a workstation or server" in the interop README, its conanfile
and its test's comments; `TransformWithVelocity` without the product name in the benchmark README
and `transform_batch.hpp`.

## QUESTION

**Q1 (T-Q1, M) — D-BIND-195 holds for the null refusal only.** `GatewayRowCodec.Plan` now names a
list element or map entry `readings[]` / `tags{}.value` in every message, the open-time refusals
too (`GatewayRowCodec.cs:137, 235, 264, 268, 577`). The C ABI codec's `PlanNode` builds open-time
paths from the Arrow child names (`Describe(path, schema.name)`, `c-abi/src/nanoarrow_codec.cpp:110,
129`), so a `list<union>` is refused natively as `field 'readings.item'` and by the gateway client as
`field 'readings[]'`; before 10-1 both said `readings.item`. Only the bind-time null refusal writes
`[]` / `{}.value` (`:499-519`). `c-abi/README.md:181-183`, the wire-format spec and D-BIND-195's
"C#'s two transports then agree" say more than that. Nested open-time paths are untested in every
codec. Related: `{}.key` (wire-format spec `:110-112`) is written only by the two managed clients;
the C++ codecs refuse a null key with no path (`nanoarrow_codec.cpp:414`, `codec.cpp:399`).

**Q2 (L-Q2) — the packages are described as on nuget.org.** `README.md:389` ("is on nuget.org …
every version so far is a prerelease") and `dotnet/README.md:42`; nothing is published until the
first `dotnet-v` tag after the merge (D-BIND-174, D-BIND-176).

**Q3 (T-D1; found by the session too) — the archive move breaks a CI check.** D-BIND-194 moves
`plans/BIND-csharp-development-plan.md`; `scripts/check_test_matrix.py:48` reads it (`DEFAULT_DOCS`)
and `ci.check-test-matrix.yml` sparse-checks out `plans` only, on every pull request.

**Ruled 2026-10-30, one at a time.** Q1 = D-BIND-196: the C ABI codec's `PlanNode` builds its path
as it descends (`[]`, `{}.key`, `{}.value`), so open-time refusals spell a nested field as the null
refusal does; `NanoarrowCodec.AnOpenTimeRefusalNamesANestedFieldByItsPath` (list of struct, map of
list, fixed-size list, large list of list) and
`ManagedCodecAgreesWithNativeTests.BothCodecsRefuseANestedUnionAtOpenNamingItAlike` (three shapes,
both codecs) pin it, and the wire-format spec states it. Q2 = D-BIND-197: kept as written. Q3 =
D-BIND-198: the check reads the tracker only and retires with it (runbook step 8).

## DEBT

- **T-D2, M — field-path tests per shape.** No codec tests a nested list (`a[][]`), a fixed-size
  list element or a LargeList element; C# has no test of a map value struct; native and managed are
  held to one path only for a top-level field, `s.x`, a scalar list element and a scalar map value
  (`ManagedCodecAgreesWithNativeTests.cs:118-125`). The paths are built the same recursive way, so
  the risk is low, but a branch regression would stay green. *Narrowed by Q1's tests:* the
  open-time path is now pinned for a list of struct, a map value list, a fixed-size list and a list
  of lists in C++, and the first three in both C# codecs; the null-refusal shapes above stay as they
  were. Carried.
- **L-D1 — the envelope's attachment order.** `docs/wire-format-specification.md:223` names the C++
  encoders and the TypeScript client; the C# gateway client is a third encoder and sorts by key bytes
  as C++ does (`Envelope.cs`).
- **L-D2, T-N1 — TD-006** still says "`fletcher-gateway-client` on npm" (`technology-decisions.md:99`);
  10-5's record says the npm name was fixed "in three places".
- **L-D3, T-N4 — `CONTRIBUTING.md:37`** lists `protoc/` as Conan only.

L-D1, L-D2 and L-D3 fixed 2026-10-30 (the C# client in the envelope rule, TD-006's names,
CONTRIBUTING's row).

## NIT

- **M, L-N2** — `README.md:153` and `dotnet/README.md:104` say the C# view and accessor need
  `Apache.Arrow` alone; they also need `Eiva.Fletcher.Model` (`ListView`, `MapView`).
- **L-N4** — `recordbatch-accessor-spec.md:474` gives the span getter as `NameUtf8` for strings and
  bytes; the bytes getter is `<Name>Span` (`csharp_backend_accessor_visitor.cpp:137`).
- **L-N5** — the Languages table says Rust has an "Arrow view and `RecordBatch` accessor"; Rust has
  the accessor only.
- **T-N2** — the TypeScript → TypeScript cell is one client looping through a gateway
  (`end-to-end.test.ts:433-473`): valid, weaker than two clients.

The first three fixed 2026-10-30 (`Eiva.Fletcher.Model` named beside `Apache.Arrow`, `NameSpan` for
bytes, "the accessor only" for Rust); T-N2 carried.
