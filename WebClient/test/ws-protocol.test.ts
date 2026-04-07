import { describe, it, expect } from 'vitest';
import {
  buildCreateTopic,
  buildSubscribe,
  buildUnsubscribe,
  buildPublish,
  buildListTopics,
  parseResponse,
} from '../src/ws-protocol.js';

describe('WS Protocol frame builders', () => {
  it('buildCreateTopic encodes correctly', () => {
    const frame = buildCreateTopic('demo/telemetry');
    expect(frame[0]).toBe(0x01); // CREATE_TOPIC tag
    const view = new DataView(frame.buffer, frame.byteOffset);
    const topicLen = view.getUint16(1, true);
    expect(topicLen).toBe(14); // "demo/telemetry".length
    const topic = new TextDecoder().decode(frame.subarray(3, 3 + topicLen));
    expect(topic).toBe('demo/telemetry');
  });

  it('buildSubscribe encodes correctly', () => {
    const frame = buildSubscribe('test/topic');
    expect(frame[0]).toBe(0x02); // SUBSCRIBE tag
    const view = new DataView(frame.buffer, frame.byteOffset);
    const topicLen = view.getUint16(1, true);
    expect(topicLen).toBe(10);
  });

  it('buildUnsubscribe encodes sub_id as uint64 LE', () => {
    const frame = buildUnsubscribe(42n);
    expect(frame[0]).toBe(0x03); // UNSUBSCRIBE tag
    const view = new DataView(frame.buffer, frame.byteOffset);
    expect(view.getBigUint64(1, true)).toBe(42n);
  });

  it('buildPublish includes topic + envelope bytes', () => {
    const envBytes = new Uint8Array([0xAA, 0xBB]);
    const frame = buildPublish('t', envBytes);
    expect(frame[0]).toBe(0x04); // PUBLISH tag
    const view = new DataView(frame.buffer, frame.byteOffset);
    const topicLen = view.getUint16(1, true);
    expect(topicLen).toBe(1);
    expect(frame[3]).toBe(0x74); // 't'
    expect(frame[4]).toBe(0xAA);
    expect(frame[5]).toBe(0xBB);
  });

  it('buildListTopics is a single byte', () => {
    const frame = buildListTopics();
    expect(frame.byteLength).toBe(1);
    expect(frame[0]).toBe(0x05);
  });
});

describe('WS Protocol response parser', () => {
  it('parses TOPIC_CREATED', () => {
    const resp = parseResponse(new Uint8Array([0x01]));
    expect(resp.tag).toBe(0x01);
  });

  it('parses SUBSCRIBED', () => {
    // [TAG:1] [SUB_ID:8] [TOPIC_LEN:2] [TOPIC:N]
    const topic = new TextEncoder().encode('a/b');
    const buf = new Uint8Array(1 + 8 + 2 + topic.byteLength);
    const view = new DataView(buf.buffer);
    buf[0] = 0x02;
    view.setBigUint64(1, 99n, true);
    view.setUint16(9, topic.byteLength, true);
    buf.set(topic, 11);

    const resp = parseResponse(buf);
    expect(resp.tag).toBe(0x02);
    if (resp.tag === 0x02) {
      expect(resp.subId).toBe(99n);
      expect(resp.topic).toBe('a/b');
    }
  });

  it('parses TOPICS', () => {
    // [TAG:1] [COUNT:2] {[TOPIC_LEN:2] [TOPIC:N]}*
    const t1 = new TextEncoder().encode('x');
    const t2 = new TextEncoder().encode('y');
    const buf = new Uint8Array(1 + 2 + (2 + t1.byteLength) + (2 + t2.byteLength));
    const view = new DataView(buf.buffer);
    buf[0] = 0x05;
    view.setUint16(1, 2, true); // count = 2
    let pos = 3;
    view.setUint16(pos, t1.byteLength, true); pos += 2;
    buf.set(t1, pos); pos += t1.byteLength;
    view.setUint16(pos, t2.byteLength, true); pos += 2;
    buf.set(t2, pos);

    const resp = parseResponse(buf);
    expect(resp.tag).toBe(0x05);
    if (resp.tag === 0x05) {
      expect(resp.topics).toEqual(['x', 'y']);
    }
  });

  it('parses MESSAGE', () => {
    // [TAG:1] [SUB_ID:8] [ENVELOPE:rest]
    const envBytes = new Uint8Array([0xCA, 0xFE]);
    const buf = new Uint8Array(1 + 8 + envBytes.byteLength);
    const view = new DataView(buf.buffer);
    buf[0] = 0x06;
    view.setBigUint64(1, 7n, true);
    buf.set(envBytes, 9);

    const resp = parseResponse(buf);
    expect(resp.tag).toBe(0x06);
    if (resp.tag === 0x06) {
      expect(resp.subId).toBe(7n);
      expect(resp.envelope).toEqual(envBytes);
    }
  });

  it('parses ERROR', () => {
    const msg = new TextEncoder().encode('bad');
    const buf = new Uint8Array(1 + 2 + msg.byteLength);
    const view = new DataView(buf.buffer);
    buf[0] = 0xFF;
    view.setUint16(1, msg.byteLength, true);
    buf.set(msg, 3);

    const resp = parseResponse(buf);
    expect(resp.tag).toBe(0xFF);
    if (resp.tag === 0xFF) {
      expect(resp.message).toBe('bad');
    }
  });

  it('throws on empty frame', () => {
    expect(() => parseResponse(new Uint8Array(0))).toThrow();
  });

  it('throws on unknown tag', () => {
    expect(() => parseResponse(new Uint8Array([0x99]))).toThrow(/unknown tag/);
  });
});
