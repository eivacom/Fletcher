// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * WebSocket frame builders and parsers.
 *
 * Mirrors the split text/binary protocol in ws_session.hpp.
 *
 *   Text frames  → JSON control messages (subscribe, create topic, etc.)
 *   Binary frames → data messages (publish, message delivery)
 *
 * All multi-byte integers in binary frames are little-endian.
 * Sub IDs are stringified in JSON to avoid JS Number precision loss.
 */

import type { SchemaDescriptor, FieldDescriptor } from './codec/schema-descriptor.js';

const textEncoder = new TextEncoder();

// -----------------------------------------------------------------------
// Parsed response types (server → client)
// -----------------------------------------------------------------------

export interface TopicCreatedResponse {
  type: 'topic_created';
}

export interface SubscribedResponse {
  type: 'subscribed';
  subId: bigint;
  topic: string;
  /**
   * Schema descriptor parsed from the server's JSON schema, if the
   * topic was created with one. Gateway forwards whatever schema a
   * publisher announced via `buildCreateTopic(topic, schema)` — it
   * does not validate or generate schemas itself.
   */
  schema?: SchemaDescriptor;
  /** Base64-encoded Arrow IPC schema bytes (full fidelity). */
  schemaIpc?: string;
}

export interface UnsubscribedResponse {
  type: 'unsubscribed';
}

export interface PublishedResponse {
  type: 'published';
}

export interface TopicsListResponse {
  type: 'topics_list';
  topics: string[];
}

export interface ErrorResponse {
  type: 'error';
  message: string;
}

export type ServerResponse =
  | TopicCreatedResponse
  | SubscribedResponse
  | UnsubscribedResponse
  | PublishedResponse
  | TopicsListResponse
  | ErrorResponse;

/** Parsed binary MESSAGE frame (server → client). */
export interface MessageData {
  subId: bigint;
  envelope: Uint8Array;
}

// -----------------------------------------------------------------------
// Text frame builders (client → server) — return JSON strings
// -----------------------------------------------------------------------

export function buildCreateTopic(
  topic: string,
  schema?: SchemaDescriptor,
): string {
  // Schema is optional. Publishers that want subscribers to receive
  // the schema in their `subscribed` response announce it here; pure
  // byte-routers can omit it. Gateway forwards what it gets and does
  // not validate.
  return JSON.stringify(
    schema
      ? { action: 'create_topic', topic, schema }
      : { action: 'create_topic', topic }
  );
}

export function buildSubscribe(topic: string): string {
  return JSON.stringify({ action: 'subscribe', topic });
}

export function buildUnsubscribe(subId: bigint): string {
  return JSON.stringify({ action: 'unsubscribe', subId: subId.toString() });
}

export function buildListTopics(): string {
  return JSON.stringify({ action: 'list_topics' });
}

// -----------------------------------------------------------------------
// Binary frame builder (client → server) — PUBLISH only
// -----------------------------------------------------------------------

export function buildPublish(topic: string, envelopeBytes: Uint8Array): Uint8Array {
  const topicBytes = textEncoder.encode(topic);
  if (topicBytes.byteLength > 0xFFFF) {
    throw new Error(`Topic name too long: ${topicBytes.byteLength} bytes (max 65535)`);
  }
  const buf = new Uint8Array(2 + topicBytes.byteLength + envelopeBytes.byteLength);
  const view = new DataView(buf.buffer);
  view.setUint16(0, topicBytes.byteLength, true);
  buf.set(topicBytes, 2);
  buf.set(envelopeBytes, 2 + topicBytes.byteLength);
  return buf;
}

// -----------------------------------------------------------------------
// JSON schema → SchemaDescriptor conversion (used by parseTextResponse
// when the gateway forwards a publisher-supplied schema in a
// subscribed response).
// -----------------------------------------------------------------------

/* eslint-disable @typescript-eslint/no-explicit-any */

function parseFieldDescriptor(j: any): FieldDescriptor {
  const fd: FieldDescriptor = {
    name: j.name as string,
    fieldNumber: (j.fieldNumber as number) ?? 0,
    wireType: j.wireType as number,
    nullable: j.nullable as boolean,
  };
  if (j.element)        fd.element = parseFieldDescriptor(j.element);
  if (j.mapKey)         fd.mapKey  = parseFieldDescriptor(j.mapKey);
  if (j.mapValue)       fd.mapValue = parseFieldDescriptor(j.mapValue);
  if (j.fields)         fd.fields = (j.fields as any[]).map(parseFieldDescriptor);
  if (j.fixedSize != null) fd.fixedSize = j.fixedSize as number;
  return fd;
}

function parseSchemaFromJson(j: any): SchemaDescriptor {
  const fields = (j.fields as any[]).map(parseFieldDescriptor);
  return { fields };
}

/* eslint-enable @typescript-eslint/no-explicit-any */

// -----------------------------------------------------------------------
// Text response parser (server → client)
// -----------------------------------------------------------------------

export function parseTextResponse(text: string): ServerResponse {
  const j = JSON.parse(text);
  const t: string = j.type;

  switch (t) {
    case 'topic_created':
      return { type: t };
    case 'subscribed': {
      const resp: SubscribedResponse = {
        type: t,
        subId: BigInt(j.subId),
        topic: j.topic,
      };
      if (j.schemaIpc) resp.schemaIpc = j.schemaIpc as string;
      if (j.schema)    resp.schema = parseSchemaFromJson(j.schema);
      return resp;
    }
    case 'unsubscribed':
      return { type: t };
    case 'published':
      return { type: t };
    case 'topics_list':
      return { type: t, topics: j.topics };
    case 'error':
      return { type: t, message: j.message };
    default:
      throw new Error(`Unknown response type: ${t}`);
  }
}

// -----------------------------------------------------------------------
// Binary message parser (server → client) — MESSAGE only
// -----------------------------------------------------------------------

export function parseBinaryMessage(data: Uint8Array): MessageData {
  if (data.byteLength < 8) {
    throw new Error('parseBinaryMessage: frame too short');
  }
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const subId = view.getBigUint64(0, true);
  const envelope = data.slice(8);
  return { subId, envelope };
}
