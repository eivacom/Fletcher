// Raw unsafe declarations mirroring arrow_row_codec_capi.h.
// Nothing outside this module should touch C pointers directly.

use std::ffi::CStr;
use std::os::raw::c_char;

pub enum ArrowRowCodec {}

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
