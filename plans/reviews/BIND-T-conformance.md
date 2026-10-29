# BIND-T — conformance against the acceptance and the rulings

Graded at `e577332` (plus T-5's README edits) by the fresh-context tests-and-acceptance reviewer
(T) of the BIND-T review (`BIND-T-codereview.md`), from the tree only; CI results are taken from the
records. MET, PARTLY, NOT MET.

## The acceptance (tracker, BIND-T)

| Bullet | Grade | Evidence or gap |
|---|---|---|
| Pair `<Svc>_<Method>Publisher`/`Subscriber` in `<stem>.fletcher.ts`, named as C++'s | MET | `TsPair.APubSubMethodGetsAPairNamedAsCpps` (C++ header and TypeScript of one file) |
| Thin wrappers; topic and `TypedSchema<T>` bound in; row type inferred | MET | `TsPair.ThePairBindsTheTopicAndTheSchema`; `pair.test.ts` and its three `@ts-expect-error` lines under `npm run typecheck` on both legs |
| Purely additive: `DescriptorByteIdentical` green, hand-wired use works, no new runtime dependency | PARTLY | the golden did not move, but its only method is unary, so no golden holds a file with pairs (T3); type-only import; a message named `FletcherClient` now breaks (T5); output needs client 0.6 (Q4) |
| Emitter cases in `test_ts_visitor.cpp`; `tsc` passes | MET | six `TsPair` cases; `protoc-gateway-client-ts` typecheck and crosslang `tsc` on CI; `gateway-end-to-end` not type-checked (T4) |
| Live coverage (D-BIND-184) | MET in code; CI pending | `protoc-gen.test.ts` 3 cases x 2 providers; crosslang peer on the four pairs; `e577332`'s run not yet recorded (T7) |
| Attachments and unsubscribe exposed | MET | `publish(row, attachments?)`, handlers get attachments, `unsubscribe(id)`; live and stub |
| A flatten-wrapper input follows C++ | MET | `TsPair.AWrapperOrRecursiveInputGetsCppsLineAndNoTopic`; step 7 names it (recursive missing, Q4) |
| Batched delivery (D-BIND-133 k) | MET | `batched.test.ts` (21) |
| The null refusal (D-BIND-156, TypeScript) | MET; map key open | `wire-encode.test.ts` (7); a null map key is written (Q5) |
| Full-fidelity declaration (D-BIND-130) | MET | `create` -> `createTopic(topic, schema)`; `pair.test.ts` asserts `schemaIpc` |

## The rulings

| Ruling | Grade | Evidence or gap |
|---|---|---|
| 181 the pair's shape | MET; one case unruled | the input with no carriable field gets `// No pair:` (Q3) |
| 182 wrapper (and recursive) inputs: C++'s line, no constant | MET | as above; the release note names wrappers only (Q4) |
| 183 `subscribeBatched`, `onError`, `undecodableRows`, as amended | MET | `MAX_BATCH_TIMEOUT_MS`; refusals; `console.error`; a throwing `onError`; an `async` handler's rejection is not caught (Q1); the deadline wording wrong in JSDoc and README (B2) |
| 184 the live lanes | MET | as above |
| 6, 7, 90 | MET with T3, T5 | |
| Test matrix, generator row | MET | 201 |
| Test matrix, bucket 2 `TS 38` | NOT UPDATED | 66 now (T6) |
