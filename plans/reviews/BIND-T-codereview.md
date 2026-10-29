# BIND-T — code review, all slices

**Subject:** what BIND-T landed: the rulings (D-BIND-181 to 184), the TypeScript client's null
refusal (T-1), `onError`, `undecodableRows` and `subscribeBatched` (T-2), the generated pair and
the flatten-wrapper skip (T-3), the pair through a live gateway (T-4), and T-5's README edits.
Commits `84afba2..e577332` and the uncommitted README edits of T-5.

**Reviewed at:** `e577332` plus the working tree. CI: `ci.pr` green 61/61 at `49b3b50`; `e577332`'s
run was in progress.

**Outcome: 2 BLOCKERs (shipped text), 5 questions for a ruling, DEBT, NITs.** BIND-T does not
close until the blockers are fixed and the questions ruled.

## How this review was done, and its limit

The method of BIND-4 to BIND-9b. Three reviewers with **fresh context** each took one area: (M) the
code and its machinery (the emitter, the client, the batcher, CMake and workflows); (L) what ships
and what it claims (the READMEs, the JSDoc, the comments generated into user code, what changes
for a 0.5.x user); (T) tests and acceptance (graded in `BIND-T-conformance.md`). None had seen the
session that wrote the work; they read the rulings, the tracker and the code and ran nothing. The
session then checked every BLOCKER and M1 against the tree before recording them (L1 by compiling
the README's example: `error TS18046: 'row' is of type 'unknown'`). IDs keep the reviewer's letter.

## BLOCKER

**B1 (L1) — the client README's hand-built `subscribe` example does not compile.**
`gateway-client-ts/README.md`, "By hand": with `schema: SchemaDescriptor` and an unannotated
callback, `row` is `unknown`, so `row.x` is TS18046 (checked). The repository's own tests pass the
row type (`subscribe<Row>(…)`, `integration-tests/gateway-end-to-end/test/end-to-end.test.ts:450`).
`client.ts`'s comment "Overload 2 — untyped schema, callback gets Record<string, unknown>" is wrong
too: it gets `unknown`.

**B2 (L2) — the batched deadline is described wrongly where users read it.** `client.ts`'s JSDoc
for `subscribeBatched` (it ships in the `.d.ts`) says `Timeout` is "counted from the window's first
row, so a row after a long quiet spell comes at once", which contradicts itself; the README says
"The deadline starts at a window's first row"; D-BIND-183 says "counted from the window's first
row". The code (`batcher.ts` `arm()`), C# (`GatewayBatch.cs`) and `batcher.ts`'s header agree: the
window's first row or drop ARMS the deadline, which falls `timeoutMs` after the PREVIOUS flush.

## QUESTION

**Q1 (M1) — an `async` handler's rejection escapes `onError`.** `client.ts` `runCallback` catches
only a synchronous throw; the handler types return `void`, so TypeScript accepts an `async`
function, and its rejection becomes an `unhandledRejection` (which ends a Node process by default),
while the generated subscriber's comment says a throwing callback is reported through `onError`.
Options: (a) when the callback returns a thenable, report its rejection as `HandlerThrew`; (b)
document "do not pass an async handler", as C# does (D-BIND-125). Reviewer: (a), three lines and a
test per path; C# can only document because `async void` cannot be observed.
**Ruled 2026-10-29: (a), D-BIND-185.**

**Q2 (M2) — `unsubscribe` stops delivery later than C#'s does.** C#'s `UnsubscribeAsync` removes the
subscription before it sends the request (`FletcherClient.cs:308`), so its handler is not called
again even if the request fails. TypeScript keeps it until the `unsubscribed` reply: rows arriving
meanwhile still reach a per-row handler, and if the reply is an error or the socket is gone the
subscription stays (a per-row one keeps delivering; a batched one decodes and drops every row, its
batcher closed). Options: (a) as C#, remove it and close the batcher before sending (a behaviour
change for per-row users, in the release notes); (b) keep it. Reviewer: (a).
**Ruled 2026-10-29: (a), D-BIND-186.**

**Q3 (T1) — a pub/sub method whose input has no field TypeScript can carry has no ruling.** As
built (T-3) TypeScript keeps the topic constant and writes `// No pair: Svc.Method — input message
'M' has no field TypeScript can carry`; C++ writes a pair over an empty row. Options: (a) accept and
record it under D-BIND-181; (b) as (a) and drop the constant too, as D-BIND-182 does; (c) emit an
empty interface and `TypedSchema` at the end of the file so the pair exists. Reviewer: (a), ruled.
**Ruled 2026-10-29: (a), D-BIND-187.**

**Q4 (L3, L4, L5, M4) — what changes for a 0.5.x TypeScript user is not stated where they read.**
Runbook step 7 plans Release notes for `protoc` and `dotnet` only. For `gateway-client`:
`publish` and `encodePositional` now throw on a null or absent non-nullable field (every non-
`optional` field in a generated proto3 schema); a throwing callback or an undecodable row no longer
escapes as an uncaught exception. For `protoc`: D-BIND-182 also drops the constant for a RECURSIVE
input (built so, ruled so, but step 7 names wrappers only); and a generated `.fletcher.ts` with a
pub/sub method now imports `BatchCallback`, `BatchOptions` and `FletcherClient`, so it does not
compile against client 0.5.x (TS2305). No README says plugin and client must share MAJOR.MINOR,
and `protoc/npm/README.md` (in the npm package) still describes the old output. Options: (a) step 7
gains a `gateway-client` paragraph and the two `protoc` points, and both npm READMEs gain the pair
and a "use the same MAJOR.MINOR of plugin and client" line; (b) leave it to the CD Release text.
Reviewers: (a).
**Ruled 2026-10-29: (a), D-BIND-188.**

**Q5 (M3, T8) — a null map key is written, not refused.** `positional-encoder.ts` writes a `null`
string key as the text `"null"` and a `null` int key as 0; C++ and C# refuse both (D-BIND-131,
under D-BIND-156's scope). T-1 recorded it as left for later; no item owns it. Options: (a) fix in
BIND-T (one line and a test); (b) carry it as debt. Reviewers: (a).
**Ruled 2026-10-29: (a), D-BIND-189.**

## DEBT

- **T3 — no byte-identity guard for a file with a pub/sub method.** `TsVisitor.DescriptorByteIdentical`
  (protoc-coverage) has only a unary method; the ordering test would not catch a change to the
  bytes before the pairs. Commit the Feed fixture's whole output as a golden, or give a coverage
  proto a stream method.
- **T4, M5 — `gateway-end-to-end`'s TypeScript is never type-checked,** now that it drives the
  typed pair; its `typecheck` fails on the two faults T-3 fixed elsewhere (`rootDir`, `Dirent`).
  Fix both and add the step to the lane's workflow.
- **T5, M8 — a message named `FletcherClient` breaks a file that compiled** (TS2440 beside the new
  `import type`); `BatchOptions`/`BatchCallback` merge. Import under aliases, or refuse the names.
- **T6 — bucket 2's `TS 38` is stale:** `gateway-client-ts` has 66 cases; the 28 new ones mirror C#.
- **T7 — T-4 has no CI record yet.**
- **T2, L4(a) — step 7 names wrappers only** (part of Q4).
- **L6 — the README says the batch rules are C#'s** without the three differences (rows delivered,
  not received; no byte ceiling; 24.8 days).
- **L7 — `protoc/README.md` omits the `// No pair:` outcome** (after Q3).
- **L8 — `gateway-end-to-end/README.md:50,80` call the output "the `TelemetrySchema` class".**
- **M6 — the pair's `static readonly topic`/`schema` are unannotated** (TS9012 under
  `isolatedDeclarations`; every other emitted declaration is annotated).
- **M7 — the pair import is emitted when the only method gets `// No pair:`** (TS6192 under
  `noUnusedLocals`).
- **M9 — an old socket's late `onclose` can close a reconnected client's batched subscriptions**
  (and, before BIND-T already, null its socket). Guard on the socket.
- **M10 — the well-known-types CMake block is now copied three times; rows after a batcher closes
  are not counted** (C# counts them in `DroppedRows`).

## NIT

- **L9** `undecodableRows` is a getter in the "Method" table.
- **L10** the README names only the upper `timeoutMs` bound; negative, `NaN` and `maxRows` < 1 not said.
- **L11** the generated-code example does not show `positions.proto` and `Position`.
- **L12** the null paragraph could say a repeated or map field must be `[]` or `new Map()`, never
  absent; one line is far past the README's width.
- **L13** the generated subscriber comment should add "or `console.error` without one".
- **T9** the live unsubscribe check proves the client drops the subscription, not that the gateway
  ended it (the client ignores unknown ids).
- **T10** untested: a batch handler calling `unsubscribe`/`close` (safe by reading), a row during an
  in-flight unsubscribe, an `unsubscribe` answered with an error (Q2), a row whose envelope parses
  but whose row does not, `timeoutMs: 0`, a live connection drop under a pending window.
- **T11** the wrapper skip case does not assert the wrapper's own interface and schema remain.
- **T12** nothing shows `publish` sends nothing when the encoder refuses (true by code order).

## Checked and sound

The emitter's message set is C++'s and C#'s exactly; `OrderedMessages` already drops recursive
messages from emission, so the only changed existing output is D-BIND-182's; the pairs and their
type-only import are appended (D-BIND-7); cross-file inputs are skipped by `ValidateServiceMethod`
in all three languages; nested names, two services and a service without pub/sub methods emit
correctly. The batcher follows `GatewayBatch.cs` rule by rule (armed by the first event, anchored to
the previous flush, an overdue deadline flushed inline, `RowLimit` before `Timeout`, an only-drops
window delivered, the timer bound to its window, `Closing` before the request), with the amended
differences; reentrant `unsubscribe`/`close` from a handler meets only no-op paths. The null refusal
matches C#'s paths and its parent rule, and ignoring the element placeholders matches the native
schema. Tests: `batched.test.ts` is deterministic (fake timers incl. `performance`, both sides of
every boundary); the `TsPair` fixture runs the real generator for C++ and TypeScript on one pool;
`pair.test.ts` round-trips the published frame through the subscriber and its `@ts-expect-error`
lines were mutation-checked; the live cases use one topic each, `Infinity` for `RowLimit`, accept
any split for `Timeout`, and cannot pass empty. The crosslang peer keeps its `ERROR` line through
`onError`. The matrix's generator total 201 is right.
