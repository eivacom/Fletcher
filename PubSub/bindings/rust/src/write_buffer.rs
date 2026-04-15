/// Growable byte buffer with random-access patching, mirroring the C++
/// `VectorWriteBuffer`.  Used by `PositionalWriter` and generated code.
pub struct WriteBuffer {
    buf: Vec<u8>,
}

impl WriteBuffer {
    pub fn new() -> Self {
        Self { buf: Vec::new() }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            buf: Vec::with_capacity(capacity),
        }
    }

    #[inline]
    pub fn append(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    #[inline]
    pub fn append_byte(&mut self, byte: u8) {
        self.buf.push(byte);
    }

    #[inline]
    pub fn append_u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_u64(&mut self, v: u64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_i8(&mut self, v: i8) {
        self.buf.push(v as u8);
    }

    #[inline]
    pub fn append_i16(&mut self, v: i16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_i32(&mut self, v: i32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_i64(&mut self, v: i64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_f32(&mut self, v: f32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    #[inline]
    pub fn append_f64(&mut self, v: f64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    /// Current write position (byte offset from start).
    #[inline]
    pub fn position(&self) -> usize {
        self.buf.len()
    }

    /// Patch a u32 at the given byte offset (little-endian).
    #[inline]
    pub fn patch_u32(&mut self, offset: usize, value: u32) {
        self.buf[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    /// Patch a single byte at the given offset (OR into existing value).
    #[inline]
    pub fn patch_byte(&mut self, offset: usize, bits: u8) {
        self.buf[offset] |= bits;
    }

    /// Write a 4-byte zero placeholder and return its offset, so the caller
    /// can later patch it with the actual length via `patch_u32`.
    #[inline]
    pub fn write_length_placeholder(&mut self) -> usize {
        let offset = self.buf.len();
        self.buf.extend_from_slice(&[0u8; 4]);
        offset
    }

    /// Consume the buffer and return the underlying bytes.
    pub fn into_vec(self) -> Vec<u8> {
        self.buf
    }

    /// Borrow the underlying bytes.
    pub fn as_slice(&self) -> &[u8] {
        &self.buf
    }
}

impl Default for WriteBuffer {
    fn default() -> Self {
        Self::new()
    }
}
