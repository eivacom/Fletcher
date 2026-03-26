// arrow-row-batcher: Rust bindings for the ArrowRowBatcher C library.
//
// Module layout:
//   ffi     — raw unsafe C declarations (private)
//   ipc     — Arrow IPC serialization helpers (private)
//   error   — Error type
//   wal     — WriteAheadLog, SqliteWal
//   batcher — RowBatcher, FlushHandler, GenericRowBatcher

mod ffi;
mod ipc;

pub mod error;
pub mod wal;
pub mod batcher;

pub use error::Error;
pub use wal::{SqliteWal, WriteAheadLog};
pub use batcher::{FlushHandler, GenericRowBatcher, RowBatcher};
