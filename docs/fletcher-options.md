# Fletcher Proto Options

Fletcher defines custom protobuf options that control how proto schemas are
compiled to Arrow types. These options live in the `fletcher` package and
can be used by any proto file that imports `fletcher/options.proto`.

All options are generic — they are not tied to any particular domain or
schema.

---

## `(fletcher.flatten)` / `(fletcher.flatten_field)`

**Applies to:** `google.protobuf.MessageOptions` (`flatten`) and `google.protobuf.FieldOptions` (`flatten_field`)
**Type:** `bool`
**Extension field number:** `50000`

Removes intermediate struct wrappers from the Arrow schema.  There are two
forms — message-level and field-level — which serve different purposes.

### Message-level flatten

```proto
message HeadingTrue {
    option (fletcher.flatten) = true;
    double deg = 1;
}
```

When a message is annotated with `(fletcher.flatten) = true` **and has exactly
one field**, Fletcher treats the message as transparent: wherever the message
is used as a field type, the compiler strips the struct wrapper and emits the
inner field's Arrow type directly.

| Usage in a parent message | Arrow result |
|---|---|
| `HeadingTrue heading = 3;` | `Float64` (not `Struct<deg: Float64>`) |
| `optional HeadingTrue heading = 3;` | nullable `Float64` |
| `repeated HeadingTrue headings = 3;` | `List<Float64>` |

**Multi-field messages:** If `(fletcher.flatten)` is set on a message with
more than one field, the option is ignored and the compiler emits a warning.
Use field-level flatten (below) to inline specific sub-messages in that case.

**Chaining:** When a flattened message's single field is itself a message type
with `(fletcher.flatten)`, the compiler resolves the chain automatically.
Each flattened wrapper whose inner field is `repeated` adds one list nesting
level.  This is the mechanism behind geometry-style types:

```proto
message Coord  { double x = 1; double y = 2; }

message LinearRing {
    option (fletcher.flatten) = true;
    repeated Coord vertices = 1;     // → List<Struct<x,y>>
}

message Polygon {
    option (fletcher.flatten) = true;
    repeated LinearRing rings = 1;   // → List<List<Struct<x,y>>>
}
```

**Class generation:** No row-wrapper class is generated for a message that is
a valid flatten target (single-field + option set).  The message's
representation is fully absorbed into the parent.

### Field-level flatten

```proto
message Point {
    Coord coord = 1 [(fletcher.flatten_field) = true];
}
```

When a **singular message field** carries `[(fletcher.flatten_field) = true]`,
Fletcher promotes the referenced message's fields into the enclosing
message, removing the intermediate struct.

| Without flatten | With flatten |
|---|---|
| `Struct<coord: Struct<x: f64, y: f64>>` | `Struct<x: f64, y: f64>` |

This lets types compose building-block messages (e.g. coordinates) without
introducing extra nesting in the Arrow schema.

Field-level flatten is recursive: if the inlined message itself contains
fields with `[(fletcher.flatten_field)]`, those are inlined too.

**Non-message fields:** `[(fletcher.flatten_field)]` on a scalar or enum field is
a no-op — there is nothing to inline.

**Field identity:** an inlined field keeps the inner sub-message's own proto
`field_number` (so two inlined fields, or an inlined field and an enclosing
field, can share a number). The Arrow schema therefore carries an additional
`field_id` metadata key holding the dotted path from the enclosing message —
e.g. inlining `Coord` (`coord` is field 1) yields `field_id = "1.1"` and
`"1.2"` for its `x`/`y` fields. `field_id` is unique within a message even when
`field_number` repeats; for non-inlined top-level fields it equals the
`field_number` string. Decoding is positional, so neither value affects the
wire format — they are schema metadata for consumers.

### Chain-walking behaviour

When the compiler resolves a chain of flattened wrappers (message-level),
it stops at the first message that is **not** a valid flatten target
(i.e. it does not have `(fletcher.flatten) = true`, or it has more than
one field).  That message becomes the leaf struct type in the resulting
Arrow schema.

---

## `--fletcher_opt=metadata_from_option` — custom option passthrough

A **CLI flag**, not a `.proto` option. It copies values out of *your own* custom
proto options into Arrow schema/field metadata. Fletcher never interprets,
validates or normalises what it copies, and has no built-in knowledge of any
option or key — a rule is a byte-copy from an option path you name to an Arrow
metadata key you name.

```
--fletcher_opt=metadata_from_option=<scope>:<option-path>:<arrow-key>
```

The token is split on its **first two** colons only, so `<arrow-key>` may
contain colons of its own (`ARROW:extension:name`, `mypkg:unit`). Pass the flag
once per rule — protoc joins repeated `--fletcher_opt` values with commas.

### Scopes

| Scope | Reads | Lands on |
|---|---|---|
| `field` | `FieldOptions` of the Arrow field's proto field | field metadata |
| `field_type` | `MessageOptions` of that field's **message type** | field metadata |
| `message` | `MessageOptions` of the message itself | schema metadata |

`field_type` is how a "column type message" annotates every field that uses it:

```proto
message Time {
    option (mypkg.type) = { unit: "s" };
    option (fletcher.flatten) = true;
    google.protobuf.Timestamp value = 1;
}
message Sample { Time t = 1; }   // field `t` inherits unit=s
```

Note `message` scope reaches only the schema root of the message being
generated. When that same message is used as a *field* elsewhere, the nested
schema's own metadata is replaced by the field's — use `field_type` for that.

### Option paths

```
path := step ( '/' step )*
step := <extension-fqn> ( '.' <sub-field> )*
```

Extensions are named by their fully-qualified name; the boundary between the
extension name and the sub-field path is resolved automatically. `/` is the
**enum hop**: valid only after a singular enum-typed sub-field, it continues
from *that enum value's* `EnumValueOptions`. This is how a value declared once
on an enum value reaches every field that selects it:

```proto
enum Enc {
    ENC_GEO = 11 [(mypkg.enc_opts) = { extension_name: "geoarrow.point" }];
}
```
```
--fletcher_opt=metadata_from_option=field_type:mypkg.type.enc/mypkg.enc_opts.extension_name:ARROW:extension:name
```

### Value rendering

| Sub-field type | Rendered as |
|---|---|
| string / bytes | verbatim |
| enum | the value's **proto name**, verbatim (`NS_TYPE_TIME`) |
| bool | `true` / `false` |
| int32 / int64 / uint32 / uint64 | decimal |
| repeated (of the above) | elements joined with `,` |
| float / double | **rejected** — not reproducible across platforms |
| message | **rejected** — the path is incomplete |

An empty rendered value counts as **absent**.

### Precedence

Rules are evaluated in the order given, and a rule producing a non-empty value
**overwrites** that Arrow key. So list the fallback *first* and the preferred
source *last*. A key keeps the position of its first appearance, so output stays
deterministic. Within one rule, the field's own declaration is tried before any
wrapper it was inlined out of.

Per-key merge, never whole-option replace: a field that overrides only `unit`
still inherits `type` and `encoding` from its field type.

### Interaction with flatten

- `(fletcher.flatten)` wrappers: the annotation on the wrapper message lands on
  the flattened column, because the referencing field keeps its own descriptor.
- `(fletcher.flatten_field)`: the outer field disappears from the Arrow schema,
  so its annotation (and its message type's) is inherited by **each** inlined
  leaf, unless the leaf declares its own.

### Errors

Anything statically determinable from the descriptors is a **hard codegen
error**: a malformed rule, an unknown scope, an empty key or path, a key that is
one of the four generator-owned keys (`proto_package`, `proto_message`,
`field_number`, `field_id`), an unknown sub-field, a `/` hop off a non-enum, or
a rejected terminal type.

Anything depending on a particular descriptor's contents is **skipped
silently**: an option that is not applied, a sub-field left unset, a value that
renders empty. An extension that no file in the protoc invocation declares is
also skipped — one rule list is normally applied to a whole corpus where only
some files import the declaring `.proto`.

### Limitations

- **List elements.** For `repeated <flatten wrapper>` the metadata lands on the
  list field, not the element. An Arrow extension type on a list is
  semantically wrong, but the element has no attachment point today.
- **Map fields** inherit nothing: the message type of a map field is the
  synthetic `MapEntry`, which is never a metadata carrier.
- **Rules are a build-wide property.** For a cross-file nested struct the
  generated C++ deep-copies `<Nested>Schema()` from a header produced by another
  protoc invocation, while `--fletcher_opt=ipc` recomputes it in-process. Every
  invocation contributing to one generated tree must pass an identical rule
  list, or the two will disagree.
- The inner field of a `(fletcher.flatten)` wrapper is not reachable — only the
  wrapper's `MessageOptions` and the referencing field's `FieldOptions`.
- `--fletcher_opt=ts` output does not carry mapped metadata.

## `(fletcher.dictionary)`

**Applies to:** `google.protobuf.FieldOptions`
**Type:** `DictionaryOptions` message (`index_type`, `ordered`)
**Extension field number:** `50001`

Marks a field for Arrow dictionary encoding. Presence is the trigger — `[(fletcher.dictionary) = {}]` means "dictionary, with defaults" (an `int32` index).
See [docs/dictionary-option-spec.md](dictionary-option-spec.md) for the full
specification, including which shapes accept the option and where the
declaration is dropped or propagated by `(fletcher.flatten)` /
`(fletcher.flatten_field)`.

**Rejected declarations.** The plugin fails generation — for **every** option
set, with an error naming the offending field — when the option is declared on any
field that becomes a column of a generated message: the message's own fields, a
field inlined through `(fletcher.flatten_field)`, and any field of a nested
message used as a struct field, a list element or a map value (including a message
from an **imported** `.proto`). It is rejected when declared:
on a field that does not map to a **scalar** column (`repeated`, `map<K,V>`, a
struct message, a nested list); with `ordered: true` (not supported in v1); on a
`(fletcher.flatten_field)` **wrapper field** (its fields are inlined as separate
columns, so there is no single column to encode — this includes a well-known
wrapper such as `google.protobuf.StringValue` when it carries `flatten_field`);
twice on one `(fletcher.flatten)` chain with **disagreeing** settings (identical
settings are fine, and `= {}` / `DICTIONARY_INDEX_UNSPECIFIED` /
`DICTIONARY_INDEX_INT32` all count as identical); and on the inner field of a
**single-field** `(fletcher.flatten)` wrapper reached through a `repeated` field
(the resulting column is a list). `(fletcher.flatten_field)` on a plain **scalar**
field is a documented no-op, so a scalar carrying both options is legal; and a
well-known wrapper field carrying the option **without** `flatten_field` is a
legal nullable dictionary.

A few shapes are **not** reached by the check and are accepted silently, all of
them behind a `(fletcher.flatten)` wrapper — the check cannot look inside a
wrapper, because a `(fletcher.flatten_field)` declaration there really *is*
honoured and rejecting it would break working protos. Specifically: a wrapper
message reached through a `map` value or a struct field; a declaration below a
`repeated` hop inside a singular wrapper chain; and a declaration on a **nested
message reached through a wrapper** (so an imported message's illegal declaration
is reported when it is used directly, but not when a wrapper sits in between). All
of these drop `ordered: true` silently. See
[docs/dictionary-option-spec.md §7.1.1](dictionary-option-spec.md) for the exact
boundary.

**v1 limitation.** A proto with a `(fletcher.dictionary)` field must omit
`--fletcher_opt=accessor,rust` — the plugin rejects generation for those two
backends with an error naming the field, until round RIR migrates the
RecordBatch accessor / Rust backend onto the IR and can emit the
`dictionary(idx, val)` column type. The `edge`, Arrow view, IPC schema, and
`ts` backends accept dictionary fields today.

## Extension field number registry

| Number | Scope | Option |
|--------|-------|--------|
| `50000` | MessageOptions | `fletcher.flatten` |
| `50000` | FieldOptions | `fletcher.flatten_field` |
| `50001` | FieldOptions | `fletcher.dictionary` |

Field numbers 50000/50001 are well within the `extensions 1000 to max` range
that `google.protobuf.MessageOptions` and `FieldOptions` reserve for
third-party extensions.

Extension numbers named in a `metadata_from_option` rule are the **consumer's
own**, not Fletcher's, and are not registered here.
