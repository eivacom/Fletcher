// Mirrors C++ RowCodec: encodes and decodes Arrow rows to/from the compact
// binary ArrowRow wire format.

use std::os::raw::c_char;

use arrow::array::RecordBatch;
use arrow::datatypes::Schema;

use crate::{error::Error, ffi, ipc};

/// Binds a schema to [`encode_row`](RowCodec::encode_row) /
/// [`decode_row`](RowCodec::decode_row) so callers do not pass it on every call.
///
/// Mirrors `RowCodec` in `row_codec.hpp`.
pub struct RowCodec(*mut ffi::ArrowRowCodec);

unsafe impl Send for RowCodec {}
unsafe impl Sync for RowCodec {}

impl RowCodec {
    /// Create a codec for the given schema.
    pub fn new(schema: &Schema) -> Result<Self, Error> {
        let ipc_bytes = ipc::serialize_schema(schema)?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe {
            ffi::arrow_row_codec_new(ipc_bytes.as_ptr(), ipc_bytes.len(), &mut err)
        };
        if ptr.is_null() {
            Err(Error(unsafe { ffi::read_error(err) }))
        } else {
            Ok(Self(ptr))
        }
    }

    /// Encode a single-row `RecordBatch` into a compact `ArrowRow` byte buffer.
    pub fn encode_row(&self, batch: &RecordBatch) -> Result<Vec<u8>, Error> {
        let ipc_bytes = ipc::serialize_batch(batch)?;
        let mut out_data: *mut u8 = std::ptr::null_mut();
        let mut out_len: usize = 0;
        let mut err: *mut c_char = std::ptr::null_mut();

        let ok = unsafe {
            ffi::arrow_row_codec_encode_row(
                self.0,
                ipc_bytes.as_ptr(), ipc_bytes.len(),
                &mut out_data, &mut out_len,
                &mut err,
            )
        };

        if ok {
            let row = unsafe { std::slice::from_raw_parts(out_data, out_len).to_vec() };
            unsafe { ffi::arrow_row_free_bytes(out_data) };
            Ok(row)
        } else {
            Err(Error(unsafe { ffi::read_error(err) }))
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
            ipc::deserialize_batch(&bytes)
        } else {
            Err(Error(unsafe { ffi::read_error(err) }))
        }
    }
}

impl Drop for RowCodec {
    fn drop(&mut self) {
        unsafe { ffi::arrow_row_codec_free(self.0) };
    }
}
