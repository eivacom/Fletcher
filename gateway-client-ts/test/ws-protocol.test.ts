// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { describe, it, expect } from 'vitest';
import {
  buildCreateTopic,
  buildSubscribe,
  buildUnsubscribe,
  buildPublish,
  buildListTopics,
  parseTextResponse,
  parseBinaryMessage,
} from '../src/ws-protocol.js';

describe('Text frame builders (client → server)', () => {
  it('buildCreateTopic returns JSON with action and topic', () => {
    const json = JSON.parse(buildCreateTopic('demo/telemetry'));
    expect(json.action).toBe('create_topic');
    expect(json.topic).toBe('demo/telemetry');
  });

  it('buildSubscribe returns JSON with action and topic', () => {
    const json = JSON.parse(buildSubscribe('test/topic'));
    expect(json.action).toBe('subscribe');
    expect(json.topic).toBe('test/topic');
  });

  it('buildUnsubscribe stringifies bigint subId', () => {
    const json = JSON.parse(buildUnsubscribe(9007199254740993n)); // > 2^53
    expect(json.action).toBe('unsubscribe');
    expect(json.subId).toBe('9007199254740993');
  });

  it('buildListTopics returns JSON with action only', () => {
    const json = JSON.parse(buildListTopics());
    expect(json.action).toBe('list_topics');
    expect(Object.keys(json)).toEqual(['action']);
  });
});

describe('Binary frame builder (client → server)', () => {
  it('buildPublish encodes [TOPIC_LEN:2][TOPIC:N][ENVELOPE:rest]', () => {
    const envBytes = new Uint8Array([0xaa, 0xbb]);
    const frame = buildPublish('t', envBytes);
    const view = new DataView(frame.buffer, frame.byteOffset);
    expect(view.getUint16(0, true)).toBe(1); // topic length
    expect(frame[2]).toBe(0x74); // 't'
    expect(frame[3]).toBe(0xaa);
    expect(frame[4]).toBe(0xbb);
    expect(frame.byteLength).toBe(2 + 1 + 2);
  });
});

describe('Text response parser (server → client)', () => {
  it('parses topic_created', () => {
    const resp = parseTextResponse('{"type":"topic_created"}');
    expect(resp.type).toBe('topic_created');
  });

  it('parses subscribed with bigint subId', () => {
    const resp = parseTextResponse(
      '{"type":"subscribed","subId":"9007199254740993","topic":"a/b"}',
    );
    expect(resp.type).toBe('subscribed');
    if (resp.type === 'subscribed') {
      expect(resp.subId).toBe(9007199254740993n);
      expect(resp.topic).toBe('a/b');
    }
  });

  it('parses unsubscribed', () => {
    const resp = parseTextResponse('{"type":"unsubscribed"}');
    expect(resp.type).toBe('unsubscribed');
  });

  it('parses published', () => {
    const resp = parseTextResponse('{"type":"published"}');
    expect(resp.type).toBe('published');
  });

  it('parses topics_list', () => {
    const resp = parseTextResponse('{"type":"topics_list","topics":["x","y"]}');
    expect(resp.type).toBe('topics_list');
    if (resp.type === 'topics_list') {
      expect(resp.topics).toEqual(['x', 'y']);
    }
  });

  it('parses error', () => {
    const resp = parseTextResponse('{"type":"error","message":"bad"}');
    expect(resp.type).toBe('error');
    if (resp.type === 'error') {
      expect(resp.message).toBe('bad');
    }
  });

  it('throws on unknown type', () => {
    expect(() => parseTextResponse('{"type":"unknown"}')).toThrow(/Unknown response type/);
  });
});

describe('Binary message parser (server → client)', () => {
  it('parses [SUB_ID:8][ENVELOPE:rest]', () => {
    const buf = new Uint8Array(8 + 3);
    const view = new DataView(buf.buffer);
    view.setBigUint64(0, 42n, true);
    buf[8] = 0xca;
    buf[9] = 0xfe;
    buf[10] = 0x00;

    const msg = parseBinaryMessage(buf);
    expect(msg.subId).toBe(42n);
    expect(msg.envelope).toEqual(new Uint8Array([0xca, 0xfe, 0x00]));
  });

  it('throws on frame shorter than 8 bytes', () => {
    expect(() => parseBinaryMessage(new Uint8Array(7))).toThrow(/too short/);
  });
});
