# Fletcher Proto Options

Fletcher defines custom protobuf options that control how proto schemas are
compiled to Arrow types. These options live in the `fletcher` package and
can be used by any proto file that imports `fletcher/options.proto`.

All options are generic — they are not tied to any particular domain or
schema.

---

## `(fletcher.flatten)`

**Applies to:** `google.protobuf.MessageOptions` and `google.protobuf.FieldOptions`
**Type:** `bool`
**Extension field number:** `1`

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
    Coord coord = 1 [(fletcher.flatten) = true];
}
```

When a **singular message field** carries `[(fletcher.flatten) = true]`,
Fletcher promotes the referenced message's fields into the enclosing
message, removing the intermediate struct.

| Without flatten | With flatten |
|---|---|
| `Struct<coord: Struct<x: f64, y: f64>>` | `Struct<x: f64, y: f64>` |

This lets types compose building-block messages (e.g. coordinates) without
introducing extra nesting in the Arrow schema.

Field-level flatten is recursive: if the inlined message itself contains
fields with `[(fletcher.flatten)]`, those are inlined too.

**Non-message fields:** `[(fletcher.flatten)]` on a scalar or enum field is
a no-op — there is nothing to inline.

### Chain-walking behaviour

When the compiler resolves a chain of flattened wrappers (message-level),
it stops at the first message that is **not** a valid flatten target
(i.e. it does not have `(fletcher.flatten) = true`, or it has more than
one field).  That message becomes the leaf struct type in the resulting
Arrow schema.

---

## Extension field number registry

| Number | Scope | Option |
|--------|-------|--------|
| `1` | MessageOptions | `fletcher.flatten` |
| `1` | FieldOptions | `fletcher.flatten` |

Field number 1 is valid because the extensions are in the `fletcher`
package namespace, separate from the base `google.protobuf.*Options`
field numbering.
