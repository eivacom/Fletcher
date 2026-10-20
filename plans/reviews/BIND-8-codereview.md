# BIND-8 — code review, all slices

**Subject:** what BIND-8 landed: the managed gateway client `Eiva.Fletcher.GatewayClient`
(`dotnet/src/Fletcher.GatewayClient`), the plugin's C# gateway pair (`csharp_gateway`), the
TypeScript backend changes (D-BIND-118, D-BIND-119), the gateway's `schemaIpc` and `FieldToJson`
changes, the tests, both live lanes, CI and records. Commits `ad4fb9c..89f9a34`.

**Reviewed at:** `89f9a34`. CI: `ci.pr` run 39248253992 green 56/56, every side workflow green.

**Outcome: 5 BLOCKERs, 12 questions for a ruling (including the 8 defaults chosen without one),
DEBT, NITs.** BIND-8 does not close until the blockers are fixed and the questions ruled.

> **CLOSED 2026-10-20, on CI at `750d79d` (`ci.pr` 39375700668, 56/56, every side workflow
> green).** All five blockers fixed and all twelve questions ruled and implemented (records
> `058755b`, fixes `cac7626`, green at `7e2acca`, `ci.pr` 39281811974), and the DEBT the maintainer
> chose fixed (the cheap fixes, client robustness, tests and AOT); C-D7 (performance), G-D2, G-D4,
> T-D4, T-D6, T-D7, N6, N9 and N14 are carried as KNOWN debt (audited below), listed in the tracker's
> BIND-8 section. Two things came out of the close itself: the maintainer's question why 8e deleted
> `DecodeBatch` showed the gateway client could not deliver batches as C++ and the native pair do,
> so `DecodeBatch` stays (D-BIND-128 amended) and slice 8f added `SubscribeBatchedAsync`
> (D-BIND-133, `7e2acca`); and verifying 8f turned a one-in-25 test failure into A1 below, two
> writes reaching the socket at once, fixed in `750d79d`.

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

> **FIXED 2026-10-19** with D-BIND-122: one namespace import per file, by a path relative to the
> importing file (`test_ts_visitor` `AnImportIsRelativeToTheImportingFile`: the same directory,
> a parent's and a sibling's; `OneNameInThreeFilesDoesNotClash`).

`protoc/src/ts_backend_visitor.cpp` writes `'./' + StripProtoSuffix(imported) + '.fletcher.js'`, but
the importing file is written at its own path (`a/user.fletcher.ts`). Any importer not at the root
imports a module that does not exist; the lane and `AnotherFilesMessageIsImportedByName` both put the
importer at the root, and the test pins the wrong path. Fix: the path relative to the importer's
directory, with cases for the same and a sibling directory. (Reviewer 2 B1; reviewer 3 Q2.)

### B2 — a cancelled `SubscribeAsync` still installs its handler, under an id the caller never gets

> **FIXED 2026-10-19:** a request the caller abandoned before its reply was read is not
> registered; the loop releases the subscription on the gateway, through the gate
> (`ACancelledSubscribeLeavesNoSubscriptionBehind`, a mutant that registers it anyway is caught).

Cancelled after the frame is sent, the call throws `OperationCanceledException`, and the receive loop
then registers the subscription when `subscribed` arrives: the handler fires for the life of the
connection and cannot be unsubscribed. In the no-schema branch the gateway-side release never runs.
Fix: a reply whose caller has gone is not registered and is unsubscribed through the gate. (Reviewer 1
B1; reviewer 3 Q3.)

### B3 — the cross-language lane's retry pools rows from both attempts

> **FIXED 2026-10-19:** each attempt keeps its own lines and rows and must deliver all four; and
> with D-BIND-129 the rows travel live after a probe row has arrived.

`seen` (C# → peer) and `rows` (peer → C#) span both attempts, so two partial deliveries pass as one
whole, weaker than the ruling ("retry the exchange once"). It happened on the green run. Fix: each
attempt must deliver all four rows on its own. (Reviewer 3 B3.)

### B4 — the `test_schema_codec` port tests code no shipped path runs

> **FIXED 2026-10-19** with D-BIND-128: the JSON-schema code and its tests are deleted; Bucket 2
> maps `test_schema_codec` to the client's `schemaIpc` reading back as the same declaration at
> every depth (`BuildCreateTopicAnnouncesTheSchemaAsSchemaIpcAndNotAsJson` over every `.ipc`
> golden, with `TheSchemaComparisonSeesNestedMetadataAndTimestampUnits` holding the comparison).
> `GatewayRowCodec.DecodeBatch` was deleted with it at first, then restored by the maintainer's
> amendment of D-BIND-128: it is row decoding, kept for a batched subscription (slice 8f).

Since D-BIND-112 the client announces `schemaIpc` only; `SchemaToJson` (with `WriteSchema`,
`WriteField`, the `WireTypes` table, `SubscribedReply.SchemaJson`) and `GatewayRowCodec.DecodeBatch`
are reached from tests alone, so the Bucket 2 "port" of `test_schema_codec` proves nothing about what
the client sends, and 8a's Done paragraph ("properties of the descriptor the client writes") is no
longer true. How to resolve it is Q8. (Reviewer 3 B1; reviewer 1 default (h).)

### B5 — `FletcherClientTests.ClosingClearsSubscriptions` cannot fail

> **FIXED 2026-10-19:** it reads the subscriptions the client holds (an internal count); a mutant
> that keeps them after the end is caught.

It disposes the client (the loop stops) and then delivers a row, which is never read whether or not
the subscriptions were cleared. (Reviewer 3 B2.)

---

## Found after the review

### A1 — two writes can reach the socket at once (since 8b)

> **FIXED 2026-10-20:** every write takes a second lock, `_write`: a request's send, and
> `DisposeAsync`'s close (bounded by the close grace). Three deterministic cases
> (`TheNextRequestIsNotSentWhileThePreviousSendIsStillReturning`,
> `DisposingWaitsForASendStillRunningBeforeClosing`,
> `DisposingDoesNotWaitForeverForASendThatNeverReturns`), the fake transport able to hold a send
> open after the gateway has answered and counting the close as a write; a mutant per lock caught.

`ClientWebSocket` allows one write at a time (`SendAsync` or `CloseOutputAsync`) and throws on a
second. The one-request gate (D-BIND-110) is released when a reply is READ, and a reply can be read
before its send has returned, so the next request's send could start over it; and `DisposeAsync`
closed without regard to a send in flight. Found while verifying 8f: `OneRequestIsInFlightAtATime`
failed about one run in 25 (two sends at once on the fake transport). Neither the reviewers nor
CI saw it; the client then failed the request and ended the connection.

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

> **AUDITED 2026-10-20 against the code at `e9f37fe`:** each item now has an ID and a state. Of 19
> DEBT items, 13 are FIXED and 6 OPEN; of 15 NITs, 12 are FIXED and 3 OPEN. The OPEN ones are the
> known debt BIND-8 carries, listed by ID in the tracker's BIND-8 section. "8e" = `cac7626`.

Client:

- **C-D1** a call during `ConnectAsync` ends the client for good. FIXED in 8e: the call is refused
  and the client stays usable.
- **C-D2** a locally ended connection never closes its socket. FIXED in 8e: the socket is aborted
  before `Completion` says so.
- **C-D3** `DisposeAsync` edge cases (exception filter, unbounded wait on the loop, a handler disposing
  deadlocks). FIXED in 8e (bounded, safe from a handler); its close no longer overlaps a send since A1
  (`750d79d`).
- **C-D4** the receive buffer grows without cap and never shrinks. FIXED in 8e: `MaxMessageBytes`
  (D-BIND-126), and the buffer shrinks back after a large message.
- **C-D5** silent drops counted nowhere. FIXED in 8e: `DroppedRows` (D-BIND-132).
- **C-D6** `Deliver` catches only `InvalidDataException`. FIXED in 8e: any decode failure is one row's.
- **C-D7** per-row allocation in decode and a codec planned per `PublishAsync`. **OPEN** (performance,
  carried by the maintainer's choice at 8e).
- **C-D8** the AOT claim is analyzer-deep. FIXED in 8e: `AotSmoke` NativeAOT-compiles the client and
  its generated pair (and, since 8f, a batched window).

Generator and gateway:

- **G-D1** gateway-pair names are not clash-checked as D-BIND-100 does the accessor's. FIXED in 8e:
  `CsGateway.TwoTypesOfOneNameAreRefusedBeforeAnyOutput`.
- **G-D2** no size cap on `schemaIpc`. **OPEN**: the gateway still decodes a `schemaIpc` of any size
  its WebSocket accepts.
- **G-D3** `"schemaIpc": null` refused. FIXED in 8e: read as absent.
- **G-D4** a struct-mapped WKT import would name a module TypeScript users do not generate (reported
  as uncertain). **OPEN, unverified**: no case exercises it yet; BIND-T is the natural place.

Tests and CI:

- **T-D1** the Windows jobs' filters miss `.github/actions/conan-install/**`. FIXED in 8e (`ci.pr`).
- **T-D2** schema metadata only lightly asserted. FIXED in 8e:
  `TheSchemaComparisonSeesNestedMetadataAndTimestampUnits` over every `.ipc` golden.
- **T-D3** the real WebSocket transport never sees a frame above 64 KiB. FIXED in 8e: a 100 KiB row in
  the end-to-end lane, in process (Fast DDS caps a sample at 64 KiB).
- **T-D4** the C++ comparer is hand-written, so a new field is skipped there. **OPEN**: still
  `integration-tests/gateway-dotnet-crosslang/cpp/row_compare.hpp`, field by field.
- **T-D5** the end-to-end lane discards the gateway's stderr. FIXED in 8e: forwarded.
- **T-D6** fixed ports and DDS domains. **OPEN**: ports 19194 and 19196 are fixed; the domains can be
  overridden by `DDS_DOMAIN_ID`.
- **T-D7** `ArrowSchemaToJsonTest` guards the old bug by stack layout only. **OPEN**:
  `AFieldWithMetadataButNoFieldNumberHasNone` still depends on the sibling's stack slot.

## NITs

- **N1** stale header comment in `ts_backend_visitor.hpp`. FIXED in 8e.
- **N2** base64 accepts non-canonical trailing bits though called strict. FIXED in 8e: the comment
  now says what holds (behaviour unchanged).
- **N3** the wire spec's `error` row assumes one request in flight. FIXED in 8e: the row says when a
  client can attribute it.
- **N4** a token cancelled between gate and send still sends. FIXED in 8e: nothing is sent.
- **N5** unobserved faulted tasks on abandoned calls. FIXED in 8e: observed.
- **N6** `NoSchemaException` loses its cause. **OPEN**: an `ArgumentException` from the gateway's
  schema reaches the caller as text only, not as an inner exception.
- **N7** `UnsubscribeAsync`'s doc overpromises. FIXED in 8e: it says a row already in its handler may
  still be there.
- **N8** `ConnectAsync` after dispose throws the wrong type. FIXED in 8e: `ObjectDisposedException`.
- **N9** undocumented `ObjectDisposedException`. **OPEN**: no `<exception>` entry for it on the
  client's methods.
- **N10** `SchemaJson` computed for nothing. FIXED: deleted with the JSON-schema code (D-BIND-128).
- **N11** the acceptance bullet's `SubscribeAsync<T>` wording is stale. FIXED in 8e: annotated in the
  tracker.
- **N12** the "own session" note on the Fast DDS retry is stale. FIXED in 8e.
- **N13** `OneRequestIsInFlightAtATime` proves less than its name. FIXED: its comment says what it
  proves (8e), and A1's `TheNextRequestIsNotSentWhileThePreviousSendIsStillReturning` holds the rest.
- **N14** `AssertAgree` never disposes a batch. **OPEN**: its input batch is still not disposed
  (`ManagedCodecAgreesWithNativeTests`).
- **N15** `WireTypeTests`' regex skips stacked cases. FIXED: the file is deleted (D-BIND-128).
