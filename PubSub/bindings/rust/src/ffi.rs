// Raw unsafe declarations mirroring pubsub_capi.h.
// Nothing outside this module should touch C pointers directly.

use std::ffi::CStr;
use std::os::raw::c_char;

pub enum FletcherPubSub {}
pub enum FletcherDriver {}
pub enum FletcherAttachments {}

/// Subscribe callback signature matching the C API.
pub type FletcherSubscribeCallback = unsafe extern "C" fn(
    data: *const u8,
    len: usize,
    schema: *const arrow_schema::ffi::FFI_ArrowSchema,
    attachments: *const FletcherAttachments,
    user_data: *mut std::ffi::c_void,
);

extern "C" {
    // Memory management
    pub fn fletcher_free_string(s: *mut c_char);
    pub fn fletcher_pubsub_free(provider: *mut FletcherPubSub);

    // Attachments — read-only accessors
    pub fn fletcher_attachments_count(a: *const FletcherAttachments) -> usize;
    pub fn fletcher_attachments_key(
        a: *const FletcherAttachments,
        index: usize,
    ) -> *const c_char;
    pub fn fletcher_attachments_value(
        a: *const FletcherAttachments,
        index: usize,
        out_len: *mut usize,
    ) -> *const u8;

    // Attachments — builder
    pub fn fletcher_attachments_new() -> *mut FletcherAttachments;
    pub fn fletcher_attachments_add(
        a: *mut FletcherAttachments,
        key: *const c_char,
        data: *const u8,
        len: usize,
    );
    pub fn fletcher_attachments_free(a: *mut FletcherAttachments);

    // Driver
    pub fn fletcher_driver_new(
        provider: *mut FletcherPubSub,
        out_error: *mut *mut c_char,
    ) -> *mut FletcherDriver;
    pub fn fletcher_driver_free(driver: *mut FletcherDriver);

    pub fn fletcher_driver_create_topic(
        driver: *mut FletcherDriver,
        segments: *const *const c_char,
        segment_count: usize,
        schema: *const arrow_schema::ffi::FFI_ArrowSchema,
        out_error: *mut *mut c_char,
    ) -> bool;

    pub fn fletcher_driver_publish(
        driver: *mut FletcherDriver,
        segments: *const *const c_char,
        segment_count: usize,
        row_data: *const u8,
        row_len: usize,
        attachments: *const FletcherAttachments,
        out_error: *mut *mut c_char,
    ) -> bool;

    pub fn fletcher_driver_subscribe(
        driver: *mut FletcherDriver,
        segments: *const *const c_char,
        segment_count: usize,
        callback: FletcherSubscribeCallback,
        user_data: *mut std::ffi::c_void,
        out_subscription_id: *mut u64,
        out_schema: *mut arrow_schema::ffi::FFI_ArrowSchema,
        out_error: *mut *mut c_char,
    ) -> bool;

    pub fn fletcher_driver_unsubscribe(
        driver: *mut FletcherDriver,
        subscription_id: u64,
        out_error: *mut *mut c_char,
    ) -> bool;

    pub fn fletcher_driver_has_topic(
        driver: *const FletcherDriver,
        segments: *const *const c_char,
        segment_count: usize,
    ) -> bool;
}

/// Read a C error string into a Rust String and free it.
pub(crate) unsafe fn read_error(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return "unknown error".into();
    }
    let msg = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    fletcher_free_string(ptr);
    msg
}
