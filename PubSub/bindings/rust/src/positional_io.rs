/// Native Rust implementation of the positional wire format.
///
/// Wire format: little-endian, schema-driven, tightly packed.
/// See `docs/foreign-language-bindings.md` for the full specification.
use crate::write_buffer::WriteBuffer;

// ---------------------------------------------------------------------------
// PositionalWriter
// ---------------------------------------------------------------------------

/// Writes fields in positional wire format into a [`WriteBuffer`].
pub struct PositionalWriter<'a> {
    buf: &'a mut WriteBuffer,
    bitfield_offset: usize,
    num_fields: usize,
}

impl<'a> PositionalWriter<'a> {
    /// Begin writing a row/struct with `num_fields` fields.
    /// Allocates the null bitfield (all non-null initially).
    pub fn new(buf: &'a mut WriteBuffer, num_fields: usize) -> Self {
        let bitfield_bytes = (num_fields + 7) / 8;
        let bitfield_offset = buf.position();
        // Zero-initialize: all fields non-null by default.
        for _ in 0..bitfield_bytes {
            buf.append_byte(0);
        }
        Self {
            buf,
            bitfield_offset,
            num_fields,
        }
    }

    /// Mark a field as null (sets the corresponding bit in the null bitfield).
    #[inline]
    pub fn set_null(&mut self, field_index: usize) {
        debug_assert!(field_index < self.num_fields);
        let byte_idx = self.bitfield_offset + field_index / 8;
        let bit = 1u8 << (field_index % 8);
        self.buf.patch_byte(byte_idx, bit);
    }

    #[inline]
    pub fn write_bool(&mut self, v: bool) {
        self.buf.append_byte(if v { 1 } else { 0 });
    }

    #[inline]
    pub fn write_i8(&mut self, v: i8) {
        self.buf.append_i8(v);
    }

    #[inline]
    pub fn write_i16(&mut self, v: i16) {
        self.buf.append_i16(v);
    }

    #[inline]
    pub fn write_i32(&mut self, v: i32) {
        self.buf.append_i32(v);
    }

    #[inline]
    pub fn write_i64(&mut self, v: i64) {
        self.buf.append_i64(v);
    }

    #[inline]
    pub fn write_u8(&mut self, v: u8) {
        self.buf.append_byte(v);
    }

    #[inline]
    pub fn write_u16(&mut self, v: u16) {
        self.buf.append_u16(v);
    }

    #[inline]
    pub fn write_u32(&mut self, v: u32) {
        self.buf.append_u32(v);
    }

    #[inline]
    pub fn write_u64(&mut self, v: u64) {
        self.buf.append_u64(v);
    }

    #[inline]
    pub fn write_f32(&mut self, v: f32) {
        self.buf.append_f32(v);
    }

    #[inline]
    pub fn write_f64(&mut self, v: f64) {
        self.buf.append_f64(v);
    }

    #[inline]
    pub fn write_timestamp(&mut self, v: i64) {
        self.buf.append_i64(v);
    }

    #[inline]
    pub fn write_duration(&mut self, v: i64) {
        self.buf.append_i64(v);
    }

    /// Write a length-prefixed string (UTF-8).
    #[inline]
    pub fn write_string(&mut self, s: &str) {
        self.buf.append_u32(s.len() as u32);
        self.buf.append(s.as_bytes());
    }

    /// Write a length-prefixed binary blob.
    #[inline]
    pub fn write_binary(&mut self, data: &[u8]) {
        self.buf.append_u32(data.len() as u32);
        self.buf.append(data);
    }

    /// Begin writing a nested struct with `num_fields` fields.
    /// Returns a new `PositionalWriter` scoped to the struct.
    #[inline]
    pub fn begin_struct(&mut self, num_fields: usize) -> PositionalWriter<'_> {
        PositionalWriter::new(self.buf, num_fields)
    }

    /// Begin writing a list.  Returns a [`ListContext`] for writing elements.
    pub fn begin_list(&mut self, count: u32) -> ListContext<'_> {
        self.buf.append_u32(count);
        let bitfield_bytes = (count as usize + 7) / 8;
        let bitfield_offset = self.buf.position();
        for _ in 0..bitfield_bytes {
            self.buf.append_byte(0);
        }
        ListContext {
            buf: self.buf,
            bitfield_offset,
            count,
        }
    }

    /// Begin writing a map.  Returns a [`MapContext`] for writing entries.
    pub fn begin_map(&mut self, count: u32) -> MapContext<'_> {
        self.buf.append_u32(count);
        MapContext {
            buf: self.buf,
            count,
        }
    }

    /// Access the underlying buffer (for advanced use by generated code).
    #[inline]
    pub fn buf(&mut self) -> &mut WriteBuffer {
        self.buf
    }
}

/// Context for writing list elements.
pub struct ListContext<'a> {
    buf: &'a mut WriteBuffer,
    bitfield_offset: usize,
    count: u32,
}

impl<'a> ListContext<'a> {
    /// Mark a list element as null.
    #[inline]
    pub fn set_element_null(&mut self, index: u32) {
        debug_assert!(index < self.count);
        let byte_idx = self.bitfield_offset + index as usize / 8;
        let bit = 1u8 << (index % 8);
        self.buf.patch_byte(byte_idx, bit);
    }

    /// Access the underlying buffer for writing element payloads.
    #[inline]
    pub fn buf(&mut self) -> &mut WriteBuffer {
        self.buf
    }
}

/// Context for writing map entries.
pub struct MapContext<'a> {
    buf: &'a mut WriteBuffer,
    count: u32,
}

impl<'a> MapContext<'a> {
    /// After writing all key payloads, call this to begin the value section.
    /// Returns a [`ListContext`] for writing value payloads (values can be
    /// null, keys cannot).
    pub fn begin_values(&mut self) -> ListContext<'_> {
        let bitfield_bytes = (self.count as usize + 7) / 8;
        let bitfield_offset = self.buf.position();
        for _ in 0..bitfield_bytes {
            self.buf.append_byte(0);
        }
        ListContext {
            buf: self.buf,
            bitfield_offset,
            count: self.count,
        }
    }

    /// Access the underlying buffer for writing key payloads.
    #[inline]
    pub fn buf(&mut self) -> &mut WriteBuffer {
        self.buf
    }
}

// ---------------------------------------------------------------------------
// PositionalReader
// ---------------------------------------------------------------------------

/// Reads fields from positional wire format bytes.
pub struct PositionalReader<'a> {
    data: &'a [u8],
    pos: usize,
    bitfield_offset: usize,
    num_fields: usize,
}

impl<'a> PositionalReader<'a> {
    /// Begin reading a row/struct with `num_fields` fields.
    pub fn new(data: &'a [u8], num_fields: usize) -> Self {
        let bitfield_bytes = (num_fields + 7) / 8;
        Self {
            data,
            pos: bitfield_bytes,
            bitfield_offset: 0,
            num_fields,
        }
    }

    /// Check whether field `field_index` is null.
    #[inline]
    pub fn is_null(&self, field_index: usize) -> bool {
        debug_assert!(field_index < self.num_fields);
        let byte = self.data[self.bitfield_offset + field_index / 8];
        (byte >> (field_index % 8)) & 1 != 0
    }

    #[inline]
    pub fn read_bool(&mut self) -> bool {
        let v = self.data[self.pos] != 0;
        self.pos += 1;
        v
    }

    #[inline]
    pub fn read_i8(&mut self) -> i8 {
        let v = self.data[self.pos] as i8;
        self.pos += 1;
        v
    }

    #[inline]
    pub fn read_u8(&mut self) -> u8 {
        let v = self.data[self.pos];
        self.pos += 1;
        v
    }

    #[inline]
    pub fn read_i16(&mut self) -> i16 {
        let v = i16::from_le_bytes(self.data[self.pos..self.pos + 2].try_into().unwrap());
        self.pos += 2;
        v
    }

    #[inline]
    pub fn read_u16(&mut self) -> u16 {
        let v = u16::from_le_bytes(self.data[self.pos..self.pos + 2].try_into().unwrap());
        self.pos += 2;
        v
    }

    #[inline]
    pub fn read_i32(&mut self) -> i32 {
        let v = i32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap());
        self.pos += 4;
        v
    }

    #[inline]
    pub fn read_u32(&mut self) -> u32 {
        let v = u32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap());
        self.pos += 4;
        v
    }

    #[inline]
    pub fn read_i64(&mut self) -> i64 {
        let v = i64::from_le_bytes(self.data[self.pos..self.pos + 8].try_into().unwrap());
        self.pos += 8;
        v
    }

    #[inline]
    pub fn read_u64(&mut self) -> u64 {
        let v = u64::from_le_bytes(self.data[self.pos..self.pos + 8].try_into().unwrap());
        self.pos += 8;
        v
    }

    #[inline]
    pub fn read_f32(&mut self) -> f32 {
        let v = f32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap());
        self.pos += 4;
        v
    }

    #[inline]
    pub fn read_f64(&mut self) -> f64 {
        let v = f64::from_le_bytes(self.data[self.pos..self.pos + 8].try_into().unwrap());
        self.pos += 8;
        v
    }

    #[inline]
    pub fn read_timestamp(&mut self) -> i64 {
        self.read_i64()
    }

    #[inline]
    pub fn read_duration(&mut self) -> i64 {
        self.read_i64()
    }

    /// Read a length-prefixed string.  Returns a `&str` borrowing from the
    /// input buffer (zero-copy).
    #[inline]
    pub fn read_string(&mut self) -> &'a str {
        let len = self.read_u32() as usize;
        let s = std::str::from_utf8(&self.data[self.pos..self.pos + len])
            .expect("invalid UTF-8 in positional string field");
        self.pos += len;
        s
    }

    /// Read a length-prefixed binary blob.  Returns a slice borrowing from
    /// the input buffer (zero-copy).
    #[inline]
    pub fn read_binary(&mut self) -> &'a [u8] {
        let len = self.read_u32() as usize;
        let slice = &self.data[self.pos..self.pos + len];
        self.pos += len;
        slice
    }

    /// Begin reading a nested struct.  Returns a sub-reader positioned at
    /// the struct's null bitfield.
    #[inline]
    pub fn read_struct(&mut self, num_fields: usize) -> PositionalReader<'a> {
        let sub = PositionalReader::new(&self.data[self.pos..], num_fields);
        sub
    }

    /// Advance past a nested struct that was read via `read_struct`.
    /// Call this after the sub-reader is finished to update this reader's
    /// position.
    #[inline]
    pub fn advance(&mut self, bytes_consumed: usize) {
        self.pos += bytes_consumed;
    }

    /// Read list header: returns count and a [`ListHeader`].
    pub fn read_list_header(&mut self) -> ListHeader<'a> {
        let count = self.read_u32();
        let bitfield_bytes = (count as usize + 7) / 8;
        let elem_bitfield = &self.data[self.pos..self.pos + bitfield_bytes];
        self.pos += bitfield_bytes;
        ListHeader {
            count,
            elem_bitfield,
        }
    }

    /// Read map count and return it.  Keys follow immediately (no null
    /// bitfield for keys).
    #[inline]
    pub fn read_map_count(&mut self) -> u32 {
        self.read_u32()
    }

    /// Read the value null bitfield for a map (after all keys are read).
    pub fn read_map_value_bitfield(&mut self, count: u32) -> &'a [u8] {
        let bitfield_bytes = (count as usize + 7) / 8;
        let bf = &self.data[self.pos..self.pos + bitfield_bytes];
        self.pos += bitfield_bytes;
        bf
    }

    /// Number of bytes consumed so far.
    #[inline]
    pub fn bytes_consumed(&self) -> usize {
        self.pos
    }

    /// Remaining bytes in the buffer.
    #[inline]
    pub fn remaining(&self) -> usize {
        self.data.len() - self.pos
    }
}

/// List header with element count and null bitfield.
pub struct ListHeader<'a> {
    pub count: u32,
    pub elem_bitfield: &'a [u8],
}

impl<'a> ListHeader<'a> {
    /// Check whether element at `index` is null.
    #[inline]
    pub fn is_element_null(&self, index: u32) -> bool {
        let byte = self.elem_bitfield[index as usize / 8];
        (byte >> (index % 8)) & 1 != 0
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::write_buffer::WriteBuffer;

    #[test]
    fn round_trip_scalars() {
        let mut buf = WriteBuffer::new();
        {
            let mut w = PositionalWriter::new(&mut buf, 6);
            w.write_bool(true);
            w.write_i32(42);
            w.write_f64(99.5);
            w.write_string("hello");
            w.write_u64(0xDEAD_BEEF);
            w.set_null(5); // field 5 is null
        }
        let bytes = buf.into_vec();

        let mut r = PositionalReader::new(&bytes, 6);
        assert!(!r.is_null(0));
        assert!(!r.is_null(1));
        assert!(!r.is_null(4));
        assert!(r.is_null(5));

        assert_eq!(r.read_bool(), true);
        assert_eq!(r.read_i32(), 42);
        assert_eq!(r.read_f64(), 99.5);
        assert_eq!(r.read_string(), "hello");
        assert_eq!(r.read_u64(), 0xDEAD_BEEF);
        // field 5 is null — skip reading
    }

    #[test]
    fn round_trip_list() {
        let mut buf = WriteBuffer::new();
        {
            let mut w = PositionalWriter::new(&mut buf, 1);
            // List of 3 i32s: [10, null, 30]
            let mut list = w.begin_list(3);
            list.buf().append_i32(10);
            list.set_element_null(1);
            list.buf().append_i32(30);
        }
        let bytes = buf.into_vec();

        let mut r = PositionalReader::new(&bytes, 1);
        assert!(!r.is_null(0));

        let header = r.read_list_header();
        assert_eq!(header.count, 3);
        assert!(!header.is_element_null(0));
        assert!(header.is_element_null(1));
        assert!(!header.is_element_null(2));

        assert_eq!(r.read_i32(), 10);
        // element 1 is null — skip
        assert_eq!(r.read_i32(), 30);
    }

    #[test]
    fn round_trip_nested_struct() {
        let mut buf = WriteBuffer::new();
        {
            let mut w = PositionalWriter::new(&mut buf, 2);
            w.write_i32(1); // field 0: outer int
            // field 1: nested struct with 2 fields
            let mut inner = w.begin_struct(2);
            inner.write_string("nested");
            inner.write_f32(3.14);
        }
        let bytes = buf.into_vec();

        let mut r = PositionalReader::new(&bytes, 2);
        assert_eq!(r.read_i32(), 1);

        let mut inner = r.read_struct(2);
        assert_eq!(inner.read_string(), "nested");
        let f = inner.read_f32();
        assert!((f - 3.14).abs() < 0.001);
        r.advance(inner.bytes_consumed());
    }

    #[test]
    fn round_trip_map() {
        let mut buf = WriteBuffer::new();
        {
            let mut w = PositionalWriter::new(&mut buf, 1);
            // Map with 2 entries: {"a": 1, "b": null}
            let mut map = w.begin_map(2);
            map.buf().append_u32(1);
            map.buf().append(b"a");
            map.buf().append_u32(1);
            map.buf().append(b"b");
            let mut vals = map.begin_values();
            vals.buf().append_i32(1);
            vals.set_element_null(1);
        }
        let bytes = buf.into_vec();

        let mut r = PositionalReader::new(&bytes, 1);
        let count = r.read_map_count();
        assert_eq!(count, 2);
        // Read keys
        assert_eq!(r.read_string(), "a");
        assert_eq!(r.read_string(), "b");
        // Read value bitfield
        let vbf = r.read_map_value_bitfield(2);
        assert_eq!(vbf[0] & 1, 0); // value 0 not null
        assert_eq!((vbf[0] >> 1) & 1, 1); // value 1 null
        assert_eq!(r.read_i32(), 1);
    }

    #[test]
    fn all_null_row() {
        let mut buf = WriteBuffer::new();
        {
            let mut w = PositionalWriter::new(&mut buf, 3);
            w.set_null(0);
            w.set_null(1);
            w.set_null(2);
        }
        let bytes = buf.into_vec();
        // Only the null bitfield: ceil(3/8) = 1 byte, value = 0b111 = 7
        assert_eq!(bytes.len(), 1);
        assert_eq!(bytes[0], 0x07);
    }
}
