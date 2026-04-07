/**
 * @fletcher/web-client — public API.
 */

// Wire format
export { WireTypeId, scalarByteSize } from './wire-types.js';

// Envelope
export { serializeEnvelope, deserializeEnvelope } from './envelope.js';
export type { Envelope } from './envelope.js';

// WebSocket protocol
export {
  buildCreateTopic,
  buildSubscribe,
  buildUnsubscribe,
  buildPublish,
  buildListTopics,
  parseResponse,
} from './ws-protocol.js';
export type {
  ServerResponse,
  TopicCreatedResponse,
  SubscribedResponse,
  UnsubscribedResponse,
  PublishedResponse,
  TopicsResponse,
  MessageResponse,
  ErrorResponse,
} from './ws-protocol.js';

// Codec
export type { SchemaDescriptor, FieldDescriptor } from './codec/schema-descriptor.js';
export type { DecoderBackend } from './codec/row-decoder.js';
export { readScalar, decodeFieldValue } from './codec/row-decoder.js';
export { encodeRow } from './codec/row-encoder.js';
export { ObjectBackend } from './codec/object-backend.js';
export { ArrowBackend } from './codec/arrow-backend.js';

// WASM decoder
export { WasmDecoder } from './wasm-decoder.js';
export type { FieldEntry } from './wasm-decoder.js';

// Client
export { FletcherClient } from './client.js';
export type { FletcherClientOptions, MessageCallback, BackendType } from './types.js';
