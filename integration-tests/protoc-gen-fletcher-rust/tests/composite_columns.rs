// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! RBA-6a forcing + acceptance tests for the generated Rust composite accessor.
//!
//! `composite_and_metadata_read` is the RBA-6a slice of the RBA-6 acceptance
//! target. It builds an arrow-rs `RecordBatch` for `CompositeMain` (package
//! `rba.main`, file composite_main.proto) plus arbitrary schema / per-field
//! metadata, and proves:
//!
//!   - generic metadata read-back: `schema_metadata()` / `field_metadata(i)`
//!     return the live maps verbatim by borrowed reference (no domain key), and
//!     absent metadata -> an empty map (never an error, D-RBA-5);
//!   - STRUCT, cross-file + cross-PACKAGE, >= 2 levels: the deep row chain
//!     `a.outer(row).inner().leaf().value()` reads through composed Rows and
//!     resolves the grandchild via `crate::fletcher_gen::rba::child::LeafAccessor`
//!     (D-RBA-10 cross-package);
//!   - nullable 1:1 struct: a null `maybe_outer` row -> `None` (B2);
//!   - non-nullable struct with a runtime null element -> `try_new` returns `Err`
//!     (D-RBA-4 recursed through the composite null_count gate);
//!   - REPEATED_SCALAR: ScalarSpan len / is_empty / value(i) / is_null(i);
//!   - REPEATED_STRUCT: StructSpan::get(j) -> `Some(Row)`, `None` on a null
//!     struct element (element-level B2 via the inner accessor is_null);
//!   - `from_struct` parity with a NON-ZERO offset (the §2.4 child slicing), incl.
//!     the struct-sourced empty schema-metadata map;
//!   - the same-package two-file co-mount (composite_main + composite_aux both
//!     under `rba::main`, both emitting a composite getter — R1 collision check).

use std::sync::Arc;

use arrow::array::{
    ArrayRef, Float64Array, Int32Array, Int64Array, ListArray, MapArray, StringArray, StructArray,
};
use arrow::buffer::{NullBuffer, OffsetBuffer};
use arrow::datatypes::{DataType, Field, Fields, Schema};
use arrow::record_batch::RecordBatch;

use protoc_gen_fletcher_rust::fletcher_gen::rba::main::{AuxSamplesAccessor, CompositeMainAccessor};

// ----------------------------------------------------------------------------
// Fixture builders. 3 rows:
//   row 0: outer.inner.leaf.value=10, maybe_outer=present(110),
//          readings=[1.5, 2.5], track=[leaf=21, leaf=22]
//   row 1: outer.inner.leaf.value=20, maybe_outer=NULL,
//          readings=[] (empty),       track=[leaf=23 (NULL element)]
//   row 2: outer.inner.leaf.value=30, maybe_outer=present(130),
//          readings=[3.5 (NULL elem)], track=[] (empty)
// ----------------------------------------------------------------------------

// leaf = struct< value: int32 (non-nullable) >  (this is rba.child.Leaf).
fn leaf_fields() -> Fields {
    Fields::from(vec![Field::new("value", DataType::Int32, false)])
}
fn leaf_type() -> DataType {
    DataType::Struct(leaf_fields())
}

// inner = struct< leaf: leaf (non-nullable) >.
fn inner_fields() -> Fields {
    Fields::from(vec![Field::new("leaf", leaf_type(), false)])
}
fn inner_type() -> DataType {
    DataType::Struct(inner_fields())
}

// outer = struct< inner: inner (non-nullable) >.
fn outer_fields() -> Fields {
    Fields::from(vec![Field::new("inner", inner_type(), false)])
}
fn outer_type() -> DataType {
    DataType::Struct(outer_fields())
}

// Build a Leaf StructArray (no struct-level nulls) from value ints.
fn make_leaf(values: Vec<i32>) -> StructArray {
    let value_arr: ArrayRef = Arc::new(Int32Array::from(values));
    StructArray::new(leaf_fields(), vec![value_arr], None)
}

// Build an Inner StructArray wrapping a Leaf array.
fn make_inner(leaf: StructArray) -> StructArray {
    StructArray::new(inner_fields(), vec![Arc::new(leaf)], None)
}

// Build an Outer StructArray wrapping an Inner array, optional struct validity.
fn make_outer(inner: StructArray, valid: Option<Vec<bool>>) -> StructArray {
    let nulls = valid.map(NullBuffer::from);
    StructArray::new(outer_fields(), vec![Arc::new(inner)], nulls)
}

fn outer_col() -> ArrayRef {
    Arc::new(make_outer(
        make_inner(make_leaf(vec![10, 20, 30])),
        None,
    ))
}

fn maybe_outer_col() -> ArrayRef {
    Arc::new(make_outer(
        make_inner(make_leaf(vec![110, 0, 130])),
        Some(vec![true, false, true]),
    ))
}

// readings: list<double>. row0=[1.5,2.5], row1=[], row2=[NULL].
fn readings_col() -> ArrayRef {
    let values: ArrayRef = Arc::new(Float64Array::from(vec![Some(1.5), Some(2.5), None]));
    let offsets = OffsetBuffer::new(vec![0, 2, 2, 3].into());
    let field = Arc::new(Field::new("item", DataType::Float64, true));
    Arc::new(ListArray::new(field, offsets, values, None))
}

// track: list<struct<leaf:struct<value:int32>>>. Built over an Inner values array
// that carries ONE struct-level null at flattened index 2 (row1's only element).
// row0=[leaf.value=21, leaf.value=22], row1=[NULL element], row2=[].
fn track_col() -> ArrayRef {
    let leaf = make_leaf(vec![21, 22, 0]);
    // Inner with a struct-level null at index 2.
    let inner = StructArray::new(
        inner_fields(),
        vec![Arc::new(leaf)],
        Some(NullBuffer::from(vec![true, true, false])),
    );
    let offsets = OffsetBuffer::new(vec![0, 2, 3, 3].into());
    let field = Arc::new(Field::new("item", inner_type(), true));
    Arc::new(ListArray::new(field, offsets, Arc::new(inner), None))
}

// tag = struct<code:int32 (non-nullable)>  (the NO-PACKAGE Tag message). Resolves
// to crate::fletcher_gen::TagAccessor (D-RBA-10 no-package edge case).
fn tag_fields() -> Fields {
    Fields::from(vec![Field::new("code", DataType::Int32, false)])
}
fn tag_type() -> DataType {
    DataType::Struct(tag_fields())
}
fn tag_col() -> ArrayRef {
    let code: ArrayRef = Arc::new(Int32Array::from(vec![1001, 1002, 1003]));
    Arc::new(StructArray::new(tag_fields(), vec![code], None))
}

// The entries struct field shape for a map<utf8, V>. Map keys are non-null;
// the value field carries `value_nullable`.
fn map_entries_field(value_type: DataType, value_nullable: bool) -> Arc<Field> {
    Arc::new(Field::new(
        "entries",
        DataType::Struct(Fields::from(vec![
            Field::new("keys", DataType::Utf8, false),
            Field::new("values", value_type, value_nullable),
        ])),
        false,
    ))
}

// counts: map<utf8, int32>. row0={"a":1,"b":NULL}, row1={} (empty), row2={"c":3}.
// Scalar map value nulls are probed via value_is_null(j) (B2).
fn counts_col() -> ArrayRef {
    let keys: ArrayRef = Arc::new(StringArray::from(vec!["a", "b", "c"]));
    let values: ArrayRef = Arc::new(Int32Array::from(vec![Some(1), None, Some(3)]));
    let entries_field = map_entries_field(DataType::Int32, true);
    let entries = StructArray::new(
        match entries_field.data_type() {
            DataType::Struct(f) => f.clone(),
            _ => unreachable!(),
        },
        vec![keys, values],
        None,
    );
    let offsets = OffsetBuffer::new(vec![0, 2, 2, 3].into());
    Arc::new(MapArray::new(entries_field, offsets, entries, None, false))
}

// waypoints: map<utf8, Inner>. row0={"p":leaf=61,"q":NULL value}, row1={} (empty),
// row2={"r":leaf=63}. The value at flattened index 1 ("q") is a null struct value;
// StructMapSpan::value(j) -> None for it (B2).
fn waypoints_col() -> ArrayRef {
    let keys: ArrayRef = Arc::new(StringArray::from(vec!["p", "q", "r"]));
    // Inner value array with a struct-level null at index 1.
    let leaf = make_leaf(vec![61, 0, 63]);
    let inner = StructArray::new(
        inner_fields(),
        vec![Arc::new(leaf)],
        Some(NullBuffer::from(vec![true, false, true])),
    );
    let entries_field = map_entries_field(inner_type(), true);
    let entries = StructArray::new(
        match entries_field.data_type() {
            DataType::Struct(f) => f.clone(),
            _ => unreachable!(),
        },
        vec![keys, Arc::new(inner)],
        None,
    );
    let offsets = OffsetBuffer::new(vec![0, 2, 2, 3].into());
    Arc::new(MapArray::new(entries_field, offsets, entries, None, false))
}

fn counts_type() -> DataType {
    DataType::Map(map_entries_field(DataType::Int32, true), false)
}
fn waypoints_type() -> DataType {
    DataType::Map(map_entries_field(inner_type(), true), false)
}

// rings: list<list<struct<leaf:struct<value:int32>>>> (NESTED_LIST depth 2).
//   row0 = [[leaf=71], []]   (two inner lists: one with one element, one empty)
//   row1 = [NULL inner list] (one inner list that is null -> get(i) None)
//   row2 = []                (empty outer list)
// Flattened leaf Inner values: [71] (one element). Inner list offsets carve it.
fn rings_col() -> ArrayRef {
    // Leaf Inner values: a single element leaf.value=71.
    let leaf = make_leaf(vec![71]);
    let leaf_inner = StructArray::new(inner_fields(), vec![Arc::new(leaf)], None);
    // Inner list level: 3 inner lists total.
    //   inner[0] = [71]    (offsets 0..1)
    //   inner[1] = []      (offsets 1..1)
    //   inner[2] = NULL    (row1's single inner list is null)
    let inner_offsets = OffsetBuffer::new(vec![0, 1, 1, 1].into());
    let inner_item = Arc::new(Field::new("item", inner_type(), true));
    let inner_list = ListArray::new(
        inner_item.clone(),
        inner_offsets,
        Arc::new(leaf_inner),
        Some(NullBuffer::from(vec![true, true, false])),
    );
    // Outer list level over the inner lists:
    //   row0 = inner[0..2]  ([ [71], [] ])
    //   row1 = inner[2..3]  ([ NULL ])
    //   row2 = inner[3..3]  ([] empty)
    let outer_offsets = OffsetBuffer::new(vec![0, 2, 3, 3].into());
    let outer_item = Arc::new(Field::new("item", DataType::List(inner_item), true));
    Arc::new(ListArray::new(
        outer_item,
        outer_offsets,
        Arc::new(inner_list),
        None,
    ))
}

fn rings_type() -> DataType {
    let inner_item = Arc::new(Field::new("item", inner_type(), true));
    DataType::List(Arc::new(Field::new(
        "item",
        DataType::List(inner_item),
        true,
    )))
}

fn composite_schema_fields() -> Vec<Field> {
    vec![
        Field::new("outer", outer_type(), false),
        Field::new("maybe_outer", outer_type(), true),
        Field::new(
            "readings",
            DataType::List(Arc::new(Field::new("item", DataType::Float64, true))),
            false,
        ),
        Field::new(
            "track",
            DataType::List(Arc::new(Field::new("item", inner_type(), true))),
            false,
        ),
        Field::new("tag", tag_type(), false),
        Field::new("counts", counts_type(), false),
        Field::new("waypoints", waypoints_type(), false),
        Field::new("rings", rings_type(), false),
    ]
}

fn composite_columns() -> Vec<ArrayRef> {
    vec![
        outer_col(),
        maybe_outer_col(),
        readings_col(),
        track_col(),
        tag_col(),
        counts_col(),
        waypoints_col(),
        rings_col(),
    ]
}

// Schema carrying arbitrary, DOMAIN-NEUTRAL metadata (the RBA-3 lesson): generic
// k0=v0 schema metadata, plus per-field metadata on the `outer` column only.
fn composite_schema_with_metadata() -> Arc<Schema> {
    use std::collections::HashMap;
    let mut fields = composite_schema_fields();
    let mut f0_md = HashMap::new();
    f0_md.insert("fk0".to_string(), "fv0".to_string());
    fields[0] = fields[0].clone().with_metadata(f0_md);

    let mut schema_md = HashMap::new();
    schema_md.insert("k0".to_string(), "v0".to_string());
    Arc::new(Schema::new(fields).with_metadata(schema_md))
}

fn composite_batch_with_metadata() -> RecordBatch {
    RecordBatch::try_new(composite_schema_with_metadata(), composite_columns()).unwrap()
}

// Per-src-row expected REPEATED_SCALAR (readings) content: Some(v) is a present
// element, None is a null element. row0=[1.5,2.5], row1=[], row2=[null].
fn expected_readings(src: usize) -> Vec<Option<f64>> {
    match src {
        0 => vec![Some(1.5), Some(2.5)],
        1 => vec![],
        2 => vec![None],
        _ => unreachable!(),
    }
}

// Per-src-row expected REPEATED_STRUCT (track) leaf values: Some(v) present
// element, None null struct element. row0=[21,22], row1=[null elem], row2=[].
fn expected_track(src: usize) -> Vec<Option<i32>> {
    match src {
        0 => vec![Some(21), Some(22)],
        1 => vec![None],
        2 => vec![],
        _ => unreachable!(),
    }
}

// Assert the deep cross-package chain + repeated reads for `base`-windowed rows.
// Repeated columns are asserted for EVERY windowed row (incl. the sliced
// from_struct path, base != 0) — this is the FIX 3 sliced-list read coverage.
fn assert_reads(a: &CompositeMainAccessor, base: usize, len: usize) {
    assert_eq!(a.num_rows(), len);

    let leaves = [10, 20, 30];
    let maybe = [Some(110), None, Some(130)];
    let tags = [1001, 1002, 1003];

    for r in 0..len {
        let src = base + r;

        // STRUCT, cross-file + cross-PACKAGE, >= 2 levels: deep chain through Rows.
        // a.outer(r).inner().leaf().value() -> the grandchild Leaf in rba.child.
        assert_eq!(
            a.outer(r).inner().leaf().value(),
            leaves[src],
            "outer.inner.leaf.value row {r}"
        );

        // nullable 1:1 struct: None on the null row; Some otherwise.
        match maybe[src] {
            Some(v) => {
                let mo = a.maybe_outer(r).expect("maybe_outer present");
                assert_eq!(mo.inner().leaf().value(), v, "maybe_outer row {r}");
            }
            None => assert!(a.maybe_outer(r).is_none(), "maybe_outer null row {r}"),
        }

        // NO-PACKAGE cross-file struct: crate::fletcher_gen::TagAccessor chain.
        assert_eq!(a.tag(r).code(), tags[src], "tag.code row {r}");

        // REPEATED_SCALAR: ScalarSpan len / is_empty / value / is_null, asserted
        // for EVERY windowed row (sliced-list read on the from_struct path).
        let exp_r = expected_readings(src);
        let rs = a.readings(r);
        assert_eq!(rs.len(), exp_r.len(), "readings len row {r}");
        assert_eq!(rs.is_empty(), exp_r.is_empty(), "readings is_empty row {r}");
        for (j, want) in exp_r.iter().enumerate() {
            match want {
                Some(v) => {
                    assert!(!rs.is_null(j), "readings[{j}] not null row {r}");
                    assert_eq!(rs.value(j), *v, "readings[{j}] value row {r}");
                }
                None => assert!(rs.is_null(j), "readings[{j}] null row {r}"),
            }
        }

        // REPEATED_STRUCT: StructSpan::get(j) -> Some(Row); None on null element.
        let exp_t = expected_track(src);
        let ts = a.track(r);
        assert_eq!(ts.len(), exp_t.len(), "track len row {r}");
        assert_eq!(ts.is_empty(), exp_t.is_empty(), "track is_empty row {r}");
        for (j, want) in exp_t.iter().enumerate() {
            match want {
                Some(v) => {
                    assert!(!ts.is_null(j), "track[{j}] not null row {r}");
                    assert_eq!(
                        ts.get(j).expect("track element present").leaf().value(),
                        *v,
                        "track[{j}] leaf row {r}"
                    );
                }
                None => {
                    assert!(ts.is_null(j), "track[{j}] null row {r}");
                    assert!(ts.get(j).is_none(), "track[{j}] None row {r}");
                }
            }
        }
    }
}

#[test]
fn composite_and_metadata_read() {
    use std::collections::HashMap;

    let batch = composite_batch_with_metadata();
    let acc = CompositeMainAccessor::try_new(batch).expect("try_new should succeed");

    // Generic schema metadata read-back (domain-neutral k0=v0), by borrowed ref.
    let sm: &HashMap<String, String> = acc.schema_metadata();
    assert_eq!(sm.get("k0").map(String::as_str), Some("v0"));

    // Field metadata by index: field 0 ('outer') carries fk0=fv0.
    let fm0 = acc.field_metadata(0);
    assert_eq!(fm0.get("fk0").map(String::as_str), Some("fv0"));

    // Absent field metadata is an empty map, never an error (D-RBA-5).
    assert!(acc.field_metadata(1).is_empty());

    // Out-of-bounds index -> empty map, never panic.
    assert!(acc.field_metadata(99999).is_empty());

    // Deep cross-package chain + nullable struct + repeated reads.
    assert_reads(&acc, 0, 3);

    // MAP scalar value (counts): key / value / value_is_null, empty map row.
    // row0={"a":1,"b":NULL}, row1={} empty, row2={"c":3}.
    {
        let m0 = acc.counts(0);
        assert_eq!(m0.len(), 2);
        assert!(!m0.is_empty());
        assert_eq!(m0.key(0), "a");
        assert!(!m0.value_is_null(0));
        assert_eq!(m0.value(0), 1);
        assert_eq!(m0.key(1), "b");
        assert!(m0.value_is_null(1)); // null scalar map value via value_is_null(j)

        let m1 = acc.counts(1);
        assert_eq!(m1.len(), 0);
        assert!(m1.is_empty());

        let m2 = acc.counts(2);
        assert_eq!(m2.len(), 1);
        assert_eq!(m2.key(0), "c");
        assert_eq!(m2.value(0), 3);
    }

    // MAP message value (waypoints): value(j) -> Option<Row>, None on a null map
    // value (B2). row0={"p":leaf=61,"q":NULL}, row1={} empty, row2={"r":leaf=63}.
    {
        let w0 = acc.waypoints(0);
        assert_eq!(w0.len(), 2);
        assert_eq!(w0.key(0), "p");
        assert_eq!(
            w0.value(0).expect("waypoints[0] present").leaf().value(),
            61
        );
        assert_eq!(w0.key(1), "q");
        assert!(w0.value(1).is_none(), "null map message value -> None");

        let w1 = acc.waypoints(1);
        assert!(w1.is_empty());

        let w2 = acc.waypoints(2);
        assert_eq!(w2.len(), 1);
        assert_eq!(
            w2.value(0).expect("waypoints[2] present").leaf().value(),
            63
        );
    }

    // NESTED_LIST depth 2 (rings): get(i) -> Option<inner span>, None on a null
    // inner list (B2). row0=[[71],[]], row1=[NULL inner list], row2=[].
    {
        let r0 = acc.rings(0);
        assert_eq!(r0.len(), 2);
        assert!(!r0.is_empty());
        let s00 = r0.get(0).expect("rings[0][0] inner list present");
        assert_eq!(s00.len(), 1);
        assert_eq!(s00.get(0).expect("rings[0][0][0]").leaf().value(), 71);
        let s01 = r0.get(1).expect("rings[0][1] inner list present (empty)");
        assert_eq!(s01.len(), 0);
        assert!(s01.is_empty());

        let r1 = acc.rings(1);
        assert_eq!(r1.len(), 1);
        assert!(r1.is_null(0)); // the single inner list is null
        assert!(r1.get(0).is_none(), "null inner list -> None");

        let r2 = acc.rings(2);
        assert_eq!(r2.len(), 0);
        assert!(r2.is_empty());
    }

    // Non-nullable struct field with a RUNTIME null -> Err (D-RBA-4 recursed). The
    // full (8-column) field set is supplied so the column-count gate passes and the
    // struct null_count gate is the thing under test. Field 0 ('outer') flag is
    // flipped to nullable so arrow-rs accepts the injected null at construction.
    {
        let bad_outer = Arc::new(make_outer(
            make_inner(make_leaf(vec![10, 20, 30])),
            Some(vec![true, false, true]), // a null in a proto-non-nullable struct
        )) as ArrayRef;
        let mut fields = composite_schema_fields();
        fields[0] = Field::new("outer", outer_type(), true); // flag tolerant
        let schema = Arc::new(Schema::new(fields));
        let mut cols = composite_columns();
        cols[0] = bad_outer;
        let bad = RecordBatch::try_new(schema, cols).unwrap();
        assert!(
            CompositeMainAccessor::try_new(bad).is_err(),
            "a runtime null in a proto-non-nullable struct must yield Err"
        );
    }

    // from_struct parity with a NON-ZERO offset (exercises §2.4 child slicing) +
    // struct-sourced empty schema metadata.
    {
        let fields: Fields = Fields::from(composite_schema_fields());
        let sa = StructArray::new(fields, composite_columns(), None);
        let sliced = sa.slice(1, 2); // window rows [1, 3): src rows 1 and 2.
        let acc_struct =
            CompositeMainAccessor::from_struct(&sliced).expect("from_struct should succeed");
        // A struct-sourced accessor has no schema-level metadata: empty map.
        assert!(acc_struct.schema_metadata().is_empty());
        assert_reads(&acc_struct, 1, 2);
    }
}

#[test]
fn from_struct_sliced_input_reads_correctly() {
    // FIX 3: prove the sliced-input offset math empirically. Pass a NON-ZERO-offset
    // sliced StructArray (containing nested struct, REPEATED_SCALAR, and
    // REPEATED_STRUCT columns) to from_struct and read both the sliced LIST columns
    // and the sliced nested-struct/validity handle. If the offset math or the
    // struct-validity windowing were wrong, the windowed reads would mismatch or
    // panic. The repeated reads inside assert_reads index value_offsets()[row] with
    // the LOGICAL (0-based) row, which is correct because ListArray::slice rebases
    // the offsets buffer while sharing the (unsliced) values child (arrow-rs 59).
    let fields: Fields = Fields::from(composite_schema_fields());
    let sa = StructArray::new(fields, composite_columns(), None);

    // Window [2, 3): only src row 2 — readings=[null], track=[] (empty), the deep
    // chain leaf=30, maybe_outer present(130), tag.code=1003. Exercises a window
    // that starts at a non-zero offset distinct from the [1,2) case above.
    let sliced = sa.slice(2, 1);
    let acc = CompositeMainAccessor::from_struct(&sliced).expect("from_struct should succeed");
    assert_eq!(acc.num_rows(), 1);
    assert_reads(&acc, 2, 1);

    // is_null(row) on the struct-sourced accessor reflects the SLICED struct's own
    // top-level validity (here: no top-level nulls -> false), proving the retained
    // validity handle shares the 0-based window origin with the sliced children.
    use protoc_gen_fletcher_rust::fletcher_gen::__rba::RowAccess;
    assert!(!RowAccess::is_null(&acc, 0), "no top-level struct null in window");
}

// cell = struct<n:int32 (non-nullable)>  (composite_aux.proto Cell).
fn cell_fields() -> Fields {
    Fields::from(vec![Field::new("n", DataType::Int32, false)])
}
fn cell_type() -> DataType {
    DataType::Struct(cell_fields())
}

// regions: list<list<list<struct<n:int32>>>> (NESTED_LIST depth 3). 2 rows:
//   row0 = [ [ [n=1, n=2] ], NULL_mid_list ]
//          (outer[0] has two mid lists: the first has one inner list [1,2];
//           the second mid list is null -> NestedStructSpan3::get returns None)
//   row1 = []  (empty outer)
// Flattened leaf Cell values: [1, 2].
fn regions_col() -> ArrayRef {
    // Leaf Cell values.
    let n: ArrayRef = Arc::new(Int32Array::from(vec![1, 2]));
    let leaf = StructArray::new(cell_fields(), vec![n], None);
    // Inner list level: one inner list = [Cell(1), Cell(2)].
    let inner_item = Arc::new(Field::new("item", cell_type(), true));
    let inner_offsets = OffsetBuffer::new(vec![0, 2].into());
    let inner_list = ListArray::new(inner_item.clone(), inner_offsets, Arc::new(leaf), None);
    // Mid list level: two mid lists — mid[0]=[inner[0]] , mid[1]=NULL.
    let mid_item = Arc::new(Field::new("item", DataType::List(inner_item), true));
    let mid_offsets = OffsetBuffer::new(vec![0, 1, 1].into());
    let mid_list = ListArray::new(
        mid_item.clone(),
        mid_offsets,
        Arc::new(inner_list),
        Some(NullBuffer::from(vec![true, false])),
    );
    // Outer list level: row0 = mid[0..2], row1 = [] (empty).
    let outer_item = Arc::new(Field::new("item", DataType::List(mid_item), true));
    let outer_offsets = OffsetBuffer::new(vec![0, 2, 2].into());
    Arc::new(ListArray::new(
        outer_item,
        outer_offsets,
        Arc::new(mid_list),
        None,
    ))
}

fn regions_type() -> DataType {
    let inner_item = Arc::new(Field::new("item", cell_type(), true));
    let mid_item = Arc::new(Field::new("item", DataType::List(inner_item), true));
    DataType::List(Arc::new(Field::new("item", DataType::List(mid_item), true)))
}

#[test]
fn same_package_two_file_composite_co_mount() {
    // R1: composite_main (CompositeMain) and composite_aux (AuxSamples) both live
    // under `rba::main` and BOTH emit a composite getter. This test compiles only
    // if the fully-qualified `crate::fletcher_gen::__rba::*` helper paths in both
    // co-mounted files do NOT collide (no per-file `use`, no duplicate helper
    // defs). AuxSamples.samples is a REPEATED_SCALAR -> ScalarSpan; AuxSamples
    // also carries a NESTED_LIST depth 3 (`regions`) -> NestedStructSpan3.
    let values: ArrayRef = Arc::new(Int64Array::from(vec![7i64, 8, 9, 10]));
    let offsets = OffsetBuffer::new(vec![0, 2, 4].into());
    let field = Arc::new(Field::new("item", DataType::Int64, true));
    let list = Arc::new(ListArray::new(field.clone(), offsets, values, None)) as ArrayRef;
    let schema = Arc::new(Schema::new(vec![
        Field::new("samples", DataType::List(field), false),
        Field::new("regions", regions_type(), false),
    ]));
    let batch = RecordBatch::try_new(schema, vec![list, regions_col()]).unwrap();
    let aux = AuxSamplesAccessor::try_new(batch).expect("AuxSamples accessor");
    assert_eq!(aux.num_rows(), 2);

    let s0 = aux.samples(0);
    assert_eq!(s0.len(), 2);
    assert_eq!(s0.value(0), 7);
    assert_eq!(s0.value(1), 8);

    let s1 = aux.samples(1);
    assert_eq!(s1.len(), 2);
    assert_eq!(s1.value(0), 9);
    assert_eq!(s1.value(1), 10);

    // NESTED_LIST depth 3 (regions): outer -> mid -> inner -> leaf, with a null
    // mid list yielding None (B2). row0 = [ [ [1,2] ], NULL_mid ], row1 = [].
    let g0 = aux.regions(0);
    assert_eq!(g0.len(), 2); // two mid lists in the outer row
    let mid0 = g0.get(0).expect("regions[0][0] mid list present");
    assert_eq!(mid0.len(), 1); // one inner list
    let inner0 = mid0.get(0).expect("regions[0][0][0] inner list present");
    assert_eq!(inner0.len(), 2);
    assert_eq!(inner0.get(0).expect("cell 0").n(), 1);
    assert_eq!(inner0.get(1).expect("cell 1").n(), 2);
    assert!(g0.is_null(1), "second mid list is null");
    assert!(g0.get(1).is_none(), "null mid list -> None");

    let g1 = aux.regions(1);
    assert_eq!(g1.len(), 0);
    assert!(g1.is_empty());
}
