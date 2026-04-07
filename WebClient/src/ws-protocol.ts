/**
 * WebSocket binary frame builders and parsers.
 *
 * Mirrors the protocol defined in ws_session.hpp.
 * All multi-byte integers are little-endian.
 */

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

// -----------------------------------------------------------------------
// Action tags (client → server)
// -----------------------------------------------------------------------

export const enum ActionTag {
  CREATE_TOPIC = 0x01,
  SUBSCRIBE    = 0x02,
  UNSUBSCRIBE  = 0x03,
  PUBLISH      = 0x04,
  LIST_TOPICS  = 0x05,
}

// -----------------------------------------------------------------------
// Response tags (server → client)
// -----------------------------------------------------------------------

export const enum ResponseTag {
  TOPIC_CREATED = 0x01,
  SUBSCRIBED    = 0x02,
  UNSUBSCRIBED  = 0x03,
  PUBLISHED     = 0x04,
  TOPICS        = 0x05,
  MESSAGE       = 0x06,
  ERROR         = 0xFF,
}

// -----------------------------------------------------------------------
// Parsed response types
// -----------------------------------------------------------------------

export interface TopicCreatedResponse {
  tag: ResponseTag.TOPIC_CREATED;
}

export interface SubscribedResponse {
  tag: ResponseTag.SUBSCRIBED;
  subId: bigint;
  topic: string;
}

export interface UnsubscribedResponse {
  tag: ResponseTag.UNSUBSCRIBED;
}

export interface PublishedResponse {
  tag: ResponseTag.PUBLISHED;
}

export interface TopicsResponse {
  tag: ResponseTag.TOPICS;
  topics: string[];
}

export interface MessageResponse {
  tag: ResponseTag.MESSAGE;
  subId: bigint;
  envelope: Uint8Array; // Raw serialized envelope bytes.
}

export interface ErrorResponse {
  tag: ResponseTag.ERROR;
  message: string;
}

export type ServerResponse =
  | TopicCreatedResponse
  | SubscribedResponse
  | UnsubscribedResponse
  | PublishedResponse
  | TopicsResponse
  | MessageResponse
  | ErrorResponse;

// -----------------------------------------------------------------------
// Frame builders (client → server)
// -----------------------------------------------------------------------

export function buildCreateTopic(topic: string): Uint8Array {
  const topicBytes = textEncoder.encode(topic);
  const buf = new Uint8Array(1 + 2 + topicBytes.byteLength);
  const view = new DataView(buf.buffer);
  buf[0] = ActionTag.CREATE_TOPIC;
  view.setUint16(1, topicBytes.byteLength, true);
  buf.set(topicBytes, 3);
  return buf;
}

export function buildSubscribe(topic: string): Uint8Array {
  const topicBytes = textEncoder.encode(topic);
  const buf = new Uint8Array(1 + 2 + topicBytes.byteLength);
  const view = new DataView(buf.buffer);
  buf[0] = ActionTag.SUBSCRIBE;
  view.setUint16(1, topicBytes.byteLength, true);
  buf.set(topicBytes, 3);
  return buf;
}

export function buildUnsubscribe(subId: bigint): Uint8Array {
  const buf = new Uint8Array(1 + 8);
  const view = new DataView(buf.buffer);
  buf[0] = ActionTag.UNSUBSCRIBE;
  view.setBigUint64(1, subId, true);
  return buf;
}

export function buildPublish(topic: string, envelopeBytes: Uint8Array): Uint8Array {
  const topicBytes = textEncoder.encode(topic);
  const buf = new Uint8Array(1 + 2 + topicBytes.byteLength + envelopeBytes.byteLength);
  const view = new DataView(buf.buffer);
  buf[0] = ActionTag.PUBLISH;
  view.setUint16(1, topicBytes.byteLength, true);
  buf.set(topicBytes, 3);
  buf.set(envelopeBytes, 3 + topicBytes.byteLength);
  return buf;
}

export function buildListTopics(): Uint8Array {
  return new Uint8Array([ActionTag.LIST_TOPICS]);
}

// -----------------------------------------------------------------------
// Response parser (server → client)
// -----------------------------------------------------------------------

export function parseResponse(data: Uint8Array): ServerResponse {
  if (data.byteLength < 1) {
    throw new Error('parseResponse: empty frame');
  }

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const tag = data[0] as ResponseTag;

  switch (tag) {
    case ResponseTag.TOPIC_CREATED:
      return { tag };

    case ResponseTag.SUBSCRIBED: {
      if (data.byteLength < 1 + 8 + 2)
        throw new Error('parseResponse: SUBSCRIBED frame too short');
      const subId = view.getBigUint64(1, true);
      const topicLen = view.getUint16(9, true);
      if (data.byteLength < 11 + topicLen)
        throw new Error('parseResponse: SUBSCRIBED topic truncated');
      const topic = textDecoder.decode(data.subarray(11, 11 + topicLen));
      return { tag, subId, topic };
    }

    case ResponseTag.UNSUBSCRIBED:
      return { tag };

    case ResponseTag.PUBLISHED:
      return { tag };

    case ResponseTag.TOPICS: {
      if (data.byteLength < 3)
        throw new Error('parseResponse: TOPICS frame too short');
      const count = view.getUint16(1, true);
      const topics: string[] = [];
      let pos = 3;
      for (let i = 0; i < count; i++) {
        if (pos + 2 > data.byteLength)
          throw new Error('parseResponse: TOPICS entry truncated');
        const tLen = view.getUint16(pos, true);
        pos += 2;
        if (pos + tLen > data.byteLength)
          throw new Error('parseResponse: TOPICS string truncated');
        topics.push(textDecoder.decode(data.subarray(pos, pos + tLen)));
        pos += tLen;
      }
      return { tag, topics };
    }

    case ResponseTag.MESSAGE: {
      if (data.byteLength < 1 + 8)
        throw new Error('parseResponse: MESSAGE frame too short');
      const subId = view.getBigUint64(1, true);
      const envelope = data.slice(9);
      return { tag, subId, envelope };
    }

    case ResponseTag.ERROR: {
      if (data.byteLength < 3)
        throw new Error('parseResponse: ERROR frame too short');
      const msgLen = view.getUint16(1, true);
      if (data.byteLength < 3 + msgLen)
        throw new Error('parseResponse: ERROR message truncated');
      const message = textDecoder.decode(data.subarray(3, 3 + msgLen));
      return { tag, message };
    }

    default:
      throw new Error(`parseResponse: unknown tag 0x${(tag as number).toString(16)}`);
  }
}
