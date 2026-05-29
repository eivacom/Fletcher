// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Smoke test that the generated TypedSchema from protoc-gen-fletcher is
// well-formed at runtime. Exit code 0 = pass; anything else fails the CI
// step. Kept deliberately minimal so the test exercises the npm plumbing
// (install, prebuild hook, tsc) without depending on a running gateway.

import { Greeting, type IGreeting } from './generated/example.fletcher';

function fail(msg: string): never {
  console.error(`FAIL: ${msg}`);
  process.exit(1);
}

if (Greeting.protoPackage !== 'example') {
  fail(`Expected protoPackage='example', got '${Greeting.protoPackage}'`);
}
if (Greeting.protoMessage !== 'Greeting') {
  fail(`Expected protoMessage='Greeting', got '${Greeting.protoMessage}'`);
}
if (Greeting.fields.length !== 3) {
  fail(`Expected 3 fields in Greeting, got ${Greeting.fields.length}`);
}

// Type-level check: the IGreeting interface emitted by the plugin should
// have all three fields with their proto-mapped TypeScript types.
const msg: IGreeting = {
  name: 'world',
  priority: 42,
  tags: [1, 2, 3],
};

if (msg.name !== 'world' || msg.priority !== 42 || msg.tags.length !== 3) {
  fail(`IGreeting initializer round-trip failed: ${JSON.stringify(msg)}`);
}

console.log('OK: TypedSchema<IGreeting> generated and instantiated correctly');
console.log(`  protoPackage = ${Greeting.protoPackage}`);
console.log(`  protoMessage = ${Greeting.protoMessage}`);
console.log(`  fields       = ${Greeting.fields.map((f) => f.name).join(', ')}`);
