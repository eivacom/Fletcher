// arrow-row: Rust bindings for the arrow_row C library.
//
// Module layout mirrors the C++ header structure:
//
//   ffi      — raw unsafe C declarations (private)
//   error    — Error type
//   ipc      — Arrow IPC serialization helpers (private)
//   codec    — RowCodec          ← row_codec.hpp
//   wal      — WriteAheadLog, SqliteWal  ← write_ahead_log.hpp / sqlite_wal.hpp
//   batcher  — RowBatcher, FlushHandler, GenericRowBatcher
//                               ← row_batcher.hpp / generic_row_batcher.hpp

mod ffi;
mod ipc;

pub mod error;
pub mod codec;
pub mod wal;
pub mod batcher;

pub use error::Error;
pub use codec::RowCodec;
pub use wal::{SqliteWal, WriteAheadLog};
pub use batcher::{FlushHandler, GenericRowBatcher, RowBatcher};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use arrow::array::{Float64Array, Int32Array, StringArray};
    use arrow::datatypes::{DataType, Field, Schema};
    use arrow::array::RecordBatch;

    use crate::{GenericRowBatcher, RowBatcher, RowCodec, SqliteWal, FlushHandler};

    fn make_schema() -> Arc<Schema> {
        Arc::new(Schema::new(vec![
            Field::new("id",    DataType::Int32,   false),
            Field::new("name",  DataType::Utf8,    false),
            Field::new("score", DataType::Float64, false),
        ]))
    }

    fn make_batch(schema: &Arc<Schema>, id: i32, name: &str, score: f64) -> RecordBatch {
        RecordBatch::try_new(
            Arc::clone(schema),
            vec![
                Arc::new(Int32Array::from(vec![id])),
                Arc::new(StringArray::from(vec![name])),
                Arc::new(Float64Array::from(vec![score])),
            ],
        )
        .unwrap()
    }

    #[test]
    fn codec_roundtrip() {
        let schema = make_schema();
        let codec = RowCodec::new(&schema).unwrap();
        let batch = make_batch(&schema, 42, "Alice", 98.6);
        let encoded = codec.encode_row(&batch).unwrap();
        let decoded = codec.decode_row(&encoded).unwrap();
        assert_eq!(decoded.num_rows(), 1);
        assert_eq!(decoded.num_columns(), 3);
    }

    #[test]
    fn batcher_new_with_closure() {
        let schema = make_schema();
        let mut wal = SqliteWal::new(":memory:").unwrap();
        let codec = RowCodec::new(&schema).unwrap();

        let received = Arc::new(Mutex::new(Vec::<RecordBatch>::new()));
        let rx = Arc::clone(&received);

        let batcher = GenericRowBatcher::new(&mut wal, &schema, 2, move |batch| {
            rx.lock().unwrap().push(batch);
            true
        })
        .unwrap();

        let r1 = codec.encode_row(&make_batch(&schema, 1, "Alice", 1.0)).unwrap();
        let r2 = codec.encode_row(&make_batch(&schema, 2, "Bob",   2.0)).unwrap();
        batcher.append(&r1).unwrap();
        batcher.append(&r2).unwrap();  // triggers flush

        drop(batcher);  // joins background flush thread

        let batches = received.lock().unwrap();
        assert_eq!(batches.len(), 1);
        assert_eq!(batches[0].num_rows(), 2);
    }

    // Demonstrates the FlushHandler trait: mirrors C++ "subclassing RowBatcher
    // to override OnBatchFlushSucceeded / OnBatchFlushFailed".
    #[test]
    fn batcher_with_handler() {
        struct CountingHandler {
            succeeded: Arc<Mutex<u32>>,
            failed:    Arc<Mutex<u32>>,
        }

        impl FlushHandler for CountingHandler {
            fn on_flush(&self, _batch: RecordBatch) -> bool { true }

            fn on_batch_flush_succeeded(&self) {
                *self.succeeded.lock().unwrap() += 1;
            }

            fn on_batch_flush_failed(&self) {
                *self.failed.lock().unwrap() += 1;
            }
        }

        let schema = make_schema();
        let mut wal = SqliteWal::new(":memory:").unwrap();
        let codec = RowCodec::new(&schema).unwrap();

        let succeeded = Arc::new(Mutex::new(0u32));
        let failed    = Arc::new(Mutex::new(0u32));

        let batcher = GenericRowBatcher::with_handler(
            &mut wal,
            &schema,
            2,
            CountingHandler {
                succeeded: Arc::clone(&succeeded),
                failed:    Arc::clone(&failed),
            },
        )
        .unwrap();

        let r1 = codec.encode_row(&make_batch(&schema, 1, "Alice", 1.0)).unwrap();
        let r2 = codec.encode_row(&make_batch(&schema, 2, "Bob",   2.0)).unwrap();
        batcher.append(&r1).unwrap();
        batcher.append(&r2).unwrap();

        drop(batcher);

        assert_eq!(*succeeded.lock().unwrap(), 1);
        assert_eq!(*failed.lock().unwrap(),    0);
    }
}
