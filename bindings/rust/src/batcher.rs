// Mirrors C++ RowBatcher (abstract base) and GenericRowBatcher (concrete).
//
// Because Rust does not have subclass-based virtual dispatch, the C++ pattern
// of overriding OnBatchFlushSucceeded / OnBatchFlushFailed by subclassing
// RowBatcher is instead expressed through the FlushHandler trait: callers
// implement FlushHandler on their own type and pass it to
// GenericRowBatcher::with_handler.

use std::os::raw::{c_char, c_void};

use arrow::array::RecordBatch;
use arrow::datatypes::Schema;

use crate::wal::sealed::WalPtr as _;  // bring as_mut_ptr into scope
use crate::wal::WriteAheadLog;
use crate::{error::Error, ffi, ipc};

// ---------------------------------------------------------------------------
// FlushHandler — mirrors C++ OnBatchFlushSucceeded / OnBatchFlushFailed
// ---------------------------------------------------------------------------

/// Handles flush events produced by a [`GenericRowBatcher`].
///
/// Implement this trait to customise flush behaviour, mirroring the C++ pattern
/// of overriding `OnBatchFlushSucceeded` and `OnBatchFlushFailed` by
/// subclassing `RowBatcher`.
///
/// All methods may be called concurrently from background threads; the trait
/// requires `Send + Sync` so the compiler enforces this.
pub trait FlushHandler: Send + Sync {
    /// Called with the accumulated batch when the row count reaches
    /// `batch_size`.  Return `true` to signal success, `false` for failure.
    fn on_flush(&self, batch: RecordBatch) -> bool;

    /// Called on the flush thread after `on_flush` returns `true`.
    /// Default implementation is a no-op.
    fn on_batch_flush_succeeded(&self) {}

    /// Called on the flush thread after `on_flush` returns `false`, or when
    /// the batch cannot be deserialised.
    /// Default implementation is a no-op.
    fn on_batch_flush_failed(&self) {}
}

// Convenience wrapper: turns any `Fn(RecordBatch) -> bool` into a FlushHandler.
struct ClosureFlushHandler<F>(F);

impl<F> FlushHandler for ClosureFlushHandler<F>
where
    F: Fn(RecordBatch) -> bool + Send + Sync,
{
    fn on_flush(&self, batch: RecordBatch) -> bool {
        (self.0)(batch)
    }
}

// ---------------------------------------------------------------------------
// RowBatcher — mirrors C++ abstract base
// ---------------------------------------------------------------------------

/// Abstract row batcher.
///
/// Mirrors `RowBatcher` in `row_batcher.hpp`.
pub trait RowBatcher: Send + Sync {
    /// Append one encoded row (produced by [`RowCodec::encode_row`](crate::RowCodec::encode_row)).
    /// Thread-safe.  Returns an error if the underlying C++ call fails.
    fn append(&self, row: &[u8]) -> Result<(), Error>;

    /// Rows accumulated since the last automatic flush.
    fn row_count(&self) -> i64;
}

// ---------------------------------------------------------------------------
// Internal trampoline userdata
// ---------------------------------------------------------------------------

// Holds the FlushHandler behind an opaque pointer passed through the C API.
// SAFETY: FlushHandler: Send + Sync, so concurrent trampoline calls are safe.
struct BatcherUserdata(Box<dyn FlushHandler>);

unsafe extern "C" fn flush_trampoline(
    ipc_data: *const u8,
    ipc_len:  usize,
    userdata: *mut c_void,
) -> bool {
    // SAFETY: userdata is a valid *mut BatcherUserdata for the duration of this
    // call. arrow_row_batcher_free (called in Drop) joins all flush threads
    // before we ever free the userdata, so the pointer cannot dangle here.
    let ud = &*(userdata as *const BatcherUserdata);
    let bytes = std::slice::from_raw_parts(ipc_data, ipc_len);

    match ipc::deserialize_batch(bytes) {
        Ok(batch) => {
            let ok = ud.0.on_flush(batch);
            if ok { ud.0.on_batch_flush_succeeded(); }
            else  { ud.0.on_batch_flush_failed();    }
            ok
        }
        Err(_) => {
            ud.0.on_batch_flush_failed();
            false
        }
    }
}

// ---------------------------------------------------------------------------
// GenericRowBatcher — mirrors C++ concrete class
// ---------------------------------------------------------------------------

/// Concrete row batcher backed by a [`WriteAheadLog`].
///
/// Mirrors `GenericRowBatcher` in `generic_row_batcher.hpp`.
///
/// Rows are appended via [`append`](RowBatcher::append).  Once `batch_size`
/// rows have accumulated, the flush handler is invoked on a background thread
/// and the internal counter is reset; `append` returns without waiting.
///
/// The destructor blocks until all in-flight flush threads have completed.
pub struct GenericRowBatcher {
    inner:    *mut ffi::ArrowRowBatcher,
    userdata: *mut c_void,  // owns a Box<BatcherUserdata>
}

unsafe impl Send for GenericRowBatcher {}
unsafe impl Sync for GenericRowBatcher {}

impl GenericRowBatcher {
    /// Construct with a custom [`FlushHandler`], allowing `on_batch_flush_succeeded`
    /// and `on_batch_flush_failed` to be overridden.
    ///
    /// Mirrors creating a subclass of `RowBatcher` in C++.
    pub fn with_handler<W, H>(
        wal:        &mut W,
        schema:     &Schema,
        batch_size: i64,
        handler:    H,
    ) -> Result<Self, Error>
    where
        W: WriteAheadLog,
        H: FlushHandler + 'static,
    {
        let schema_ipc = ipc::serialize_schema(schema)?;
        let userdata =
            Box::into_raw(Box::new(BatcherUserdata(Box::new(handler)))) as *mut c_void;

        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe {
            ffi::arrow_row_batcher_new(
                wal.as_mut_ptr(),
                schema_ipc.as_ptr(), schema_ipc.len(),
                batch_size,
                flush_trampoline,
                userdata,
                &mut err,
            )
        };

        if ptr.is_null() {
            unsafe { drop(Box::from_raw(userdata as *mut BatcherUserdata)) };
            Err(Error(unsafe { ffi::read_error(err) }))
        } else {
            Ok(Self { inner: ptr, userdata })
        }
    }

    /// Construct with a simple closure as the flush callback.
    ///
    /// For custom success/failure handling, use [`with_handler`](Self::with_handler).
    pub fn new<W, F>(
        wal:        &mut W,
        schema:     &Schema,
        batch_size: i64,
        on_flush:   F,
    ) -> Result<Self, Error>
    where
        W: WriteAheadLog,
        F: Fn(RecordBatch) -> bool + Send + Sync + 'static,
    {
        Self::with_handler(wal, schema, batch_size, ClosureFlushHandler(on_flush))
    }
}

impl RowBatcher for GenericRowBatcher {
    fn append(&self, row: &[u8]) -> Result<(), Error> {
        let mut err: *mut c_char = std::ptr::null_mut();
        let ok = unsafe {
            ffi::arrow_row_batcher_append(self.inner, row.as_ptr(), row.len(), &mut err)
        };
        if ok { Ok(()) } else { Err(Error(unsafe { ffi::read_error(err) })) }
    }

    fn row_count(&self) -> i64 {
        unsafe { ffi::arrow_row_batcher_row_count(self.inner) }
    }
}

impl Drop for GenericRowBatcher {
    fn drop(&mut self) {
        // Join all flush threads before freeing the handler.
        unsafe { ffi::arrow_row_batcher_free(self.inner) };
        unsafe { drop(Box::from_raw(self.userdata as *mut BatcherUserdata)) };
    }
}
