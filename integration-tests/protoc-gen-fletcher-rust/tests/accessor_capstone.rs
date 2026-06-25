// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! RBA-7 capstone (Rust half of accessor_cpp_and_rust_agree_on_same_batch).
//!
//! This test and integration-tests/protoc-arrow-bridge/tests/
//! test_accessor_capstone.cpp are paired: BOTH read the SAME committed
//!   - schema  : integration-tests/accessor-capstone/proto/accessor_capstone.proto
//!   - fixture : integration-tests/accessor-capstone/fixtures/
//!               accessor_capstone_fixture.json
//!   - oracle  : integration-tests/accessor-capstone/fixtures/
//!               accessor_capstone_expected.json
//! build an in-memory arrow-rs batch from the fixture (native builders, not IPC),
//! construct the generated CapstoneBatchAccessor, normalize every read into the
//! oracle's shape, and assert observed == expected. Because both languages
//! compare to the ONE committed oracle, observed_rust == expected (here) AND
//! observed_cpp == expected (C++) transitively proves observed_rust ==
//! observed_cpp (D-RBA-8). Expected values are NEVER hand-transcribed here — they
//! are parsed from the shared JSON.
//!
//! Covered (every field kind + a representative null in each optional path):
//! scalar non-nullable (id), scalar nullable (label, null row), STRUCT
//! (maybe_child, null row), REPEATED_SCALAR (samples, null element via is_null),
//! REPEATED_STRUCT (children, null element), MAP scalar value (scores, null value
//! via value_is_null), MAP message value (child_by_key, null message value),
//! NESTED_LIST (nested_children, null inner list), plus schema + positional field
//! metadata (absent -> empty map normalized to {}).

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use arrow::array::{ArrayRef, Int32Array, ListArray, MapArray, StringArray, StructArray};
use arrow::buffer::{NullBuffer, OffsetBuffer};
use arrow::datatypes::{DataType, Field, Fields, Schema};
use arrow::record_batch::RecordBatch;

use serde_json::{json, Map, Value};

use protoc_gen_fletcher_rust::fletcher_gen::rba::capstone::CapstoneBatchAccessor;

const CAPSTONE_FIXTURES: &str = env!("FLETCHER_TEST_CAPSTONE_FIXTURES");

fn load_json(name: &str) -> Value {
    let path = PathBuf::from(CAPSTONE_FIXTURES).join(name);
    let text = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("read {}: {e}", path.display()));
    serde_json::from_str(&text).unwrap_or_else(|e| panic!("parse {}: {e}", path.display()))
}

// ---------------------------------------------------------------------------
// Arrow type helpers — the schema the generated accessor validates against.
// ---------------------------------------------------------------------------

// Child = struct<score:int32 (non-nullable), name:utf8 (non-nullable)>.
fn child_fields() -> Fields {
    Fields::from(vec![
        Field::new("score", DataType::Int32, false),
        Field::new("name", DataType::Utf8, false),
    ])
}
fn child_type() -> DataType {
    DataType::Struct(child_fields())
}

// Build a Child StructArray from flat score/name vecs + per-element validity
// (false -> a null Child).
fn make_child_array(scores: Vec<i32>, names: Vec<String>, valid: Vec<bool>) -> StructArray {
    let score_arr: ArrayRef = Arc::new(Int32Array::from(scores));
    let name_arr: ArrayRef = Arc::new(StringArray::from(names));
    let nulls = if valid.iter().all(|&v| v) {
        None
    } else {
        Some(NullBuffer::from(valid))
    };
    StructArray::new(child_fields(), vec![score_arr, name_arr], nulls)
}

fn map_entries_field(value_type: DataType) -> Arc<Field> {
    Arc::new(Field::new(
        "entries",
        DataType::Struct(Fields::from(vec![
            Field::new("keys", DataType::Utf8, false),
            Field::new("values", value_type, true),
        ])),
        false,
    ))
}

// ---------------------------------------------------------------------------
// Build the Arrow columns directly from the shared fixture JSON `rows`.
// ---------------------------------------------------------------------------

fn build_id(rows: &[Value]) -> ArrayRef {
    let v: Vec<i32> = rows.iter().map(|r| r["id"].as_i64().unwrap() as i32).collect();
    Arc::new(Int32Array::from(v))
}

fn build_label(rows: &[Value]) -> ArrayRef {
    let v: Vec<Option<String>> = rows
        .iter()
        .map(|r| {
            let l = &r["label"];
            if l.is_null() {
                None
            } else {
                Some(l.as_str().unwrap().to_string())
            }
        })
        .collect();
    Arc::new(StringArray::from(v))
}

fn build_maybe_child(rows: &[Value]) -> ArrayRef {
    let mut scores = Vec::new();
    let mut names = Vec::new();
    let mut valid = Vec::new();
    for r in rows {
        let c = &r["maybe_child"];
        if c.is_null() {
            scores.push(0);
            names.push(String::new());
            valid.push(false);
        } else {
            scores.push(c["score"].as_i64().unwrap() as i32);
            names.push(c["name"].as_str().unwrap().to_string());
            valid.push(true);
        }
    }
    Arc::new(make_child_array(scores, names, valid))
}

fn build_samples(rows: &[Value]) -> ArrayRef {
    let mut vals: Vec<Option<i32>> = Vec::new();
    let mut offsets: Vec<i32> = vec![0];
    let mut off = 0i32;
    for r in rows {
        for s in r["samples"].as_array().unwrap() {
            if s.is_null() {
                vals.push(None);
            } else {
                vals.push(Some(s.as_i64().unwrap() as i32));
            }
            off += 1;
        }
        offsets.push(off);
    }
    let values: ArrayRef = Arc::new(Int32Array::from(vals));
    let offsets = OffsetBuffer::new(offsets.into());
    let field = Arc::new(Field::new("item", DataType::Int32, true));
    Arc::new(ListArray::new(field, offsets, values, None))
}

fn build_children(rows: &[Value]) -> ArrayRef {
    let mut scores = Vec::new();
    let mut names = Vec::new();
    let mut valid = Vec::new();
    let mut offsets: Vec<i32> = vec![0];
    let mut off = 0i32;
    for r in rows {
        for c in r["children"].as_array().unwrap() {
            if c.is_null() {
                scores.push(0);
                names.push(String::new());
                valid.push(false);
            } else {
                scores.push(c["score"].as_i64().unwrap() as i32);
                names.push(c["name"].as_str().unwrap().to_string());
                valid.push(true);
            }
            off += 1;
        }
        offsets.push(off);
    }
    let child = Arc::new(make_child_array(scores, names, valid)) as ArrayRef;
    let offsets = OffsetBuffer::new(offsets.into());
    let field = Arc::new(Field::new("item", child_type(), true));
    Arc::new(ListArray::new(field, offsets, child, None))
}

fn build_scores(rows: &[Value]) -> ArrayRef {
    let mut keys: Vec<String> = Vec::new();
    let mut vals: Vec<Option<i32>> = Vec::new();
    let mut offsets: Vec<i32> = vec![0];
    let mut off = 0i32;
    for r in rows {
        for kv in r["scores"].as_array().unwrap() {
            keys.push(kv["key"].as_str().unwrap().to_string());
            let v = &kv["value"];
            if v.is_null() {
                vals.push(None);
            } else {
                vals.push(Some(v.as_i64().unwrap() as i32));
            }
            off += 1;
        }
        offsets.push(off);
    }
    let keys_arr: ArrayRef = Arc::new(StringArray::from(keys));
    let vals_arr: ArrayRef = Arc::new(Int32Array::from(vals));
    let entries_field = map_entries_field(DataType::Int32);
    let entries = StructArray::new(
        match entries_field.data_type() {
            DataType::Struct(f) => f.clone(),
            _ => unreachable!(),
        },
        vec![keys_arr, vals_arr],
        None,
    );
    let offsets = OffsetBuffer::new(offsets.into());
    Arc::new(MapArray::new(entries_field, offsets, entries, None, false))
}

fn build_child_by_key(rows: &[Value]) -> ArrayRef {
    let mut keys: Vec<String> = Vec::new();
    let mut scores = Vec::new();
    let mut names = Vec::new();
    let mut valid = Vec::new();
    let mut offsets: Vec<i32> = vec![0];
    let mut off = 0i32;
    for r in rows {
        for kv in r["child_by_key"].as_array().unwrap() {
            keys.push(kv["key"].as_str().unwrap().to_string());
            let v = &kv["value"];
            if v.is_null() {
                scores.push(0);
                names.push(String::new());
                valid.push(false);
            } else {
                scores.push(v["score"].as_i64().unwrap() as i32);
                names.push(v["name"].as_str().unwrap().to_string());
                valid.push(true);
            }
            off += 1;
        }
        offsets.push(off);
    }
    let keys_arr: ArrayRef = Arc::new(StringArray::from(keys));
    let vals = Arc::new(make_child_array(scores, names, valid)) as ArrayRef;
    let entries_field = map_entries_field(child_type());
    let entries = StructArray::new(
        match entries_field.data_type() {
            DataType::Struct(f) => f.clone(),
            _ => unreachable!(),
        },
        vec![keys_arr, vals],
        None,
    );
    let offsets = OffsetBuffer::new(offsets.into());
    Arc::new(MapArray::new(entries_field, offsets, entries, None, false))
}

fn build_nested_children(rows: &[Value]) -> ArrayRef {
    // Flatten leaf Child elements.
    let mut scores = Vec::new();
    let mut names = Vec::new();
    let mut leaf_valid = Vec::new();
    // Inner list level: offsets + per-inner-list validity.
    let mut inner_offsets: Vec<i32> = vec![0];
    let mut inner_valid: Vec<bool> = Vec::new();
    let mut leaf_off = 0i32;
    // Outer list level: per-row offsets over inner lists.
    let mut outer_offsets: Vec<i32> = vec![0];
    let mut inner_idx = 0i32;

    for r in rows {
        for inner in r["nested_children"].as_array().unwrap() {
            if inner.is_null() {
                inner_offsets.push(leaf_off);
                inner_valid.push(false);
            } else {
                for c in inner.as_array().unwrap() {
                    scores.push(c["score"].as_i64().unwrap() as i32);
                    names.push(c["name"].as_str().unwrap().to_string());
                    leaf_valid.push(true);
                    leaf_off += 1;
                }
                inner_offsets.push(leaf_off);
                inner_valid.push(true);
            }
            inner_idx += 1;
        }
        outer_offsets.push(inner_idx);
    }

    let leaf = Arc::new(make_child_array(scores, names, leaf_valid)) as ArrayRef;

    // Inner list array, with row-level nulls for the null inner lists.
    let inner_item = Arc::new(Field::new("item", child_type(), true));
    let inner_nulls = if inner_valid.iter().all(|&v| v) {
        None
    } else {
        Some(NullBuffer::from(inner_valid))
    };
    let inner_list = Arc::new(ListArray::new(
        inner_item.clone(),
        OffsetBuffer::new(inner_offsets.into()),
        leaf,
        inner_nulls,
    )) as ArrayRef;

    // Outer list array.
    let outer_item = Arc::new(Field::new("item", DataType::List(inner_item), true));
    Arc::new(ListArray::new(
        outer_item,
        OffsetBuffer::new(outer_offsets.into()),
        inner_list,
        None,
    ))
}

fn nested_children_type() -> DataType {
    let inner_item = Arc::new(Field::new("item", child_type(), true));
    DataType::List(Arc::new(Field::new(
        "item",
        DataType::List(inner_item),
        true,
    )))
}

fn build_columns(rows: &[Value]) -> Vec<ArrayRef> {
    vec![
        build_id(rows),
        build_label(rows),
        build_maybe_child(rows),
        build_samples(rows),
        build_children(rows),
        build_scores(rows),
        build_child_by_key(rows),
        build_nested_children(rows),
    ]
}

fn build_schema(fixture: &Value) -> Arc<Schema> {
    let mut fields = vec![
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
        Field::new("maybe_child", child_type(), true),
        Field::new(
            "samples",
            DataType::List(Arc::new(Field::new("item", DataType::Int32, true))),
            false,
        ),
        Field::new(
            "children",
            DataType::List(Arc::new(Field::new("item", child_type(), true))),
            false,
        ),
        Field::new("scores", DataType::Map(map_entries_field(DataType::Int32), false), false),
        Field::new(
            "child_by_key",
            DataType::Map(map_entries_field(child_type()), false),
            false,
        ),
        Field::new("nested_children", nested_children_type(), false),
    ];

    // Positional per-field metadata (absent -> none).
    let fmeta = fixture["field_metadata"].as_array().unwrap();
    for (i, m) in fmeta.iter().enumerate() {
        if i >= fields.len() {
            break;
        }
        let obj = m.as_object().unwrap();
        if !obj.is_empty() {
            let md: HashMap<String, String> = obj
                .iter()
                .map(|(k, v)| (k.clone(), v.as_str().unwrap().to_string()))
                .collect();
            fields[i] = fields[i].clone().with_metadata(md);
        }
    }

    let schema_md: HashMap<String, String> = fixture["schema_metadata"]
        .as_object()
        .unwrap()
        .iter()
        .map(|(k, v)| (k.clone(), v.as_str().unwrap().to_string()))
        .collect();
    Arc::new(Schema::new(fields).with_metadata(schema_md))
}

// ---------------------------------------------------------------------------
// Normalize the observed accessor readout into the oracle's JSON shape.
// ---------------------------------------------------------------------------

// Rust empty map <=> C++ nullptr <=> {}. serde_json::Map preserves insertion
// order; we sort by key so the comparison is order-independent.
fn metadata_to_json(md: &HashMap<String, String>) -> Value {
    let mut keys: Vec<&String> = md.keys().collect();
    keys.sort();
    let mut obj = Map::new();
    for k in keys {
        obj.insert(k.clone(), Value::String(md[k].clone()));
    }
    Value::Object(obj)
}

fn normalize_readout(a: &CapstoneBatchAccessor, field_count: usize) -> Value {
    let mut out = Map::new();
    out.insert("schema_metadata".into(), metadata_to_json(a.schema_metadata()));

    let mut fmeta = Vec::new();
    for i in 0..field_count {
        fmeta.push(metadata_to_json(a.field_metadata(i)));
    }
    out.insert("field_metadata".into(), Value::Array(fmeta));

    let mut rows = Vec::new();
    for r in 0..a.num_rows() {
        rows.push(normalize_row(a, r));
    }
    out.insert("rows".into(), Value::Array(rows));
    Value::Object(out)
}

fn normalize_row(a: &CapstoneBatchAccessor, row: usize) -> Value {
    let mut o = Map::new();

    // scalar non-nullable.
    o.insert("id".into(), json!(a.id(row)));

    // scalar nullable -> value | null.
    o.insert(
        "label".into(),
        match a.label(row) {
            Some(s) => json!(s),
            None => Value::Null,
        },
    );

    // STRUCT (nullable 1:1) -> {...} | null.
    o.insert(
        "maybe_child".into(),
        match a.maybe_child(row) {
            Some(c) => json!({"score": c.score(), "name": c.name()}),
            None => Value::Null,
        },
    );

    // REPEATED_SCALAR -> [value|null,...], element null via is_null(j).
    {
        let span = a.samples(row);
        let mut arr = Vec::new();
        for j in 0..span.len() {
            if span.is_null(j) {
                arr.push(Value::Null);
            } else {
                arr.push(json!(span.value(j)));
            }
        }
        o.insert("samples".into(), Value::Array(arr));
    }

    // REPEATED_STRUCT -> [{...}|null,...], element null via get(j) == None.
    {
        let span = a.children(row);
        let mut arr = Vec::new();
        for j in 0..span.len() {
            match span.get(j) {
                Some(c) => arr.push(json!({"score": c.score(), "name": c.name()})),
                None => arr.push(Value::Null),
            }
        }
        o.insert("children".into(), Value::Array(arr));
    }

    // MAP scalar value -> [{key,value}], value null via value_is_null(j).
    {
        let m = a.scores(row);
        let mut arr = Vec::new();
        for j in 0..m.len() {
            let value = if m.value_is_null(j) {
                Value::Null
            } else {
                json!(m.value(j))
            };
            arr.push(json!({"key": m.key(j), "value": value}));
        }
        o.insert("scores".into(), Value::Array(arr));
    }

    // MAP message value -> [{key,value}], value null via value(j) == None.
    {
        let m = a.child_by_key(row);
        let mut arr = Vec::new();
        for j in 0..m.len() {
            let value = match m.value(j) {
                Some(c) => json!({"score": c.score(), "name": c.name()}),
                None => Value::Null,
            };
            arr.push(json!({"key": m.key(j), "value": value}));
        }
        o.insert("child_by_key".into(), Value::Array(arr));
    }

    // NESTED_LIST depth 2 -> [[{...},...] | null, ...], inner null via get(i)
    // == None.
    {
        let outer = a.nested_children(row);
        let mut arr = Vec::new();
        for i in 0..outer.len() {
            match outer.get(i) {
                Some(inner) => {
                    let mut inner_arr = Vec::new();
                    for j in 0..inner.len() {
                        match inner.get(j) {
                            Some(c) => {
                                inner_arr.push(json!({"score": c.score(), "name": c.name()}))
                            }
                            None => inner_arr.push(Value::Null),
                        }
                    }
                    arr.push(Value::Array(inner_arr));
                }
                None => arr.push(Value::Null),
            }
        }
        o.insert("nested_children".into(), Value::Array(arr));
    }

    Value::Object(o)
}

// Strip the documentation-only "_comment" key so it never affects comparison.
fn strip_comment(mut v: Value) -> Value {
    if let Value::Object(ref mut m) = v {
        m.remove("_comment");
    }
    v
}

#[test]
fn accessor_cpp_and_rust_agree_on_same_batch() {
    let fixture = load_json("accessor_capstone_fixture.json");
    let expected = strip_comment(load_json("accessor_capstone_expected.json"));
    let rows = fixture["rows"].as_array().unwrap();

    let schema = build_schema(&fixture);
    let cols = build_columns(rows);
    let batch = RecordBatch::try_new(schema, cols).expect("build capstone batch");

    let acc = CapstoneBatchAccessor::try_new(batch).expect("try_new should succeed");

    const FIELD_COUNT: usize = 8;
    let observed = normalize_readout(&acc, FIELD_COUNT);

    // The single, decisive assertion: the Rust readout equals the ONE shared
    // committed oracle. (C++ asserts the same against the same file.)
    assert_eq!(
        observed,
        expected,
        "\nobserved:\n{}\nexpected:\n{}",
        serde_json::to_string_pretty(&observed).unwrap(),
        serde_json::to_string_pretty(&expected).unwrap()
    );

    // Explicit null-path spot checks (per the design: not only implied by the
    // snapshot). These read the fixture's documented null placements directly.
    assert!(acc.maybe_child(1).is_none(), "null 1:1 struct row");
    assert!(acc.label(1).is_none(), "null scalar row");
    assert!(acc.samples(0).is_null(1), "null scalar element");
    assert!(acc.children(0).get(1).is_none(), "null struct element");
    assert!(acc.scores(0).value_is_null(1), "null scalar map value");
    assert!(
        acc.child_by_key(0).value(1).is_none(),
        "null map message value"
    );
    assert!(acc.nested_children(0).get(1).is_none(), "null inner list");

    // Explicit metadata-equivalence contract (S1): lock in the Rust-empty-map ==
    // C++-nullptr normalization DIRECTLY, not only via the snapshot. A field with
    // ABSENT metadata reads back as an EMPTY map (the Rust mirror of C++ nullptr);
    // a field WITH metadata reads its keys back verbatim. The C++ capstone makes
    // the mirror-image assertion (nullptr for the same absent indices). The
    // expected oracle encodes {} for absent indices 1,2,4..7 and the keys for
    // present indices 0,3 (field_metadata order is positional, spec §5).
    //
    // Present fields (read verbatim).
    let fm0 = acc.field_metadata(0);
    assert_eq!(fm0.get("role").map(String::as_str), Some("id"), "field 0 metadata");
    assert_eq!(metadata_to_json(fm0), expected["field_metadata"][0]);
    let fm3 = acc.field_metadata(3);
    assert_eq!(
        fm3.get("role").map(String::as_str),
        Some("samples"),
        "field 3 metadata"
    );
    assert_eq!(metadata_to_json(fm3), expected["field_metadata"][3]);
    // Absent fields: Rust reports an EMPTY map (the mirror of C++ nullptr).
    for absent in [1usize, 2, 4, 5, 6, 7] {
        let m = acc.field_metadata(absent);
        assert!(
            m.is_empty(),
            "field {absent} has no metadata -> Rust empty map (== C++ nullptr)"
        );
        assert_eq!(
            metadata_to_json(m),
            serde_json::json!({}),
            "absent field {absent} normalizes to {{}}"
        );
        assert_eq!(metadata_to_json(m), expected["field_metadata"][absent]);
    }
    // Schema-level metadata reads its keys back verbatim.
    let sm = acc.schema_metadata();
    assert_eq!(sm.get("capstone").map(String::as_str), Some("rba-7"));
    assert_eq!(sm.get("owner").map(String::as_str), Some("accessor"));
}
