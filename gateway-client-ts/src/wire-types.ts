// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * Wire type identifiers — mirrors the C++ WireTypeId enum in
 * schema_evolution.hpp.  Values must stay in sync with the codec.
 */
export enum WireTypeId {
  UNKNOWN = 0x00,

  // Scalar types
  BOOL = 0x01,
  INT8 = 0x02,
  INT16 = 0x03,
  INT32 = 0x04,
  INT64 = 0x05,
  UINT8 = 0x06,
  UINT16 = 0x07,
  UINT32 = 0x08,
  UINT64 = 0x09,
  FLOAT32 = 0x0a,
  FLOAT64 = 0x0b,
  STRING = 0x0c,
  BINARY = 0x0d,
  DATE32 = 0x0e,
  DATE64 = 0x0f,
  TIMESTAMP_NANO = 0x10,
  TIME32 = 0x11,
  TIME64 = 0x12,
  DURATION_NANO = 0x13,
  FIXED_SIZE_BINARY = 0x14,
  HALF_FLOAT = 0x15,
  DECIMAL128 = 0x16,
  DECIMAL256 = 0x17,

  LARGE_STRING = 0x18,
  LARGE_BINARY = 0x19,
  STRING_VIEW = 0x1a,
  BINARY_VIEW = 0x1b,

  INTERVAL_MONTHS = 0x1c,
  INTERVAL_DAY_TIME = 0x1d,
  INTERVAL_MONTH_DAY_NANO = 0x1e,

  // Composite types
  STRUCT = 0x20,
  LIST = 0x21,
  LARGE_LIST = 0x22,
  FIXED_SIZE_LIST = 0x23,
  MAP = 0x24,
  SPARSE_UNION = 0x25,
  DENSE_UNION = 0x26,
}

/** Returns the fixed byte size for scalar wire types, or 0 for variable-length types. */
export function scalarByteSize(wt: WireTypeId): number {
  switch (wt) {
    case WireTypeId.BOOL:
    case WireTypeId.INT8:
    case WireTypeId.UINT8:
      return 1;
    case WireTypeId.INT16:
    case WireTypeId.UINT16:
    case WireTypeId.HALF_FLOAT:
      return 2;
    case WireTypeId.INT32:
    case WireTypeId.UINT32:
    case WireTypeId.FLOAT32:
    case WireTypeId.DATE32:
    case WireTypeId.TIME32:
    case WireTypeId.INTERVAL_MONTHS:
      return 4;
    case WireTypeId.INT64:
    case WireTypeId.UINT64:
    case WireTypeId.FLOAT64:
    case WireTypeId.DATE64:
    case WireTypeId.TIME64:
    case WireTypeId.TIMESTAMP_NANO:
    case WireTypeId.DURATION_NANO:
      return 8;
    case WireTypeId.INTERVAL_DAY_TIME:
      return 8;
    case WireTypeId.DECIMAL128:
      return 16;
    case WireTypeId.INTERVAL_MONTH_DAY_NANO:
      return 16;
    case WireTypeId.DECIMAL256:
      return 32;
    default:
      return 0; // variable-length or composite
  }
}
