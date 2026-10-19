# BIND-8 — code review, all slices

**Subject:** what BIND-8 landed: the managed gateway client `Eiva.Fletcher.GatewayClient`
(`dotnet/src/Fletcher.GatewayClient`), the plugin's C# gateway pair (`csharp_gateway`), the
TypeScript backend changes (D-BIND-118, D-BIND-119), the gateway's `schemaIpc` and `FieldToJson`
changes, the tests, both live lanes, CI and records. Commits `ad4fb9c..89f9a34`.

**Reviewed at:** `89f9a34`. CI: `ci.pr` run 39248253992 green 56/56, every side workflow green.

**Outcome: 5 BLOCKERs, 12 questions for a ruling (including the 8 defaults chosen without one),
DEBT, NITs.** BIND-8 does not close until the blockers are fixed and the questions ruled.

> **ALL TWELVE QUESTIONS RULED 2026-10-19 (D-BIND-121 to 132):** Q1 `Completion` (121); Q2 one
> namespace import per file, path relative to the importer (122, amends 118); Q3 reply before keeping
> the subscription, tolerant `field_number` (123); Q4 an unparseable reply fails only its request
> (124); Q5 handlers stay synchronous, documented (125); Q6 `ConfigureWebSocket` and
> `MaxMessageBytes` (126); Q7 optional `ReplyTimeout` (127); Q8 delete the JSON-schema code, re-map
> Bucket 2, record exclusions (128); Q9 the lane waits for a probe row before publishing (129); Q10
> TypeScript `schemaIpc` pulled into 8e (130, amends 117); Q11 the C ABI codec gains the two
> refusals (131); Q12 the defaults locked, `HandlerFaulted.Reason` and `DroppedRows` (132). Left:
> fix B1 to B5, implement the rulings, close.

## How this review was done, and its limit

The method of BIND-4 to BIND-7. Three reviewers with **fresh context** each took one area: (1) the
managed client library; (2) the plugin's gateway pair, the TypeScript backend changes and the
gateway; (3) tests, CI and acceptance (graded in `BIND-8-conformance.md`). None had seen the session
that wrote the code; they read the locked decisions, the tracker, the protocol's authority
(`gateway/src/ws_session.*`), the TypeScript client and the code, and ran nothing.

**Every BLOCKER was then checked by the author**, never on the working tree:
- **B1** by generating `a/user.proto` importing `a/dep.proto` and `y/dep.proto` with the real
  plugin: `a/user.fletcher.ts` imports `'./a/dep.fletcher.js'` (resolves to `a/a/...`) and imports
  `Leaf` twice (Q2 in the same output).
- **B2** by reading `FletcherClient.cs`: the caller's token ends the wait at line 326; the receive
  loop still `Register`s the `subscribed` reply (410-448). A red test comes with the fix.
- **B3** in CI's own log (run 39248253992, Linux): "attempt 1: this client received rows 0,1 ... the
  retry passed"; `CrossLangTests` takes `seen` once and keeps one `rows` dictionary across attempts.
- **B4** by search: `ControlMessages.SchemaToJson` has no caller in `dotnet/src`.
- **B5** by reading the test: it disposes the client, then delivers.

The limit BIND-4 named remains: the reviewers are the same model as the author, briefed by the author.

---

## BLOCKERs

### B1 — TypeScript cross-file imports are relative to the include root, not to the importing file
`protoc/src/ts_backend_visitor.cpp` writes `'./' + StripProtoSuffix(imported) + '.fletcher.js'`, but
the importing file is written at its own path (`a/user.fletcher.ts`). Any importer not at the root
imports a module that does not exist; the lane and `AnotherFilesMessageIsImportedByName` both put the
importer at the root, and the test pins the wrong path. Fix: the path relative to the importer's
directory, with cases for the same and a sibling directory. (Reviewer 2 B1; reviewer 3 Q2.)

### B2 — a cancelled `SubscribeAsync` still installs its handler, under an id the caller never gets
Cancelled after the frame is sent, the call throws `OperationCanceledException`, and the receive loop
then registers the subscription when `subscribed` arrives: the handler fires for the life of the
connection and cannot be unsubscribed. In the no-schema branch the gateway-side release never runs.
Fix: a reply whose caller has gone is not registered and is unsubscribed through the gate. (Reviewer 1
B1; reviewer 3 Q3.)

### B3 — the cross-language lane's retry pools rows from both attempts
`seen` (C# → peer) and `rows` (peer → C#) span both attempts, so two partial deliveries pass as one
whole, weaker than the ruling ("retry the exchange once"). It happened on the green run. Fix: each
attempt must deliver all four rows on its own. (Reviewer 3 B3.)

### B4 — the `test_schema_codec` port tests code no shipped path runs
Since D-BIND-112 the client announces `schemaIpc` only; `SchemaToJson` (with `WriteSchema`,
`WriteField`, the `WireTypes` table, `SubscribedReply.SchemaJson`) and `GatewayRowCodec.DecodeBatch`
are reached from tests alone, so the Bucket 2 "port" of `test_schema_codec` proves nothing about what
the client sends, and 8a's Done paragraph ("properties of the descriptor the client writes") is no
longer true. How to resolve it is Q8. (Reviewer 3 B1; reviewer 1 default (h).)

### B5 — `FletcherClientTests.ClosingClearsSubscriptions` cannot fail
It disposes the client (the loop stops) and then delivers a row, which is never read whether or not
the subscriptions were cleared. (Reviewer 3 B2.)

---

## Questions for a ruling

1. **A disconnect signal** (reviewer 1 B2): when the gateway closes, every subscription is dropped
   silently; there is no `Completion` task, event or state. With no reconnect (default (f)) an app
   cannot even notice. Recommended: a public `Task Completion` carrying the reason.
2. **Imported TypeScript names can collide** (reviewer 2 Q1, verified): two files declaring `Leaf`, an
   imported name equal to a local one, or to `WireTypeId`/`TypedSchema`. Recommended: one namespace
   import per file (`import * as dep_fletcher from ...`).
3. **A `schemaIpc` with a non-numeric `field_number` breaks every `subscribe` to its topic** and leaks a
   gateway subscription per attempt (reviewer 2 Q2): the subscription is registered before the reply's
   `std::stoi` throws. Recommended: build the reply first, parse with `from_chars`, omit on failure.
4. **An unparseable text frame ends the connection** (reviewer 1 Q1), e.g. a `subscribed` schema
   Apache.Arrow cannot read, where TypeScript ignores it. Recommended: treat an unreadable `schemaIpc`
   as absent; an unknown reply fails only the request in flight.
5. **`async` lambdas as the handler are `async void`** (reviewer 1 Q2): a throw after the first await
   crashes the process, and the envelope is recycled. Recommended: document now; an async overload
   later if wanted.
6. **No socket configuration** (reviewer 1 Q3): headers/auth, proxy, certificates, keep-alive, size
   cap. Recommended: `Action<ClientWebSocketOptions>` and `MaxMessageBytes` on the options.
7. **No reply timeout** (reviewer 1 Q4): one lost reply wedges the client until disposed.
   Recommended: an optional timeout that ends the connection (with Q1's signal).
8. **The dead JSON-descriptor code and Bucket 2** (B4; reviewer 3 Q1): delete or move it to the tests,
   re-map `test_schema_codec` to what the client owes, and record the gateway-internal cases BIND-8
   added as exclusions.
9. **The cross-language lane routinely rides the Fast DDS defect path** (reviewer 3 Q4): every peer →
   C# row is a TRANSIENT_LOCAL backlog a new gateway reader must replay. Recommended after B3: wait for
   the match before `publish`, and report peer-side decode failures as `ERROR`.
10. **TypeScript and C# publishers of one topic conflict until BIND-T** (reviewer 2 Q3): say so in the
   spec and `CreateAsync`'s remarks.
11. **The C ABI codec is laxer than the managed one** (reviewer 1 D9): it encodes a null map key and a
   null dictionary entry the managed codec refuses. Recommended: bring the C ABI up to it.
12. **The eight defaults chosen without a ruling** (reviewer 1): (a) accept list = C ABI's — sound;
   (b) `ArgumentException`/`InvalidDataException` — sound; (c) null struct → null children — sound;
   (d) dictionary JSON wireType — moot (JSON unused); (e) string sub-ids — sound, the gateway requires
   it; (f) no reconnect — only with Q1's signal; (g) undecodable rows via `HandlerFaulted` — sound, but
   give the event a reason and count the other silent drops; (h) `SchemaToJson` dead — Q8.

## DEBT (reported, not verified unless marked)

- Client: a call during `ConnectAsync` ends the client for good; a locally ended connection never
  closes its socket; `DisposeAsync` edge cases (exception filter, unbounded wait on the loop, a handler
  disposing deadlocks); the receive buffer grows without cap and never shrinks; silent drops counted
  nowhere (early rows past the cap, unknown ids, rows after a failed unsubscribe); `Deliver` catches
  only `InvalidDataException`; per-row allocation in decode and a codec planned per `PublishAsync`;
  the AOT claim is analyzer-deep (AotSmoke does not link the gateway client).
- Generator and gateway: gateway-pair names are not clash-checked as D-BIND-100 does the accessor's;
  no size cap on `schemaIpc`; `"schemaIpc": null` refused; a struct-mapped WKT import would name a
  module TypeScript users do not generate (uncertain).
- Tests and CI: the Windows jobs' filters miss `.github/actions/conan-install/**`; schema metadata
  only lightly asserted; the real WebSocket transport never sees a frame above 64 KiB; the C++
  comparer is hand-written (a new field is skipped there); the end-to-end lane discards the gateway's
  stderr; fixed ports and DDS domains; `ArrowSchemaToJsonTest` guards the old bug by stack layout only.

## NITs

Stale header comment in `ts_backend_visitor.hpp`; base64 accepts non-canonical trailing bits though
called strict; the wire spec's `error` row assumes one request in flight; a token cancelled between
gate and send still sends; unobserved faulted tasks on abandoned calls; `NoSchemaException` loses its
cause; `UnsubscribeAsync`'s doc overpromises; `ConnectAsync` after dispose throws the wrong type;
undocumented `ObjectDisposedException`; `SchemaJson` computed for nothing; the acceptance bullet's
`SubscribeAsync<T>` wording and the "own session" note on the Fast DDS retry are stale;
`OneRequestIsInFlightAtATime` proves less than its name; `AssertAgree` never disposes a batch;
`WireTypeTests`' regex skips stacked cases.
