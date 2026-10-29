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
import type {
  ServerResponse,
  SubscribedResponse,
  TopicsListResponse,
  ErrorResponse,
} from './ws-protocol.js';
import { ObjectBackend } from './codec/object-backend.js';
import type { DecoderBackend } from './codec/object-backend.js';
import { encodePositional } from './codec/positional-encoder.js';
import type { SchemaDescriptor, TypedSchema } from './codec/schema-descriptor.js';
import { Batcher, resolveBatchOptions } from './batcher.js';
import type { BatchCallback, BatchOptions } from './batcher.js';
import type {
  BackendType,
  FletcherClientFault,
  FletcherClientOptions,
  FletcherFaultReason,
  MessageCallback,
} from './types.js';

interface PendingRequest {
  resolve: (resp: ServerResponse) => void;
  reject: (err: Error) => void;
  expectedType: string;
}

// A subscription delivers each row to `callback`, or gathers rows into `batcher`'s windows.
interface Subscription {
  schema: SchemaDescriptor;
  callback?: MessageCallback;
  batcher?: Batcher<unknown>;
}

export class FletcherClient {
  private ws: WebSocket | null = null;
  private backend: DecoderBackend<unknown>;
  private opts: {
    url: string;
    backend: BackendType;
    reconnectDelay: number;
    wasmFactory?: () => Promise<unknown>;
    onError?: (fault: FletcherClientFault) => void;
  };
  private pendingQueue: PendingRequest[] = [];
  private subscriptions = new Map<bigint, Subscription>();
  private undecodable = 0;

  constructor(options: FletcherClientOptions) {
    this.opts = {
      url: options.url,
      backend: options.backend ?? 'object',
      reconnectDelay: options.reconnectDelay ?? 0,
      onError: options.onError,
    };

    if (this.opts.backend === 'arrow') {
      throw new Error('Arrow backend not yet implemented. Use backend: "object" for now.');
    }
    this.backend = new ObjectBackend();
  }

  /** Connect to the gateway. */
  async connect(): Promise<void> {
    await this.connectWs();
  }

  /**
   * Create a topic on the server. Optionally announce a schema for
   * the topic — gateway will forward it to future subscribers in
   * their `subscribed` response. Pure byte-router publishers can
   * omit the schema and let subscribers bring their own (e.g. from
   * protoc-gen-fletcher).
   */
  async createTopic(topic: string, schema?: SchemaDescriptor): Promise<void> {
    const resp = await this.sendAndWait(buildCreateTopic(topic, schema), 'topic_created');
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
  }

  /**
   * Subscribe to a topic.  Returns the subscription ID.
   *
   * Schema sources, in order of preference:
   *   1. Caller-supplied (`subscribe(topic, schema, callback)`).
   *      Typed clients with a proto-gen `TypedSchema<T>` should use
   *      this — the callback parameter is inferred as `T`.
   *   2. Gateway-supplied — falls back to the schema in the
   *      `subscribed` response when a publisher announced one via
   *      `createTopic(topic, schema)`. Useful for dynamic/debug
   *      clients that don't have proto-gen output.
   *
   * Throws if no schema is available from either source.
   */
  // Overload 1 — typed schema, callback inferred from phantom type.
  subscribe<T>(
    topic: string,
    schema: TypedSchema<T>,
    callback: (row: T, attachments: Map<string, Uint8Array>) => void,
  ): Promise<bigint>;
  // Overload 2 — untyped schema; the callback's row is `unknown` unless the caller names the
  // row type (`subscribe<Row>(topic, schema, callback)`, which overload 1 then takes).
  subscribe(topic: string, schema: SchemaDescriptor, callback: MessageCallback): Promise<bigint>;
  // Overload 3 — schema from gateway, caller asserts row type via generic.
  subscribe<T = Record<string, unknown>>(
    topic: string,
    callback: (row: T, attachments: Map<string, Uint8Array>) => void,
  ): Promise<bigint>;
  async subscribe<T>(
    topic: string,
    callbackOrSchema:
      | ((row: T, attachments: Map<string, Uint8Array>) => void)
      | SchemaDescriptor
      | TypedSchema<T>,
    maybeCallback?: (row: T, attachments: Map<string, Uint8Array>) => void,
  ): Promise<bigint> {
    let schema: SchemaDescriptor | undefined;
    let callback: (row: T, attachments: Map<string, Uint8Array>) => void;
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

    const resp = await this.sendAndWait(buildSubscribe(topic), 'subscribed');
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
    const sub = resp as SubscribedResponse;

    const resolvedSchema = schema ?? sub.schema;
    if (!resolvedSchema) {
      throw new Error(
        `subscribe: no schema available for topic "${topic}". ` +
          'Either pass a SchemaDescriptor / TypedSchema or ensure the publisher created the topic with a schema.',
      );
    }

    this.subscriptions.set(sub.subId, {
      schema: resolvedSchema,
      callback: callback as MessageCallback,
    });
    return sub.subId;
  }

  /**
   * Subscribe to a topic, delivering rows in windows (D-BIND-183, as the C# client's
   * `SubscribeBatchedAsync` and the native `SubscriberArrow`). Returns the subscription ID.
   *
   * A window is delivered when it holds `maxRows` rows (`RowLimit`), when its deadline passes
   * (`Timeout`), or when the subscription ends through `unsubscribe`, `close` or the connection
   * closing (`Closing`). The deadline is armed by the window's first row (or undecodable row)
   * and falls `timeoutMs` after the previous window, so a steady stream gets a window every
   * `timeoutMs` and a row after a longer quiet spell is delivered at once. Row `i`'s
   * attachments are at index `i`. A row that does not decode is reported through `onError`,
   * counted in the window's `rowsDropped`, and left out; a window of only such rows is
   * delivered empty.
   *
   * In a browser tab that is hidden, timers are throttled, so a `Timeout` window can come
   * late there; it is never lost.
   */
  subscribeBatched<T>(
    topic: string,
    schema: TypedSchema<T>,
    callback: BatchCallback<T>,
    options?: BatchOptions,
  ): Promise<bigint>;
  subscribeBatched(
    topic: string,
    schema: SchemaDescriptor,
    callback: BatchCallback<Record<string, unknown>>,
    options?: BatchOptions,
  ): Promise<bigint>;
  async subscribeBatched<T>(
    topic: string,
    schema: SchemaDescriptor | TypedSchema<T>,
    callback: BatchCallback<T>,
    options?: BatchOptions,
  ): Promise<bigint> {
    // Refused before anything is sent, as the C# client refuses its options.
    const { maxRows, timeoutMs } = resolveBatchOptions(options);

    const resp = await this.sendAndWait(buildSubscribe(topic), 'subscribed');
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
    const subId = (resp as SubscribedResponse).subId;
    const batcher = new Batcher<T>(
      (rows, attachments, status) =>
        this.runCallback(subId, () => callback(rows, attachments, status)),
      maxRows,
      timeoutMs,
    );
    this.subscriptions.set(subId, { schema, batcher: batcher as Batcher<unknown> });
    return subId;
  }

  /**
   * How many rows did not decode against their subscription's schema and were dropped
   * (each also reported through `onError`).
   */
  get undecodableRows(): number {
    return this.undecodable;
  }

  /**
   * Unsubscribe from a subscription. Its callback is not called again from the moment this is
   * called, whatever the gateway replies (D-BIND-186, as the C# client); a batched one delivers
   * its pending window first. Rejects when the gateway refuses or the connection is gone.
   */
  async unsubscribe(subId: bigint): Promise<void> {
    this.subscriptions.get(subId)?.batcher?.close();
    this.subscriptions.delete(subId);
    const resp = await this.sendAndWait(buildUnsubscribe(subId), 'unsubscribed');
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
  }

  /**
   * Publish a message to a topic.
   *
   * Pass a `TypedSchema<T>` (the proto-gen output) for full type
   * inference on `data`, or a plain `SchemaDescriptor` if you've
   * constructed the schema by hand.
   */
  publish<T>(
    topic: string,
    schema: TypedSchema<T>,
    data: T,
    attachments?: Map<string, Uint8Array>,
  ): Promise<void>;
  publish(
    topic: string,
    schema: SchemaDescriptor,
    data: Record<string, unknown>,
    attachments?: Map<string, Uint8Array>,
  ): Promise<void>;
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
    const resp = await this.sendAndWait(buildPublish(topic, envBytes), 'published');
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
  }

  /** List all topics on the server. */
  async listTopics(): Promise<string[]> {
    const resp = await this.sendAndWait(buildListTopics(), 'topics_list');
    if (resp.type === 'error') {
      throw new Error((resp as ErrorResponse).message);
    }
    return (resp as TopicsListResponse).topics;
  }

  /** Close the connection and clean up. Batched subscriptions deliver their pending windows first. */
  close(): void {
    this.closeBatchers();
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
        // A socket this client has already replaced (close, then connect again before this
        // event arrived) closes nothing of the new connection's.
        if (this.ws !== null && this.ws !== ws) return;
        this.ws = null;
        this.closeBatchers();
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
        (p) => p.expectedType === resp.type || resp.type === 'error',
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
      if (sub) this.deliver(msg.subId, sub, msg.envelope);
    }
  }

  // One row for `sub`: decoded, then to its callback or its window. Nothing here throws on to
  // the WebSocket's handler (D-BIND-183): a row that does not decode, or a callback that
  // throws, is reported and the next row is delivered as usual.
  private deliver(subId: bigint, sub: Subscription, envelopeBytes: Uint8Array): void {
    let decoded: unknown;
    let attachments: Map<string, Uint8Array>;
    try {
      const envelope = deserializeEnvelope(envelopeBytes);
      decoded = this.backend.decode(sub.schema, envelope.row);
      attachments = envelope.attachments;
    } catch (e) {
      ++this.undecodable;
      this.report(subId, 'RowUndecodable', e);
      sub.batcher?.drop();
      return;
    }

    if (sub.batcher) {
      sub.batcher.add(decoded, attachments);
    } else {
      const callback = sub.callback!;
      this.runCallback(subId, () => callback(decoded, attachments));
    }
  }

  // Runs a subscription's callback. A throw, or the rejection of the promise an async callback
  // returns (D-BIND-185), is reported as HandlerThrew and never reaches the receive loop.
  private runCallback(subId: bigint, call: () => void): void {
    let result: unknown;
    try {
      result = call();
    } catch (e) {
      this.report(subId, 'HandlerThrew', e);
      return;
    }
    if (typeof (result as PromiseLike<unknown> | undefined)?.then === 'function') {
      (result as PromiseLike<unknown>).then(undefined, (e: unknown) =>
        this.report(subId, 'HandlerThrew', e),
      );
    }
  }

  private report(subscriptionId: bigint, reason: FletcherFaultReason, error: unknown): void {
    const fault: FletcherClientFault = { reason, subscriptionId, error };
    if (!this.opts.onError) {
      console.error(`FletcherClient: subscription ${subscriptionId}: ${reason}`, error);
      return;
    }
    try {
      this.opts.onError(fault);
    } catch {
      // The receive loop must survive whatever onError does.
    }
  }

  // Every batched subscription ends: its pending window is delivered as `Closing`.
  private closeBatchers(): void {
    for (const sub of this.subscriptions.values()) sub.batcher?.close();
  }

  private sendAndWait(frame: string | Uint8Array, expectedType: string): Promise<ServerResponse> {
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
