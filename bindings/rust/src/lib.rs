// Rust bindings for the arrow_row C API.
//
// The `ffi` module contains raw unsafe declarations mirroring arrow_row_capi.h.
// The public types (RowCodec, SqliteWal, GenericRowBatcher) provide safe
// wrappers with standard Rust ownership and error handling.

use std::ffi::{CStr, CString};
use std::io::Cursor;
use std::os::raw::{c_char, c_void};
use std::sync::Arc;

use arrow::array::RecordBatch;
use arrow::datatypes::Schema;
use arrow::ipc::reader::StreamReader;
use arrow::ipc::writer::StreamWriter;

// ---------------------------------------------------------------------------
// Raw FFI declarations
// ---------------------------------------------------------------------------

mod ffi {
    use std::os::raw::{c_char, c_void};

    pub enum ArrowRowCodec {}
    pub enum ArrowRowSQLiteWAL {}
    pub enum ArrowRowBatcher {}

    pub type FlushFn = unsafe extern "C" fn(
        ipc_data: *const u8,
        ipc_len: usize,
        userdata: *mut c_void,
    ) -> bool;

    extern "C" {
        pub fn arrow_row_free_string(s: *mut c_char);
        pub fn arrow_row_free_bytes(data: *mut u8);

        pub fn arrow_row_codec_new(
            schema_ipc: *const u8,
            schema_ipc_len: usize,
            out_error: *mut *mut c_char,
        ) -> *mut ArrowRowCodec;
        pub fn arrow_row_codec_free(codec: *mut ArrowRowCodec);
        pub fn arrow_row_codec_encode_row(
            codec: *const ArrowRowCodec,
            ipc_data: *const u8,
            ipc_len: usize,
            out_data: *mut *mut u8,
            out_len: *mut usize,
            out_error: *mut *mut c_char,
        ) -> bool;
        pub fn arrow_row_codec_decode_row(
            codec: *const ArrowRowCodec,
            row_data: *const u8,
            row_len: usize,
            out_ipc: *mut *mut u8,
            out_ipc_len: *mut usize,
            out_error: *mut *mut c_char,
        ) -> bool;

        pub fn arrow_row_sqlite_wal_new(
            path: *const std::os::raw::c_char,
            out_error: *mut *mut c_char,
        ) -> *mut ArrowRowSQLiteWAL;
        pub fn arrow_row_sqlite_wal_free(wal: *mut ArrowRowSQLiteWAL);

        pub fn arrow_row_batcher_new(
            wal: *mut ArrowRowSQLiteWAL,
            schema_ipc: *const u8,
            schema_ipc_len: usize,
            batch_size: i64,
            on_flush: FlushFn,
            userdata: *mut c_void,
            out_error: *mut *mut c_char,
        ) -> *mut ArrowRowBatcher;
        pub fn arrow_row_batcher_free(batcher: *mut ArrowRowBatcher);
        pub fn arrow_row_batcher_append(
            batcher: *mut ArrowRowBatcher,
            row_data: *const u8,
            row_len: usize,
            out_error: *mut *mut c_char,
        ) -> bool;
        pub fn arrow_row_batcher_row_count(batcher: *const ArrowRowBatcher) -> i64;
    }
}

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

#[derive(Debug)]
pub struct Error(String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

unsafe fn read_error(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return "unknown error".into();
    }
    let msg = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    ffi::arrow_row_free_string(ptr);
    msg
}

fn serialize_schema(schema: &Schema) -> Result<Vec<u8>, Error> {
    let mut buf = Vec::new();
    let mut writer = StreamWriter::try_new(&mut buf, schema)
        .map_err(|e| Error(e.to_string()))?;
    writer.finish().map_err(|e| Error(e.to_string()))?;
    Ok(buf)
}

fn serialize_batch(batch: &RecordBatch) -> Result<Vec<u8>, Error> {
    let mut buf = Vec::new();
    let mut writer = StreamWriter::try_new(&mut buf, batch.schema_ref())
        .map_err(|e| Error(e.to_string()))?;
    writer.write(batch).map_err(|e| Error(e.to_string()))?;
    writer.finish().map_err(|e| Error(e.to_string()))?;
    Ok(buf)
}

fn deserialize_batch(bytes: &[u8]) -> Result<RecordBatch, Error> {
    let cursor = Cursor::new(bytes);
    let mut reader = StreamReader::try_new(cursor, None)
        .map_err(|e| Error(e.to_string()))?;
    reader
        .next()
        .ok_or_else(|| Error("IPC stream contained no record batches".into()))?
        .map_err(|e| Error(e.to_string()))
}

// ---------------------------------------------------------------------------
// RowCodec
// ---------------------------------------------------------------------------

pub struct RowCodec(*mut ffi::ArrowRowCodec);

unsafe impl Send for RowCodec {}
unsafe impl Sync for RowCodec {}

impl RowCodec {
    pub fn new(schema: &Schema) -> Result<Self, Error> {
        let ipc = serialize_schema(schema)?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe {
            ffi::arrow_row_codec_new(ipc.as_ptr(), ipc.len(), &mut err)
        };
        if ptr.is_null() {
            Err(Error(unsafe { read_error(err) }))
        } else {
            Ok(RowCodec(ptr))
        }
    }

    /// Encode a single-row `RecordBatch` into an `ArrowRow` byte buffer.
    pub fn encode_row(&self, batch: &RecordBatch) -> Result<Vec<u8>, Error> {
        let ipc = serialize_batch(batch)?;
        let mut out_data: *mut u8 = std::ptr::null_mut();
        let mut out_len: usize = 0;
        let mut err: *mut c_char = std::ptr::null_mut();

        let ok = unsafe {
            ffi::arrow_row_codec_encode_row(
                self.0,
                ipc.as_ptr(), ipc.len(),
                &mut out_data, &mut out_len,
                &mut err,
            )
        };

        if ok {
            let bytes = unsafe { std::slice::from_raw_parts(out_data, out_len).to_vec() };
            unsafe { ffi::arrow_row_free_bytes(out_data) };
            Ok(bytes)
        } else {
            Err(Error(unsafe { read_error(err) }))
        }
    }

    /// Decode an `ArrowRow` byte buffer into a single-row `RecordBatch`.
    pub fn decode_row(&self, row: &[u8]) -> Result<RecordBatch, Error> {
        let mut out_ipc: *mut u8 = std::ptr::null_mut();
        let mut out_len: usize = 0;
        let mut err: *mut c_char = std::ptr::null_mut();

        let ok = unsafe {
            ffi::arrow_row_codec_decode_row(
                self.0,
                row.as_ptr(), row.len(),
                &mut out_ipc, &mut out_len,
                &mut err,
            )
        };

        if ok {
            let bytes = unsafe { std::slice::from_raw_parts(out_ipc, out_len).to_vec() };
            unsafe { ffi::arrow_row_free_bytes(out_ipc) };
            deserialize_batch(&bytes)
        } else {
            Err(Error(unsafe { read_error(err) }))
        }
    }
}

impl Drop for RowCodec {
    fn drop(&mut self) {
        unsafe { ffi::arrow_row_codec_free(self.0) };
    }
}

// ---------------------------------------------------------------------------
// SqliteWal
// ---------------------------------------------------------------------------

pub struct SqliteWal(*mut ffi::ArrowRowSQLiteWAL);

unsafe impl Send for SqliteWal {}

impl SqliteWal {
    pub fn new(path: &str) -> Result<Self, Error> {
        let cpath = CString::new(path).map_err(|e| Error(e.to_string()))?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe { ffi::arrow_row_sqlite_wal_new(cpath.as_ptr(), &mut err) };
        if ptr.is_null() {
            Err(Error(unsafe { read_error(err) }))
        } else {
            Ok(SqliteWal(ptr))
        }
    }
}

impl Drop for SqliteWal {
    fn drop(&mut self) {
        unsafe { ffi::arrow_row_sqlite_wal_free(self.0) };
    }
}

// ---------------------------------------------------------------------------
// GenericRowBatcher
// ---------------------------------------------------------------------------

// Trampoline called by the C++ background flush thread.
unsafe extern "C" fn flush_trampoline(
    ipc_data: *const u8,
    ipc_len: usize,
    userdata: *mut c_void,
) -> bool {
    // SAFETY: userdata is a *mut Box<dyn FnMut(RecordBatch) -> bool + Send>
    // created in GenericRowBatcher::new and freed in GenericRowBatcher::drop,
    // after arrow_row_batcher_free has joined all flush threads.
    let callback =
        &mut *(userdata as *mut Box<dyn FnMut(RecordBatch) -> bool + Send>);

    let bytes = std::slice::from_raw_parts(ipc_data, ipc_len);
    match deserialize_batch(bytes) {
        Ok(batch) => callback(batch),
        Err(_)    => false,
    }
}

pub struct GenericRowBatcher {
    inner:    *mut ffi::ArrowRowBatcher,
    userdata: *mut c_void,
}

// SAFETY: the C++ batcher and SQLiteWAL are both internally synchronized.
unsafe impl Send for GenericRowBatcher {}

impl GenericRowBatcher {
    pub fn new<F>(
        wal:        &mut SqliteWal,
        schema:     &Schema,
        batch_size: i64,
        on_flush:   F,
    ) -> Result<Self, Error>
    where
        F: FnMut(RecordBatch) -> bool + Send + 'static,
    {
        let ipc = serialize_schema(schema)?;

        // Double-box so we can erase the type and pass as *mut c_void.
        let callback: Box<Box<dyn FnMut(RecordBatch) -> bool + Send>> =
            Box::new(Box::new(on_flush));
        let userdata = Box::into_raw(callback) as *mut c_void;

        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe {
            ffi::arrow_row_batcher_new(
                wal.0,
                ipc.as_ptr(), ipc.len(),
                batch_size,
                flush_trampoline,
                userdata,
                &mut err,
            )
        };

        if ptr.is_null() {
            // Free the callback before returning the error.
            unsafe {
                drop(Box::from_raw(
                    userdata as *mut Box<dyn FnMut(RecordBatch) -> bool + Send>,
                ))
            };
            Err(Error(unsafe { read_error(err) }))
        } else {
            Ok(GenericRowBatcher { inner: ptr, userdata })
        }
    }

    /// Append one encoded row.  Thread-safe.
    pub fn append(&self, row: &[u8]) -> Result<(), Error> {
        let mut err: *mut c_char = std::ptr::null_mut();
        let ok = unsafe {
            ffi::arrow_row_batcher_append(self.inner, row.as_ptr(), row.len(), &mut err)
        };
        if ok {
            Ok(())
        } else {
            Err(Error(unsafe { read_error(err) }))
        }
    }

    /// Rows accumulated since the last automatic flush.
    pub fn row_count(&self) -> i64 {
        unsafe { ffi::arrow_row_batcher_row_count(self.inner) }
    }
}

impl Drop for GenericRowBatcher {
    fn drop(&mut self) {
        // Destroy the C++ batcher first; its destructor joins all flush threads,
        // ensuring no further calls to flush_trampoline / userdata after this.
        unsafe { ffi::arrow_row_batcher_free(self.inner) };
        // Now safe to free the callback.
        unsafe {
            drop(Box::from_raw(
                self.userdata as *mut Box<dyn FnMut(RecordBatch) -> bool + Send>,
            ))
        };
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use arrow::array::{Float64Array, Int32Array, StringArray};
    use arrow::datatypes::{DataType, Field};

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
    fn batcher_flush_callback() {
        use std::sync::{Arc as SArc, Mutex};

        let schema = make_schema();
        let mut wal = SqliteWal::new(":memory:").unwrap();

        let received: SArc<Mutex<Vec<RecordBatch>>> = SArc::new(Mutex::new(vec![]));
        let received_clone = SArc::clone(&received);

        let codec = RowCodec::new(&schema).unwrap();
        let mut batcher = GenericRowBatcher::new(
            &mut wal,
            &schema,
            2,  // flush every 2 rows
            move |batch| {
                received_clone.lock().unwrap().push(batch);
                true
            },
        )
        .unwrap();

        let row1 = codec.encode_row(&make_batch(&schema, 1, "Alice", 1.0)).unwrap();
        let row2 = codec.encode_row(&make_batch(&schema, 2, "Bob",   2.0)).unwrap();

        batcher.append(&row1).unwrap();
        batcher.append(&row2).unwrap();  // triggers flush

        // Drop batcher to wait for background flush thread.
        drop(batcher);

        let batches = received.lock().unwrap();
        assert_eq!(batches.len(), 1);
        assert_eq!(batches[0].num_rows(), 2);
    }
}
