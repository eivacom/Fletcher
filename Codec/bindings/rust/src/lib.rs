// arrow-row-codec: Rust bindings for the Codec C library.
//
// Module layout:
//   ffi    — raw unsafe C declarations (private)
//   ipc    — Arrow IPC serialization helpers (private)
//   error  — Error type
//   codec  — RowCodec

mod ffi;
mod ipc;

pub mod error;
pub mod codec;

pub use error::Error;
pub use codec::RowCodec;
