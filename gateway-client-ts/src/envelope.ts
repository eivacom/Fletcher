/**
 * Envelope serialize/deserialize — pure TypeScript.
 *
 * Wire format (little-endian):
 *   [ROW_LEN      : 4] uint32
 *   [ROW_DATA     : ROW_LEN]
 *   [ATTACH_COUNT : 4] uint32
 *   For each attachment:
 *     [KEY_LEN : 4] uint32
 *     [KEY     : KEY_LEN] UTF-8
 *     [BLOB_LEN: 4] uint32
 *     [BLOB    : BLOB_LEN]
 */

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

export interface Envelope {
  row: Uint8Array;
  attachments: Map<string, Uint8Array>;
}

export function serializeEnvelope(env: Envelope): Uint8Array {
  // Pre-compute total size.
  let total = 4 + env.row.byteLength + 4;
  const encodedKeys: Uint8Array[] = [];
  for (const [key, blob] of env.attachments) {
    const encoded = textEncoder.encode(key);
    encodedKeys.push(encoded);
    total += 4 + encoded.byteLength + 4 + blob.byteLength;
  }

  const buf = new Uint8Array(total);
  const view = new DataView(buf.buffer);
  let pos = 0;

  // Row.
  view.setUint32(pos, env.row.byteLength, true);
  pos += 4;
  buf.set(env.row, pos);
  pos += env.row.byteLength;

  // Attachments.
  view.setUint32(pos, env.attachments.size, true);
  pos += 4;

  let keyIdx = 0;
  for (const [, blob] of env.attachments) {
    const encodedKey = encodedKeys[keyIdx++];

    view.setUint32(pos, encodedKey.byteLength, true);
    pos += 4;
    buf.set(encodedKey, pos);
    pos += encodedKey.byteLength;

    view.setUint32(pos, blob.byteLength, true);
    pos += 4;
    buf.set(blob, pos);
    pos += blob.byteLength;
  }

  return buf;
}

export function deserializeEnvelope(data: Uint8Array): Envelope {
  if (data.byteLength < 8) {
    throw new Error('deserializeEnvelope: buffer too small');
  }

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let pos = 0;

  const readU32 = (): number => {
    if (pos + 4 > data.byteLength)
      throw new Error('deserializeEnvelope: unexpected end of buffer');
    const v = view.getUint32(pos, true);
    pos += 4;
    return v;
  };

  // Row.
  const rowLen = readU32();
  if (pos + rowLen > data.byteLength)
    throw new Error('deserializeEnvelope: row data truncated');
  const row = data.slice(pos, pos + rowLen);
  pos += rowLen;

  // Attachments.
  const attachCount = readU32();
  const attachments = new Map<string, Uint8Array>();
  for (let i = 0; i < attachCount; i++) {
    const keyLen = readU32();
    if (pos + keyLen > data.byteLength)
      throw new Error('deserializeEnvelope: key data truncated');
    const key = textDecoder.decode(data.subarray(pos, pos + keyLen));
    pos += keyLen;

    const blobLen = readU32();
    if (pos + blobLen > data.byteLength)
      throw new Error('deserializeEnvelope: blob data truncated');
    const blob = data.slice(pos, pos + blobLen);
    pos += blobLen;

    attachments.set(key, blob);
  }

  return { row, attachments };
}
