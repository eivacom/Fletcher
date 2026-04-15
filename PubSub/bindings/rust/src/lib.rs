// fletcher-pubsub: Rust bindings for the Fletcher PubSub library.
//
// Module layout:
//   ffi            — raw unsafe C declarations (private)
//   error          — Error type
//   types          — EncodedRow, Attachments
//   write_buffer   — Native WriteBuffer (no FFI)
//   positional_io  — Native PositionalWriter/Reader (no FFI)
//   driver         — Safe Driver and PubSub wrappers

mod ffi;

pub mod error;
pub mod types;
pub mod write_buffer;
pub mod positional_io;
pub mod driver;

pub use error::Error;
pub use types::{Attachments, EncodedRow};
pub use write_buffer::WriteBuffer;
pub use positional_io::{ListContext, ListHeader, MapContext, PositionalReader, PositionalWriter};
pub use driver::{Driver, PubSub, SubscribeResult};
