# BIND-7 — conformance to its acceptance

Graded at `8e97ebb` by the tests-and-acceptance reviewer of `BIND-7-codereview.md`, read-only, against
the acceptance in `plans/BIND-csharp-bindings.md` (### BIND-7) and the tracker row's forcing tests.
**Five of six criteria MET, one PARTLY; both forcing tests MET.**

| # | Criterion | Grade | Evidence |
|---|---|---|---|
| 1 | `<stem>.fletcher.arrow.cs` and `<stem>.fletcher.accessor.cs` under the one `csharp_accessor` token | MET | `CsAccessor.TheTokenIsParsedAndOtherTokensAreUnchanged`; the no-drift case `CsVisitor.CsharpTokenChangesNoExistingOutputAndAddsOnlyItsOwnFiles` (two named files added, every other file byte-identical, 7 bases × 2 tokens); `CsVisitorPair.TheCommittedGoldensAreThePluginsOutput` covers the three goldens, which `Fletcher.Tests` compiles. |
| 2 | The `StructArray.Fields` windowing question settled empirically and recorded (D-BIND-10), with a nested fixture at a non-zero offset as the forcing test | MET | D-BIND-10's settlement; `GeneratedAccessorTests.AStructArrayAtANonZeroOffsetReadsItsOwnRows` (nested `Position` included) and `GeneratedAccessorCollectionTests.ACollectionOfAStructAtANonZeroOffsetReadsItsOwnRows`; a re-slicing mutant fails it. |
| 3 | `<Class>View`: a readonly struct of (accessor, row), field properties only, named and typed as the model, the accessor's `RowView`; `ToRow(row)` on the accessor; string and UTF-8 span getters | MET | `CsAccessor.TheViewHasTheModelsPropertiesWithTheirNamesAndTypes`, `ACollectionViewKeepsTheModelsPropertyNames`, `AStringOrBytesFieldHasASpanGetterNamedAfterEveryProperty`, `ToRowIsTheModelsFromArrowOverTheAccessorsColumns`; at runtime `AStringOrBytesValueReadsInPlaceAsItsBytes`, `ReadingThroughTheViewAllocatesNothing`. |
| 4 | `<Class>Accessor`: validate once and cast once, positional type-only validation, no per-cell materialisation, cached columns own their buffers, `TryMake` never throwing, no per-field getters, metadata API, D-BIND-96, nesting capped at 2/3 | PARTLY | Met: `TheAccessorHasOnlyItsFixedMembers`, `AColumnIsValidatedAgainstTheModelsSchema`, `NamesAndTheNullableFlagAreTolerated`, `EveryRefusalSaysWhatAndWhere`, `EveryCollectionRefusalSaysWhatAndWhere`, `MetadataIsTheBatchsAndEmptyNeverNullWhenAbsent`, `AColumnItCannotReadKeepsTheClassAndRefusesWithTheReason`, `AListNestedBeyondTheCapRefusesNamingIt`, the two allocation tests. Not met: "own their buffers" has no C# meaning, test or documentation (review Q1); the leaf set at the cap is wider than C++'s (review Q2); "never throwing" has reported gaps (R-D1). |
| 5 | Full type parity: scalar, nullable scalar, struct, repeated scalar, repeated struct, map (scalar and message value), nested list to the cap, metadata | MET | The `Sample` and `Track` goldens cover every kind, depth 3 included (`Cube`); `EveryColumnReadsBackAsTheModelWroteIt`, `EveryCollectionReadsBackAsTheModelWroteIt`, `ANullElementReadsAsCppReadsIt`, `MetadataIsTheBatchsAndEmptyNeverNullWhenAbsent`. |
| 6 | C# joins `accessor-capstone` as the third arm: same proto, fixture and oracle, `observed == expected`, nothing transcribed | MET | `CapstoneTests.TheCSharpAccessorReadsTheSharedFixtureAsTheOracleSays` loads both JSON files and generates the proto in `FletcherGen`; no expected value in the C# file; `TheComparisonCanFail` keeps it honest. The arm's input types come from C#'s own Schema (review T-D2). |
| T | Tracker forcing tests: the capstone's C# arm `observed == expected`; a windowing fixture at a non-zero offset | MET | As rows 6 and 2. |

**At the close (2026-10-15, `268dceb`):** row 4's PARTLY is answered by rulings, not by code. "Cached
columns own their buffers" is C++'s; C#'s accessor borrows its source and says so (D-BIND-103). The
wider leaf set at the cap is kept as a superset (D-BIND-104), and "never throwing" is met (review
R-D1). Row 6's circular input types are known debt (T-D2).
