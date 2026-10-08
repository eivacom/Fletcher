# BIND-6 — code review, all slices

**Subject:** the code BIND-6 landed: the protoc plugin's C# backend (`protoc/src/csharp_backend_*`, the
C# paths in `generator.cpp`), its output as a consumer compiles and runs it (the committed goldens, the
generated publisher/subscriber pair), `Eiva.Fletcher.Model`, the runtime changes of 6e-2
(`NativeLoader`, `IsAotCompatible`, `Directory.Build.targets`), and the tests and lanes that go with
them. Commits `7d740e3..c8ae231`: 15 commits, 65 files.

**Reviewed at:** `c8ae231`. CI: `ci.pr` green at `4e90f52` (run 37741456084). The run at `d32b9f7`
(37768542754) was cancelled by the push of `c8ae231` after its `dotnet` jobs had passed, NativeAOT
step included, on both platforms. The run at `c8ae231` (37771476240) was still in progress.

**Outcome: 5 BLOCKERs, 10 questions for a ruling, 17 DEBT, NITs.** All five blockers were verified,
four of them by running something. Three are generated C# that does not compile for ordinary
`.proto` files. One is a wire-format disagreement with C++ for the most common message field. One is
a missing test the acceptance asks for. BIND-6 does not close.

## How this review was done, and its limit

The method of BIND-4 and BIND-5. Three reviewers with **fresh context** each took one area:
1. the plugin's C# backend (C++);
2. the generated C# and the managed runtime it calls;
3. tests, CI and acceptance (graded in `BIND-6-conformance.md`).

None had seen the session that wrote the code. They read the locked decisions, the tracker, the C++
and TypeScript backends as the reference, and the code. They built and ran nothing.

**Every BLOCKER was then checked by the author**, on throwaway copies, never the working tree:
- **B1** was reproduced across both languages. A test spliced into a copy of `Fletcher.Tests` wrote
  C#'s bytes. A gtest spliced into a copy of `protoc-coverage` decoded them with the C++ generated class.
- **B2 to B4** were reproduced by generating three small `.proto` files with the real plugin and
  compiling each one alone (`scratchpad/repro2`).
- **B5** was confirmed by reading.

DEBT and NIT items are marked *verified* where that was done and *reported* where they rest on the
reviewer's reading.

**The limit that remains** is the one BIND-4 named. The reviewers are the same model as the author,
briefed by the author. Two of them found B1 independently, from opposite sides of the wire.

---

## BLOCKERs

### B1 — An absent non-`optional` message field is a null on the wire, and C++ cannot decode the row
*Found by the runtime reviewer (G-B1) and the plugin reviewer (P-B3). Reproduced.*

A proto3 message field without `optional` (`Stamp stamp = 7;`) is non-nullable in the class's own
`Schema` (C++'s choice: `csharp_pair.fletcher.cs:74`, `..., false, ...`). The C# property is
`Stamp?` (D-BIND-75), defaulting to `null`, and `ToArrowColumns` writes `null` as a cleared validity
bit (`:143-149`: `else { r6.Add(new Stamp()); m6.Append(false); }`).

- **The codec** sets the field's wire null bit from Arrow validity and writes no payload, whatever
  the schema says (`c-abi/src/nanoarrow_codec.cpp:221-229`).
- **The C++ decoder** reads a non-nullable struct field unconditionally
  (`protoc/src/cpp_backend_decode_visitor.cpp:120-122`), so it parses the NEXT field's bytes as the
  struct.
- **C++ itself never writes that null.** It encodes `value_or(Cls())`, a default instance
  (`cpp_backend_type_table.cpp:216-221`).

**Reproduction.** C# encoded the coverage fixture's `CompositeCoverage` twice, through `ToArrow` and
the native codec:
- **`Scalars = new()`:** 558 bytes, **byte-identical** to C++'s own `Encode()` of the same row. C++
  decodes them to the expected row.
- **`Scalars = null`:** 449 bytes, first byte `0x09` (the null bit set). The C++ generated class
  **throws: `PositionalReader: buffer underrun`**.

**Why the tests missed it.** `CoverageWireBytesTests` always populates the non-optional message
fields. `GeneratedPairTests.AnEmptyRowArrivesAsAnEmptyRowNotAsAFullOne` asserts
`Assert.Null(got!.Stamp)`, which pins the divergent behaviour, because only C#-to-C# is run.

**It breaks** the round's central promise (one codec, C# and C++ agree on the wire), the acceptance's
G-5 bullet, and the visitor's own rule that "no ToArrow can build a batch its own Schema disagrees
with". **The fix needs a ruling** (Q1), because it changes how D-BIND-75's "`?` when the field can be
absent" reads.

### B2 — A class name in an expression binds to a member of the same name, and the C# does not compile
*Plugin reviewer P-B1. Reproduced.*

A same-namespace class is named bare (`CsTypeRef`). In a TYPE position that is fine. In an
EXPRESSION position (`Cls.ToArrowColumns(..)`, `Cls.FromArrow(..)`, `Cls.ToArrow(..)`,
`Cls.Schema`), C# simple-name lookup finds a member of the enclosing class first. The "Color Color"
rule rescues only a member whose type IS `Cls`.

Reproduced with `pb1.proto`:

| Input | Result |
|---|---|
| `message RouteList { repeated Waypoint waypoint = 1; }` | CS0120 ×2 |
| `message RouteMap { map<string, Waypoint> waypoint = 1; }` | CS0120 ×2 |
| `message RouteMixed { Waypoint start = 1; int32 waypoint = 2; }` | CS0120 ×2 |
| `message row {..} message UsesRow { row r = 1; }` | CS0176 (a generated local named `row`) |
| `message Topic {..}` as a pub/sub input | CS1061 ×7 in the native pair (`Topic.Schema` binds to the pair's own `Topic`) |
| **Control:** `message RouteSingle { Waypoint waypoint = 1; }` | compiles, as predicted |

The golden passes only because its one such case is singular (`Stamp? Stamp`). A singular-named
repeated field of a message type is common in real `.proto` files. **Fix, no ruling needed:** in
expression positions always emit `global::<namespace>.<Name>`.

### B3 — A message named after one of the generated class's own members does not compile
*Plugin reviewer P-B2, runtime reviewer G-Q2. Reproduced.*

`message Schema { int32 x = 1; }` emits `public sealed class Schema { public static
global::Apache.Arrow.Schema Schema { get; } ... }`. That is **CS0542** (`pb2.fletcher.cs(13,47)`).
The same holds for `ToArrow`, `FromArrow` and `ToArrowColumns`. D-BIND-73 reserves these names for
PROPERTIES only. **Needs a ruling** (Q2): rename the type (`Schema_`), or refuse the file as D-BIND-82
does.

### B4 — A map whose value is a flatten wrapper names conversions the wrapper never gets
*Plugin reviewer P-B4. Reproduced in C#.*

`map<string, W>`, where `W` has `(fletcher.flatten)`, is a STRUCT node pointing at the wrapper
(`ir.cpp` `BuildMapNode`). `ConversionBlocker(W)` finds W's fields convertible, so the parent emits
`W.ToArrowColumns(f0)` and `W.FromArrow(e, j)`. But a flatten wrapper gets neither (`GenerateMessage`,
"No Schema: a flatten wrapper is inlined"). **CS0117 ×2** (`pb4.fletcher.cs(87)`, `(103)`).

**Not verified:** whether C++ handles this input. Its wrapper classes are skipped as well, so it may be
broken there too. **Needs a ruling** (Q3) on what such a map is: convert it (C# would need
conversions for wrappers) or refuse it.

### B5 — The acceptance's C#-against-C++ topic test does not exist
*Tests reviewer T-B1. Confirmed by reading.*

The tracker's D-BIND-72 bullet asks for "a test that a C# and a C++ publisher of one method register
the same topic". `protoc/tests/test_topic_path.cpp`'s `ExpectBackendsAgree` (`:83-88`, `:135-156`)
generates C++ and TypeScript only. `GeneratedPairTests.TheTopicIsCppsForm` and
`CsVisitorPair.TheModelFileSpellsEachTopicAsAStaticClass` compare C# with hand-written literals, so a
change to C++'s segment form passes. **Fix, no ruling needed:** generate with `csharp` in
`ExpectBackendsAgree` and compare `Segments` and `Key`.

---

## Questions for a ruling

| # | Question | Source |
|---|---|---|
| Q1 | **B1's fix.** Write a present default instance for a schema-non-nullable message field, as C++'s `value_or` does (and make the property non-null with a `new()` default, as C++'s getter returns `kDefault`)? Or keep `T?` and only write the default on the wire? | G-B1, P-B3 |
| Q2 | **B3's fix.** Suffix the TYPE (`Schema_`), or refuse the file with an error, as D-BIND-82 does? | P-B2, G-Q2 |
| Q3 | **B4.** A map whose value is a flatten wrapper: convert it, or refuse it? Which does C++ do? | P-B4 |
| Q4 | **Nullability of children (G-5).** List items and map values are nullable in the schema (nanoarrow's defaults) and non-nullable in C# (`List<int>`). Does G-5 cover children, or only top-level fields? A reflection test (`NullabilityInfoContext`) would pin whichever is ruled. | T-Q1 |
| Q5 | **The `CollectCrossFileIncludes` deviation** (6e-1) was recorded but never ruled. The dev plan's G-6 still says to reuse it. | T-Q2 |
| Q6 | **The two open AOT notes.** The tests reviewer recommends neither block the close. Park both to BIND-9: Windows AOT with the win-x64 assets, and an AotSmoke run over `fastdds` for the foreign-thread callback. | T-Q3 |
| Q7 | **Two fields that PascalCase to one property** (CS0102). Proto3's JSON-name check rejects most pairs. Proto2, and a field-level flatten that inlines a field named like an outer one, are not covered. D-BIND-69 left this open. | P-Q2, T-Q4 |
| Q8 | **A flatten-wrapper pub/sub input.** C# gives it a topic class and a skip reason. C++ excludes wrappers from its `generated` set and skips the method with "no generated Arrow mapping". C# matches TypeScript, not C++. Which is canonical? | P-Q1 |
| Q9 | **The other direction of D-BIND-82.** protoc can define a TYPE that clashes with our namespace: `csharp_namespace = "Fletcher"` with `message Gen`, or package `fletcher` with `message Gen`. In scope? | P-Q3 |
| Q10 | **Null where the schema says non-nullable, by caller error** (`Name = null!`, a null map key, a null list element). It writes a null that C++ reads past, as B1 does. Coerce to `""`, or throw in `ToArrow`? Also: `default(Timestamp)` is `(0, Second)` and returns as `(0, Nanosecond)`, which compares unequal (G-Q3). Accept, or default to nanoseconds? | G-Q1, G-Q3 |

### The rulings (the maintainer, 2026-10-08, one question each)

They become D-BIND-83 to D-BIND-93 in the digest when the fixes land, and are then recorded in the
four places.

| Q | Ruling | Facts it rested on |
|---|---|---|
| Q1 → D-BIND-83 | **Mirror C++ fully.** A non-`optional` message field is a non-null property defaulting to `new()`, always written present; `optional` stays `T?`. Amends D-BIND-75's "`?` when the field can be absent": absent now means `optional` | B1's reproduction |
| Q2 → D-BIND-84 | **Suffix the TYPE** with `_` (`Schema_`) when a message is named `Schema`, `ToArrow`, `FromArrow` or `ToArrowColumns`, everywhere the type is named, as protoc does for a clash | B3's CS0542 |
| Q3 → D-BIND-85 | **A map whose value is a flatten wrapper is UNSUPPORTED in the IR**, with a reason, so every backend drops it consistently | Measured: **C++ fails too** (`'W' was not declared`); it never compiled in any backend |
| Q4 → D-BIND-86 | **G-5 covers fields, not children.** List items and map values follow the C++ row class's storage (non-null). A reflection test pins each property against its field's `IsNullable` | C++ stores `std::vector<int32_t>`; the schema's child nullability is nanoarrow's default |
| Q5 → D-BIND-87 | **Require reuse.** C#'s cross-file qualification goes through `CollectCrossFileIncludes`, or a shared helper both backends derive from it, so dependencies are found in one place. C++ and C# output stay byte-identical | The 6e-1 deviation |
| Q6 → D-BIND-88 | **Park both AOT notes to BIND-9**, with an owner: a win-x64 AotSmoke publish and run with the RID matrix, and an AotSmoke run over `fastdds` for the foreign-thread callback | D-BIND-81 put the step on Linux only |
| Q7 → D-BIND-89 | **(a) proto2 `foo_bar` + `fooBar`:** C# suffixes the later field's property with `_` until unique, in field order; C++ is unaffected. **(b) A flattened field that inlines an existing name:** refused for EVERY backend, with an error naming both fields | Measured: proto3 cases are rejected by protoc 3.21.12 itself (case-insensitively). Proto2 is accepted, with C++ fine and C# CS0102. The flatten case breaks C++ too (`set_x` redeclared) and puts two columns named `x` in the schema |
| Q8 → D-BIND-90 | **A flatten-wrapper pub/sub input follows C++:** no topic class, and C++'s skip line and reason. TypeScript's divergence is recorded as debt for its own lane | P-Q1 |
| Q9 → D-BIND-91 | **Extend D-BIND-82** to the reverse clash: also refuse a file where protoc would declare a TYPE named `Fletcher` or `Fletcher.Gen` | P-Q3 |
| Q10a → D-BIND-92 | **The generated `ToArrow` throws `ArgumentException`** (message, field, row) on a caller-forced null in a non-nullable string or bytes field, map key or list element. **Separately recorded and parked to BIND-9/10:** an Arrow-tier batch with a null in a non-nullable column is encoded in BOTH languages, because no codec path checks nullability | Checked: C++'s row class cannot hold such a null (`std::string`), so C++ never throws; no C++ Arrow path checks either |
| Q10b → D-BIND-93 | **A non-nullable Timestamp or Duration property is initialised in its column's unit** (`new Timestamp(0, TimeUnit.Nanosecond)`), so a fresh row round-trips equal; the Model is unchanged | `default(Timestamp)` is `(0, Second)` |

---

## DEBT

**Plugin**
- **P-D1, a compile failure for a legal input:** `CsStringLiteral` passes U+0085, U+2028 and U+2029
  through raw. C# treats them as line breaks, so an option-metadata value containing one is CS1010.
  Escape them. *(reported)*
- P-D2: `ConversionBlocker` has no memo; it re-walks shared sub-messages. *(reported)*
- **P-D3:** fields the IR drops (oneof members and the like) vanish from the C# class without the
  note C++ writes. *(reported)*
- P-D4: an exception from the schema sink would crash the plugin rather than return `*error`;
  unreachable today. *(reported)*

**Generated C# and runtime**
- **G-D1:** `NativeLoader` (6e-2) no longer probes `runtimes/<rid>/native/` beside the ASSEMBLY.
  In a plugin-style host (a PowerShell module, an MSBuild task, a custom AssemblyLoadContext) that is
  not the app directory. Probe both: `Assembly.Location` when non-empty, IL3000 suppressed under that
  guard, and `AppContext.BaseDirectory`. *(reported; the author agrees)*
- G-D2: D-BIND-81 pins ILCompiler but not the runtime pack it is used with. On an SDK other than
  10.0.400 an AOT publish mixes 10.0.11 and 10.0.12. CI is unaffected. *(reported)*
- G-D3: the generated subscriber allocates a `StructType` per delivery; build it once. *(verified by
  reading)*
- G-D4: decode failures surface through `HandlerFaulted` as if the handler had thrown; document them
  or tell them apart. *(reported)*
- G-Q4, accepted as DEBT: an `optional` flatten-wrapper list loses null versus empty, only for a
  non-generated Arrow publisher. This matches C++'s storage.

**Tests, CI, documents**
- **T-D1:** the public-surface note is stale.
  - §2.7 omits `.fletcher.native.cs`, `csharp_model_only` and the topic class.
  - Its recipe glob misses `*.fletcher.native.cs` and `google/protobuf/*.fletcher.cs`.
  - §5.3 still shows `Subscribe(Action<..>)`.
- **T-D2:** the architecture diagrams are stale.
  - The native pair is still drawn `<<planned>>`.
  - "Emitted only when asked for" contradicts D-BIND-76.
  - There is no topic class (D-BIND-77).
  - Model box text predates 6c.
- **T-D3:** the no-drift test covers `""`, `ts`, `ts,ipc` and `schema_only`, not `accessor` or
  `rust`.
- **T-D4:** `ci.pr`'s `integration-protoc-dotnet` filter lacks `dotnet/Directory.Build.targets`.
  *(verified by reading)*
- **T-D5:** stale text.
  - The tracker's D-BIND-72 bullet says "emitted only when requested".
  - The 6b-2 slice note overclaims the topic test.
  - The exit gate names `CsharpVisitor.*`.
  - The protoc-dotnet README still says "until BIND-6c" and that the wire comparison arrives with 6d.
- T-D6: the Arrow-only model is proven on net10.0 only.
- **T-D7: answered.** A NativeAOT publish with warnings as errors was shown to fail, on IL3000, in
  6e-2's probe (`aot_probe2`). AotSmoke inherits `TreatWarningsAsErrors` from
  `Directory.Build.props`.
- T-D8: the generated subscriber's schema handling (S-9) has never run over a schema-less transport.
- **T-D9:** digest entries amended later are not annotated.
  - D-BIND-75's "a comment naming BIND-6e" was superseded by D-BIND-80.
  - D-BIND-72 point 3 was amended by D-BIND-76.

## NITs

- **Plugin:**
  - P-N1: an enum value named `_` gives an empty member name, as in protoc.
  - P-N2: a package segment `_1` gives a namespace segment `1`, as in protoc.
  - P-N3: `message Clone_ { int32 clone = 1; }` collides, as in protoc.
  - P-N4: `Outer.Inner` versus `Outer_Inner`, and service and method underscores, collide as in every
    backend.
  - P-N5: `Topic.Segments` exposes a mutable array through a cast.
- **Generated C# and runtime:**
  - Always-true validity bitmaps for non-nullable temporal columns.
  - The `HandlerFaulted` sender is the inner subscriber.
  - A null element in `Publish(IEnumerable<T>)` throws an NRE.
  - The provider-lifetime doc overstates the caller's burden.
- **Tests, CI, documents:**
  - T-N1: `ci.dotnet.yml:45-49`'s comment misdescribes what a missing golden does.
  - T-N2: two `GeneratedModelTests` cases test the BCL and Model rather than the generator.
  - T-N3: a text-only Arrow-expression test, and stale headers in the plugin tests.
  - T-N4: culture-dependent formatting in a symmetric `Describe`.

## Checked and found correct (from the three reports, condensed)

- **Naming:** protoc's naming functions as D-BIND-73 states them; the reserved list; namespace and
  nesting per D-BIND-69; D-BIND-82's check (segment boundary, before any output, only with `csharp`).
- **Options:** the two option tokens fold into three states, and the model is byte-identical in both
  C# states.
- **Topics:** segments match C++'s `EmitTopicSegments`.
- **Cross-file (D-BIND-80):** the cross-file path and its `global::` qualification.
- **Generator structure:**
  - the field set and order come from the same walk as the schema;
  - generated local names do not collide with each other;
  - recursion is skipped transitively;
  - output is deterministic, and no-drift holds by construction.
- **The generated pair:**
  - lifetimes, dispose order, and Dispose inside a handler;
  - per-delivery ownership (everything copied out, the batch disposed);
  - thread-safety of the shared codec;
  - `Publish(T)` and `Publish(IEnumerable<T>)` as D-BIND-78 says;
  - the net8.0 delegate (D-BIND-79).
- **ToArrow/FromArrow layout:** offsets, map entries in order with duplicates kept, list-of-list
  levels, null struct slots, and sliced input.
- **Model:** checked arithmetic, exact recount, range checks.
- **AOT:** no reflection, and the D-BIND-81 pin targets the right items.
- **CI and goldens:**
  - every new project runs in a lane;
  - the gate waits for the new lane;
  - sparse checkouts carry the goldens;
  - matrix counts agree;
  - no other backend's golden changed in the range.
