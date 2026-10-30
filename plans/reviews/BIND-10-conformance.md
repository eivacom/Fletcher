# BIND-10 — conformance against the acceptance and the rulings

Graded at `8efa3a8` by the fresh-context tests-and-acceptance reviewer (T) of the BIND-10 review
(`BIND-10-codereview.md`), from the tree only. MET, PARTLY, NOT MET.

> **After the fixes, 2026-10-30:** the matrix's C++ → C++ cell names `pubsub-arrow-fastdds` (B2),
> so the matrix and D-BIND-192 are MET; D-BIND-195 is MET with D-BIND-196 (open-time paths); the
> archive is the close's (D-BIND-194, with D-BIND-198).

## The acceptance (tracker, BIND-10)

| Bullet | Grade | Evidence or gap |
|---|---|---|
| `dotnet/README.md`: build, test, consume, the packages, TFMs, single-copy rule, licence files | MET | build and test `:74-112`, consume `:40-49`, the four packages (D-BIND-74) `:10-32`, TFMs and the Arrow pin `:34`, single copy `:147-157`, licences `:163-171` |
| `c-abi/README.md`: ownership, codec surface, a second binding | MET | `:113-155`, `:157-183`, `:185-214`; 47 entry points and ABI 0.7 match `binding.h:163-164` |
| `README.md`: layout rows, dependency graph, generated files, releasing, "Consume in .NET" | MET, with B1, B3 | `:258-259`, `:52-87`, `:151-153`, `:423`, `:216-234` |
| `CONTRIBUTING.md`: layout rows, NuGet naming, licence headers | MET | `:40-41`, `:78`, `:55-61` (matches `Fletcher.csproj:1-3`) |
| Wire-format spec: "Proto to C# Type Mapping" with the lossless temporal rule | MET | `:133-179`, temporal `:165-174` |
| Accessor spec: C# beside C++ and Rust, `TryMake`, the windowing finding | MET | `:42`, `:68`, `:314-316`, §11 `:444ff`, `TryMake` `:455` |
| TD-009, the .NET binding | MET, with B4 | `technology-decisions.md:135ff` |
| The pubsub-conformance caveat | MET | made precise rather than retired: `README.md:340-363`, `:525-544` |
| The cross-language matrix | PARTLY | 15 of 16 cells proven; C++ → C++ names a C# lane (B2) |
| Archived to `docs/archive/BIND/` | NOT MET (10-6) | `docs/archive/` holds GIR and HARD; see Q3 |

## The rulings

| Ruling | Grade | Evidence or gap |
|---|---|---|
| 191 the architecture docs | MET, with B1 | overview, component and data-flow diagrams and the index name the C ABI, the packages, the gateway client and the generator package; diagrams validated by render (the record) |
| 192 the Languages section | PARTLY | `README.md:91-115`, six capabilities; one matrix cell wrong (B2) |
| 193 (superseded) | MET | no `readings.item`, `tags.value` or `childPath(path,'item')` outside the plans |
| 194 the archive in two steps | NOT YET | 10-6; the move must touch `scripts/check_test_matrix.py` (Q3) and, at the second step, `binding.h:86`, `c-abi/benchmarks/README.md:5`, `gateway-dotnet-end-to-end/README.md:10` |
| 195 C++'s spelling | PARTLY | every null refusal agrees; open-time refusals do not (Q1) |

## The cross-language matrix (`README.md:108-113`)

Every named lane exists and is in `ci.pr.yml` (`:654-697`), on Linux and Windows except
`gateway-fastdds-ts` (Linux, as the README says).

| Publisher → subscriber | Lane | The test that proves it |
|---|---|---|
| C++ → C++ | pubsub-transport-conformance | **not that lane** (C# only); `pubsub-arrow-fastdds`, `PubSubArrowFastDdsTest.*` |
| C++ → C# native, C# gateway | crosslang | `CppToCSharpNative` (`CrossLangTests.cs:95`), `CppToCSharpGateway` (`:78`) |
| C++ → TypeScript | gateway-fastdds-ts | `gateway-fastdds.test.ts:252` |
| C# native → C++, C# native, C# gateway, TypeScript | crosslang | `:92`, `:104`, `:82`, `:98` |
| C# gateway → C++, C# native, TypeScript | crosslang | `:62`, `:66`, `:70` |
| C# gateway → C# gateway | gateway-dotnet-end-to-end | `GeneratedRowsRoundTripFieldForField` (`EndToEndTests.cs:71`) |
| TypeScript → C++ | gateway-fastdds-ts | `gateway-fastdds.test.ts:278` (the C++ peer echoes) |
| TypeScript → C# native, C# gateway | crosslang | `:101`, `:86` |
| TypeScript → TypeScript | gateway-end-to-end | `end-to-end.test.ts:431` (one client, T-N2) |

## 10-1's tests (D-BIND-195)

| Path | C# gateway client | C# against native | TypeScript | C++ |
|---|---|---|---|---|
| list element `readings[]` | through the theory | `ManagedCodecAgreesWithNativeTests.cs:122` | — | `test_nanoarrow_codec.cpp:941`, `test_codec_edge.cpp:480` |
| field of a list element `[].x` | `RowCodecTests.cs:548` | — | `wire-encode.test.ts:204` | `test_codec_edge.cpp:490` |
| map value `{}.value` | through the theory | `:123` | — | `:957`, `:507` |
| field of a map value `{}.value.x` | — | — | `:210` | — |
| nested list, fixed-size list, map key | — | — | — | — |

Each test matches `field '<path>'` exactly, so a codec that reverted would fail it.
