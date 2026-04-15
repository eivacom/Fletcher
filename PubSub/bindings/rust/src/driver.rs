use std::ffi::{c_void, CString};
use std::os::raw::c_char;

use arrow_schema::Schema;
use arrow_schema::ffi::FFI_ArrowSchema;

use crate::error::Error;
use crate::ffi;
use crate::types::Attachments;

/// Opaque handle to a PubSub provider (e.g. FastDDS, XRCE-DDS).
///
/// Created by provider-specific factory functions.  Passed to
/// [`Driver::new`] to create a driver instance.
pub struct PubSub(*mut ffi::FletcherPubSub);

unsafe impl Send for PubSub {}
unsafe impl Sync for PubSub {}

impl PubSub {
    /// Wrap a raw `FletcherPubSub` handle.
    ///
    /// # Safety
    /// The pointer must have been returned by a `fletcher_*_new` factory
    /// function and must not have been freed.
    pub unsafe fn from_raw(ptr: *mut ffi::FletcherPubSub) -> Self {
        Self(ptr)
    }

    pub(crate) fn as_ptr(&self) -> *mut ffi::FletcherPubSub {
        self.0
    }
}

impl Drop for PubSub {
    fn drop(&mut self) {
        unsafe { ffi::fletcher_pubsub_free(self.0) };
    }
}

/// Multi-subscriber fan-out over a single PubSub provider.
pub struct Driver(*mut ffi::FletcherDriver);

unsafe impl Send for Driver {}
unsafe impl Sync for Driver {}

/// Result of subscribing to a topic.
pub struct SubscribeResult {
    pub subscription_id: u64,
    pub schema: Schema,
}

impl Driver {
    /// Create a driver wrapping the given provider.  Takes shared ownership.
    pub fn new(provider: &PubSub) -> Result<Self, Error> {
        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe { ffi::fletcher_driver_new(provider.as_ptr(), &mut err) };
        if ptr.is_null() {
            Err(Error(unsafe { ffi::read_error(err) }))
        } else {
            Ok(Self(ptr))
        }
    }

    /// Declare a topic with the given Arrow schema.
    pub fn create_topic(
        &self,
        segments: &[&str],
        schema: &Schema,
    ) -> Result<(), Error> {
        let c_segments = to_c_segments(segments);
        let ptrs = to_c_ptrs(&c_segments);
        let mut err: *mut c_char = std::ptr::null_mut();

        let ffi_schema = FFI_ArrowSchema::try_from(schema)
            .map_err(|e| Error(e.to_string()))?;

        let ok = unsafe {
            ffi::fletcher_driver_create_topic(
                self.0,
                ptrs.as_ptr(),
                ptrs.len(),
                &ffi_schema as *const FFI_ArrowSchema,
                &mut err,
            )
        };
        if ok {
            Ok(())
        } else {
            Err(Error(unsafe { ffi::read_error(err) }))
        }
    }

    /// Publish pre-encoded row bytes with optional attachments.
    pub fn publish(
        &self,
        segments: &[&str],
        row_data: &[u8],
        attachments: Option<&Attachments>,
    ) -> Result<(), Error> {
        let c_segments = to_c_segments(segments);
        let ptrs = to_c_ptrs(&c_segments);
        let mut err: *mut c_char = std::ptr::null_mut();

        // Build C attachments if present.
        let c_att = attachments.map(|att| {
            let a = unsafe { ffi::fletcher_attachments_new() };
            for (key, val) in att {
                let c_key = CString::new(key.as_str()).unwrap();
                unsafe {
                    ffi::fletcher_attachments_add(
                        a,
                        c_key.as_ptr(),
                        val.as_ptr(),
                        val.len(),
                    );
                }
            }
            a
        });

        let att_ptr = c_att.unwrap_or(std::ptr::null_mut()) as *const _;

        let ok = unsafe {
            ffi::fletcher_driver_publish(
                self.0,
                ptrs.as_ptr(),
                ptrs.len(),
                row_data.as_ptr(),
                row_data.len(),
                att_ptr,
                &mut err,
            )
        };

        // Free the builder if we created one.
        if let Some(a) = c_att {
            unsafe { ffi::fletcher_attachments_free(a) };
        }

        if ok {
            Ok(())
        } else {
            Err(Error(unsafe { ffi::read_error(err) }))
        }
    }

    /// Subscribe to a topic.  The callback receives borrowed data that is
    /// valid for the callback duration — copy if needed beyond that.
    ///
    /// The callback receives `(row_data, schema, attachments)`.
    pub fn subscribe<F>(
        &self,
        segments: &[&str],
        callback: F,
    ) -> Result<SubscribeResult, Error>
    where
        F: Fn(&[u8], &Schema, Attachments) + Send + Sync + 'static,
    {
        let c_segments = to_c_segments(segments);
        let ptrs = to_c_ptrs(&c_segments);
        let mut err: *mut c_char = std::ptr::null_mut();
        let mut sub_id: u64 = 0;
        let mut ffi_schema = FFI_ArrowSchema::empty();

        // Box the closure and pass a raw pointer as user_data.
        let boxed = Box::new(callback);
        let raw = Box::into_raw(boxed) as *mut c_void;

        let ok = unsafe {
            ffi::fletcher_driver_subscribe(
                self.0,
                ptrs.as_ptr(),
                ptrs.len(),
                subscribe_trampoline::<F>,
                raw,
                &mut sub_id,
                &mut ffi_schema,
                &mut err,
            )
        };

        if !ok {
            // Reclaim the Box so it doesn't leak.
            unsafe { drop(Box::from_raw(raw as *mut F)) };
            return Err(Error(unsafe { ffi::read_error(err) }));
        }

        let schema = Schema::try_from(&ffi_schema).map_err(|e| Error(e.to_string()))?;

        Ok(SubscribeResult {
            subscription_id: sub_id,
            schema,
        })
    }

    /// Unsubscribe by subscription ID.
    pub fn unsubscribe(&self, subscription_id: u64) -> Result<(), Error> {
        let mut err: *mut c_char = std::ptr::null_mut();
        let ok = unsafe {
            ffi::fletcher_driver_unsubscribe(self.0, subscription_id, &mut err)
        };
        if ok {
            Ok(())
        } else {
            Err(Error(unsafe { ffi::read_error(err) }))
        }
    }

    /// Check whether a topic has been created.
    pub fn has_topic(&self, segments: &[&str]) -> bool {
        let c_segments = to_c_segments(segments);
        let ptrs = to_c_ptrs(&c_segments);
        unsafe {
            ffi::fletcher_driver_has_topic(
                self.0,
                ptrs.as_ptr(),
                ptrs.len(),
            )
        }
    }
}

impl Drop for Driver {
    fn drop(&mut self) {
        unsafe { ffi::fletcher_driver_free(self.0) };
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn to_c_segments(segments: &[&str]) -> Vec<CString> {
    segments
        .iter()
        .map(|s| CString::new(*s).unwrap())
        .collect()
}

fn to_c_ptrs(segments: &[CString]) -> Vec<*const c_char> {
    segments.iter().map(|s| s.as_ptr()).collect()
}

/// Trampoline function that bridges the C callback to a Rust closure.
unsafe extern "C" fn subscribe_trampoline<F>(
    data: *const u8,
    len: usize,
    schema: *const FFI_ArrowSchema,
    attachments: *const ffi::FletcherAttachments,
    user_data: *mut c_void,
)
where
    F: Fn(&[u8], &Schema, Attachments),
{
    let cb = &*(user_data as *const F);

    // Borrow the row data (zero-copy for the callback duration).
    let row_data = std::slice::from_raw_parts(data, len);

    // Import schema via Arrow C Data Interface.
    // We create a temporary reference — the C side owns the schema.
    let arrow_schema = match Schema::try_from(&*schema) {
        Ok(s) => s,
        Err(_) => return, // Schema import failed — skip this message.
    };

    // Read attachments into a HashMap.
    let att_count = ffi::fletcher_attachments_count(attachments);
    let mut att = Attachments::new();
    for i in 0..att_count {
        let key_ptr = ffi::fletcher_attachments_key(attachments, i);
        let key = std::ffi::CStr::from_ptr(key_ptr)
            .to_string_lossy()
            .into_owned();
        let mut val_len: usize = 0;
        let val_ptr = ffi::fletcher_attachments_value(attachments, i, &mut val_len);
        let val = if val_ptr.is_null() || val_len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(val_ptr, val_len).to_vec()
        };
        att.insert(key, val);
    }

    cb(row_data, &arrow_schema, att);
}
