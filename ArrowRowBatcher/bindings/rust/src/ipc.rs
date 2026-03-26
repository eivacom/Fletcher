// Crate-private helpers for Arrow IPC stream serialization / deserialization.

use std::io::Cursor;

use arrow::array::RecordBatch;
use arrow::datatypes::Schema;
use arrow::ipc::reader::StreamReader;
use arrow::ipc::writer::StreamWriter;

use crate::error::Error;

pub(crate) fn serialize_schema(schema: &Schema) -> Result<Vec<u8>, Error> {
    let mut buf = Vec::new();
    let mut w = StreamWriter::try_new(&mut buf, schema)
        .map_err(|e| Error(e.to_string()))?;
    w.finish().map_err(|e| Error(e.to_string()))?;
    Ok(buf)
}

pub(crate) fn deserialize_batch(bytes: &[u8]) -> Result<RecordBatch, Error> {
    let mut r = StreamReader::try_new(Cursor::new(bytes), None)
        .map_err(|e| Error(e.to_string()))?;
    r.next()
        .ok_or_else(|| Error("IPC stream contained no record batches".into()))?
        .map_err(|e| Error(e.to_string()))
}
