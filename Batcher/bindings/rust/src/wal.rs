// Mirrors C++ WriteAheadLog (abstract base) and SQLiteWAL (concrete).

use std::ffi::CString;
use std::os::raw::c_char;

use crate::{error::Error, ffi};

// ---------------------------------------------------------------------------
// Sealed trait — prevents external crates from implementing WriteAheadLog.
// Mirrors C++ making WriteAheadLog a pure-virtual class that only the library
// can concretely subclass.
// ---------------------------------------------------------------------------

pub(crate) mod sealed {
    pub trait WalPtr {
        fn as_mut_ptr(&mut self) -> *mut crate::ffi::ArrowRowSQLiteWAL;
    }
}

// ---------------------------------------------------------------------------
// WriteAheadLog — mirrors C++ abstract base
// ---------------------------------------------------------------------------

/// Abstract write-ahead log factory.
///
/// Mirrors `WriteAheadLog` in `write_ahead_log.hpp`.  The trait is sealed:
/// only types provided by this crate can implement it.
pub trait WriteAheadLog: sealed::WalPtr + Send {}

// ---------------------------------------------------------------------------
// SqliteWal — mirrors C++ SQLiteWAL
// ---------------------------------------------------------------------------

/// SQLite-backed write-ahead log.
///
/// Mirrors `SQLiteWAL` in `sqlite_wal.hpp`.
pub struct SqliteWal(*mut ffi::ArrowRowSQLiteWAL);

unsafe impl Send for SqliteWal {}

impl SqliteWal {
    /// Open (or create) a SQLite database at `path`.
    /// Pass `":memory:"` for an in-memory database.
    pub fn new(path: &str) -> Result<Self, Error> {
        let cpath = CString::new(path).map_err(|e| Error(e.to_string()))?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let ptr = unsafe { ffi::arrow_row_sqlite_wal_new(cpath.as_ptr(), &mut err) };
        if ptr.is_null() {
            Err(Error(unsafe { ffi::read_error(err) }))
        } else {
            Ok(Self(ptr))
        }
    }
}

impl sealed::WalPtr for SqliteWal {
    fn as_mut_ptr(&mut self) -> *mut ffi::ArrowRowSQLiteWAL {
        self.0
    }
}

impl WriteAheadLog for SqliteWal {}

impl Drop for SqliteWal {
    fn drop(&mut self) {
        unsafe { ffi::arrow_row_sqlite_wal_free(self.0) };
    }
}
