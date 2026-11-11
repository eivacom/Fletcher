// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// protoc-coverage's four CompositeCoverage fixture rows in TypeScript, by the `seq` the
// cross-language lane sends them under (BIND-8d): a transcription of
// integration-tests/protoc-coverage/tests/coverage_fixture.hpp, function by function and value
// by value, in the types the generated module declares (int64 as bigint, Timestamp and Duration
// as bigint nanoseconds, bytes as Uint8Array, unset optionals as null, maps as Map in entry
// order). The C# transcription is dotnet/tests/Shared/CoverageFixture.cs.
import type {
  IBranch,
  ICompositeCoverage,
  ILeaf,
  IScalarCoverage,
} from '../generated-ts/coverage.fletcher.js';

const TOP_OK = 1;
const TOP_WARN = 2;
const TOP_ERROR = 3;
const INNER_ACTIVE = 1;
const INNER_DISABLED = 2;
const TIMESTAMP_NS = 1_700_000_000_000_000_000n;
const DURATION_NS = 5_000_000_000n;

export const ROW_COUNT = 4;

function scalars(): IScalarCoverage {
  return {
    bool_value: true,
    int32_value: -42,
    int64_value: 9_000_000_000n,
    uint32_value: 7,
    uint64_value: 8n,
    sint32_value: -9,
    sint64_value: -10n,
    fixed32_value: 11,
    fixed64_value: 12n,
    sfixed32_value: -13,
    sfixed64_value: -14n,
    float_value: 1.5,
    double_value: 2.5,
    string_value: 'coverage',
    bytes_value: new Uint8Array([0x01, 0x02, 0x03]),
    optional_bool: null,
    optional_int32: 7,
    optional_string: null,
    optional_bytes: null,
    wrapped_bool: null,
    wrapped_int32: 123,
    wrapped_int64: null,
    wrapped_uint32: null,
    wrapped_uint64: null,
    wrapped_float: null,
    wrapped_double: null,
    wrapped_string: null,
    wrapped_bytes: null,
    timestamp_value: TIMESTAMP_NS,
    duration_value: DURATION_NS,
    status: TOP_WARN,
    nested_status: INNER_ACTIVE,
  };
}

function scalarsAllSet(): IScalarCoverage {
  return {
    bool_value: false,
    int32_value: 100,
    int64_value: 200n,
    uint32_value: 3,
    uint64_value: 4n,
    sint32_value: -5,
    sint64_value: -6n,
    fixed32_value: 7,
    fixed64_value: 8n,
    sfixed32_value: -9,
    sfixed64_value: -10n,
    float_value: 0.5,
    double_value: 0.25,
    string_value: 'all-set',
    bytes_value: new Uint8Array([0x0a, 0x0b, 0x0c, 0x0d]),
    optional_bool: true,
    optional_int32: null,
    optional_string: 'opt-str',
    optional_bytes: new Uint8Array([0x01, 0x02]),
    wrapped_bool: true,
    wrapped_int32: null,
    wrapped_int64: 123_456_789_012n,
    wrapped_uint32: 4_000_000_000,
    wrapped_uint64: 9_000_000_000_000_000_000n,
    wrapped_float: 1.5,
    wrapped_double: 2.5,
    wrapped_string: 'wrapped-str',
    wrapped_bytes: new Uint8Array([0x03, 0x04, 0x05]),
    timestamp_value: TIMESTAMP_NS,
    duration_value: DURATION_NS,
    status: TOP_ERROR,
    nested_status: INNER_DISABLED,
  };
}

// A default instance: every field at its proto3 default (C++'s `gen::ScalarCoverage{}`).
function scalarsDefault(): IScalarCoverage {
  return {
    bool_value: false,
    int32_value: 0,
    int64_value: 0n,
    uint32_value: 0,
    uint64_value: 0n,
    sint32_value: 0,
    sint64_value: 0n,
    fixed32_value: 0,
    fixed64_value: 0n,
    sfixed32_value: 0,
    sfixed64_value: 0n,
    float_value: 0,
    double_value: 0,
    string_value: '',
    bytes_value: new Uint8Array(),
    optional_bool: null,
    optional_int32: null,
    optional_string: null,
    optional_bytes: null,
    wrapped_bool: null,
    wrapped_int32: null,
    wrapped_int64: null,
    wrapped_uint32: null,
    wrapped_uint64: null,
    wrapped_float: null,
    wrapped_double: null,
    wrapped_string: null,
    wrapped_bytes: null,
    timestamp_value: 0n,
    duration_value: 0n,
    status: 0,
    nested_status: 0,
  };
}

function leaf(id: number, label: string, status: number): ILeaf {
  return { id, label, status };
}

function branch(): IBranch {
  return {
    leaf: leaf(1, 'root', TOP_OK),
    optional_leaf: leaf(2, 'opt', TOP_WARN),
    leaves: [leaf(3, 'a', TOP_OK), leaf(4, 'b', TOP_WARN)],
  };
}

function composite(): ICompositeCoverage {
  return {
    scalars: scalars(),
    optional_scalars: scalars(),
    branch: branch(),
    optional_branch: null,
    repeated_scalar: [10, 20, 30],
    repeated_string: ['x', 'y'],
    repeated_bytes: [],
    repeated_struct: [leaf(5, 's0', TOP_OK), leaf(6, 's1', TOP_WARN)],
    map_scalar: new Map([
      ['a', 1],
      ['b', 2],
    ]),
    map_struct: new Map([['k', leaf(7, 'mk', TOP_WARN)]]),
    flattened_struct_list: [leaf(8, 'f0', TOP_OK)],
    nested_struct_lists: [[leaf(9, 'n0', TOP_OK)], []],
    depth3_struct_lists: [[[leaf(10, 'd0', TOP_WARN)]]],
    optional_flattened_struct_list: [leaf(11, 'of', TOP_OK)],
    message_flattened_point: { x: 1.25, y: 2.5 },
    field_flattened_position: { x: 3.75, y: 4.5 },
  };
}

function compositeAlternate(): ICompositeCoverage {
  return {
    scalars: scalarsAllSet(),
    optional_scalars: null,
    branch: branch(),
    optional_branch: branch(),
    repeated_scalar: [],
    repeated_string: [],
    repeated_bytes: [new Uint8Array([0x09, 0x08]), new Uint8Array([0x07])],
    repeated_struct: [],
    map_scalar: new Map(),
    map_struct: new Map(),
    flattened_struct_list: [],
    nested_struct_lists: [],
    depth3_struct_lists: [],
    optional_flattened_struct_list: [leaf(30, 'alt', TOP_ERROR)],
    message_flattened_point: { x: 1.25, y: 2.5 },
    field_flattened_position: { x: 3.75, y: 4.5 },
  };
}

function compositeMapsNonSorted(): ICompositeCoverage {
  return {
    ...composite(),
    map_scalar: new Map([
      ['z', 26],
      ['a', 1],
      ['m', 13],
    ]),
    map_struct: new Map([
      ['y', leaf(25, 'y-leaf', TOP_WARN)],
      ['b', leaf(2, 'b-leaf', TOP_OK)],
    ]),
  };
}

// Nothing set: every plain message field a default instance (D-BIND-83), every optional
// message null, every container empty. C++'s `gen::CompositeCoverage{}`.
function compositeDefaulted(): ICompositeCoverage {
  return {
    scalars: scalarsDefault(),
    optional_scalars: null,
    branch: { leaf: leaf(0, '', 0), optional_leaf: null, leaves: [] },
    optional_branch: null,
    repeated_scalar: [],
    repeated_string: [],
    repeated_bytes: [],
    repeated_struct: [],
    map_scalar: new Map(),
    map_struct: new Map(),
    flattened_struct_list: [],
    nested_struct_lists: [],
    depth3_struct_lists: [],
    optional_flattened_struct_list: [],
    message_flattened_point: { x: 0, y: 0 },
    field_flattened_position: { x: 0, y: 0 },
  };
}

export function fixtureRow(seq: number): ICompositeCoverage {
  switch (seq) {
    case 0:
      return composite();
    case 1:
      return compositeAlternate();
    case 2:
      return compositeMapsNonSorted();
    default:
      return compositeDefaulted();
  }
}
