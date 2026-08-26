# GIR-13 — Option metadata on the IR schema visitor (design)

**Item:** GIR-13 · **Story/acceptance:** [GIR-generator-ir-rewrite.md § GIR-13](GIR-generator-ir-rewrite.md#gir-13--121-option-metadata-on-the-ir-schema-visitor)
**Locked decisions:** [GIR-locked-decisions.md](GIR-locked-decisions.md) (#2, #3, #5, #9, #10)
**Spec:** [docs/fletcher-options.md § `--fletcher_opt=metadata_from_option`](../docs/fletcher-options.md) (authoritative, **unchanged by this item**), [docs/robustness-plan.md § Phase 2](../docs/robustness-plan.md)
**Base:** `feature/generator-ir-rewrite` rebased onto `hard/3-7-consolidated` (locked base — already done)

**Revision 3 (2026-08-26)** — §6 T2 respecified against the re-review (its
grandchild key was unsatisfiable); T1 also pins the escaped **key**; fixture
spelled out in full; `TraceCpp`/`TraceNano` gain the resolver parameter. See
[Rework log](#rework-log-revision-3).
**Revision 2 (2026-08-26)** — reworked against the first Step-2 review.

> This file replaces an earlier orchestrator scoping note at the same path. Both
> of that note's load-bearing claims were re-verified against the tree and hold;
> several of its secondary claims about *what is missing* do not. See
> [Prior-art verification](#prior-art-verification).

## Summary

> **Revision 4 note (2026-08-26, PM).** Five files the "Files to touch" section
> lists as "no change" WERE edited during the review fix cycle:
> `protoc/src/option_metadata.cpp`, `protoc/include/option_metadata.hpp`,
> `protoc/tests/test_option_metadata.cpp`, and both
> `integration-tests/protoc-arrow-bridge/{proto/option_metadata.proto,tests/test_metadata_options.cpp}`.
> Reason: step-4b found MSVC **C4125** in the *generated* header (an octal escape
> terminated by an ASCII digit), which breaks a consumer building at `/W4 /WX`.
> The bug is in `EscapeCppStringLiteral`, which arrived with #121 and had **zero
> production callers** until this item's §4 put its output into a
> consumer-compiled header. Step-4a re-reviewed the deviation and returned
> CONFORMS: the "no change" rows were *porting* statements, not invariants like
> the RBA / goldens / `docs/fletcher-options.md` rows, which all still hold.
> Full rationale: `plans/reviews/GIR-13-conformance.md` (re-review section).
> Also corrected there: `test_option_metadata.cpp` holds **33** tests, not 32.


Thread the already-present, already-compiled `OptionMetadataResolver` into the
GIR-5 unified IR schema visitor so `--fletcher_opt=metadata_from_option` works
exactly as `docs/fletcher-options.md` specifies on both rendering paths. Three
pieces of real work: (1) accumulate the `(fletcher.flatten_field)` wrapper chain
in the visitor's own flatten walk, which currently discards it; (2) build one
metadata pair vector per node (builtins first, resolver extras appended) at the
single `SetMetadata` call site each; (3) escape resolver-supplied bytes in the
C++ source sink, which today writes metadata raw into string literals. No wire
change, no `fletcher-options.md` change, RBA untouched.

## Prior-art verification

Both claims the scoping note flagged for verification are **true**:

- **(a) The flatten wrapper is discarded.** `cpp_backend_schema_visitor.cpp:74-78`
  recurses into `fd->message_type()` and `continue`s without recording `fd`, so
  from a recorded leaf there is no path back to the wrapper field. `ForField`'s
  `flatten_chain` argument is therefore unreachable today.
- **(b) The C++ sink writes metadata raw.** `CppSchemaSink::SetMetadata`
  (`cpp_backend_schema_visitor.cpp:232-236`) interpolates `key`/`value` directly
  between `ArrowCharView("` and `")`. Safe for the four builtins, unsafe for
  arbitrary option bytes (a `"` breaks the header; `\` mis-escapes; non-ASCII
  makes the header's meaning depend on the compiler's source encoding).

Corrections to the scoping note — these shrink the item materially:

| Scoping-note claim | Reality in the tree |
|---|---|
| "`option_metadata.{hpp,cpp}` port from `main`" | Already present **and compiled** (`protoc/CMakeLists.txt:27`). Nothing to port. Do not edit them. |
| "port #121's 3 integration TUs + CMakeLists/conanfile deltas" | Already present **and fully wired**: `tests/test_metadata_options.cpp`, `test_metadata_nodrift.cpp`, `test_ipc_parity.cpp`; `proto/option_metadata.proto`; the `_META_RULES` / `generate_metadata_headers` block (`CMakeLists.txt:111-158`) and both test targets (`:244-246`, `:272`); `conanfile.py` `FLETCHER_METADATA_RULES` (`:54-73`). **Zero changes needed under `integration-tests/`.** |
| "port `test_option_metadata.cpp` from `main`" | Already present (32 tests, verified by count). Only the `protoc/tests/CMakeLists.txt` wiring line is missing. |
| "`generator_internal.hpp` include union" | **Not needed** — this design does not touch `FieldInfo` (see [Why not `FieldInfo`](#why-not-fieldinfo)). |
| "`SchemaFieldRecord::source_field` is nullptr for flatten-inlined leaves" (header comment at `cpp_backend_schema_visitor.hpp:49-53`) | **The header comment is wrong today.** The inlined-leaf recursion records the *inner* `fd` unconditionally (`:92`), so `source_field` is always the leaf descriptor and never null. That is exactly what `ForField` needs; the comment must be corrected as part of this item. |
| Risk "nodrift may need re-baselining" | **Non-issue.** `test_metadata_nodrift.cpp` compares two *live* plugin runs into temp dirs (`RunPlugin` → `ReadFileBytes`); there is no committed baseline. Nothing to re-baseline anywhere: escaping is a no-op on the builtins (identifiers, decimal integers, dotted paths), so with no rules every emitted byte is unchanged. |

## Design

### Data flow

```
--fletcher_opt=…            protoc CodeGeneratorRequest
      |                              |
      v                              v
ParseMetadataRules(parameter) --> if (!rules.empty())
                                     OptionMetadataResolver::Create(rules, file->pool())
                                            |  const*, NULL when no rules were given
      +-------------------------------------+------------------------------------+
      v                                                                          v
GenerateFile(file, schema_only, resolver)                        BuildMessageSchema(msg, resolver)
  -> GenerateSchemaFunction(cls, fields, msg, resolver)            -> BuildMessageSchemaInto(msg, s, resolver)
     -> GenerateSchemaFunctionFromIr(cls, msg, file, resolver)        -> BuildMessageSchemaIntoFromIr(msg, s, resolver)
                       \                                                          /
                        \______________  SchemaVisitor(msg, file, sink, resolver) /
                                               |
                        ONE pair vector per node, ONE SetMetadata call per node
                                               |
                        +----------------------+----------------------+
                        v                                            v
              CppSchemaSink::SetMetadata                  NanoarrowSchemaSink::SetMetadata
              (escapes at render time)                    (raw bytes, strlen-bounded)
```

The invariant #121 kept by hand ("both paths consume the identical pair vector")
becomes structural: there is exactly one place each pair vector is built, and two
renderers consume it. This is the argument for doing the reconciliation on the IR
rather than reverting GIR-5 (locked #5).

### 1. `SchemaFieldRecord` carries the flatten chain

`protoc/include/cpp_backend_schema_visitor.hpp`, on `SchemaFieldRecord` (`:44`):

```cpp
// The outer->inner (fletcher.flatten_field) wrapper fields this leaf was
// inlined through; empty for a field that was not inlined. Descriptor-level
// counterpart of the numeric `field_id` path: field_id "2.1" <-> chain
// {fd(2)} with source_field == fd(1)-of-the-wrapper's-message.
std::vector<const google::protobuf::FieldDescriptor*> flatten_chain;
```

`BuildFlattenedFieldListImpl` (`cpp_backend_schema_visitor.cpp:63`) gains a
`const std::vector<const FieldDescriptor*>& flatten_chain` parameter carried
**next to `id_prefix`** — the two are the same path expressed numerically and by
descriptor, so they must be threaded together or they can disagree:

```cpp
if (fd->type() == TYPE_MESSAGE && !fd->is_repeated() && HasFieldFlatten(fd)) {
    std::vector<const FieldDescriptor*> inner = flatten_chain;
    inner.push_back(fd);                       // append while descending => outer->inner
    BuildFlattenedFieldListImpl(fd->message_type(), out, path, inner);
    continue;
}
...
rec.flatten_chain = flatten_chain;             // chain of wrappers ABOVE this leaf
rec.source_field  = fd;                        // the leaf itself
```

**Orientation is load-bearing.** `OptionMetadataResolver::ForField`
(`option_metadata.cpp:488-492`) builds its candidate list as `{leaf}` followed by
`flatten_chain` iterated with `rbegin()/rend()`, i.e. it *requires* outer→inner
and yields innermost-wrapper-first ("most specific declaration wins"). Appending
on descent gives exactly that. A reversed chain still passes every fixture in the
tree — all of them are single-level — and fails only on a two-level wrapper. It
is therefore pinned by construction **and** by a required new test (§6 T3);
construction alone is not acceptable evidence.

The public `BuildFlattenedFieldList(msg, id_prefix = "")` signature does **not**
change (no caller passes a non-default prefix; the TS visitor calls it with one
argument and only reads `source_field`). Only the file-local `…Impl` grows the
parameter, with the top-level entry passing `{}`.

### 2. One pair vector per node, at the existing single call sites

`SchemaVisitor` gains `const OptionMetadataResolver* resolver_` (nullable,
explicitly defaulted `nullptr` in the ctor) and two private helpers so the
"builtins first, resolver extras appended" order lives in exactly one place per
scope:

```cpp
using MetaPairs = std::vector<std::pair<std::string, std::string>>;

MetaPairs SchemaVisitor::RootMetadata() const {
    MetaPairs pairs = {{"proto_package", message_->file()->package()},
                       {"proto_message",  message_->name()}};
    if (resolver_) Append(pairs, resolver_->ForMessage(message_));
    return pairs;
}

MetaPairs SchemaVisitor::FieldMetadata(const SchemaFieldRecord& f) const {
    MetaPairs pairs = {{"field_number", std::to_string(f.field_number)},
                       {"field_id",     f.field_id}};
    if (resolver_) Append(pairs, resolver_->ForField(f.source_field, f.flatten_chain));
    return pairs;
}
```

`Visit()` (`:436-457`) is otherwise unchanged: `sink_.SetMetadata(root,
RootMetadata())` and `sink_.SetMetadata(child, FieldMetadata(f))`.

- **No de-duplication at the emitter.** Per-key last-non-empty-wins and
  first-appearance ordering are already applied inside the resolver (`Upsert`,
  `option_metadata.cpp:454`). A resolver key can never collide with a builtin:
  the four generator-owned keys are rejected in `ParseMetadataRules`. So a plain
  append is both correct and the whole of the emitter's ordering duty.
- **Overlay order stays as-is.** For a nested struct child, `EmitNodeType`
  deep-copies first and the overlay then *replaces* the child's metadata with the
  field pairs. That is the documented behaviour ("when that same message is used
  as a field elsewhere, the nested schema's own metadata is replaced by the
  field's") and it must not be reordered; grandchildren keep theirs.
- **`List<Struct>`'s `item` child (and a map's `value` child) keep the
  deep-copied nested *root* metadata** (`proto_package`/`proto_message` plus any
  `message:`-scope keys), because the visitor's overlay applies to the *list* /
  *map* child, not to `item`/`value` — only `SetName` runs there (`:399`,
  `:421`). This sits oddly beside the spec's "`message` scope reaches only the
  schema root", but it is **pre-existing** (identical pre-GIR-5), **identical on
  both sinks**, and pinned by
  `MetadataOptionsTest.RepeatedFlattenPutsMetadataOnTheListFieldOnly`. Not a
  drift risk; called out so it is not "fixed" by accident — and §6 T2 uses it as
  the probe for the nested `RootMetadata` call.
- **Empty pair vector.** Unreachable — the builtins are always present — so no
  new "skip SetMetadata entirely" branch is introduced and no emitted-byte shape
  changes for the no-rules path.

### 3. Nested struct recursion must use the same resolver

`NanoarrowSchemaSink::DeepCopyMessageStruct` (`:318-323`) builds the nested
schema by re-entering `BuildMessageSchemaIntoFromIr`. If that recursion loses the
resolver, a nested struct's *grandchildren* lose their mapped metadata on the
in-process path only, while the generated C++ (which deep-copies the nested
`<Nested>Schema()` emitted *with* rules) keeps them — the `.ipc` file and the
runtime schema would then disagree. So the resolver must reach the recursion.

**Decision: pass it as a parameter on the sink operation, not as sink state.**

```cpp
virtual void DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                                   SchemaRef dst,
                                   const OptionMetadataResolver* resolver) = 0;
```

- The visitor is the **single owner** of the resolver pointer, so the nested
  build provably uses the same resolver as the top-level walk. The alternative
  (a resolver member on `NanoarrowSchemaSink`) creates two holders that a future
  refactor can desynchronise.
- `CppSchemaSink` ignores the argument (`(void)resolver;` + comment): it emits a
  *call* to `<Nested>Schema()`, which was generated by the same protoc
  invocation under the same rule list. Cross-invocation rule mismatch is already
  a documented limitation ("Rules are a build-wide property") — unchanged.
- `RecordingSink` in `protoc/tests/test_schema_visitor.cpp:336` must grow the
  parameter (mechanical; that file is being touched anyway for §6).
- `SchemaSink` is an internal plugin header (`protoc/include` is not installed —
  `protoc/CMakeLists.txt` installs only the executable), so this is not a public
  API change and triggers no version bump.

**There are TWO call sites, both in `EmitNodeType` (`:385-434`), and both must
pass `resolver_`:**

| Site | Node shape | Reached by |
|---|---|---|
| `:404` | `NodeKind::STRUCT` | a singular nested struct field **and** the element of `List<Struct>` / `List<List<…<Struct>>>` (the LIST case recurses into `EmitNodeType`) |
| `:419` | `NodeKind::MAP` with a struct **value** | `map<K, MessageWithOptions>` |

Missing `:419` loses grandchild metadata on the in-process path only — precisely
the `.ipc`-vs-runtime disagreement §3 exists to prevent — and **no fixture
anywhere in the tree uses `map<K, MessageWithOptions>`**, so it would be silent.
§6 T2 covers both sites explicitly.

### 4. Escaping — sink-local, uniform, octal

`CppSchemaSink::SetMetadata` applies `EscapeCppStringLiteral` (declared in
`option_metadata.hpp:95`) to **both** members of **every** pair:

```cpp
out_ << indent_ << "    ArrowMetadataBuilderAppend(&buf,\n"
     << indent_ << "        ArrowCharView(\"" << EscapeCppStringLiteral(key) << "\"),\n"
     << indent_ << "        ArrowCharView(\"" << EscapeCppStringLiteral(value) << "\"));\n";
```

Load-bearing details:

- **Escaping happens in the sink, never in the visitor.** The pair vector must
  stay raw bytes so both sinks receive *identical* input: the C++ sink renders
  into source that the compiler un-escapes back to those bytes, while the
  nanoarrow sink writes them as final metadata. Escaping in the visitor would
  compile fine and leave all 32 unit tests green while storing escape sequences
  literally in the in-process/`.ipc` bytes. This is the one place in the item
  where a silent wrong answer is possible, and the asymmetry is exactly why §6
  T1 asserts *both* layers.
- **Uniform, not conditional.** The builtins are escape-invariant (verified by
  `EscapeCppStringLiteralTest.PrintableAsciiIsUnchanged`, which pins
  `"proto_package"`, `"ARROW:extension:name"`, `"2.1"`, `""`), so applying it to
  everything is a byte-level no-op for them and removes a
  builtin-vs-resolver branch that could rot. **Keys need it too**: `arrow_key` is
  caller-named arbitrary bytes (`ParseMetadataRules` splits the token on its
  first two colons only and copies the remainder verbatim), so a key can legally
  contain `"` or `\`. §6 T1 asserts the escaped key, not just the escaped value.
- **Octal, never `\x`** — preserved verbatim from #121 and pinned by
  `OctalEscapeIsNotGreedyAcrossFollowingCharacters`: a C++ hex escape consumes an
  unbounded digit run, so `0x01` followed by `'A'` would emit `"\x01A"` and read
  back as one character. Do not "simplify" to hex.
- **`ArrowCharView` stays.** It is `strlen`-based, so a value containing NUL is
  truncated at emission — and the in-process sink truncates identically via
  `key.c_str()`/`value.c_str()` (`:309`). The two paths therefore still agree
  byte-for-byte, which is exactly what
  `EscapeCppStringLiteralTest.EmbeddedNulIsEscapedAsThreeDigitOctal`'s comment
  documents. Do **not** switch to explicit-length `ArrowStringView`: it would
  change generated source for every message for no contract gain, and NUL-bearing
  metadata is not a promised capability.
- `CppSchemaSink::SetName` (`:215`) stays unescaped: names come from
  `FieldDescriptor::name()` / the literals `"item"`/`"value"`, all proto
  identifiers.

### 5. Option plumbing in `generator.cpp`

- `#include "option_metadata.hpp"`.
- In `ArrowRowGenerator::Generate`, **after** the `--fletcher_opt` token loop
  (`:1789-1804`; unclaimed tokens already fall through harmlessly, so
  `metadata_from_option=…` needs no case there) and **before**
  `ValidateBackendsSupportFields` (`:1810`):

  ```cpp
  std::vector<MetadataRule> rules;
  if (!ParseMetadataRules(parameter, &rules, error)) return false;

  // No rules => NO resolver. Keeps `resolver == nullptr` literally synonymous
  // with "no metadata_from_option was passed", which is what the locked-#2
  // golden argument rests on, and avoids building a DynamicMessageFactory on
  // the overwhelmingly common path.
  std::unique_ptr<OptionMetadataResolver> resolver;
  if (!rules.empty()) {
      std::string rule_error;
      resolver = OptionMetadataResolver::Create(std::move(rules), file->pool(), &rule_error);
      if (!resolver) { *error = rule_error; return false; }
  }
  ```

  **Ordering rationale.** Rule errors are invocation-wide and file-independent,
  so reporting them first keeps the diagnostic stable regardless of which file
  protoc happens to process first; `ValidateNoUnsupportedIr` (#55) already runs
  ahead of everything (`:1781`). The scoping note's worry that a resolver error
  could mask GIR-10's backend error is unfounded either way: rule compilation
  reads only the flag string and the descriptor **pool**, never field *shapes*,
  so neither validator can mask the other's root cause. Both are hard errors with
  distinct messages.
- `Create` is invoked once per `Generate()` (i.e. per input file), mirroring
  #121. `file->pool()` is the same pool for every file of one invocation, so the
  compiled rule set — including which rules are dropped as "extension absent from
  the pool" — is identical across files. Cost is negligible.
- Thread `const OptionMetadataResolver* resolver` (nullable) through, all with an
  **explicit** `= nullptr` default so existing call sites and tests keep
  compiling at today's arity (`test_schema_visitor.cpp:273`, `:355-369`;
  `test_schema_builder.cpp`'s nine one-argument `BuildMessageSchema` calls):
  - `GenerateFile(file, schema_only, resolver)` (`:1281`) → `GenerateSchemaFunction(cls, fields, msg, resolver)` (`:823`, `:1370`) → `cpp_backend::GenerateSchemaFunctionFromIr(cls, msg, msg->file(), resolver)`.
  - `BuildMessageSchemaInto(msg, schema, resolver)` (`:915`) → `cpp_backend::BuildMessageSchemaIntoFromIr(msg, schema, resolver)`.
  - the `emit_ipc` loop's `BuildMessageSchema(msg, resolver)` (`:1851`).
- `protoc/include/schema_builder.hpp:21` becomes:

  ```cpp
  nanoarrow::UniqueSchema BuildMessageSchema(
      const google::protobuf::Descriptor* msg,
      const OptionMetadataResolver* resolver = nullptr);
  ```

  Required verbatim by `test_option_metadata.cpp` (`BuildMessageSchema(msg,
  resolver.get())` and `BuildMessageSchema(msg, nullptr)`). This header includes
  `option_metadata.hpp` for the type.

**Do NOT revive the dead helpers.** `generator.cpp` still carries
`EmitNanoarrowTypeSetup` (`:642`), `SetScalarSchemaType` (`:845`),
`SetMetadataPairs` (`:884`) and `RequireNestedMsg` (`:902`) — leftovers of the
pre-GIR-5 emitter pair, all in the anonymous namespace `:634`-`:1730` with zero
references tree-wide. #121 wired the resolver into `SetMetadataPairs`' ancestor
and emitted metadata text next to `EmitNanoarrowTypeSetup`, so an implementer
porting from `main` by grep will land in one of those two. Both are dead; all
metadata now flows through the visitor. Leave the leftovers alone (removing them
is not this item's scope) and wire nothing into them.

### 6. Tests

**Wire the existing unit TU:** add `test_option_metadata.cpp` to
`protoc/tests/CMakeLists.txt`'s `add_executable(fletcher_proto_plugin_tests …)`
list. No extra link deps (it uses `google::protobuf::compiler::Parser` from
`libprotoc` and nanoarrow, both already `PUBLIC` on `fletcher_plugin_core`).
Leave the TU's contents byte-for-byte as ported — it is the forcing suite.

**Nothing to add under `integration-tests/`** — already wired (see the
corrections table).

**Four new unit tests in `protoc/tests/test_schema_visitor.cpp`** (GIR-5's own
TU, whose stated job is the two-sink invariant). Rationale: the three
behaviours this item adds that can fail *silently* — the escaping **layer**, the
resolver reaching the **nested** build, and chain **orientation** — are today
covered only by the Conan-gated integration lane, and `test_option_metadata.cpp`
cannot cover them (it asserts only in-process schemas, and it builds `Pos`
**directly** (`:618-632`) rather than through `Sample.pos`, so it never exercises
a deep-copied grandchild).

Test-plumbing prerequisites in that TU:
`RecordingSink::DeepCopyMessageStruct` (`:336`) gains the resolver parameter
(§3), and `TraceCpp`/`TraceNano` (`:355-369`) gain a **defaulted**
`const OptionMetadataResolver* resolver = nullptr` that they forward to the
`SchemaVisitor` ctor — without it T4 cannot run "with a resolver active", and
with the default `CppAndIpcByteIdentical` keeps calling them unchanged.

#### Shared fixture

A source-text pool (`google::protobuf::compiler::Parser`, same shape as
`test_option_metadata.cpp:114-145`'s `FixturePool`; extract or duplicate, either
is fine). `flatten`/`flatten_field` are declared locally at extension **number
50000** because the plugin matches by number, not by name. Copy-able in full:

```proto
syntax = "proto3";
package sv;
import "google/protobuf/descriptor.proto";

// Option carriers — deliberately neutral; the resolver must key off no vocabulary.
message ColOpts {                       // FieldOptions payload
  string meta  = 1;                     // chain-independent, field-scope
  string nasty = 2;                     // T1's arbitrary-bytes value
}
message TypeDef {                       // MessageOptions payload
  string unit  = 1;
  string group = 2;
}
extend google.protobuf.FieldOptions   { ColOpts col = 60100; bool flatten_field = 50000; }
extend google.protobuf.MessageOptions { TypeDef typ = 60101; bool flatten       = 50000; }

// --- nested-recursion fixtures (T2) ---
message Coord {
  option (sv.typ) = { unit: "m", group: "g-coord" };
  double x = 1 [(sv.col) = { meta: "mx" }];   // field-scope key ON THE LEAF
  double y = 2;
}
message Pos {
  option (sv.typ) = { unit: "deg" };
  Coord coord = 1 [(sv.flatten_field) = true];
}
message Sample {
  Pos pos = 1;                          // struct child          -> EmitNodeType :404
  map<string, Coord> byname = 2;        // map w/ struct value    -> EmitNodeType :419
  repeated Coord path = 3;              // list<struct>           -> :404 via LIST recursion
}

// --- escaping fixture (T1) ---
message Nasty { double v = 1 [(sv.col) = { nasty: "{\"crs\":\"EPSG:4326\"}\\\001\302\260" }]; }

// --- two-level flatten_field (T3): a distinct key declared on EACH wrapper ---
message Inner { option (sv.typ) = { unit: "inner" };               double leaf = 1; }
message Mid   { option (sv.typ) = { unit: "mid", group: "g-mid" }; Inner inner = 1 [(sv.flatten_field) = true]; }
message Outer { Mid mid = 1 [(sv.flatten_field) = true]; }
```

Rules (order matters only for same-key rules; these keys are disjoint):

| Rule token | Used by |
|---|---|
| `field:sv.col.meta:x:meta` | **T2** — field scope, so it needs no flatten chain |
| `field_type:sv.typ.unit:x:unit` | T2 (`pos`, `path`), **T3** (innermost-wins) |
| `field_type:sv.typ.group:x:group` | **T3** (outer-wrapper key still reaches the leaf) |
| `message:sv.typ.group:x:group` | **T2** — probes the nested `RootMetadata` call |
| `field:sv.col.nasty:<kNastyKey>` | **T1** — arbitrary bytes in both key and value |

The two `…typ.group…` rules share the arrow key `x:group` but cannot interfere:
`ForMessage` considers only `kMessage` rules and `ForField` only the other two
scopes (`option_metadata.cpp:471`, `:495`).

#### T1 `SchemaVisitor.CppSinkEscapesResolverBytesAndInProcessKeepsThemRaw`

Pins the *layer* §4 escapes in, for **key and value**. Build both expectations in
C++ so the test is self-checking against the fixture's proto-source escaping
(`.proto` string literals support the same 3-digit octal form):

```cpp
const std::string kNasty    = "{\"crs\":\"EPSG:4326\"}\\" "\x01" "\xC2\xB0";
const std::string kNastyKey = "x:k\"\\q";     // literally  x:k"\q  — legal: the
                                              // token splits on the first two
                                              // colons only, key copied verbatim
```

- `GenerateSchemaFunctionFromIr("Nasty", msg, msg->file(), resolver.get())` → the
  emitted text **contains** `"ArrowCharView(\"" + EscapeCppStringLiteral(kNastyKey) + "\")"`
  **and** `"ArrowCharView(\"" + EscapeCppStringLiteral(kNasty) + "\")"`, and
  **contains neither** `kNasty` nor `kNastyKey` raw.
- `BuildMessageSchema(msg, resolver.get())` → the `v` child's `kNastyKey` value
  equals `kNasty` **exactly** (raw bytes, no escape sequences).

Failure discrimination: no escaping ⇒ the two positive source assertions go red
(the escaped render is pure ASCII, so the 0x01/0xC2 0xB0 raw bytes cannot appear
in it); escaping the value only ⇒ the key assertion goes red; escaping in the
visitor ⇒ the source assertions pass but the in-process assertion goes red;
double escaping ⇒ the positive source assertions go red.

#### T2 `SchemaVisitor.NestedStructGrandchildrenKeepMappedMetadata`

Covers §3 and **both** `DeepCopyMessageStruct` sites. The grandchild key must be
**`field:`-scope, declared on `Coord`'s own leaf** (`x:meta = "mx"`): a
`field_type:` key cannot reach a *scalar* grandchild, because those grandchildren
are reached through a **fresh nested build with an empty flatten chain** and
`field_type` skips non-message candidates (`option_metadata.cpp:501`; the tree
documents this at `test_option_metadata.cpp:490-491`). So `x:meta` depends on
exactly one thing — the resolver reaching the nested build.

`BuildMessageSchema(Sample, resolver)`, then assert:

| Node | Key | Expected | Pins |
|---|---|---|---|
| `pos` → `x` | `x:meta` | `mx` | `:404` singular-struct deep copy |
| `byname` → `entries` → `value` → `x` | `x:meta` | `mx` | **`:419` map-value-struct** — the only coverage in the tree |
| `path` → `item` → `x` | `x:meta` | `mx` | `:404` via the LIST recursion |
| `byname` → `entries` → `value` | `x:group` | `g-coord` | the nested **`RootMetadata`** (`ForMessage`) call inside the recursion — kept because no overlay applies to a `value`/`item` child (§2) |
| `path` → `item` | `x:group` | `g-coord` | same, list side |
| `pos` → `x` | `x:unit` | `m` | flatten chain inside the nested build (`Pos` inlines `coord`) |
| `pos` | `x:unit` | `deg` | the overlay **replaced** the deep-copied root metadata |
| `path` | `x:unit` | `m` | spec: for `repeated <wrapper>` the metadata lands on the **list field**, not the element |
| `byname` | `x:unit` / `x:meta` | absent | a map field's message type is the synthetic `MapEntry` (`option_metadata.cpp:504`) |

Dropping `resolver_` at `:404` or `:419` turns the corresponding row red for the
right reason.

#### T3 `SchemaVisitor.TwoLevelFlattenFieldChainPrefersTheInnermostWrapper`

The required orientation guard (§1). `BuildMessageSchema(Outer, resolver)` has
one child `leaf`; assert `field_id == "1.1.1"`, `x:unit == "inner"` (**not**
`"mid"` — innermost wins) and `x:group == "g-mid"` (a key declared *only* on the
outer wrapper still reaches the leaf, so the whole chain is searched, not just
its last element). A reversed chain fails the first; a chain truncated to its
innermost element fails the second.

#### T4 `SchemaVisitor.CppAndIpcTracesAgreeWithResolverActive`

Keeps a cheap ordering guard: `RecordingSink`-wrapped `TraceCpp(msg, resolver)
== TraceNano(msg, resolver)` over `Sample`/`Pos`/`Outer`, which pins top-level
**key order and content parity** (builtins first, extras appended) and the
deep-copy call sequence. Documented blind spots — `RecordingSink::SetMetadata`
logs the raw pair vector *above* `CppSchemaSink`'s escaping, and the nested
nanoarrow build runs on a fresh sink the recorder never wraps — are covered by T1
and T2 respectively. Do not present T4 alone as evidence for §3 or §4.

`SchemaVisitor.CppAndIpcByteIdentical` itself is unchanged and must stay green
(no resolver → no behavioural delta).

### Why not `FieldInfo`

#121 added `flatten_chain` to `FieldInfo` (`generator_internal.hpp:45`) because
the flat generator's schema emitter walked `GatherFields`. Post-GIR-5 the schema
path walks `SchemaFieldRecord` instead, and the remaining `FieldInfo` consumers
(edge row class setters/getters, the read-only RBA accessor emitter, the
`FieldMapping` bridge) emit no metadata. Adding the chain there would create a
second, unused source of truth that must be kept in step with the visitor's walk
— exactly the hand-maintained lockstep this round exists to delete. `FieldInfo`,
`generator_internal.hpp` and the RBA emitter are therefore untouched (locked #3).

### Safety / lifetime / concurrency / error paths

- **The resolver is internally memoising — it is NOT read-only and NOT
  thread-safe.** `OptionMetadataResolver::Impl` holds
  `mutable google::protobuf::DynamicMessageFactory factory;` and
  `mutable std::map<const Message*, std::unique_ptr<Message>> cache;`
  (`option_metadata.cpp:241-245`), both mutated from the `const` methods
  `ParsedOptions`/`Evaluate` (`:247-260`). `ForMessage`/`ForField` are therefore
  `const` in signature only.
  **Contract:** the resolver must stay a **per-`Generate()` stack-local**, used
  by exactly one thread for the duration of that call. It must **not** be hoisted
  to a member of `ArrowRowGenerator`, a static/global, a cross-file cache, or
  shared with any parallel emission. It is safe today only because the plugin is
  serial (`Generate`/`GenerateAll` are driven sequentially by protoc), not
  because the object is immutable. Any future parallelism over messages/files
  needs one resolver per worker (or external locking) — and note the memo cache
  is keyed on descriptor *addresses*, which are stable for the pool's lifetime,
  so per-worker instances are correct, just less warm.
- **Lifetime.** The resolver is a stack-local `unique_ptr` in `Generate()`; every
  consumer holds a `const*` for the duration of that call only. Nothing stores it
  past `Generate()` returning. `SchemaVisitor`/`CppSchemaSink` are per-call
  objects.
- **Nullability, and what the locked-#2 argument rests on.** Two distinct cases,
  both byte-neutral, for two distinct reasons — this must not be conflated:
  1. **No `metadata_from_option` token at all** ⇒ `rules.empty()` ⇒ **`resolver
     == nullptr`** (§5) ⇒ the `if (resolver_)` guards make `RootMetadata`/
     `FieldMetadata` bit-for-bit the current expressions. This is the path the 10
     committed `protoc/tests/golden/*.ipc` files and every existing golden/
     no-drift baseline take, so **locked #2 rests on the null guard**, literally.
  2. **Rules given but matching nothing** (e.g. the extension is absent from the
     pool, so `Create` drops the rule) ⇒ a **non-null, zero-compiled-rule**
     resolver whose `ForMessage`/`ForField` return empty vectors ⇒ appending is a
     no-op. This is the path `MetadataNoDriftTest.InertRulesLeaveEveryOutputByteIdentical`
     pins, and it rests on the empty-append property, not on the guard.
  `ForField(nullptr, …)` also returns empty (`option_metadata.cpp:483`), so a
  hypothetical record without a `source_field` degrades to builtins only rather
  than crashing.
- **Errors.** Statically determinable rule problems are hard protoc errors from
  `ParseMetadataRules`/`Create` (spec § Errors); descriptor-content misses are
  silent skips, produced inside the resolver. The visitor adds no new error path.
  The `emit_ipc` loop's existing `try/catch` around `BuildMessageSchema` still
  reports nanoarrow failures per message.
- **RBA interaction.** The generated accessor compares types with
  `check_metadata=false` and exposes metadata generically/positionally, so extra
  keys cannot break it. `MetadataNoDriftTest` pins that rules add and remove no
  output files.

## Forcing-test mapping

| Forcing test | Design element that turns it green |
|---|---|
| `OptionMetadataTest.FlattenFieldWrapperContextReachesEachInlinedLeaf` | Pure resolver semantics (`ForField(x, {coord})`) — green on the `protoc/tests/CMakeLists.txt` wiring line alone (§6); **§1 is not needed for it**. It is the tracker's named forcing test but the weakest of the four; the round must not be closed on it (see Files-to-touch). |
| `OptionMetadataTest.InlinedLeavesInheritTheFlattenFieldWrapper` (`x`/`y` get `x:unit=m`, `field_id` `1.1`) | §1 chain accumulation + §2 `FieldMetadata` passing `f.flatten_chain` to `ForField`. Red while the wrapper is discarded. |
| `OptionMetadataTest.FlattenedWrapperFieldCarriesTheWrapperMessagesMetadata` | §2 `field_type` scope over `f.source_field->message_type()`; message-level flatten keeps the referencing field, so no chain is needed — `source_field` (already set) suffices. |
| `OptionMetadataTest.FieldScopeOverridesInheritedFieldTypeScopePerKey` | Resolver `Upsert` per-key last-wins + §2 appending the resolver vector unmodified. |
| `OptionMetadataTest.BuiltinKeysSurviveAndMappedKeysAreAppended` | §2 builtins-first/extras-appended in `RootMetadata`/`FieldMetadata`. |
| `OptionMetadataTest.NullResolverEmitsExactlyTheFourBuiltinKeys` | §5 `BuildMessageSchema(msg, nullptr)` overload + the `if (resolver_)` guards. |
| `MetadataRuleParseTest.*`, `MetadataRuleCompileTest.*`, `EscapeCppStringLiteralTest.*` (15 tests) | Already-correct `option_metadata.{hpp,cpp}`; green on wiring alone. §4 is what makes the escaper actually *used*. |
| `MetadataOptionsTest.JsonValueSurvivesCppStringLiteralEscaping` (compiled generated C++) | §4 escaping in `CppSchemaSink::SetMetadata`. Without it the generated header does not even compile. Locally mirrored, without Conan, by **§6 T1**. |
| `MetadataOptionsTest.NestedStructChildMetadataSurvivesTheDeepCopy` | §2 overlay-after-deep-copy order (unchanged) + §3 resolver reaching the nested build. Locally mirrored by **§6 T2**. |
| `MetadataOptionsTest.FlattenFieldInlinedLeavesInheritTheOuterWrapper` | §1 + §2, end-to-end through compiled generated code. |
| `MetadataOptionsTest.RepeatedFlattenPutsMetadataOnTheListFieldOnly` | §2 list-child overlay / `item`-child pass-through (unchanged); mirrored by T2's `path` rows. |
| `IpcParityTest.MappedMetadataIpcFileMatchesRuntimeSchemaBytes` / `…OnFlattenedFieldsMatchesRuntimeSchemaBytes` | §3 (nested recursion carries the resolver, **both** `EmitNodeType` sites) + §4 (escaping round-trips exactly) + §2 (identical pair vector ⇒ identical key order). This pair is the strongest assertion in the item: byte identity of the two rendered outputs *with rules active*. |
| `MetadataNoDriftTest.InertRulesLeaveEveryOutputByteIdentical` | §5 plumbing + the zero-compiled-rule empty-append property (Nullability case 2). |
| `MetadataNoDriftTest.RepeatedRunsWithTheSameRulesAreDeterministic` | Deterministic pair order: resolver `Upsert` first-appearance ordering + §2's fixed builtins-then-extras append; no map/set iteration reaches the emitted order. |
| `MetadataNoDriftTest.RulesActuallyChangeOutputWhenTheyMatch` | Anti-vacuity control for the two above; §2 must actually append. |
| `MetadataNoDriftTest.MalformedRulesFailProtocWithNonZeroExit` / `…RulesTargetingReservedKeysFail…` | §5 hard-error placement (`ParseMetadataRules`/`Create` failure ⇒ `return false` ⇒ non-zero protoc exit). |
| `SchemaVisitor.CppAndIpcByteIdentical` + the 10 `protoc/tests/golden/*.ipc` | Unchanged code path because **no rules ⇒ `resolver == nullptr`** (§5) ⇒ the `if (resolver_)` guards leave both pair expressions bit-identical; escaping is a byte-level no-op on builtins. A moved golden means the resolver leaked into the no-rules path (locked #2). |
| **§6 T1** `CppSinkEscapesResolverBytesAndInProcessKeepsThemRaw` | §4 — pins escaping to the C++ sink layer, for key **and** value, *and* raw bytes in-process. |
| **§6 T2** `NestedStructGrandchildrenKeepMappedMetadata` | §3 — both `EmitNodeType` deep-copy sites (`:404`, `:419`) via a chain-independent `field:`-scope key, plus the nested `RootMetadata` call. Only map-value-struct coverage in the tree. |
| **§6 T3** `TwoLevelFlattenFieldChainPrefersTheInnermostWrapper` | §1 — chain orientation and full-chain search, otherwise pinned by construction only. |
| **§6 T4** `CppAndIpcTracesAgreeWithResolverActive` | §2 — top-level key order/content parity across both sinks with a resolver active (acceptance clause "byte-identical **with a resolver active**", unit level). |
| `test_schema_builder` (9 one-arg `BuildMessageSchema` calls), `TsVisitor.DescriptorByteIdentical`, RBA no-drift | Explicit `= nullptr` defaults + `FieldInfo`/RBA untouched. |

Locked #9 (red-first for feature items) is satisfied by tests that already exist
and already fail: `FlattenFieldWrapperContextReachesEachInlinedLeaf` fails at
*link/compile* today (`BuildMessageSchema` two-arg overload absent, TU unwired),
and once wired the flatten and escaping tests fail for the right reason. The
existing suite remains **the** forcing suite; T1–T4 are supplementary guards and
are compile-red before the change, so they claim no red-first exemption.
**Confirm each of the four flatten tests plus T1/T2/T3 is red before
implementing**, and record which failure mode each showed.

## Risks / Unknowns

1. **Escaping is the only place a silent wrong answer is possible.** Everything
   else fails loudly. Do not weaken any of the 8 `EscapeCppStringLiteralTest`
   cases, do not move escaping out of the sink, do not switch octal→hex. §6 T1 is
   the local guard; `IpcParityTest` is the end-to-end one.
2. **Chain orientation** (now required as §6 T3). Outer→inner is required by
   `ForField`'s `rbegin()` scan; no two-level `flatten_field` fixture exists
   anywhere in the tree, so without T3 a reversed chain is green.
3. **The `:419` map-value-struct deep-copy site is unfixtured** outside §6 T2. If
   T2 is dropped or narrowed — in particular if its `x:meta` rows are weakened
   back to a `field_type:` key, which **cannot** reach a scalar grandchild — that
   site becomes silent drift again. This is the trap the re-review caught; the
   `field:`-scope choice is load-bearing, not stylistic.
4. **Very large option values.** An escaped value can exceed MSVC's ~16380-byte
   string-literal limit (C2026). Pre-existing in #121 and not promised by
   `fletcher-options.md`; accepted, not fixed here.
5. **NUL-bearing values are truncated** identically on both paths (`ArrowCharView`
   is `strlen`-based). Accepted and documented by the ported test's own comment;
   changing it is a behaviour change outside this item.
6. **Integration harness requires Conan.** The three integration TUs need
   `conan build .` in `integration-tests/protoc-arrow-bridge` (the CMakeLists
   hard-errors without `FLETCHER_METADATA_RULES`). §6 T1–T4 exist so that the
   item has local evidence for every silent-failure mode even if that lane cannot
   be run; if it is skipped, say so explicitly rather than marking the item green.
7. **Assumption:** the rebase left `option_metadata.{hpp,cpp}` at `main`'s
   content (verified by reading both; `ForField`/`Upsert`/`EscapeCppStringLiteral`
   match the ported tests' expectations). If a reviewer finds a divergence, fix
   the divergence rather than adapting the emitter to it.
8. **Fixture escaping is fiddly** (T1's value exists in both `.proto` source and
   C++ source). The in-process assertion is self-checking: if the two spellings
   drift, T1 goes red with a clear diff rather than silently passing.

**No STOP-AND-ASK.** Nothing here is in tension with a locked decision: wire
bytes are untouched (#2), RBA/`FieldKind` untouched (#3), the two-sink
unification is strengthened rather than bypassed (#5), the change is confined to
`protoc/` + protoc tests (#10), and no installed public header changes.

## Files to touch

> See the **Revision 4 note** under Summary: five rows below that read
> "no change" were overridden during the review fix cycle, with step-4a
> approval recorded in `plans/reviews/GIR-13-conformance.md`.

| Path | Change |
|---|---|
| `protoc/include/cpp_backend_schema_visitor.hpp` | `flatten_chain` on `SchemaFieldRecord` (`:44`); correct the wrong `source_field` comment (`:49-53`); `resolver` param on `DeepCopyMessageStruct` (`:106`, `:128`, `:160`); `resolver` ctor param (explicit `= nullptr`) + `RootMetadata`/`FieldMetadata` on `SchemaVisitor` (`:170-183`); `resolver` param (explicit `= nullptr`) on both entry points (`:188-193`); `#include "option_metadata.hpp"` |
| `protoc/src/cpp_backend_schema_visitor.cpp` | chain accumulation in `BuildFlattenedFieldListImpl` (`:63-95`); `EscapeCppStringLiteral` on **key and value** in `CppSchemaSink::SetMetadata` (`:226-241`); resolver forwarded to the recursive build in `NanoarrowSchemaSink::DeepCopyMessageStruct` (`:318-323`); **`EmitNodeType` (`:385-434`) — pass `resolver_` at BOTH deep-copy sites, `:404` (STRUCT, also reached via the LIST recursion) and `:419` (MAP struct value)**; `RootMetadata`/`FieldMetadata` + `Visit()` (`:436-457`); entry points (`:463-485`) |
| `protoc/include/schema_builder.hpp` | `BuildMessageSchema` resolver parameter (`:21`), defaulted `nullptr`; include `option_metadata.hpp` |
| `protoc/src/generator.cpp` | `ParseMetadataRules` + conditional `Create` in `Generate` (`~:1804`); thread through `GenerateFile` (`:1281`, `:1370`), `GenerateSchemaFunction` (`:823`), `BuildMessageSchemaInto` (`:915`), `BuildMessageSchema` (`:1765`), the `emit_ipc` loop (`:1851`); include `option_metadata.hpp` |
| `protoc/tests/CMakeLists.txt` | add `test_option_metadata.cpp` to the test executable |
| `protoc/tests/test_schema_visitor.cpp` | `RecordingSink::DeepCopyMessageStruct` gains the param (`:336`); `TraceCpp`/`TraceNano` (`:355-369`) gain a **defaulted** resolver param they forward to the visitor; new source-text option fixture + **T1–T4** (§6) |
| `plans/GIR-generator-ir-rewrite.md` | tracker row `:100` — note that the named forcing test goes green on wiring alone, so the round closes on the §6 T1–T3 + integration set, not on it; status bookkeeping at close |
| `plans/GIR-progress-log.md` | progress entries |
| `protoc/include/option_metadata.hpp`, `protoc/src/option_metadata.cpp` | **no change** |
| `protoc/tests/test_option_metadata.cpp` | **no change** (already in tree; only wired) |
| `integration-tests/**` | **no change** (already present and wired) |
| `docs/fletcher-options.md` | **no change** — user-facing contract is unaltered |
| `protoc/include/generator_internal.hpp`, RBA emitter, `protoc/tests/golden/*.ipc` | **no change** |

## Out of scope

- Any change to the `metadata_from_option` grammar, scopes, value rendering,
  precedence or error classification (`docs/fletcher-options.md` is authoritative).
- Removing `generator.cpp`'s dead pre-GIR-5 schema helpers.
- Explicit-length metadata views / NUL-safe metadata.
- Mapped metadata in `--fletcher_opt=ts` output (documented limitation).
- Changing the `List<Struct>` `item` / map `value` child metadata behaviour
  (§2, pre-existing).
- Migrating the RBA accessor onto the IR — round **RIR** (locked #3).
- Any wire-format change (locked #2).

## Rework log (revision 3)

| Re-review item | Resolution |
|---|---|
| 1 (blocking) — T2 unsatisfiable, `:419` still uncovered | T2 respecified around a **`field:`-scope key declared on `Coord`'s own leaf** (`x:meta = "mx"`), which is chain-independent and therefore depends only on the resolver reaching the nested build; `Coord.x` gains `[(sv.col) = { meta: "mx" }]`. Assertions are now a table: `x:meta` at all three grandchildren (`pos→x`, `byname→entries→value→x`, `path→item→x`), plus the reviewer's optional extras — `message:`-scope `x:group == "g-coord"` on the `value`/`item` children (pins the nested `RootMetadata` call), `x:unit == "m"` on `pos→x` (chain) and on `path` (spec's list-field rule), `x:unit == "deg"` on `pos` (overlay), and `byname` correctly carrying nothing. Why `field_type:` cannot work is written into T2 itself and into risk #3 so it is not "simplified" back. |
| 2 — `TraceCpp`/`TraceNano` need a resolver param | Added as an explicit test-plumbing prerequisite in §6 and to the `test_schema_visitor.cpp` Files-to-touch row: a **defaulted** `const OptionMetadataResolver* = nullptr` forwarded to the visitor ctor, so `CppAndIpcByteIdentical` keeps calling them unchanged. T4 now reads `TraceCpp(msg, resolver)`. |
| 3 — T1 pins only the escaped value | T1 now also pins the escaped **key**: the rule uses `kNastyKey = x:k"\q` (legal — the token splits on the first two colons only), and T1 asserts both escaped forms present and both raw forms absent. §4's "keys need it too" bullet now names the mechanism and points at T1. New failure-mode row: escaping the value only ⇒ key assertion red. |
| 4 — fixture omits option carriers | §6 now carries a complete, copy-able `.proto` fixture: `ColOpts{meta,nasty}`, `TypeDef{unit,group}`, both `extend` blocks (incl. `flatten`/`flatten_field` at 50000), all seven messages with their options, plus a rules table saying which test each rule serves and why the two `x:group` rules cannot interfere. |
| 5 — wording ("proto text format") | Corrected: the fixture is `.proto` **source** parsed by `compiler::Parser`, so it is proto-language string-literal escaping (same 3-digit octal). Self-checking remark kept and promoted to risk #8. |

## Rework log (revision 2)

| Review item | Resolution |
|---|---|
| 1 — new guard blind to both failure modes | §6 rewritten. T4 (trace) demoted to an *ordering* guard with its two blind spots stated explicitly; new **T1** pins the escaping layer (escaped literal present in emitted source + raw value absent, **and** raw bytes in the in-process schema) and new **T2** asserts a deep-copied **grandchild** (`Sample.pos` → `x`), which `test_option_metadata.cpp` never reaches because it builds `Pos` directly. |
| 2 — `EmitNodeType` missing, two deep-copy sites | Added a dedicated table in §3 for `:404` / `:419` with how each is reached; added `:385-434` to Files-to-touch; T2 covers the map-value-struct site (the only coverage in the tree) plus `List<Struct>`. New risk #3. |
| 3 — safety section factually wrong | "Safety / lifetime / concurrency" rewritten: the resolver **memoises** via `mutable factory`/`cache` (`option_metadata.cpp:241-245`) mutated from `const` methods, so it is neither read-only nor thread-safe. Stated as a contract: per-`Generate()` stack-local, never hoisted to a member/static/shared object, never touched from a second thread; per-worker instances if parallelism ever lands. |
| 4 — nullptr-vs-empty-resolver contradiction | Resolved by **construction**: §5 now creates the resolver only `if (!rules.empty())`, so `resolver == nullptr` is literally "no rules given". The Nullability paragraph and the `golden/*.ipc` mapping row now distinguish case 1 (no token ⇒ null guard ⇒ locked #2) from case 2 (rules given but matching nothing ⇒ non-null, zero-rule, empty-append ⇒ `MetadataNoDrift`). |
| 5 — promote orientation hardening to required | §6 **T3** is now a required test: two-level `A.b`→`B.c`→`C.leaf` wrapper chain, asserting `field_id == "1.1.1"`, innermost-wins (`x:unit == "inner"`, not `"mid"`) and that a key declared only on the *outer* wrapper still reaches the leaf. Risk #2 restated as pinned-by-test. |
| 6 — explicit `= nullptr` | Stated explicitly for `GenerateSchemaFunctionFromIr`, `BuildMessageSchemaIntoFromIr` and the `SchemaVisitor` ctor in §5 and in Files-to-touch. |
| 7 — `EmitNanoarrowTypeSetup` | Added to the "do NOT revive" list with the anonymous-namespace evidence. |
| 8 — nodrift has five tests | All five now in the mapping table (added `RepeatedRunsWithTheSameRulesAreDeterministic`, `RulesActuallyChangeOutputWhenTheyMatch`, and the two failure-exit tests). |
| 9 — `List<Struct>` `item` child metadata | New bullet in §2: the `item` child keeps the deep-copied nested **root** metadata with no overlay; pre-existing, identical on both sinks, pinned by `RepeatedFlattenPutsMetadataOnTheListFieldOnly`; added to Out of scope. |
| 10 — tracker note | Mapping table now says the named forcing test goes green on wiring alone; Files-to-touch adds the `plans/GIR-generator-ir-rewrite.md:100` note so the round is not closed on it. Left as an explicit edit rather than done silently, since that file is the PM's tracker. |

## Step-2 review (2026-08-26)

**Verdict: NEEDS-REWORK** (no locked-decision deviation → **no STOP-AND-ASK**).

The design is unusually accurate: every line reference I checked resolves, and the
four claims I was asked to pressure-test hold as follows.

- **Chain orientation — CONFIRMED CORRECT.** `ForField` (`option_metadata.cpp:488-492`)
  pushes `leaf` then walks `flatten_chain` with `rbegin()`, so the **last** element
  must be the innermost wrapper ⇒ **outer→inner**, which is what append-on-descent
  produces. `option_metadata.hpp:73-76` independently documents "outer→inner". Also
  confirmed: **no fixture in the tree has a two-level `flatten_field`** — the unit
  fixture (`test_option_metadata.cpp:220-223`, `Pos.coord`) and the integration
  fixture (`integration-tests/protoc-arrow-bridge/proto/option_metadata.proto:95-103`)
  are both single-level. Risk #1 is real and currently unguarded (see item 5).
- **Escaping asymmetry — CONFIRMED CORRECT and necessary.** The nanoarrow sink must
  not escape (it writes final metadata bytes); the C++ sink must, because the
  compiler un-escapes. `EscapeCppStringLiteral` is an exact inverse of C++ narrow
  string-literal decoding (3-digit octal ⇒ non-greedy; `\377` is the max needed), and
  both paths truncate identically at an embedded NUL (`ArrowCharView` vs `.c_str()`).
  No byte disagreement is introduced by the asymmetry itself — but see item 1: the
  *layer* the escaper is applied in is not guarded by any lane that can run without
  Conan.
- **`DeepCopyMessageStruct(…, resolver)` — CONFIRMED it reaches the recursion** and
  keeps one holder: `NanoarrowSchemaSink::DeepCopyMessageStruct` (`:318-323`) is the
  only nested re-entry, and `SchemaVisitor` is the only place a resolver pointer
  lives. `RecordingSink` (`test_schema_visitor.cpp:336`) is the only third
  implementer. But there are **two call sites**, not one (item 2).
- **Dead helpers — CONFIRMED dead.** `SetScalarSchemaType` (`generator.cpp:845`),
  `SetMetadataPairs` (`:884`), `RequireNestedMsg` (`:902`) sit in the anonymous
  namespace opened at `:634`/closed at `:1730` with zero references tree-wide.
  `EmitNanoarrowTypeSetup` (`:642`) is dead by the same test (it emits no metadata).
- **New unit guard — NOT sufficient** to discharge the acceptance clause (item 1).
  It does *not* violate locked #9 (it is compile-red before the change).

### Required changes (blocking)

1. **§6's `CppAndIpcTracesAgreeWithResolverActive` is blind to both of this item's
   silent-failure modes; replace/augment it with render-level assertions.**
   `RecordingSink::SetMetadata` (`test_schema_visitor.cpp:328-335`) logs the **raw
   pair vector**, i.e. it sits *above* `CppSchemaSink::SetMetadata`, so escaping is
   invisible to the trace; and the nested build inside
   `NanoarrowSchemaSink::DeepCopyMessageStruct` runs on a **fresh** sink the outer
   recorder never wraps, so the nested resolver is invisible too. The proposed test
   is therefore green by construction for *any* resolver and *any* escaping bug —
   including "escape in the visitor instead of the sink", which would leave all 32
   unit tests green while corrupting the in-process/`.ipc` bytes. Combined with
   risk #5 (Conan lane may not be runnable), the item currently has **no** local
   evidence for its two load-bearing behaviours. Add, at unit level:
   (a) **escaping**: render `GenerateSchemaFunctionFromIr` with a resolver whose
   value contains `"`, `\`, a control byte, a non-ASCII byte and JSON; assert the
   emitted text contains `ArrowCharView("` + `EscapeCppStringLiteral(v)` + `")` and
   does **not** contain the raw value; and assert the same resolver's in-process
   schema metadata holds the **raw** bytes (that pair of assertions pins the layer);
   (b) **nested recursion**: `BuildMessageSchema(<Sample-like>, resolver)` and assert
   a **grandchild** (`pos` → `x`) carries the mapped key. Note that
   `test_option_metadata.cpp` never does this — it builds `Pos` **directly**
   (`:618-632`), so §3 is currently covered only by the Conan-gated
   `MetadataOptionsTest.NestedStructChildMetadataSurvivesTheDeepCopy`.
2. **`EmitNodeType` is missing from Files-to-touch, and its second `DeepCopy` site is
   untested.** `EmitNodeType` (`cpp_backend_schema_visitor.cpp:385-434`) has **two**
   `DeepCopyMessageStruct` call sites — `:404` (STRUCT) and `:419` (MAP value struct)
   — both of which must pass `resolver_`. The Files-to-touch row for that file lists
   `:63-95`, `:226-241`, `:318-323`, `:436-457`, `:463-485` and skips `:385-434`
   entirely. No fixture anywhere uses `map<K, MessageWithOptions>` or a
   non-collapsing `repeated MessageWithOptions`, so missing the map site loses
   grandchild metadata on the in-process path only — the exact `.ipc`-vs-runtime
   disagreement §3 exists to prevent — with every listed test still green. Add
   `EmitNodeType` (both sites) to Files-to-touch and cover one map-value-struct (or
   list-of-struct) case with rules in the new unit test.
3. **The concurrency/safety rationale is factually wrong and must be restated.**
   `OptionMetadataResolver::Impl` holds `mutable google::protobuf::DynamicMessageFactory
   factory;` and `mutable std::map<const Message*, std::unique_ptr<Message>> cache;`
   (`option_metadata.cpp:241-245`), both mutated from `const ParsedOptions`/`Evaluate`.
   So "`ForMessage`/`ForField` are `const` and, once compiled, read-only over the
   descriptor pool" is false: the resolver **memoises** and is **not** thread-safe.
   The conclusion (safe today) survives only because the plugin is serial. Restate as:
   the resolver is internally memoising, must stay a per-`Generate()` stack-local, and
   must not be hoisted to a member/static/shared object or called from more than one
   thread. As written, the claim licenses exactly the wrong future refactor.
4. **"`resolver == nullptr` is the no-rules path" is not what §5 implements.** §5
   creates the resolver unconditionally, so a no-rules `--fletcher_opt=ipc` run passes
   a **non-null, zero-rule** resolver. The locked-#2 golden argument then rests on
   "an empty rule set contributes no pairs", **not** on the `if (resolver_)` guards.
   Pick one explicitly and fix the Nullability paragraph plus the
   `golden/*.ipc` row of the forcing-test table: either pass `nullptr` when
   `rules.empty()` (keeps the guard argument literally true and skips construction),
   or state why an empty resolver is byte-neutral. This is the argument decision #2
   rests on, so it must not be ambiguous.
5. **Promote risk #1's "optional hardening" to required.** Chain orientation is the
   one invariant the design pins by *construction alone*, and it is verifiably
   untested (no two-level `flatten_field` fixture exists anywhere in the tree). Add a
   two-level wrapper (`A.b` → `B.c` → `C.leaf`, both wrapper fields
   `(flatten_field)`, with a `field_type` rule on each wrapper's message) to the new
   `test_schema_visitor.cpp` fixture and assert innermost-wins. ~6 lines; it converts
   "solves the class" from an argument into a test.

### Non-blocking (fix inline if convenient)

6. Pin `= nullptr` **explicitly** on `GenerateSchemaFunctionFromIr`,
   `BuildMessageSchemaIntoFromIr` and the `SchemaVisitor` ctor: `test_schema_visitor.cpp`
   calls all three at today's arity (`:273`, `:355-369`).
7. Add `EmitNanoarrowTypeSetup` (`generator.cpp:642`) to the "do NOT revive" list — it
   is dead by the same argument and is the other grep magnet for an implementer
   porting #121 from `main`. (It emits no metadata, so it is inert either way.)
8. `test_metadata_nodrift.cpp` has **five** tests; the mapping table lists four
   (`RepeatedRunsWithTheSameRulesAreDeterministic` is missing).
9. Worth one line under §2's overlay bullet: a `List<Struct>` **item** child receives
   the deep-copied nested **root** metadata (including `message:`-scope keys) with no
   overlay, which sits oddly next to the spec's "`message` scope reaches only the
   schema root". Pre-existing (identical pre-GIR-5) and identical on both sinks, so
   not a drift risk — but say so rather than leaving it implicit.
10. The tracker's named forcing test
    (`FlattenFieldWrapperContextReachesEachInlinedLeaf`) goes green on **wiring
    alone** — §1 is not needed for it. The design says so honestly; carry that note
    into `plans/GIR-generator-ir-rewrite.md:100` so the round is not closed on it.

### Confirmed non-issues

Locked #2 (no-rules path untouched; escaping is a byte-level no-op on the four
builtins), #3 (`FieldInfo`/`generator_internal.hpp`/RBA untouched — verified
`FieldInfo` has no `flatten_chain` on this branch), #5 (strengthened), #10
(`protoc/` only; `protoc/CMakeLists.txt:27` already compiles `option_metadata.cpp`,
`protoc/tests/CMakeLists.txt:6-11` is the only missing wiring, and the
`integration-tests/` CMake + `conanfile.py:54-73` rule list are present as claimed).
`docs/fletcher-options.md` needs no change. The corrections table checks out,
including that the `source_field` header comment (`cpp_backend_schema_visitor.hpp:49-53`)
is wrong today (`:92` sets it unconditionally).

## Step-2 re-review (2026-08-26, revision 2)

**Verdict: NEEDS-REWORK** — one blocking item (T2 is unsatisfiable as specified).
Still **no locked-decision deviation → no STOP-AND-ASK**. Revision-2 items 3, 4, 5
and 6–10 are genuinely discharged; item 1 is discharged for escaping (T1) but not
for the nested recursion (T2), and item 2's `:419` coverage claim therefore does
not hold yet.

### Verified discharged

- **Item 1(a) / T1 — DISCHARGED, and it does assert at the render layer.** T1
  inspects the *string returned by* `GenerateSchemaFunctionFromIr`, which is
  produced by `CppSchemaSink::SetMetadata` *after* escaping, so it is below the
  `RecordingSink` layer that made the old guard blind. The four-way failure
  analysis is correct and complete: no escaping ⇒ both source assertions red
  (the raw value contains 0x01/0xC2 0xB0, which cannot occur in the pure-ASCII
  escaped render, so "does not contain raw" is a robust discriminator);
  escape-in-the-visitor ⇒ source assertions *pass* but the in-process assertion
  (`x:meta == kNasty` exactly) goes red; double escaping ⇒ the positive source
  assertion goes red. `kNasty`'s construction is also correct C++: adjacent-literal
  concatenation happens in phase 6, after escape conversion in phase 5, so
  `"…\\" "\x01" "\xC2\xB2"` really is backslash, 0x01, 0xC2, 0xB0 and the hex
  escape cannot run on. `\302\260` is valid UTF-8, so protobuf will accept it in a
  proto3 `string` option field (a lone `\377` would have been rejected).
- **Item 3 — DISCHARGED.** The safety section now states the memoisation
  (`mutable factory` / `mutable cache`, `option_metadata.cpp:241-245`, mutated from
  `const ParsedOptions`/`Evaluate`) and turns it into a contract (per-`Generate()`
  stack-local, never hoisted to member/static/shared, one thread, per-worker
  instances if parallelism lands). The address-keyed-cache remark matches the tree.
- **Item 4 — DISCHARGED by construction.** `if (!rules.empty())` makes
  `resolver == nullptr` literally synonymous with "no rules", and the two
  byte-neutral cases are now separated with the right test attached to each. I
  re-checked case 2: rules naming an absent extension parse, then `Create` drops
  every rule and still returns a **non-null** resolver, so
  `InertRulesLeaveEveryOutputByteIdentical` does rest on the empty-append property,
  exactly as written.
- **Item 5 / T3 — DISCHARGED and sound.** I traced the fixture: `Outer.mid` →
  `Mid.inner` → `Inner.leaf` yields `field_id == "1.1.1"` and chain
  `{mid, inner}`; `ForField` candidates become `[leaf, inner, mid]`, so
  `x:unit == "inner"` and `x:group == "g-mid"`. Both stated failure modes really
  do discriminate: a reversed chain gives `x:unit == "mid"` (red) while `x:group`
  still passes; a chain truncated to its innermost element loses `x:group` (red)
  while `x:unit` still passes. This is a genuine construction-independent guard.
- **Items 6–10 — DISCHARGED.** Explicit `= nullptr` on all three signatures (§5 +
  Files-to-touch); `EmitNanoarrowTypeSetup` in the do-not-revive list with the
  anonymous-namespace evidence; all five `MetadataNoDrift` tests mapped;
  `List<Struct>` `item`-child behaviour documented in §2 and placed out of scope;
  tracker note carried into Files-to-touch rather than done silently.
- **Locked decisions — still clean.** #2 (golden argument now anchored on the null
  guard; `golden/*.ipc` untouched), #3 (`FieldInfo`/`generator_internal.hpp`/RBA
  untouched), #5 (strengthened — one visitor, two sinks, one pair vector), #10
  (`protoc/` + protoc tests only). **#9 is not violated by T1–T4:** the existing
  `test_option_metadata` suite remains the forcing suite, T1–T4 are supplementary
  guards, and all four are compile-red before the change (the `SchemaVisitor` ctor
  / entry points do not yet take a resolver), so they claim no red-first exemption.

### Required change (blocking)

1. **T2 cannot pass as specified — two of its three assertions are unsatisfiable,
   so the `:419` map-value-struct site is still uncovered.** T2 asserts
   `x:unit == "m"` on `byname → entries → value → x` and on `path → item → x`, with
   `x:unit` supplied by the `field_type:typ.unit:x:unit` rule. But `field_type`
   scope skips any candidate that is not message-typed
   (`option_metadata.cpp:501`: `if (cand->cpp_type() != CPPTYPE_MESSAGE) continue;`),
   and both of those grandchildren are `Coord`'s scalar `double x` reached through a
   **fresh nested build with an empty flatten chain** — there is no message-typed
   candidate anywhere in their candidate list. `x:unit` is therefore **absent**
   there however correct the code is. (The tree already says so:
   `OptionMetadataTest.FieldScopeOverridesInheritedFieldTypeScopePerKey`,
   `test_option_metadata.cpp:490-491` — "A scalar has no message type, so nothing is
   inherited".) Only `pos → x` works, and it works because `Pos` inlines `coord`,
   i.e. because of the flatten chain — nothing to do with the map or list sites.
   Consequences if shipped as written: the map assertion is red for a reason
   unrelated to §3, and the likely "fix" under time pressure is to delete it, which
   silently restores exactly the gap review item 2 opened.
   **Fix:** make the grandchild key **`field:`-scope on `Coord`'s own leaves**, so it
   is chain-independent and depends only on the resolver reaching the nested build:

   ```proto
   message Coord { option (typ) = { unit: "m" };
                   double x = 1 [(col) = { meta: "mx" }];
                   double y = 2; }
   ```
   then assert `x:meta == "mx"` on all three of `pos → x`,
   `byname → entries → value → x` and `path → item → x` (keep `x:unit == "m"` on
   `pos → x` — it is valid there and doubles as a chain check, and keep
   `x:unit == "deg"` on `pos` itself for the overlay). Dropping `resolver_` at
   `:404` or `:419` then turns the corresponding assertion red for the right
   reason. Optionally also add a `message:typ.group:x:group` rule and assert it on
   the `value`/`item` child itself — that pins the nested `RootMetadata` call,
   which is the other thing the recursion carries.
   I re-verified the rest of T2's structural claims: `map<string, Coord>` *is*
   schema-representable (scalar key, struct value) and *does* reach `:419`;
   `repeated Coord` reaches `:404` via the LIST recursion; `byname` itself correctly
   gets nothing (`is_map()` skip at `:504`); and `path` (the list field) correctly
   gets `x:unit == "m"` under the `field_type` rule — worth asserting as the
   spec's documented "metadata lands on the list field, not the element".

### Non-blocking

2. **T4 needs a resolver-carrying entry point.** `TraceCpp`/`TraceNano`
   (`test_schema_visitor.cpp:355-369`) construct `SchemaVisitor(msg, msg->file(), rec)`
   with no resolver; T4 cannot run "with the resolver active" until they take one
   (defaulted, so `CppAndIpcByteIdentical` keeps calling them unchanged). Add that to
   the `test_schema_visitor.cpp` row.
3. **T1 pins only the escaped *value*, not the escaped *key*.** §4 requires escaping
   "both members of every pair" and justifies it ("`arrow_key` is caller-named
   arbitrary bytes"), but T1's key (`x:meta`) is escape-invariant, so
   escaping-values-only stays green. One-line fix: give T1's rule an arrow key
   containing `"` and `\` (`ParseMetadataRules` splits on the first two colons only
   and copies the key verbatim, so this is legal) and assert its escaped form too.
   Lower severity than the layer bug — an unescaped key breaks the generated header
   loudly — but it is free to cover.
4. §6's fixture sketch omits the option-carrier definitions the rules reference
   (`ColOpts.meta`, `TypeDef.unit`/`.group` + the two `extend` blocks). Implied by
   "declaring its own `FieldOptions`/`MessageOptions` extensions"; spell them out so
   the fixture is copy-able.
5. Wording: T1 says "proto text format accepts C-style octal" — the fixture is
   `.proto` **source** parsed by `compiler::Parser`, so it is proto-language string
   literal escaping (same octal support). Harmless, but the self-checking remark is
   what actually protects this, and it is correct.

## Step-2 review — final (2026-08-26, revision 3)

**Verdict: APPROVE.** No locked-decision deviation; no STOP-AND-ASK. The blocking
item is genuinely discharged, not merely acknowledged. Cleared for implementation
with the nits below (non-blocking; fix while implementing or log and skip).

### Blocking item — discharged, verified row by row

I traced every T2 row against `option_metadata.cpp`'s scope rules and the visitor's
walk. All nine are satisfiable, and the two that matter reach `:419`:

- `pos → x` / `byname → entries → value → x` / `path → item → x`, `x:meta == "mx"`:
  `ForField(x, …)` hits R1 (`kField`) on the **leaf's own** `FieldOptions`
  (`option_metadata.cpp:499`), which is chain-independent — so it fires identically
  in the chain-bearing nested build of `Pos` and in the **empty-chain** nested
  builds of `Coord` reached from the map value (`:419`) and the list element
  (`:404` via the LIST recursion). This is exactly the property the previous
  `field_type:` key lacked. Dropping `resolver_` at either site leaves the
  grandchild with builtins only ⇒ red for the right reason.
- `byname → entries → value` / `path → item`, `x:group == "g-coord"`: supplied by
  R4 (`kMessage`) through the nested build's `RootMetadata` → `ForMessage(Coord)`,
  and retained because the MAP/LIST branches apply only `SetName` to those children
  (`:421`, `:399`), never a metadata overlay. So these rows independently pin the
  `ForMessage` half of the recursion — a real strengthening over revision 2.
- `pos → x`, `x:unit == "m"`: R2 via candidate `coord` (message-typed) — valid here
  precisely because `Pos` inlines `coord`, which is what makes it a chain check.
- `pos`, `x:unit == "deg"`: R2 on `pos`'s own message type.
- `path`, `x:unit == "m"`: R2 on the repeated field (`cpp_type()` is `MESSAGE` for a
  repeated message field, `is_map()` false) — matches the spec's "metadata lands on
  the list field, not the element".
- `byname`, absent: R1 finds no `(sv.col)` on the map field and R2/R3 bail at the
  `is_map()` skip (`:504`).

The "cannot be simplified back later" note is now in both T2 and risk #3, with the
`:501` citation and the in-tree corroboration (`test_option_metadata.cpp:490-491`).

### Fixture and rules table — checked

- **Well-formed.** proto3 may `extend` option messages; 50000/60100/60101 are all
  inside the `1000 to max` third-party range; two extension fields per `extend`
  block is legal; the `{ a: 1, b: 2 }` aggregate-option comma style is the same one
  the already-proven `test_option_metadata.cpp` fixture uses; declaring
  `flatten`/`flatten_field` locally at number 50000 works because the plugin matches
  by number (proven by the ported #121 suite using the same trick).
- **`map<string, Coord>` is a supported visitor shape** — `IsSchemaRepresentable`
  admits scalar key + struct value, and the existing `unit.MapStructHolder` golden
  (`test_schema_visitor.cpp:255`) already exercises that node kind, so T2 needs no
  new emitter capability.
- **T1's two escapings agree exactly.** The `.proto` literal
  `"{\"crs\":\"EPSG:4326\"}\\\001\302\260"` decodes to
  `{"crs":"EPSG:4326"}` + `\` + 0x01 + 0xC2 + 0xB0, byte-identical to the C++
  `kNasty`; `\302\260` is valid UTF-8 so a proto3 `string` option accepts it.
  `kNastyKey` = `x:k"\q` survives `ParseMetadataRules` (split on the first two
  colons, key copied verbatim, not a reserved key) and renders as
  `ArrowCharView("x:k\"\\q")`. Both negative assertions are sound: the raw value
  contains 0x01, which cannot occur in the pure-ASCII escaped render, and the raw
  key is not a substring of its own escaped form.
- **Non-interference claim holds.** R3 (`field_type:…group`) and R4
  (`message:…group`) share the arrow key `x:group` but can never reach the same
  pair vector: `ForMessage` skips non-`kMessage` rules (`:471`) and `ForField`
  skips `kMessage` (`:495`). Verified, as cited.
- **T3 re-verified against the new fixture:** chain `{mid, inner}` ⇒ candidates
  `[leaf, inner, mid]` ⇒ `x:unit == "inner"` (R2 stops at `inner`) and
  `x:group == "g-mid"` (R2/R3 fall through `inner`, which has no `group`, to `mid`),
  with `field_id == "1.1.1"`. Reversed chain fails the first, innermost-only chain
  fails the second — both discriminate, as claimed.

### Nits (non-blocking)

1. **T2 row 7 pins "overlay replaced" only weakly.** `x:unit == "deg"` on `pos`
   would also hold if the overlay *appended*. One extra assertion makes it exact:
   `pos` carries **no** `proto_message` key (the deep-copied `Pos` root had one, and
   `ArrowSchemaSetMetadata` replaces the whole blob).
2. **T2 rows 4–5 now make the `List<Struct>`/map-value item-child quirk a *tested*
   behaviour**, not just a documented one (§2 says it must not be "fixed" by
   accident, and Out-of-scope agrees). That is consistent and arguably a benefit —
   just be aware the two statements are now coupled: changing the quirk later means
   editing T2.
3. T1's rule token has to be assembled in C++ (`"metadata_from_option=field:sv.col.nasty:" + kNastyKey`);
   the rules table's `<kNastyKey>` placeholder is clear enough but says so nowhere.
4. Cosmetic: the fixture comment `// T1's arbitrary-bytes value` on `ColOpts.nasty`
   and the `sv.col.meta`/`sv.col.nasty` split mean `Nasty.v` has `nasty` set and
   `meta` unset — worth one word noting that this is deliberate (it also exercises
   the "sub-field unset ⇒ no key" silent-skip path for R1 on the same field).
