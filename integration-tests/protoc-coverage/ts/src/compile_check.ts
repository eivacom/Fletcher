// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-1 TypeScript compile-check entrypoint. Imports the generated coverage
// schemas so `tsc --noEmit` type-checks the emitted coverage.fletcher.ts
// against the @eiva/fletcher-gateway-client surface (stubbed in
// types/fletcher-gateway-client.d.ts). The generated file is copied into
// generated/ by cmake/run_tsc_check.cmake before tsc runs.
//
// This file has no runtime role (GIR-1 does not execute JS); it exists to give
// tsc a concrete reference into the generated module so a broken emission fails
// the check.

import {
  ScalarCoverage,
  CompositeCoverage,
  Leaf,
  Branch,
  ServiceRequest,
  ServiceReply,
  type IScalarCoverage,
  type ICompositeCoverage,
} from '../generated/coverage.fletcher';

// Touch representative exports so the generated module is fully type-checked.
export const coverageMessageNames: readonly string[] = [
  ScalarCoverage.protoMessage,
  CompositeCoverage.protoMessage,
  Leaf.protoMessage,
  Branch.protoMessage,
  ServiceRequest.protoMessage,
  ServiceReply.protoMessage,
];

export const compositeFieldCount: number = CompositeCoverage.fields.length;

// Exercise the emitted message interfaces at the type level.
const _scalars: Partial<IScalarCoverage> = { int32_value: 0, string_value: '' };
const _composite: Partial<ICompositeCoverage> = {};
void _scalars;
void _composite;
