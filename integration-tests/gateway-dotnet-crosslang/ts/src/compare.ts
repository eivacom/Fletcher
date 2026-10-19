// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The TypeScript side's field-by-field comparison of a received CompositeCoverage with the
// fixture row it was built from (BIND-8d). It walks the decoded object, so every field of every
// message is named in a difference (`row.branch.leaves[1].label: expected "b" got "x"`). The
// C++ (row_compare.hpp) and C# (RowCompare.cs) peers compare the same fields.
import type { ICompositeCoverage } from '../generated-ts/coverage.fletcher.js';
import { ROW_COUNT, fixtureRow } from './fixture.js';

function show(v: unknown): string {
  if (v === null || v === undefined) return String(v);
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'bigint') return `${v}n`;
  if (v instanceof Uint8Array) {
    return '0x' + Array.from(v, (b) => b.toString(16).padStart(2, '0')).join('');
  }
  if (Array.isArray(v)) return `array(${v.length})`;
  if (v instanceof Map) return `map(${v.size})`;
  if (typeof v === 'object') return 'object';
  return String(v);
}

function walk(path: string, expected: unknown, actual: unknown, out: string[]): void {
  if (expected === null || actual === null || expected === undefined || actual === undefined) {
    if (expected !== actual) out.push(`${path}: expected ${show(expected)} got ${show(actual)}`);
    return;
  }
  if (expected instanceof Uint8Array) {
    const same =
      actual instanceof Uint8Array &&
      actual.length === expected.length &&
      expected.every((b, i) => actual[i] === b);
    if (!same) out.push(`${path}: expected ${show(expected)} got ${show(actual)}`);
    return;
  }
  if (Array.isArray(expected)) {
    if (!Array.isArray(actual)) {
      out.push(`${path}: expected ${show(expected)} got ${show(actual)}`);
      return;
    }
    if (expected.length !== actual.length) {
      out.push(`${path}.length: expected ${expected.length} got ${actual.length}`);
      return;
    }
    expected.forEach((e, i) => walk(`${path}[${i}]`, e, actual[i], out));
    return;
  }
  if (expected instanceof Map) {
    // In entry order: a map's order is part of what crosses (the maps-non-sorted row).
    if (!(actual instanceof Map)) {
      out.push(`${path}: expected ${show(expected)} got ${show(actual)}`);
      return;
    }
    if (expected.size !== actual.size) {
      out.push(`${path}.size: expected ${expected.size} got ${actual.size}`);
      return;
    }
    const e = [...expected.entries()];
    const a = [...actual.entries()];
    e.forEach(([k, v], i) => {
      walk(`${path}[${i}].key`, k, a[i][0], out);
      walk(`${path}[${i}].value`, v, a[i][1], out);
    });
    return;
  }
  if (typeof expected === 'object') {
    if (typeof actual !== 'object') {
      out.push(`${path}: expected ${show(expected)} got ${show(actual)}`);
      return;
    }
    const a = actual as Record<string, unknown>;
    for (const [key, value] of Object.entries(expected as Record<string, unknown>)) {
      walk(`${path}.${key}`, value, a[key], out);
    }
    return;
  }
  if (!Object.is(expected, actual)) {
    out.push(`${path}: expected ${show(expected)} got ${show(actual)}`);
  }
}

export function compareRow(expected: ICompositeCoverage, actual: ICompositeCoverage): string[] {
  const out: string[] = [];
  walk('row', expected, actual, out);
  return out;
}

/** `RECV <seq> OK`, `RECV <seq> MISMATCH <n>: <diffs>` or `RECV <seq> UNKNOWN`. */
export function reportRow(seq: number, actual: ICompositeCoverage): string {
  if (seq < 0 || seq >= ROW_COUNT) return `RECV ${seq} UNKNOWN`;
  const diffs = compareRow(fixtureRow(seq), actual);
  if (diffs.length === 0) return `RECV ${seq} OK`;
  return `RECV ${seq} MISMATCH ${diffs.length}:` + diffs.map((d) => ` ${d};`).join('');
}
