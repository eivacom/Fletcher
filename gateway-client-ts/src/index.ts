/**
 * eiva-fletcher-gateway-client — public API.
 *
 * Re-exports the codec primitives from eiva-fletcher-codec for convenience
 * so callers can pull both client + codec from a single import.
 */

// Re-export codec primitives
export {
  WireTypeId,
  scalarByteSize,
  ObjectBackend,
  ArrowBackend,
  encodePositional,
  decodePositional,
} from 'eiva-fletcher-codec';
export type {
  SchemaDescriptor,
  FieldDescriptor,
  DecoderBackend,
} from 'eiva-fletcher-codec';

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
  parseTextResponse,
  parseBinaryMessage,
} from './ws-protocol.js';
export type {
  ServerResponse,
  TopicCreatedResponse,
  SubscribedResponse,
  UnsubscribedResponse,
  PublishedResponse,
  TopicsListResponse,
  MessageData,
  ErrorResponse,
} from './ws-protocol.js';

// Client
export { FletcherClient } from './client.js';
export type { FletcherClientOptions, MessageCallback, BackendType } from './types.js';
