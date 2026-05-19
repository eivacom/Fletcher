// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * eiva-fletcher-gateway-client — public API.
 */

// Wire format
export { WireTypeId, scalarByteSize } from './wire-types.js';

// Schema descriptors
export type { SchemaDescriptor, FieldDescriptor, TypedSchema } from './codec/schema-descriptor.js';

// Codec
export { encodePositional } from './codec/positional-encoder.js';
export { decodePositional } from './codec/positional-decoder.js';

// Backends
export type { DecoderBackend } from './codec/object-backend.js';
export { ObjectBackend } from './codec/object-backend.js';
export { ArrowBackend } from './codec/arrow-backend.js';

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
