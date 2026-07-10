// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Ambient stub for the @eiva/fletcher-gateway-client package that the generated
// coverage.fletcher.ts imports (`TypedSchema`, `WireTypeId`). It lets the GIR-1
// TS compile-check run with only `tsc` on PATH — no network install of the real
// client package. The shape mirrors exactly what the generated code uses; if
// the generator's emitted TS surface changes, this stub must track it.

declare module '@eiva/fletcher-gateway-client' {
  export enum WireTypeId {
    BOOL,
    INT32,
    INT64,
    UINT32,
    UINT64,
    FLOAT32,
    FLOAT64,
    STRING,
    BINARY,
    TIMESTAMP_NANO,
    DURATION_NANO,
    STRUCT,
    LIST,
    MAP,
  }

  export interface FieldSpec {
    name: string;
    fieldNumber: number;
    wireType: WireTypeId;
    nullable: boolean;
    fields?: readonly FieldSpec[];
    element?: FieldSpec;
    mapKey?: FieldSpec;
    mapValue?: FieldSpec;
  }

  // T is the message interface the schema describes; kept as a phantom
  // parameter so callers write `TypedSchema<IFoo>` exactly as the generator
  // emits it.
  export interface TypedSchema<T> {
    readonly __message?: T;
    fields: readonly FieldSpec[];
    protoPackage: string;
    protoMessage: string;
  }
}
