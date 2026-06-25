// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! RBA-5 forcing + acceptance tests for the generated Rust scalar accessor.
//!
//! The forcing test `scalar_columns_read_and_validate_positionally` builds a
//! RecordBatch matching `Telemetry`, asserts per-row scalar reads (incl.
//! nullable `Option`, and the timestamp/duration -> i64 temporal path that
//! exercises the generic unit-parsing in the emitter), a type-mismatch -> `Err`,
//! a name-mismatch type-compatible -> `Ok`, and that the same fixture built as a
//! `StructArray` (with a non-zero offset) reads identically via `from_struct` —
//! proving the §2.4 child-slicing.
//!
//! Acceptance tests cover wrong-column-count -> `Err`, a runtime null in a
//! non-nullable column -> `Err`, and the D-RBA-10 same-package multi-file
//! mounting (`Pressure` + `Humidity` both under `fletcher::rba::shared`).

use std::sync::Arc;

use arrow::array::{
    ArrayRef, BinaryArray, DurationNanosecondArray, Float32Array, Float64Array, Int32Array,
    Int64Array, StringArray, StructArray, TimestampNanosecondArray,
};
use arrow::datatypes::{DataType, Field, Fields, Schema, TimeUnit};
use arrow::record_batch::RecordBatch;

use protoc_gen_fletcher_rust::fletcher_gen::fletcher::rba::shared::{
    HumidityAccessor, PressureAccessor,
};
use protoc_gen_fletcher_rust::fletcher_gen::fletcher::rba::telem::TelemetryAccessor;

// Fixture: 3 rows. Row 1 has the nullable fields null.
const TEMPS: [f64; 3] = [10.5, -20.25, 30.125];
const IDS: [i32; 3] = [-1, 0, 2147483647];
// occurred_at (timestamp ns) and elapsed (duration ns) are non-nullable i64.
const OCCURRED: [i64; 3] = [1_700_000_000_000_000_000, 0, -42];
const ELAPSED: [i64; 3] = [5, 1_000_000, 9_223_372_036_854_775_807];

fn temperature_col() -> ArrayRef {
    Arc::new(Float64Array::from(TEMPS.to_vec()))
}
fn id_col() -> ArrayRef {
    Arc::new(Int32Array::from(IDS.to_vec()))
}
fn label_col() -> ArrayRef {
    Arc::new(StringArray::from(vec![Some("alpha"), None, Some("gamma")]))
}
fn blob_col() -> ArrayRef {
    Arc::new(BinaryArray::from(vec![&b"aa"[..], &b"bb"[..], &b"cc"[..]]))
}
fn opt_blob_col() -> ArrayRef {
    Arc::new(BinaryArray::from(vec![
        Some(&b"pp"[..]),
        None,
        Some(&b"rr"[..]),
    ]))
}
fn occurred_col() -> ArrayRef {
    Arc::new(TimestampNanosecondArray::from(OCCURRED.to_vec()))
}
fn elapsed_col() -> ArrayRef {
    Arc::new(DurationNanosecondArray::from(ELAPSED.to_vec()))
}

fn telemetry_schema() -> Arc<Schema> {
    Arc::new(Schema::new(vec![
        Field::new("temperature", DataType::Float64, false),
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
        Field::new("blob", DataType::Binary, false),
        Field::new("opt_blob", DataType::Binary, true),
        Field::new(
            "occurred_at",
            DataType::Timestamp(TimeUnit::Nanosecond, None),
            false,
        ),
        Field::new("elapsed", DataType::Duration(TimeUnit::Nanosecond), false),
    ]))
}

fn telemetry_columns() -> Vec<ArrayRef> {
    vec![
        temperature_col(),
        id_col(),
        label_col(),
        blob_col(),
        opt_blob_col(),
        occurred_col(),
        elapsed_col(),
    ]
}

fn telemetry_batch() -> RecordBatch {
    RecordBatch::try_new(telemetry_schema(), telemetry_columns()).unwrap()
}

/// Assert the accessor reads the full fixture identically regardless of factory.
/// `base` is the row offset into the original fixture this accessor covers (0 for
/// the RecordBatch path; the slice offset for the struct path).
fn assert_reads(acc: &TelemetryAccessor, base: usize, len: usize) {
    assert_eq!(acc.num_rows(), len);
    for r in 0..len {
        let src = base + r;
        assert_eq!(acc.temperature(r), TEMPS[src], "temperature row {r}");
        assert_eq!(acc.id(r), IDS[src], "id row {r}");
        // blob: non-null &[u8].
        let expected_blob: &[u8] = match src {
            0 => b"aa",
            1 => b"bb",
            2 => b"cc",
            _ => unreachable!(),
        };
        assert_eq!(acc.blob(r), expected_blob, "blob row {r}");
        // label: Option<&str>, null on src row 1.
        let expected_label = match src {
            0 => Some("alpha"),
            1 => None,
            2 => Some("gamma"),
            _ => unreachable!(),
        };
        assert_eq!(acc.label(r), expected_label, "label row {r}");
        // opt_blob: Option<&[u8]>, null on src row 1.
        let expected_opt_blob: Option<&[u8]> = match src {
            0 => Some(b"pp"),
            1 => None,
            2 => Some(b"rr"),
            _ => unreachable!(),
        };
        assert_eq!(acc.opt_blob(r), expected_opt_blob, "opt_blob row {r}");
        // occurred_at: timestamp(ns) -> i64 (non-null), value round-trips.
        assert_eq!(acc.occurred_at(r), OCCURRED[src], "occurred_at row {r}");
        // elapsed: duration(ns) -> i64 (non-null), value round-trips.
        assert_eq!(acc.elapsed(r), ELAPSED[src], "elapsed row {r}");
    }
}

#[test]
fn scalar_columns_read_and_validate_positionally() {
    // (2) construct from a RecordBatch.
    let batch = telemetry_batch();
    let acc = TelemetryAccessor::try_new(batch.clone()).expect("try_new should succeed");

    // (3)+(4) per-row scalar reads incl. nullable Option.
    assert_reads(&acc, 0, 3);

    // (5) type-mismatch -> Err (never panics): Int64 where Int32 'id' is expected.
    {
        let bad_id: ArrayRef =
            Arc::new(Int64Array::from(IDS.iter().map(|&v| v as i64).collect::<Vec<_>>()));
        let schema = Arc::new(Schema::new(vec![
            Field::new("temperature", DataType::Float64, false),
            Field::new("id", DataType::Int64, false), // wrong type
            Field::new("label", DataType::Utf8, true),
            Field::new("blob", DataType::Binary, false),
            Field::new("opt_blob", DataType::Binary, true),
            Field::new(
                "occurred_at",
                DataType::Timestamp(TimeUnit::Nanosecond, None),
                false,
            ),
            Field::new("elapsed", DataType::Duration(TimeUnit::Nanosecond), false),
        ]));
        let bad = RecordBatch::try_new(
            schema,
            vec![
                temperature_col(),
                bad_id,
                label_col(),
                blob_col(),
                opt_blob_col(),
                occurred_col(),
                elapsed_col(),
            ],
        )
        .unwrap();
        let res = TelemetryAccessor::try_new(bad);
        assert!(res.is_err(), "type-mismatched batch must yield Err");
    }

    // (6) name-mismatch but type-compatible -> Ok, reads the same values. Field
    // NAMES differ from the proto and the nullable FLAGS are all flipped to true
    // (arrow-rs RecordBatch construction enforces the flag against the data, so a
    // flag of `true` accepts the null `label`/`opt_blob` columns; the accessor's
    // own null_count gate is keyed on the PROTO nullability, not this flag). The
    // type-only gate still requires each column's exact DataType (incl. the
    // temporal unit), so the timestamp/duration types are kept correct here.
    {
        let schema = Arc::new(Schema::new(vec![
            Field::new("TEMP_RENAMED", DataType::Float64, true),
            Field::new("identifier", DataType::Int32, true),
            Field::new("lbl", DataType::Utf8, true),
            Field::new("payload", DataType::Binary, true),
            Field::new("opt_payload", DataType::Binary, true),
            Field::new("when", DataType::Timestamp(TimeUnit::Nanosecond, None), true),
            Field::new("dur", DataType::Duration(TimeUnit::Nanosecond), true),
        ]));
        let renamed = RecordBatch::try_new(schema, telemetry_columns()).unwrap();
        let acc2 =
            TelemetryAccessor::try_new(renamed).expect("name/flag-mismatch must still succeed");
        assert_reads(&acc2, 0, 3);
    }

    // (7) from_struct parity with a NON-ZERO offset (exercises §2.4 child slicing).
    {
        let fields: Fields = telemetry_schema().fields().clone();
        let sa = StructArray::new(fields, telemetry_columns(), None);
        // Window rows [1, 3): src rows 1 and 2.
        let sliced = sa.slice(1, 2);
        let acc_struct =
            TelemetryAccessor::from_struct(&sliced).expect("from_struct should succeed");
        // Reads must match the original fixture rows 1 and 2 exactly.
        assert_reads(&acc_struct, 1, 2);
    }
}

#[test]
fn wrong_column_count_is_err() {
    // Too few columns (drop opt_blob) -> Err, never panic.
    let schema = Arc::new(Schema::new(vec![
        Field::new("temperature", DataType::Float64, false),
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
        Field::new("blob", DataType::Binary, false),
    ]));
    let batch = RecordBatch::try_new(
        schema,
        vec![temperature_col(), id_col(), label_col(), blob_col()],
    )
    .unwrap();
    assert!(TelemetryAccessor::try_new(batch).is_err());
}

#[test]
fn nonnullable_with_runtime_null_is_err() {
    // 'temperature' is proto-non-nullable; inject a real null. The schema field is
    // declared nullable=true so arrow-rs accepts the null at RecordBatch
    // construction — the accessor's own null_count gate (keyed on PROTO
    // nullability, name/flag-tolerant) is what must reject it -> Err. The full
    // 7-column set is supplied so the column-count gate passes and the null_count
    // gate is the thing under test.
    let temp_with_null: ArrayRef = Arc::new(Float64Array::from(vec![Some(1.0), None, Some(3.0)]));
    let schema = Arc::new(Schema::new(vec![
        Field::new("temperature", DataType::Float64, true), // flag tolerant; data has a null
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
        Field::new("blob", DataType::Binary, false),
        Field::new("opt_blob", DataType::Binary, true),
        Field::new(
            "occurred_at",
            DataType::Timestamp(TimeUnit::Nanosecond, None),
            false,
        ),
        Field::new("elapsed", DataType::Duration(TimeUnit::Nanosecond), false),
    ]));
    let batch = RecordBatch::try_new(
        schema,
        vec![
            temp_with_null,
            id_col(),
            label_col(),
            blob_col(),
            opt_blob_col(),
            occurred_col(),
            elapsed_col(),
        ],
    )
    .unwrap();
    assert!(
        TelemetryAccessor::try_new(batch).is_err(),
        "a runtime null in a proto-non-nullable column must yield Err"
    );
}

#[test]
fn same_package_multi_file_mounting() {
    // D-RBA-10: Pressure (sensors_a) and Humidity (sensors_b) both live under the
    // single fletcher::rba::shared module — proving same-package multi-file
    // mounting resolves and the path scheme is correct.
    let p_schema = Arc::new(Schema::new(vec![Field::new("kpa", DataType::Float64, false)]));
    let p_batch = RecordBatch::try_new(
        p_schema,
        vec![Arc::new(Float64Array::from(vec![101.3, 99.9])) as ArrayRef],
    )
    .unwrap();
    let pressure = PressureAccessor::try_new(p_batch).expect("Pressure accessor");
    assert_eq!(pressure.num_rows(), 2);
    assert_eq!(pressure.kpa(0), 101.3);
    assert_eq!(pressure.kpa(1), 99.9);

    let h_schema = Arc::new(Schema::new(vec![Field::new("pct", DataType::Float32, false)]));
    let h_batch = RecordBatch::try_new(
        h_schema,
        vec![Arc::new(Float32Array::from(vec![55.5f32, 60.0])) as ArrayRef],
    )
    .unwrap();
    let humidity = HumidityAccessor::try_new(h_batch).expect("Humidity accessor");
    assert_eq!(humidity.num_rows(), 2);
    assert_eq!(humidity.pct(0), 55.5f32);
    assert_eq!(humidity.pct(1), 60.0f32);
}
