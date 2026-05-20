// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * Schema and field descriptors used by both codec backends.
 *
 * These are the runtime type-info objects that the protoc plugin
 * generates alongside TypeScript interfaces.
 */

import { WireTypeId } from '../wire-types.js';

export interface FieldDescriptor {
  /** Human-readable field name (from .proto). */
  name: string;
  /** Proto field number — matches FIELD_NUM in the wire format. */
  fieldNumber: number;
  /** Wire type of this field. */
  wireType: WireTypeId;
  /** Whether this field can be null. */
  nullable: boolean;
  /** For LIST/LARGE_LIST/FIXED_SIZE_LIST: element descriptor. */
  element?: FieldDescriptor;
  /** For MAP: key descriptor. */
  mapKey?: FieldDescriptor;
  /** For MAP: value descriptor. */
  mapValue?: FieldDescriptor;
  /** For STRUCT: nested field descriptors. */
  fields?: FieldDescriptor[];
  /** For FIXED_SIZE_LIST: element count. */
  fixedSize?: number;
}

export interface SchemaDescriptor {
  /** Top-level fields in schema order. */
  fields: FieldDescriptor[];
  /** @deprecated No longer used by the positional codec. Kept for backward compat with tagged row-encoder. */
  schemaHash?: bigint;
  /** Originating proto package name. */
  protoPackage?: string;
  /** Originating proto message name. */
  protoMessage?: string;
}

/**
 * A `SchemaDescriptor` bound to a TypeScript row type `T` via a
 * phantom property. Runtime-identical to `SchemaDescriptor`; the
 * `__row` slot is never accessed and exists only so the type system
 * can carry the row-shape from generated proto bindings to
 * `FletcherClient.subscribe` / `publish` overloads.
 *
 * `protoc-gen-fletcher` emits one of these per message:
 *
 *   export interface ITelemetry { ... }
 *   export const Telemetry: TypedSchema<ITelemetry> = { fields: [...] };
 *
 * which lets the call site say
 *
 *   client.subscribe(topic, Telemetry, (row) => ...);  // row: ITelemetry
 *   client.publish(topic, Telemetry, { sensor_id: 1, ... });
 *
 * without having to name the row type at the call site.
 */
export interface TypedSchema<T> extends SchemaDescriptor {
  /** Phantom — TypeScript-only. Never present at runtime. */
  readonly __row?: T;
}
