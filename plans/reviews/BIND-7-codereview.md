# BIND-7 — code review, all slices

**Subject:** the code BIND-7 landed: the plugin's C# accessor and view emitter
(`protoc/src/csharp_backend_accessor_visitor.cpp`, `csharp_backend_columns.*`, the `csharp_accessor`
paths in `generator.cpp`, the FromArrow change in `csharp_backend_visitor.cpp`), its output as a
consumer compiles and runs it (the committed `csharp_accessor` goldens), the collection windows and
element readers added to `Eiva.Fletcher.Model`, the C# capstone arm, and the tests, CI and records
that go with them. Commits `80729e5..8e97ebb`: 7a `42213cd`, 7b `b1fbfb3`, 7c `6db96cd`, 7d `8e97ebb`,
and the rulings `6c5ac21`, `9792653`.

**Reviewed at:** `8e97ebb`. CI: `ci.pr` green at `8e97ebb` (run 38729269311, 52/52, after a rerun of
the known-flaky `gateway-fastdds-ts` job, which failed with "peer RECV lines: []" on a path BIND-7
does not touch).

**Outcome: 2 BLOCKERs, 5 questions for a ruling, 9 DEBT, NITs.** Both blockers were reproduced by
running them. One is a wrong read in the model's `FromArrow`, which `ToRow` now exposes on the
accessor and the generated subscriber already had. The other is a gap in D-BIND-100's name-clash
check across imported files. BIND-7 does not close.

## How this review was done, and its limit

The method of BIND-4 to BIND-6. Three reviewers with **fresh context** each took one area:
1. the plugin's C# accessor emitter (C++);
2. the generated C# and the `Eiva.Fletcher.Model` types it uses;
3. tests, CI and acceptance (graded in `BIND-7-conformance.md`).

None had seen the session that wrote the code. They read the locked decisions, the tracker, the C++
accessor emitter and spec as the reference, and the code. They built and ran nothing.

**Every BLOCKER, and the cross-file question, was then checked by the author**, on throwaway copies,
never the working tree:
- **B1** by adding a `ToRow` assertion to the two existing null-list tests in a copy of
  `Fletcher.Tests`: both fail, with the elements the null list's offsets span.
- **B2** by generating two small `.proto` files with the real plugin: both runs succeed and declare
  `struct AView` and `class AView` in one namespace.
- **Q3** by generating `repeated google.protobuf.Timestamp` with `csharp_accessor`: the output names
  `Fletcher.Gen.Google.Protobuf.TimestampAccessor`.

DEBT and NIT items are *reported*, resting on the reviewer's reading, unless marked *verified*.

**The limit that remains** is the one BIND-4 named: the reviewers are the same model as the author,
briefed by the author.

---

## BLOCKERs

### B1 — `ToRow` and the view disagree on a null list whose offsets span elements

> **FIXED 2026-10-15.** The model's `FromArrow` reads a null list, map or inner list, at every
> level, as empty (a null-aware end offset), so `ToRow`, `FromArrow`, the generated subscriber and the
> view agree. Both null-list tests now assert `ToRow` too; a plugin case pins the emitted reads, and a
> mutant removing either the outer or the inner check fails one of them. Every model golden moves by
> the changed reads only.
*Found by the runtime reviewer. Reproduced.*

D-BIND-101 reads a null inner list (and a null row of a nullable list column) as an empty window, and
the view does so: 7b fixed exactly this case in `ListElements.Get`. The model's `FromArrow` never
consults a list's validity bit; it copies whatever the offsets span. Arrow allows a null list's
offsets to span elements (filter and take kernels, a validity mask applied after the offsets are
written), so `FromArrow` reads data that is not there. D-BIND-102 made `ToRow` exactly `FromArrow`,
so on one valid batch `accessor.ToRow(r)` and `accessor.At(r)` disagree.

- **Where:** the generated model's list reads (`csharp_backend_visitor.cpp` `EmitRead` /
  `EmitReadList`; golden `csharp_accessor.fletcher.cs`, `Track.FromArrow`: `Grid`, `Cube`, `Matrix`,
  `MaybePoints`).
- **Reproduced:** in `ANullElementReadsAsCppReadsIt`, `ToRow(0).Grid[0]` is `[P(1), P(2)]` where
  `view.Grid[0]` is empty; in `ANullRowOfAnOptionalWrapperListReadsAsAnEmptyWindow`,
  `ToRow(0).MaybePoints` is `[P(101)]` where the view's is empty.
- **Reach:** the root is BIND-6's `FromArrow`, so the generated subscriber reads such a batch the same
  wrong way. Arrow C++'s `ListBuilder.AppendNull` writes an empty span, so batches from Fletcher's
  own C++ writer are not affected.
- **Fix:** `FromArrow` reads a null list, at every level, as an empty list (the model's `List<T>`
  cannot be null, D-BIND-92), as the view does; `ToRow` assertions in both tests.

### B2 — D-BIND-100 misses a clash with an imported file's generated names

> **FIXED 2026-10-15.** The check now compares the generated names of every visible file of the
> namespace, imports included, with the types the model declares; the reproduction is refused, naming
> both messages. `CsAccessor.AClashWithAnImportedFilesNamesIsRefusedBothWays` covers both directions
> and another package (no clash). D-BIND-100 is amended.
*Found by the plugin reviewer. Reproduced.*

`CsAccessorNameConflict` compares THIS file's generated `<Msg>View` / `<Msg>Accessor` with every type
the model declares in the namespace, imports included. It never compares an imported same-namespace
file's generated names with this file's declared types.

- **Reproduced:** `dep.proto` (`package a; message A {}`) and `main.proto` (`package a; import
  "dep.proto"; message AView {}`), each generated with `csharp,csharp_accessor`: both runs succeed;
  `dep.fletcher.arrow.cs` declares `struct AView` and `main.fletcher.cs` `class AView`, both in
  `Fletcher.Gen.A`. A project compiling both fails with CS0101.
- **Why it is a blocker:** D-BIND-100 leaves only UNIMPORTED files to the compiler; `dep.proto` is
  imported.
- **Fix:** also compare the generated names of every imported file in the same namespace with this
  file's declared types. The plugin cannot see whether the import was generated with
  `csharp_accessor`, so the check assumes it was (the usual case: one protoc invocation, one
  parameter set) and refuses conservatively. Test both directions.

---

## Questions for a ruling

**Q1 — How long are an accessor, its views, windows and spans valid?** *(runtime and tests
reviewers)* The accessor keeps references to the batch's arrays. Apache.Arrow's buffers are native
memory, which `RecordBatch.Dispose` frees, so a read after the batch is disposed reads freed memory,
from safe code. C++ owns its buffers through `shared_ptr` (D-RBA-7), which the acceptance mirrors as
"cached columns own their buffers" (graded PARTLY). Options: document "valid only while the source
batch or array is not disposed" on the accessor, the view, the windows and the span getters; or make
the accessor own the source (it would then need to be disposable, or to keep the batch alive).

**Q2 — C# reads a list of lists with a scalar leaf, which C++'s accessor refuses.** *(tests
reviewer)* Q6 and D-BIND-9 say C# "caps nested lists exactly where" C++ does, and D-BIND-96 defines
"cannot support" by C++. The depth cap matches; the leaf set is wider (`Track.matrix`,
`ListView<ListView<int>>`). Recorded only in the progress log. Options: amend D-BIND-96 / D-BIND-101
to keep the superset; or refuse it, as C++ does (GIR-10).

**Q3 — A struct column or element from another file needs that file's accessor.** *(plugin and tests
reviewers; verified)* A cross-file struct, list or map element, or nested-list leaf is read through
`global::<ns>.<Msg>Accessor` and `<Msg>View`, which exist only if that file was also generated with
`csharp_accessor`. This includes `repeated google.protobuf.Timestamp` and
`map<_, google.protobuf.Timestamp>`, which the IR keeps as a message (verified: the output names
`Fletcher.Gen.Google.Protobuf.TimestampAccessor`). The plugin cannot see another run's parameters,
so it cannot refuse at generation as D-BIND-99 does. Options: document the requirement (the plan,
the public surface, the protoc-dotnet README) as BIND-6e's for the model; and/or add a cross-file
case to the goldens and lanes.

**Q4 — A null struct whose non-nullable children hold nulls is refused.** *(Implemented 2026-10-15 per D-BIND-106: `NullsUnderValidRows`; `ANullStructsChildrenMayHoldNullsAndAValidOnesMayNot`, flat and nested; a mutant counting every null fails it.)* *(runtime reviewer)* For a
nullable message field, a struct element or a message map value, the inner accessor's null gate
counts the whole child, slots under null parents included. Arrow C++'s `StructBuilder.AppendNull`
writes placeholders there, so Fletcher's own batches pass; arrow-rs's `new_null_array(Struct)` and a
struct built from children plus a mask may put nulls there, which is legal Arrow. C++'s accessor has
the same gate (parity), but D-BIND-101 says "nothing a C++ or Rust producer may write is refused".
Options: rule it out of scope (parity with C++, D-BIND-101's sentence narrowed); or count only the
nulls under valid parents.

**Q5 — How public are the runtime support types?** *(runtime reviewer)* `IArrowElements<out T>`,
`IArrowRows<out TView>`, the seven `*Elements` classes and the public `ListView` / `MapView`
constructors and `Of` methods are now public API of `Eiva.Fletcher.Model`. Generated code needs them
public (it compiles in the consumer's assembly), but adding an interface member or an `Of` parameter
later breaks existing generated code within one MAJOR.MINOR. Options: mark them
`[EditorBrowsable(Never)]` and document them as for generated code; or keep them as supported
extension points. The `out` variance buys nothing for struct types and blocks future members.

### The rulings (the maintainer, 2026-10-15, one question each)

| Q | Ruling | Decision |
|---|---|---|
| Q1 | Document: the accessor borrows; valid only while the source is not disposed | D-BIND-103 |
| Q2 | Keep the scalar-leaf superset; amend D-BIND-96 | D-BIND-104 |
| Q3 | Document the requirement and compile a cross-file case in the protoc-dotnet lane | D-BIND-105 |
| Q4 | Count nulls only under valid parents; wider than C++ | D-BIND-106 |
| Q5 | Support types hidden, for generated code; `ListView` / `MapView` stay public | D-BIND-107 |

**DEBT to fix before the close, the maintainer's choice:** the correctness items R-D1, T-D1, P-D1
and P-D2. The rest (T-D2, T-D3, R-D2, R-D3, P-D3) and the NITs are carried as known debt; the stale
records are corrected at the close as a matter of course.

---

## DEBT

- **T-D1 — FIXED 2026-10-15:** a message reaching one whose accessor refuses now refuses up front, naming the inner reason, and its view has a comment, not a property (`HyperHolder` in the golden; `CsAccessor.AMessageReachingOneThatRefusesRefusesUpFront`, `AMessageReachingOneThatRefusesRefusesNamingIt`). Was: **the transitive D-BIND-96 case is untested** *(tests and plugin reviewers)*. No fixture has
  a message that reaches `Hyper` (a struct, list element or map value of it). By reading, the parent
  refuses at runtime through the inner `TryMake`, but its view gets a `HyperView` property where
  D-BIND-96 says "a comment instead of a getter".
- **T-D2 — The capstone arm cannot catch a C#-vs-C++ schema difference** *(tests reviewer)*.
  `BuildBatch` takes every type from C#'s own `CapstoneBatch.Schema` and `TryMake` validates against
  it. Add `ipc` to the capstone Exec and compare `CapstoneBatch.Schema` with its `.ipc`, as
  `GeneratedSchemaTests` does for `model.proto`.
- **T-D3 — Coverage gaps** *(all three)*: `MapView` foreach allocation; `ListView<bool>` /
  `<double>` / enum windows in the measured loop; a null inner list at depth 3 and under a scalar
  leaf; `ToRow` of a null StructArray row; `TryMake(null StructArray)` and refusals from a StructArray
  source; lower-case `@` and D-BIND-84 (`Schema_`) messages; proto2 optional/required; an empty
  message as a struct and element; a field-level flatten; map keys `bool` / `uint32` / `uint64`, map
  values enum / bytes.
- **R-D1 — FIXED 2026-10-15:** a null array is refused by name, and what Apache.Arrow throws on malformed input inside `TryMake` becomes a refusal (`MalformedInputIsRefusedNotThrown`; a mutant dropping the null check fails it). Was: **exceptions can still escape `TryMake`** *(runtime reviewer, reported)*: a null array in a
  batch (`columns[i].Length`); `[.. array.Fields]` building a child Apache.Arrow cannot build;
  `MapArray.Keys` when the entries child is not a `StructArray`. D-BIND-94 says it never throws on
  bad input.
- **R-D2 — Malformed offsets throw from a getter** *(runtime reviewer)*. `ListView.Of` / `MapView.Of`
  go through the public constructor's checks, so a negative count throws `ArgumentOutOfRangeException`
  on read; `TryMake` validates no offsets. C++ has the same exposure. The three checks also run on
  every read (no allocation).
- **R-D3 — `default(<Msg>View)` throws NRE on every property** *(runtime reviewer)*; undocumented.
- **P-D1 — FIXED 2026-10-15** (`CsAccessor.AFieldWithoutAGetterIsNotRefusedForItsName`). The field rule of D-BIND-100 refused a field with no getter *(plugin reviewer)*: a field
  named `<msg>_view` beyond the depth cap gets a comment, not a property, yet is refused. Require a
  column.
- **P-D2 — D-BIND-102 says `ToRow` reads `Fields` once per nested struct LEVEL** *(plugin reviewer)*;
  a list of structs or a message map value calls `FromArrow` per ELEMENT, so it is 64 bytes per struct
  element. A record correction.
- **P-D3 — `FieldsOf` runs three times per message and `FileHeader` is duplicated** *(plugin
  reviewer)*.

## NITs

- `f.span == CsViewName(msg)` can never be true (accessor_visitor.cpp:109) *(plugin, tests)*.
- `csharp_backend_columns.cpp:21`: `PendingKind`'s comment lost its first line; the header says the
  move was "unchanged" though `Elem` and `Column` gained `msg` *(plugin)*.
- `ToRow`'s `<see cref="Msg"/>` inside the accessor binds to a member named like a message
  (`message At`), and is ambiguous for `message TryMake` (CS0419 with XML docs) *(plugin)*.
- The column-count refusal reports `columns.Length` when the schema's field count is what differs
  *(runtime)*.
- `ListView` / `MapView` enumerators read the wrong element for `Current` before `MoveNext` or after
  the end *(runtime)*.
- The accessor's column fields are internal and mutable; pass them through the constructor as
  `readonly` *(runtime)*.
- `ListView`, `MapView` and the views use `ValueType.Equals`: (reader, start, count), not contents
  *(runtime)*.
- `IEnumerator.Current` is `object?` on `ListView` and `object` on `MapView` *(runtime)*.
- `BinaryElements.Get` checks null twice; `GetBytes` already reads a null as empty *(runtime)*.
- `At(row)` on a null row of a StructArray source is undocumented; `ToRow` documents it *(runtime)*.
- `CapstoneTests` hand-writes `bool[] nullable`; `ProtocDotnet.csproj` checks only the accessor file
  exists; `ACollectionViewKeepsTheModelsPropertyNames` does not filter span getters *(tests)*.
- Stale records: tracker row and `BIND-csharp-bindings.md:1918` say "7d in the tree";
  `BIND-csharp-development-plan.md:793/801` name D-BIND-94 to 97 and "7c the view"; the public
  surface's class diagram still says `<<planned BIND-7>>`, shows `NameUtf8()` as a method, and lacks
  `<Prop>Span`, `ListView`, `MapView` and `IArrowRows`; its view row states the span fact twice
  *(tests)*.

## Checked and found correct (from the three reports, condensed)

- **One walk with the model:** `FieldsOf` uses the model's records, `CsFieldTypeOf`, the D-BIND-89
  taken set in the same order and the same `ClassifyColumn`, so names and types match the model's
  (D-BIND-98), structs as views and collections as windows (D-BIND-101); span names are taken after
  every property.
- **`TryMake` is C++'s `Make` contract (D-RBA-4):** positional count, names and the nullable flag
  tolerated, class plus `SameType` for leaves (utf8 vs binary by type id, timestamp unit and zone,
  duration unit), composites by type id then element by element (a map cannot pass as a list), the
  non-null gate on non-nullable columns only, refusal text "`<Label>` column i 'name': ...". No type
  check can be bypassed.
- **Offsets and slicing:** a sliced StructArray's `Fields` are windowed (D-BIND-10) and never sliced
  again; lists inside it index their whole `Values` child by `ValueOffsets`; `MapArray` keys and
  values are correct; the batch wrapper for `ToRow` has offset 0.
- **Null semantics of the view (D-BIND-101):** scalar element default with `IsNull`, struct element
  and message map value null, null inner list empty, G-Q4's nullable column as an empty window, null
  string `""`, null bytes an empty span.
- **Allocation and threading:** typed arrays cast once, no boxing on reads, struct enumerators, the
  tests' bound meaningful; the accessor is immutable after `TryMake`, so concurrent reads are safe.
- **AOT and the model layer:** no reflection, `Unsafe.As` guarded by the underlying-type check,
  Apache.Arrow only (D-BIND-72).
- **Generator:** token parse, the C++ `accessor` token independent, the D-BIND-99 and D-BIND-100
  gates before any output, the two files additive (no-drift case), `model_converts` sound (the
  `ToRow` throw branch is unreachable today), recursive messages and flatten wrappers handled.
- **CI and records:** the capstone directory in all three lanes' filters and in both protoc-dotnet
  checkouts, the Windows leg's paths, bucket 5 counts (40 to 53, total 179), the progress log's test
  and mutant claims, both READMEs.
