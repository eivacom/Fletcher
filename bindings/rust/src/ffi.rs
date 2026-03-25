// Raw unsafe declarations mirroring arrow_row_capi.h.
// Nothing outside this module should touch C pointers directly.

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};

pub enum ArrowRowCodec {}
pub enum ArrowRowSQLiteWAL {}
pub enum ArrowRowBatcher {}

pub type FlushFn = unsafe extern "C" fn(
    ipc_data: *const u8,
    ipc_len:  usize,
    userdata: *mut c_void,
) -> bool;

extern "C" {
    pub fn arrow_row_free_string(s: *mut c_char);
    pub fn arrow_row_free_bytes(data: *mut u8);

    pub fn arrow_row_codec_new(
        schema_ipc:     *const u8,
        schema_ipc_len: usize,
        out_error:      *mut *mut c_char,
    ) -> *mut ArrowRowCodec;
    pub fn arrow_row_codec_free(codec: *mut ArrowRowCodec);
    pub fn arrow_row_codec_encode_row(
        codec:     *const ArrowRowCodec,
        ipc_data:  *const u8,
        ipc_len:   usize,
        out_data:  *mut *mut u8,
        out_len:   *mut usize,
        out_error: *mut *mut c_char,
    ) -> bool;
    pub fn arrow_row_codec_decode_row(
        codec:       *const ArrowRowCodec,
        row_data:    *const u8,
        row_len:     usize,
        out_ipc:     *mut *mut u8,
        out_ipc_len: *mut usize,
        out_error:   *mut *mut c_char,
    ) -> bool;

    pub fn arrow_row_sqlite_wal_new(
        path:      *const c_char,
        out_error: *mut *mut c_char,
    ) -> *mut ArrowRowSQLiteWAL;
    pub fn arrow_row_sqlite_wal_free(wal: *mut ArrowRowSQLiteWAL);

    pub fn arrow_row_batcher_new(
        wal:            *mut ArrowRowSQLiteWAL,
        schema_ipc:     *const u8,
        schema_ipc_len: usize,
        batch_size:     i64,
        on_flush:       FlushFn,
        userdata:       *mut c_void,
        out_error:      *mut *mut c_char,
    ) -> *mut ArrowRowBatcher;
    pub fn arrow_row_batcher_free(batcher: *mut ArrowRowBatcher);
    pub fn arrow_row_batcher_append(
        batcher:   *mut ArrowRowBatcher,
        row_data:  *const u8,
        row_len:   usize,
        out_error: *mut *mut c_char,
    ) -> bool;
    pub fn arrow_row_batcher_row_count(batcher: *const ArrowRowBatcher) -> i64;
}

// Read a C error string into a Rust String and free it.
pub(crate) unsafe fn read_error(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return "unknown error".into();
    }
    let msg = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    arrow_row_free_string(ptr);
    msg
}
