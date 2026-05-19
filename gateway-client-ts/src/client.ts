// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * FletcherClient — WebSocket manager for the Fletcher WebGateway.
 *
 * Connects over WebSocket, sends JSON control frames and binary data
 * frames, and dispatches decoded messages to subscriber callbacks.
 */

import { deserializeEnvelope, serializeEnvelope } from './envelope.js';
import type { Envelope } from './envelope.js';
import {
  buildCreateTopic,
  buildSubscribe,
  buildUnsubscribe,
  buildPublish,
  buildListTopics,
  parseTextResponse,
  parseBinaryMessage,
} from './ws-protocol.js';
import type { ServerResponse, SubscribedResponse, TopicsListResponse, ErrorResponse } from './ws-protocol.js';
import { ObjectBackend } from './codec/object-backend.js';
import type { DecoderBackend } from './codec/object-backend.js';
import { encodePositional } from './codec/positional-encoder.js';
import type { SchemaDescriptor } from './codec/schema-descriptor.js';
import type { BackendType, FletcherClientOptions, MessageCallback } from './types.js';

interface PendingRequest {
  resolve: (resp: ServerResponse) => void;
  reject: (err: Error) => void;
  expectedType: string;
}

interface Subscription {
  schema: SchemaDescriptor;
  callback: MessageCallback;
}

export class FletcherClient {
  private ws: WebSocket | null = null;
  private backend: DecoderBackend<unknown>;
  private opts: {
    url: string;
    backend: BackendType;
    reconnectDelay: number;
    wasmFactory?: () => Promise<unknown>;
  };
  private pendingQueue: PendingRequest[] = [];
  private subscriptions = new Map<bigint, Subscription>();

  constructor(options: FletcherClientOptions) {
    this.opts = {
      url: options.url,
      backend: options.backend ?? 'object',
      reconnectDelay: options.reconnectDelay ?? 0,
    };

    if (this.opts.backend === 'arrow') {
      throw new Error(
        'Arrow backend not yet implemented. Use backend: "object" for now.',
      );
    }
    this.backend = new ObjectBackend();
  }

  /** Connect to the gateway. */
  async connect(): Promise<void> {
    await this.connectWs();
  }

  /** Create a topic on the server. */
  async createTopic(topic: string): Promise<void> {
    const resp = await this.sendAndWait(
      buildCreateTopic(topic),
      'topic_created',
    );
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
  }

  /**
   * Subscribe to a topic.  Returns the subscription ID.
   *
   * If `schema` is omitted, the schema is obtained from the server's
   * subscribe response (requires a publisher to have created the topic
   * with a schema).
   */
  async subscribe<T = Record<string, unknown>>(
    topic: string,
    callbackOrSchema: MessageCallback<T> | SchemaDescriptor,
    maybeCallback?: MessageCallback<T>,
  ): Promise<bigint> {
    // Support both (topic, schema, cb) and (topic, cb) signatures.
    let schema: SchemaDescriptor | undefined;
    let callback: MessageCallback<T>;
    if (typeof callbackOrSchema === 'function') {
      callback = callbackOrSchema;
    } else {
      schema = callbackOrSchema;
      if (typeof maybeCallback !== 'function') {
        throw new Error(
          'subscribe: callback is required when providing a schema. ' +
          'Use subscribe(topic, callback) or subscribe(topic, schema, callback).',
        );
      }
      callback = maybeCallback;
    }

    const resp = await this.sendAndWait(
      buildSubscribe(topic),
      'subscribed',
    );
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
    const sub = resp as SubscribedResponse;

    // Use server-provided schema if caller didn't supply one.
    const resolvedSchema = schema ?? sub.schema;
    if (!resolvedSchema) {
      throw new Error(
        `subscribe: no schema available for topic "${topic}". ` +
        'Either pass a SchemaDescriptor or ensure the publisher created the topic with a schema.',
      );
    }

    this.subscriptions.set(sub.subId, {
      schema: resolvedSchema,
      callback: callback as MessageCallback,
    });
    return sub.subId;
  }

  /** Unsubscribe from a subscription. */
  async unsubscribe(subId: bigint): Promise<void> {
    const resp = await this.sendAndWait(
      buildUnsubscribe(subId),
      'unsubscribed',
    );
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
    this.subscriptions.delete(subId);
  }

  /** Publish a message to a topic. */
  async publish(
    topic: string,
    schema: SchemaDescriptor,
    data: Record<string, unknown>,
    attachments?: Map<string, Uint8Array>,
  ): Promise<void> {
    const rowBytes = encodePositional(schema, data);
    const envelope: Envelope = {
      row: rowBytes,
      attachments: attachments ?? new Map(),
    };
    const envBytes = serializeEnvelope(envelope);
    const resp = await this.sendAndWait(
      buildPublish(topic, envBytes),
      'published',
    );
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
  }

  /** List all topics on the server. */
  async listTopics(): Promise<string[]> {
    const resp = await this.sendAndWait(
      buildListTopics(),
      'topics_list',
    );
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
    return (resp as TopicsListResponse).topics;
  }

  /** Close the connection and clean up. */
  close(): void {
    this.subscriptions.clear();
    for (const p of this.pendingQueue) {
      p.reject(new Error('Client closed'));
    }
    this.pendingQueue = [];
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }

  // -----------------------------------------------------------------------
  // Internal
  // -----------------------------------------------------------------------

  private connectWs(): Promise<void> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.opts.url);
      ws.binaryType = 'arraybuffer';

      ws.onopen = () => {
        this.ws = ws;
        resolve();
      };

      ws.onerror = () => {
        reject(new Error(`WebSocket connection failed: ${this.opts.url}`));
      };

      ws.onclose = () => {
        this.ws = null;
        for (const p of this.pendingQueue) {
          p.reject(new Error('WebSocket closed'));
        }
        this.pendingQueue = [];
      };

      ws.onmessage = (event: MessageEvent) => {
        this.onMessage(event);
      };
    });
  }

  private onMessage(event: MessageEvent): void {
    if (typeof event.data === 'string') {
      // Text frame → JSON control response.
      let resp: ServerResponse;
      try {
        resp = parseTextResponse(event.data);
      } catch {
        return; // Malformed JSON — ignore.
      }

      const idx = this.pendingQueue.findIndex(
        p => p.expectedType === resp.type || resp.type === 'error',
      );
      if (idx === -1) return;
      const [pending] = this.pendingQueue.splice(idx, 1);
      pending.resolve(resp);
    } else {
      // Binary frame → MESSAGE data delivery.
      const data = new Uint8Array(event.data as ArrayBuffer);
      let msg;
      try {
        msg = parseBinaryMessage(data);
      } catch {
        return;
      }

      const sub = this.subscriptions.get(msg.subId);
      if (sub) {
        const envelope = deserializeEnvelope(msg.envelope);
        const decoded = this.backend.decode(sub.schema, envelope.row);
        sub.callback(decoded, envelope.attachments);
      }
    }
  }

  private sendAndWait(
    frame: string | Uint8Array,
    expectedType: string,
  ): Promise<ServerResponse> {
    return new Promise((resolve, reject) => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        reject(new Error('WebSocket not connected'));
        return;
      }
      this.pendingQueue.push({ resolve, reject, expectedType });
      this.ws.send(frame);
    });
  }
}
