# D10C pre-round audit: consistency (Gate 4) and bounded-context census (Gate 4b)

Written by `round-designer` on 2026-11-15 for the C# switch round **D10C** (creates domain 10, the C#
binding), after the owner rulings of 2026-11-15 were recorded ([RIX-rulings.md](RIX-rulings.md) :536,
:551, :566) and domain 10 was specified. Tree: `a29758a` (branch `feature/csharp-switch`), no edits
except this file.

**Round purpose (owner's terms, ledger :566):** "A C# app gets its batches from Fletcher's one
batcher, the C++ one, and C#'s own copy is gone, while an app that does not need batches still runs on
the small, Arrow-free shim."

**Rulings ledger:** `plans/RIX-rulings.md`, **573 lines**; last heading (line 566) **"## 2026-11-15 —
D10C: domain 10 confirmed; the round's purpose"**. Read in full. (The brief's ":564" is inside the
:551 entry; the confirmation heading is :566.) `docs/domains/d10-csharp-binding.md` at HEAD differs
from the confirmed `74bf308` only in its status header (`git diff 74bf308 HEAD`).

**Corpus audited:** `docs/domains/d10-csharp-binding.md` (§1–§8, Findings; L1–L15, D1–D27); the
ledger; d9 and d7 where d10 cites them (d9 §4, §6, §7 Q5a/Q9/Q11, L2, L14, L18; d7 L5, L7–L9, L12,
L15, §6); `docs/domains/bounded-context-register.md` (BC-1 to BC-15); `plans/BIND-locked-decisions.md`
(D-BIND-1, -1a, -2a, -8, -13, -17 to -20, -24, -25, -27, -57 to -59, -63 to -68, -74, -79, -81, -120,
-133, -134, -141, -142, -144, -146 to -149, -155, -157, -172) and `plans/BIND-csharp-bindings.md`
(Decision table, Part 2, BIND-5), searched not read whole; `dotnet/README.md`,
`dotnet/README.package.md`, `dotnet/PREVIEW-TESTERS.md`; doc comments of `SubscriberArrow.cs`,
`FletcherException.cs`, `NativeLoader.cs`, `Fletcher.Interop.csproj`; the shim's edge refusal
(`c-abi/src/binding.cpp:228-238`) and `binding.h:1013-1015, 1098-1101, 176-179, 264-269`.

**Conventions.** Verdicts: OK; FINDING (no governing law, an inexact citation, or tier-3 text the round
owes); **BLOCKER** (a tier-1 or tier-2 clause, not annotated, that contradicts a law: a designer or
test-authority step reading it as live would build or keep what a law forbids). Tier-3 text (code, doc
comments, READMEs) that contradicts a law is a FINDING "owed by the round", as in the D9S audit, since
tier 3 always complies. Laws are d10's unless prefixed. "Same" repeats a numbered finding.

**Totals:** **145 rows** (A: 121 clause rows, B: 24 pair rows): **86 OK, 54 FINDING, 5 BLOCKER**. The
five BLOCKER rows are **one blocker, B1** (un-annotated BIND oracle text that prescribes the managed
batcher and per-row-as-a-batch-of-one). Distinct findings F1–F27. **Six tier-1 collisions for the
owner (R-1 to R-6).** Test corpus (part C of this file's scheme, "Part B" of the brief): **6 DELETE,
38 RE-ANCHOR** (34 in `SubscriberArrowTests`, of which 12 re-anchor without an edit), **d10 Q9 port
list: 4 cases**. Census: 18 contexts; 2 new register rows proposed (BC-16, BC-17), 7 rows amended.

Verified while auditing: `binding.h` has 50 `FL_ABI_EXPORT` declarations and C# imports 47 distinct
entry points, all of them in the header (d10 L2 holds); `NativeLoader.HeaderVersionMinor = 8`
(`NativeLoader.cs:34`); the shim's edge refusal message names the file `fletcher-c-abi-server`, not a
.NET package (`binding.cpp:233-236`); `SubscriberArrowTests` holds 41 `[Fact]`s, none skipped; no test
in `dotnet/tests` touches `SubscriberArrow.Dispose`'s refusal; `scripts/check_dotnet_packages.sh`
enforces "only Interop" at `:113` and lists four packages at `:74`.

## A. Clauses against the laws (Gate 4 steps 1–3)

### d10 §1–§2

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A1 | §1 Owns `Eiva.Fletcher.Interop`: "the native asset it packs (the edge shim, D-BIND-13 as revised by d9 D22)" | L15, :337 (1) | OK |
| A2 | §1 Owns the server package; "the loader prefers the server shim when it is present" | L5, :363 (4), :536 (2) | **FINDING → R-1**: "present" and "references" are two different facts (see LL/R-1) |
| A3 | §1 "Mapping each C ABI answer onto a C# idiom at one site" | L10, L12 | OK |
| A4 | §1 Refuses batch assembly; the managed `Batcher` stays as it is until deleted, no interim fixes | L4, :203 (1)(3), :278 | OK |
| A5 | §1 Refuses answering a lower layer's question; "the exceptions are ruled ones only" | L3 | OK |
| A6 | §1 Refuses shipping batched subscribe to `main` before the switch (PR #129) | :278 | OK |
| A7 | §2 Base package, server package, edge/server shim file names | L5, L15, :395 | OK |
| A8 | §2 Per-row Arrow subscribe: "after the switch the native per-row path on both packages" | L7, :536 (1) | OK |
| A9 | §2 Naming finding: `FletcherStatus.NotSupported` widened by D28 | L2, :363 (3) | OK (F17 owed) |
| A10 | §2 Batch handler `RecordBatchHandler(RecordBatch?, attachments, BatchStatus)` kept | L6, D25 | OK |

### d10 §3 (the laws' operational clauses)

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A11 | L1 exact-minor handshake; `HeaderVersionMinor` 8 | L1, :363 (2) | OK (verified `NativeLoader.cs:34`) |
| A12 | L2 "Every number the shim hands out (`fl_status`, `fl_origin`, `FL_BATCH_*`, `fl_variant`) has a C# value equal to it, never renumbered"; mark "built for fl_status and fl_origin 0-3; violated on 4" | L2 | **FINDING F2**: §4 records `BatchReason {RowLimit, Timeout, Closing}` as "0-based; the C values are 1-3" (`SubscriberArrow.cs:116-126`, `h:1013-1015`). That is a second L2 violation the mark omits. The switch must renumber `BatchReason` to 1..3 (a visible change: cast or persisted values shift by one, `default(BatchReason)` becomes unnamed; cheap now because the batched API has not reached `main`, D4) or the owner exempts it. Record it as a visible C# change. |
| A13 | L2 "47 of the 50 entry points" imported | L2 | OK (verified) |
| A14 | L2 the switch adds `FletcherOrigin.Handover = 4` | L2, :551 (3) | OK |
| A15 | L3 accepted duplications: topic segments and selector checked in managed code and natively | L3, D-BIND-19 (4), D-BIND-20 | OK |
| A16 | L3 "Settled: the exception is domain 7's refusal passed through, `FletcherException(InvalidArgument)`; the `MaxTimeout` cap goes" | L3, :536 (3) vs :235 | **FINDING → R-4**: :235's "C# throws as it does today" (today: `ArgumentOutOfRangeException`, an `ArgumentException`) and :226's "C# keeps its current behaviour" are not named by :536 (3). `FletcherException` derives from `Exception` only (`FletcherException.cs:79`). |
| A17 | L4 structural test proposed (source scan for `Timer`, `Stopwatch` deadlines, a row buffer) | L4, BC-9 | OK |
| A18 | L4 "deleted only after its tests are checked for cases domain 7's suite lacks" | :203 | OK (part C, Q9) |
| A19 | L5 "'References the server package' therefore means 'the server shim is in the application's output'" | L5, :363 (4), :536 (2) | **FINDING → R-1** |
| A20 | L5 CI staging under the edge file name retires when the loader lands | :525, :536 (2) | OK |
| A21 | L6 "a named not-supported exception that names the server package" | L3, L6, :551 (1) | **FINDING F19 → R-2**: the shim's message names the *file* `fletcher-c-abi-server` (`binding.cpp:233-236`). Naming the NuGet package means C# adds text (L3: "verbatim message") or refuses by itself (L3: a second answer to BC-10). |
| A22 | L6 "Whether C# also exposes the loaded variant as a public property is the round's design" | L3, BC-10 | OK. Design note: exposing the variant is lawful; refusing from it in managed code is not (census D2). |
| A23 | L7 "what it reports for a row it cannot decode follows domain 7's meanings (L14) and is the round's design" | L7, L9, :246 (2), :536 (1) | **FINDING F24 → R-3**: domain 7 has two meanings here (the batched path counts every row, BC-2; C++'s per-row `Subscribe` drops silently, BC-2's registered gap). :246 (2) excused that gap for C# only because "C#'s per-row `Subscribe` is a batched one"; :536 (1) removed the premise. |
| A24 | L8 move before the handler; never release what was not moved | L8, :363 (1) | OK |
| A25 | L8 source "(ledger :512): `failure` is copied to keep, never disposed" | step 5 | **FINDING F6**: :512 says "borrowed for the callback"; "copied, never disposed" is d9 D32 / d9 L2. Cite d9 D32. |
| A26 | L9 settled hand-over: null batch, rows counted, reason, failure in `BatchStatus` | :551 (3), :383 (1), :512 | OK |
| A27 | L9 tests | L9 | OK (`AHandlerThatThrowsIsAbsorbedCountedAndRaised` exists) |
| A28 | L10 one containment site per direction; the batch thunk in the reflection test's scope | L10, D-BIND-19 (2) | OK |
| A29 | L11 "Tests: `ReentrancyTests.*`, `CallerTierArmTests.*`" for disposing a `SubscriberArrow` | L11, :174 | **FINDING F3**: neither file references `SubscriberArrow`; the refusal at `SA:310-316` has no test, and the served case (dispose from a batch callback) is untestable until the switch. |
| A30 | L12 "`Timeout.InfiniteTimeSpan` (−1) becomes `INT64_MAX`; any other negative is refused" at "one site, before the call" | L3, D22, :536 (3) | **FINDING F1**: read with "before the call", this is a managed refusal, which D22 forbids for batch options; the only incumbent site (`SchemaArrival.ToMilliseconds`, `SchemaArrival.cs:216-232`) refuses negatives in managed code and truncates toward zero. Suggested L12 text (tier 2, owner approval): "any other negative is refused: by the callee for batch options (L3, D22), in managed code and again natively for the arrival (D-BIND-20); a negative is never rounded to zero". |
| A31 | L12 "a finite timeout crosses as given and domain 7 decides" | :536 (3), d7 L5, L15 | OK |
| A32 | L13 one at a time, window order, batch thread | L13, :551 (4) | OK |
| A33 | L14 divergences V1–V14 | L14 | OK (V13 ruled by :246 (2)) |
| A34 | L15 holds-at `check_dotnet_packages.sh :12-13`; Findings (a)1 ":108" | L15 | **FINDING F23** (minor): `:12-13` is the header comment; the refusal is `:113`, the expected list `:108`. |
| A35 | L15 / Q7 default for D-BIND-148, -149 and the server ceiling | L15, D-BIND-148/149/157, :566 | OK |

### d10 §4–§8

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A36 | §4 "What the switch adds": `MaxRows` int64 unchanged, `Timeout` through the one site, refusals passed through, `MaxTimeout` removed | L3, L12, D22 | OK (but see F1 for the site) |
| A37 | §4 "One `fl_subscriber` holds both a `SubscriberArrow`'s row and batched subscriptions; a same-topic mix through one provider collides (d9 D33, ledger :448 (1)); the C# docs say so" | L9 vs :448 (1) | **FINDING F9 → R-6**: on `inprocess` the later subscription silently replaces the earlier (`binding.h` "SAME TOPIC, ONE PROVIDER"). Today a `SubscriberArrow` holding both kinds on one topic delivers to both (one inner `Subscriber`). The switch turns that into a silent loss; L9 says "Nothing is lost … silently". |
| A38 | §4 `Unsubscribe` from outside blocks until the closing batch; from inside it does not wait | L13, d7 L9, :174, :484 | OK |
| A39 | §5 D3 source "Ledger :203 (3); revised in place by :278" | step 5 | **FINDING F4**: :278 revised :258 (2) ("keeping it compliant") in favour of :203 (3); it did not revise :203 (3). |
| A40 | §5 D8 "BC-2's per-row gap and BC-7 are not prerequisites of the switch" | :246 (2), :536 (1) | **FINDING F7**: D8 keeps a conclusion whose stated premise :536 (1) amended; register BC-2's status cites the same premise. → R-3 |
| A41 | §5 D19 "the D-BIND-25, -63, -66 and -68 annotations are still owed by the round" | L4 | OK as a statement; the owed annotations are B1 |
| A42 | §5 D22 "Resolves former Q4, d9 R-4 and the broad reading of :226" | step 5 | **FINDING F5**: :536's text names D-BIND-19 rule 4 / D-BIND-20, not :226 or :235. → R-4 |
| A43 | §5 D24 "A batched dictionary column reaches C# re-folded"; §6 "Per-row decode gives a dictionary's value type" | :551 (2) vs :536 (1) | **FINDING → R-5**: one `SubscriberArrow` will deliver one column as two Arrow types depending on `Subscribe` or `SubscribeBatched`. |
| A44 | §7 Q7 default | :566 | OK |
| A45 | §7 Q8 default (`fl_decode_rows_framed` stays) | :566 | OK |
| A46 | §7 Q11 (a C# table test against `binding.h`) | L2 | OK (extend to `BatchReason`, `fl_variant`; F2) |
| A47 | §8 rule 6, the managed-batcher test sort | L13, L14, D24, :484 | **FINDING F8**: (a) `AFixedSizeListArrivesNonNull` is listed as a case d7 may lack; d7 has `FixedSizeListWithNamedItemArrivesNonNull`. (b) `ADictionaryColumnKeepsItsNulls` and `BatchesAreWellFormed` are listed "kept" but each asserts the dictionary column as `StringArray` (forbidden by D24). (c) `OnTheDefaultInProcessLoopback…DropsAndCountsEveryRow` is listed "retire"; its assertions are exactly d7's `SchemaDeclaredAfterSubscribeDropsReportedAtUnsubscribe` outcome (null `Closing`, all rows dropped). (d) `ATimerHandlerThatCancelsDuringADeliveryDoesNotDeadlock` is listed "retire"; its assertions hold under d7. (e) `TopicOptionsReachTheProviderThroughBothForms` is missing: its batched half passes on the edge build's NotSupported, whatever happens to the options (tolerance). Part C has the corrected sort. |
| A48 | §8 "A case domain 7 lacks is ported to domain 7 by a domain-7 round, or the owner rules it dropped; it is never kept alive in C#" | :203 | OK |

### `plans/BIND-locked-decisions.md` (tier 2)

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A49 | D-BIND-1 one codec, native | D18 | OK |
| A50 | D-BIND-1a rider i (revised 2026-11-15) | :536 (4), D23 | OK |
| A51 | D-BIND-1a rider ii (annotated 2026-11-13) | L15 | OK |
| A52 | D-BIND-2a "everything the delivery callback receives is BORROWED"; one callback at a time; no thread affinity; a null schema is a valid mode | L8, L13 | OK. A batch is none of a row delivery's three arguments; :363 (1) governs it. |
| A53 | D-BIND-8 (revised 2026-11-15): batched dictionary re-folded; "per-row decode still gives the value type" | :551 (2), :536 (1) | OK as text; the consequence is R-5 |
| A54 | D-BIND-13 (annotated 2026-11-13): two packages carry native assets | L15 | OK (its CI check still enforces the old text: part C) |
| A55 | D-BIND-17 (annotated) | L5 | OK |
| A56 | D-BIND-18 thunk bookkeeping; refusals; per-instance count | L9, L11 | OK |
| A57 | D-BIND-19 rules 1–3, 5 | L3, L9, L10 | OK |
| A58 | D-BIND-19 rule 4 (revised 2026-11-15) | :536 (3) | OK |
| A59 | D-BIND-20 (revised): "for batch options the timeout is not pre-checked … any other value goes to domain 7" | L12, D22 | OK (and it shows the L12 wording gap, F1) |
| A60 | D-BIND-24 no managed provider | D18 | OK |
| A61 | D-BIND-25 "per-row Arrow delivery is a one-row batch (`BatchOptions.MaxRows = 1`)", **not annotated** | L7, :536 (1) | **BLOCKER B1** |
| A62 | D-BIND-27 RIDs | L15 | OK |
| A63 | D-BIND-57 `TopicOptions` forwarded on both forms | L6 (the batched form exists on every package) | OK |
| A64 | D-BIND-58 "`SubscriberArrow` copies borrowed rows by design, so no zero-copy claim covers it" | L8 | OK (after the switch the batched path moves instead of copying; the claim still holds for per-row) |
| A65 | D-BIND-59 (revised 2026-11-15) | :536 (1), :551 (2)(4) | OK |
| A66 | D-BIND-63 "`SubscriberArrow`'s window has C++'s ceiling … `DefaultWindowByteCeiling` … an internal constructor takes a lower one", **not annotated** | L4, :203 (1) | **BLOCKER B1** |
| A67 | D-BIND-64 (revised 2026-11-15) | :551 (4) | OK |
| A68 | D-BIND-66 "every row `SubscriberArrow` receives either arrives in a batch or is counted … its intake never lets an exception reach the inner `Subscriber`", **not annotated** | L4 (the intake is the managed batcher); L9 / R-3 (its per-row half) | **BLOCKER B1** for the batched half; its per-row half is live until R-3 is ruled |
| A69 | D-BIND-68 "Scope: … `SubscriberArrow` uses it, so the per-row fallback pass goes away"; "B3 on top of it: a `RowLimit` flush that finds bad rows puts the good ones back", **not annotated** | L4 | **BLOCKER B1** (the entry point itself is Q8's, default: stays) |
| A70 | D-BIND-74 the `Model` package | §1 | OK |
| A71 | D-BIND-79 the generated subscriber takes a `RowHandler`-shaped delegate | D25, §6 | OK (unaffected by the switch) |
| A72 | D-BIND-81 AOT-compatible | L10 | OK (the batch thunk is `[UnmanagedCallersOnly]`) |
| A73 | D-BIND-120 GatewayClient takes no native reference | L15 | OK |
| A74 | D-BIND-133 the gateway client batches on its own | domain 5 (BC-9 deferred) | OK, out of scope |
| A75 | D-BIND-134 "the first preview carries all four packages … The shim is the only unmanaged file, and it is self-contained" | L15, D21 | **FINDING F12**: historical (first preview), but the packed-package check copies its "four" (`check_dotnet_packages.sh:74`); the server package makes five, and its self-containment (Arrow C++ static) is the packaging stage's to measure (Q7). |
| A76 | D-BIND-141, -172 floors stated in the README and the loader's message | L15, Q7 | OK (the server shim per Q7's default) |
| A77 | D-BIND-142, -144, -147 every README block is a snippet checked against the consumer | L6, D25 | **FINDING F27**: the `native-batch` snippet (`README.package.md:155-178`, consumer `PreviewConsumer/Program.cs:133-157`) references only the base package; after the switch it throws NotSupported unless the consumer references the server package. |
| A78 | D-BIND-146 "`SubscriberArrow` drops and counts every row" for a subscription served without a schema, **not annotated** | L14 V4, :551 (4) | **FINDING F11**: not a contradiction (on `as_declared` no schema ever arrives, and d7 L8 drops and counts too), but the mechanism it describes, C#'s latch, is revised by :551 (4). Annotate: "the outcome is now domain 7's". |
| A79 | D-BIND-148 exact Windows DLL allow-list | L15, Q7 | OK (default: same list, a breach fails by name and needs a ruling, as D-BIND-148 itself says) |
| A80 | D-BIND-149 linux floors equal the stated ones | L15, Q7 | OK |
| A81 | D-BIND-155 dynamic CRT | D18 | OK |
| A82 | D-BIND-157 (annotated: kept for the base package) | L15 | OK |

### `plans/BIND-csharp-bindings.md` (tier 2)

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A83 | Decision row 5: `Apache.Arrow` a base dependency; collapse `Core` + `Arrow` | :536 (4) | OK (the split is now native, by package; the managed collapse stands) |
| A84 | Decision row 6 (annotated 2026-11-13) | L15 | OK |
| A85 | Part 2 tree "`Interop` … the ONLY runtimes/", the package table, "`Interop` is the *only* package carrying native assets" | L15 | OK (the paragraph's 2026-11-13 annotation covers the section) |
| A86 | BIND-5 acceptance: "per-row Arrow delivery is `MaxRows = 1`"; "Dictionary re-folding deferred to DICT"; "Handler calls are not serialised (D-BIND-64)"; the D-BIND-63 ceiling and D-BIND-68 put-back bullets, **not annotated** | L4, L7, L13, D24 | **BLOCKER B1** |

### `dotnet/` READMEs (tier 3)

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A87 | `README.md:12-13`, `:22-24` "`Interop` is the only package carrying native assets" | L15, D21 | **FINDING F15** (owed) |
| A88 | `README.md:147-158` "no second `fletcher-c-abi` loaded beside the one `Eiva.Fletcher.Interop` brings" | L5, d9 L17 | **FINDING F15** (owed: the server package, and the loader's choice) |
| A89 | `README.package.md:150-153` "its calls are not serialised … two may run at once and complete out of order" | L13 | **FINDING F15** (owed; V1) |
| A90 | `README.package.md:148-150` subscribe-first over default `inprocess`: "every row on it is dropped and counted" | L14, d7 L8 | OK (the outcome holds under domain 7) |
| A91 | `README.package.md:155-178` `native-batch` snippet on the base package | L6, D25 | Same as F27 |
| A92 | `README.package.md:200-201` gateway batches in order, never concurrent | domain 5 | OK, out of scope |
| A93 | `README.package.md:225-229` "`Interop` carries one native file per platform … nanoarrow linked into it" | L15 | **FINDING F15** (owed: the server package's section) |
| A94 | `README.package.md:248-250` the handshake | L1 | OK |
| A95 | `README.package.md:257-285` relinking route builds the edge shim only | L15 | **FINDING F15** (owed: the server shim's route) |
| A96 | `PREVIEW-TESTERS.md:10` "`Eiva.Fletcher.Interop` (with the native library …)" | L15 | **FINDING F15** (minor) |

### Doc comments on the public C# surface (tier 3)

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A97 | `SA:4-94` header (per-row is `MaxRows = 1`; ceiling; framed decode; D-BIND-64 threads; dictionaries as value type) | L4, L7, L13, D24 | **FINDING F16** (deleted with the batcher) |
| A98 | `SA:106-110` "NULL only when the topic's schema could not be opened … for the life of the subscription" | L14 V11, :551 (3)(4) | **FINDING F16** |
| A99 | `SA:147` "Values below 1 mean 1" | L3, D6, :235 | **FINDING F16** |
| A100 | `SA:150-157` `Timeout` "zero up to `MaxTimeout`"; `BatchOptions.MaxTimeout` | D22 | **FINDING F16** (a public member is removed: visible) |
| A101 | `SA:201-207` `Subscribe` "Exactly `SubscribeBatched` with `MaxRows` = 1" | L7 | **FINDING F16** |
| A102 | `SA:209-212` `<exception cref="ArgumentOutOfRangeException">` on `SubscribeBatched` | D22 | **FINDING F16** |
| A103 | `SA:252-260` `Unsubscribe` from the handler "can call the handler again, nested, as C++ does" | L13, V2 | **FINDING F16** |
| A104 | `SA:298-302` `Dispose` refused "inside a delivery on this subscriber's provider" | L11 | OK (true for a row delivery; a batch callback pushes no frame) |
| A105 | `FE:38` `NotSupported` "This provider does not implement the requested behaviour" | :363 (3), D28 | **FINDING F17** (owed) |
| A106 | `FE:57-75` `FletcherOrigin`: `Seam` "a PubSubError or a std::exception"; no `Handover` | L2, :551 (3) | **FINDING F17** (owed) |
| A107 | `NL:6-14`, `NL:58-87` one library name; "The ordinary route first" (`NativeLibrary.TryLoad(libraryName, …)`, `NL:73`) | L5, D21 | **FINDING F14**: the runtime's default probing loads `fletcher-c-abi` from `runtimes/<rid>/native/` before any preference runs; the D21 loader must decide in that route too (census D1). |
| A108 | `NL:225-272` `VerifyAbiVersion` | L1 | OK |
| A109 | `Fletcher.Interop.csproj:6-10` "the ONLY package in the round that carries native assets" | L15 | **FINDING F15** (owed) |

### Register rows and d9/d7 obligations on domain 10

| # | Clause (source) | Governing law / ruling | Verdict |
|---|---|---|---|
| A110 | BC-2 status: "Not a prerequisite of the C# switch (…: C#'s per-row `Subscribe` is a batched one with `MaxRows = 1`, D-BIND-59)" | :536 (1) | **FINDING F7** (premise amended; → R-3) |
| A111 | BC-8 C# copies "deleted by the C# switch round" | L3, D22 | OK |
| A112 | BC-9 the C# copy "deleted by the C# switch round" | L4 | OK |
| A113 | BC-10 deferred half (which file a binding loads) | L5, :536 (2) | OK (settled; the row's update is owed) |
| A114 | BC-14 "Each binding translates its own idiom at one site (C#: `SchemaArrival.ToMilliseconds`)" | L12, D22 | **FINDING F1** (same): the named site refuses negatives in managed code and truncates, so reusing it for batch options breaks D22, and copying it breaks "one site". |
| A115 | d9 §6 domain 10 "anticipated"; R-3 and R-4 "registered … not ruled here" | :536 (1)(3), :566 | **FINDING F10**: stale since 2026-11-15; d9 is read-only this round. Annotate with owner approval, or leave to d9's next round. |
| A116 | d9 §7 Q5a, Q9, Q11 open | :536 (2), :551 (2), :566 (Q8 default) | **FINDING F10** (same) |
| A117 | d7 §6 obligations on a binding (translate −1 ms; `MaxTimeout` dropped) | L12, D22 | OK |
| A118 | d7 L15 binding clause "a binding passes the refusal through" | L3 | OK |
| A119 | d9 L2, L14 (borrowed failure; keep by moving) | L8 | OK |
| A120 | d9 L18 / BC-12 (hand-over reported by the shim) | L9, D26 | OK |
| A121 | Ledger notes: the 2026-11-15 entries carry no "*Ingested …*" line; older ingestion notes cite pre-growth line numbers (":478" for :484, ":493" for :500, ":504" for :512, ":516" for :525, ":445" for :448, ":421" for :422) | — | **FINDING F18** (notes, not ruling text; d7, d9 and d10 themselves cite the right lines) |

**Part A clause rows: 121 (74 OK, 42 FINDING, 5 BLOCKER).**

## B. Laws against each other (4), laws against rulings (4b), rulings against each other (4c), citations (5)

| # | Pair | Verdict |
|---|---|---|
| LL-1 | **L12** ("any other negative is refused", one site before the call) vs **L3** / D22 (batch options: domain 7 refuses) | **FINDING F1**. Compatible once L12 names who refuses. Tier-2 text, owner approval. |
| LL-2 | **L3** ("a refusal … reaches the caller with its number and verbatim message") vs **L6** ("names the server package") | **FINDING → R-2** (with :551 (1)) |
| LL-3 | **L2** (`FL_BATCH_*` equal) vs §4's survey of `BatchReason` (0-based) | **FINDING F2**: law wins (code is tier 3); the renumbering is a visible change to record. |
| LL-4 | **L9** ("nothing is lost … silently") vs §4 (a same-topic mix collides, "the C# docs say so") | **FINDING → R-6** (with :448 (1)) |
| LL-5 | **L7** (the per-row path "adds no window, count or timer") vs **L9** (losses reach the handler in `RowsDropped`) | **FINDING → R-3**. Compatible if a per-row drop count is a classify-and-count, not a window count, but nothing says whether the per-row path counts at all. |
| LL-6 | L5 ("never both") vs D21 (both files in one output) | OK: "both" means both loaded |
| LL-7 | L8 (keep only by moving) vs L9 (the failure's message copied) | OK |
| LL-8 | L11 (dispose served from a batch callback) vs L13 (domain 7's thread, no frame) | OK |
| LL-9 | L4 vs L14 V13 (C# counts handler failures) | OK (:246 (2)) |
| LL-10 | L15 (one shim per package) vs L5 | OK |
| R-1 | :363 (4) "a C# app loads the server build only if it **references** the server package" vs :536 (2) "NativeLoader prefers the server shim when **present**" | **FINDING: implicit collision, owner.** d10 L5 reconciles them by redefining "references" as "present". They differ when the server file is present without a reference (a stale file after the package is removed, an xcopy deploy, a transitive reference through a library) and when the server file is present but will not load (a floor breach): load the edge shim silently, or fail? |
| R-2 | :551 (1) "throws a named NotSupported **naming the server package**" vs :337 (2) / :363 (3) (the shim's named refusal, `FL_NOT_SUPPORTED`, its own message) and L3 (verbatim) | **FINDING: implicit collision, owner.** The shim's text names the file, and d9 cannot name a NuGet package. |
| R-3 | :246 (2) ("BC-2's per-row gap … not prerequisites … : C#'s per-row `Subscribe` is a batched one") vs :536 (1) (per-row on the native path) and L9 | **FINDING: collision, owner.** :536 (1) "amends the premise" but leaves the conclusion: whether a row C#'s per-row Arrow subscribe cannot decode is reported or skipped. |
| R-4 | :226 ("C# keeps its current behaviour") and :235 ("C# throws as it does today") vs :536 (3) (`FletcherException(InvalidArgument)`; the cap goes) | **FINDING: implicit, owner (low).** d10 D22 says :536 resolves "the broad reading of :226"; the ruling's text does not say so (F5). |
| R-5 | :551 (2) ("D-BIND-8 / D-BIND-59's 'value type' is revised", and D-BIND-59 covered per-row too) vs :536 (1) (per-row through `fl_decode_rows`, which gives the value type) | **FINDING: implicit, owner (low).** The D-BIND-8 annotation chose "per-row still value type" without a ruling saying so. |
| R-6 | :448 (1) (one `fl_subscriber`; a same-topic mix "documented, not refused") vs d10 L9 | **FINDING: collision, owner.** :448 (1) was a shim ruling ("no one is exposed"); in C# a user is now exposed. |
| R-7 | :203 (2) "C# on desktop loads the server variant" vs :363 (4) | OK: explicit revision in place |
| R-8 | :258 (2) vs :278 | OK: explicit revision in place |
| R-9 | :525 (renamed-copy staging) vs :536 (2) | OK: explicit ("retires when it lands") |
| R-10 | :551 (4) (the latch revised) vs D-BIND-146 (not annotated) | **FINDING F11** |
| R-11 | :484, :500 (closing delivery; a refused subscribe never delivers) vs the per-row path | OK: both govern batched subscriptions only |
| R-12 | :566 confirmed d10 "at `74bf308`" vs HEAD | OK: only the status header changed |
| R-13 | :536 (4) vs D-BIND-1a rider i | OK: explicit, annotated |
| R-14 | :337 (1) / D-BIND-13 (revised) vs `check_dotnet_packages.sh:113` | OK as rulings; the script is tier 3 (part C) |

**Step 5 (a clause citing a ruling matches its text).** Checked: every `ledger :N` in d10 points at a
heading (174, 203, 226, 235, 246, 258, 278, 337, 363, 383, 395, 408, 448, 484, 500, 512, 525, 536, 551
all verified), and every quoted phrase matches except: **F4** (D3: :278 revised :258 (2), not :203 (3));
**F5** (D22: "the broad reading of :226" is not in :536); **F6** (L8: "copied, never disposed" is d9
D32's, not :512's); **F7** (D8 and BC-2 keep a conclusion whose cited premise :536 (1) amended);
**F10** (d9 §6 calls R-3/R-4 unruled); **F18** (ledger ingestion notes cite pre-growth lines).

**Part B pair rows: 24 (12 OK, 12 FINDING; LL-1, LL-3 and R-10 carry F1, F2, F11).** Grand
total 145: 86 OK, 54 FINDING, 5 BLOCKER.

### B1 (BLOCKER): un-annotated oracle text prescribing the managed batcher

D-BIND-25's per-row half (A61), D-BIND-63 (A66), D-BIND-66's batched half (A68), D-BIND-68's Scope and
B3 bullets (A69) and BIND-5's acceptance bullets (A86) state as locked what L4, L7, L13 and D24 now
forbid. The test-authority step and the deletion stage's designer read BIND as tier 2; un-annotated,
these make `TheDefaultCeilingIsTheMostOneManagedBufferHolds`, the put-back and the per-row batch of one
look like requirements. **Suggested correction (tier 2, owner approval), before the deletion stage:**
annotate each "*Revised 2026-11-15 by ledger :203 (1) and :536 (1): retired for `SubscriberArrow`, whose
batches come from domain 7 (d10 L4) and whose per-row form is the native per-row path (d10 L7); kept as
ruled*". D-BIND-68: "the entry point stays (d10 Q8 default); its consumer is deleted". D-BIND-66: keep
the per-row half live until R-3 is ruled. BIND-5: one annotation after the acceptance list.

### Owner questions (tier-1 collisions)

**R-1 — When does a C# app load the server shim?** (a) **Presence decides; a present server shim that
will not load fails loudly, naming it** (no fallback). User: adding the package is enough; a stale
server file left in a folder still loads, and a server shim below its floor stops the app with a clear
message. Architecture: one resolver site (L5), no fallback branch; the CI staging retires. (b) **The
reference decides**: the server package's `.targets` sets an `AppContext` switch the loader reads; a
present file without it is ignored. User: what the project references is exactly what runs; a hand
copy of the file does nothing. Architecture: two facts (switch and file) must agree; one more MSBuild
artefact to test; matches :363 (4)'s words. (c) Presence decides with a silent fallback to the edge
shim. User: the app always starts, but batched calls then throw NotSupported far from the cause.
Architecture: the loader answers BC-10 twice (L6's "asking" becomes the only signal). **Recommended (a),
default (a):** L5 asks for one site, and L6's named refusal already covers a missing server shim; a
silent fallback hides the cause.

**R-2 — What does the edge refusal tell a C# user?** (a) **The shim's message verbatim, then one
sentence naming the server package, added at the batched wrapper**; number and origin unchanged.
User: told the file and the package to add. Architecture: L3 amended to "verbatim, then at most one
binding sentence"; one site. (b) The shim's message only. User: told the DLL name, must find the
package. Architecture: L3 holds as written; L6 and :551 (1) are amended. (c) C# refuses from
`fl_binding_variant` before calling, with its own message. User: same text as (a). Architecture: BC-10
answered twice (L3 violation, needs a ruling). **Recommended (a), default (a):** it meets :551 (1)'s
user-facing words with the smallest law change.

**R-3 — On the per-row Arrow subscribe, what does a handler get for a row that cannot be decoded (no
schema yet, malformed bytes)?** (a) **A null batch with `RowsDropped = 1`** (today's outcome, D-BIND-66).
User: no silent loss, as on the batched path. Architecture: a C# classify-and-count site on the per-row
path (new BC-16); D8 and BC-2 re-worded. (b) Nothing; the row is skipped, as C++'s per-row `Subscribe`
does (BC-2's gap). User: losses invisible on edge apps. Architecture: no new site; L9 amended to exempt
per-row. **Recommended (a), default (a):** L9 ("nothing is lost silently") and today's C# behaviour.

**R-4 — Which exception does a bad batch option raise?** (a) **`FletcherException(InvalidArgument)` with
domain 7's message** (:536 (3) as written). User: code catching `ArgumentOutOfRangeException` /
`ArgumentException` stops catching it; finite timeouts above ~49.7 days are now accepted. Architecture:
L3 pass-through, one throw site. (b) `ArgumentOutOfRangeException` carrying domain 7's message, mapped
at the one throw site. User: the old catch keeps working. Architecture: a status-to-exception rule
keyed on the call (a second mapping rule in `Errors`). **Recommended (a), default (a):** :536 (3) names
the type; record the visible change and that :536 supersedes :235's "as it does today".

**R-5 — May one `SubscriberArrow` deliver a dictionary column as two Arrow types?** (a) **Yes: per-row
gives the value type, batched gives a `DictionaryArray`** (D-BIND-8's annotation). User: code shared
between the two forms must handle both. Architecture: domain 2's two codecs, no conversion in C#. (b)
Per-row re-folds too. User: one type. Architecture: C# builds dictionaries (domain 2's question
answered in C#, L3). **Recommended (a), default (a):** L3; no generated schema produces dictionaries.

**R-6 — A `SubscriberArrow` with a row and a batched subscription on one topic:** (a) **Document it and
register it for domains 3/4**, as :448 (1). User: on `inprocess` the earlier silently stops (a
regression against today). Architecture: none in C#. (b) **`SubscriberArrow` refuses the mix in managed
code** (its own object's rule, not the transport's). User: a clear exception instead of silent loss.
Architecture: one topic map in `SubscriberArrow`; L3's ruled-exception list gains it. **Recommended
(b), default (a) unless the owner chooses (b):** L9; the loss is new to C# users, which :448 (1)'s "no
one is exposed" did not foresee.

## C. Test corpus (Gate 4 step 6; the brief's "Part B")

Searched: every `[Fact]`/`[Theory]` in `dotnet/tests/**` (no `Skip`, no `[Ignore]` anywhere), the three
`integration-tests/*dotnet*` projects, `integration-tests/preview-bundle-consumer`,
`integration-tests/binding-abi-conformance/dotnet`, `scripts/check_dotnet_packages.sh`,
`scripts/check_shim_dependencies.sh`, `.github/workflows/ci.dotnet.yml`. Classes: **LAWFUL**;
**FORBIDDEN** (the subject, or an assertion, is forbidden); **FORBIDDEN-BY-TOLERANCE** (green whether or
not the law holds). **DELETE** when the subject is forbidden; **RE-ANCHOR** when the subject is lawful
(including rewriting a forbidden assertion). "Auto" = re-anchors onto the native per-row path with no
edit unless the per-row reason design differs. "Batched" rows also need the server shim in the test
run (L6: the edge shim refuses).

### `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` (41)

| # | Test | Class | Disposition | Note |
|---|---|---|---|---|
| T1 | `RowsOfAnotherSchemaAreRefusedAndNothingIsPublished` | LAWFUL | RE-ANCHOR (auto) | per-row sink |
| T2 | `APublishAfterARefusedOneStillDelivers` | LAWFUL | RE-ANCHOR (auto) | |
| T3 | `TwoPublishesDeliverBothRows` | LAWFUL | RE-ANCHOR (auto) | |
| T4 | `ARedeclaredTopicStillDecodes` | LAWFUL | RE-ANCHOR (auto) | |
| T5 | `SubscribeReturnsTheDeclaredSchema` | LAWFUL | RE-ANCHOR (auto) | |
| T6 | `ASchemaWatchIsNotSupportedWithoutASchemaChannel` | LAWFUL | KEEP | not the batcher |
| T7 | `TopicOptionsReachTheProviderThroughBothForms` | **FORBIDDEN-BY-TOLERANCE** | RE-ANCHOR | its batched half asserts `NotSupported`, which the edge build answers for the build (`binding.cpp:233`); assert the options reached the provider (message), or run it on the server shim |
| T8 | `ARowRoundTripsAsAOneRowBatch` | LAWFUL | RE-ANCHOR (auto) | asserts `RowLimit` for a per-row delivery: per-row reason is the round's design |
| T9 | `AttachmentsArriveWithTheirRow` | LAWFUL | RE-ANCHOR (auto) | |
| T10 | `ARawRowDecodesLikeAnyOther` | LAWFUL | RE-ANCHOR (auto) | |
| T11 | `UnsubscribingStopsDelivery` | LAWFUL | RE-ANCHOR (auto) | |
| T12 | `OnACarryingTransportASubscriberThatSubscribedFirstStillDecodes` | LAWFUL | RE-ANCHOR | batched; reads straight after `Publish` (must wait) |
| T13 | `OnTheDefaultInProcessLoopbackASubscriberThatSubscribedFirstDropsAndCountsEveryRow` | LAWFUL | RE-ANCHOR | its assertions are d7's F3 outcome (F8 c) |
| T14 | `FlushesAtTheRowLimit` | LAWFUL | RE-ANCHOR | batched; synchronous read |
| T15 | `FlushesAtTheTimeout` | LAWFUL | RE-ANCHOR | batched |
| T16 | `UnsubscribingFlushesThePartialWindow` | LAWFUL | RE-ANCHOR | batched |
| T17 | `AttachmentsAlignWithTheirRows` | LAWFUL | RE-ANCHOR | batched; synchronous read |
| T18 | `ADroppedRowIsReportedAndItsAttachmentDiscarded` | LAWFUL | RE-ANCHOR | batched |
| T19 | `OnlyDroppedRowsStillDeliverAnEmptyBatch` | LAWFUL | RE-ANCHOR | batched; zero-row batch is d7 L7's with a decoder |
| T20 | `ADictionaryColumnArrivesAsItsValueType` | **FORBIDDEN** (D24, :551 (2)) | **DELETE** | replaced by a C# mirror of d7's `DictionaryColumnRefoldedToDictionaryArray` |
| T21 | `ADictionaryColumnKeepsItsNulls` | FORBIDDEN (assertion `IsType<StringArray>`) | RE-ANCHOR | rewrite to `DictionaryArray`; nulls subject lawful (F8 b) |
| T22 | `ACorruptRowIsDroppedAndTheBatchStaysAligned` | LAWFUL | RE-ANCHOR | batched |
| T23 | `AFixedSizeListArrivesNonNull` | LAWFUL | RE-ANCHOR | batched; d7 has the mirror (F8 a) |
| T24 | `ASchemaTheCodecCannotOpenDropsEveryRow` | LAWFUL | RE-ANCHOR | null batch, all dropped: d7 L7, L8 |
| T25 | `UnsubscribingFromTheHandlerDuringARowLimitFlushStopsDelivery` | FORBIDDEN (assertion `delivered == 1` excludes the closing delivery, :484) | RE-ANCHOR | rewrite: one empty `Closing`, then nothing |
| T26 | `UnsubscribingFromInsideATimeoutFlushIsSafe` | LAWFUL | RE-ANCHOR | batched |
| T27 | `ACorruptRowDoesNotCountTowardMaxRows` | LAWFUL | RE-ANCHOR | batched; synchronous read |
| T28 | `RowsSplitAcrossMessagesAreDroppedNotMisaligned` | LAWFUL | RE-ANCHOR | **Q9 port** |
| T29 | `ASubscriptionFromAnotherSubscriberArrowIsRefused` | LAWFUL | RE-ANCHOR (auto) | C#'s own ownership check |
| T30 | `ATimeoutPastTheTimersMaximumIsRefused` | **FORBIDDEN** (`MaxTimeout` cap, managed `ArgumentOutOfRange` pre-check; D22) | **DELETE** | replaced by the BC-8 grid from C# (census D3) |
| T31 | `AWindowAtTheMaximumTimeoutWaits` | LAWFUL | RE-ANCHOR | uses `BatchOptions.MaxTimeout` (removed); use a timeout past 49.7 days |
| T32 | `ARowWithAnEmptyAttachmentKeyArrives` | LAWFUL | RE-ANCHOR (auto) | **Q9 port** (batched path); the C# copy keeps it on both paths |
| T33 | `ATimerHandlerThatCancelsDuringADeliveryDoesNotDeadlock` | LAWFUL | RE-ANCHOR | holds under d7 (F8 d); may be dropped as a duplicate of d7's `InCallbackUnsubscribeWhilePublishingDoesNotDeadlock` |
| T34 | `ATimeoutFlushAndARowLimitFlushMayOverlap` | **FORBIDDEN** (concurrent delivery, D-BIND-64 revised, L13) | **DELETE** | |
| T35 | `UnsubscribeWaitsForATimerHandlerStillRunning` | LAWFUL | RE-ANCHOR | d7 L9 blocking close |
| T36 | `BatchesAreWellFormed` | FORBIDDEN (assertion: dictionary column cast to `StringArray`) | RE-ANCHOR | rewrite the column-2 reads (F8 b) |
| T37 | `WindowsFollowOneAnother` | LAWFUL | RE-ANCHOR | batched |
| T38 | `ARowThatWouldPassTheCeilingFlushesTheWindowFirst` | FORBIDDEN (mechanism: the C# window ceiling, L4, D-BIND-63) | **DELETE** | **Q9 port** (a non-heavy offset-budget split) |
| T39 | `ARowLargerThanTheCeilingIsDroppedAndCounted` | FORBIDDEN (mechanism, L4) | **DELETE** | covered by d7 heavy `CraftedRowExceedsTheOffsetBudgetEvenAlone` |
| T40 | `TheDefaultCeilingIsTheMostOneManagedBufferHolds` | FORBIDDEN (L4, D-BIND-63) | **DELETE** | |
| T41 | `AHandlerThatThrowsIsAbsorbedCountedAndRaised` | LAWFUL | RE-ANCHOR (auto) | per-row; add a batched twin. **Q9 port** (d7 half: the next batch still arrives) |

Sum: **6 DELETE** (T20, T30, T34, T38, T39, T40); **34 RE-ANCHOR** (22 batched with an edit, incl. 3
rewrites of a forbidden assertion and 1 tolerance; 12 per-row "auto"); **1 KEEP**.

### Other tests and checks in scope

| # | Test or check | Class | Disposition | Note |
|---|---|---|---|---|
| T42 | `NativeLoaderTests.TheShimIsLookedForBesideTheAssemblyThenBesideTheApplication` (asserts 4 candidates, one file name) | LAWFUL | RE-ANCHOR | D21: server names come first; candidate count doubles |
| T43 | `NativeLoaderTests` other 8 (`TheShimLoads…`, `TheSingleCopyMarker…` ("fletcher-c-abi" is in both names), handshake ×2, resolver, refused-shim, no-shim, floors) | LAWFUL | KEEP | add: prefers the server shim; R-1's no-fallback case |
| T44 | `ErrorTests.*` (origins 1, 2 only) | LAWFUL | KEEP | add Q11's table test (status, origin incl. 4, `BatchReason`, `fl_variant`) |
| T45 | `ThunkDisciplineTests.*` | LAWFUL | KEEP | the batch thunk falls in its scope |
| T46 | `integration-tests/preview-bundle-consumer` `NativeBatch` (queue-based) | LAWFUL | RE-ANCHOR | must reference the server package (D25, F27) |
| T47 | `binding-abi-conformance` C# lane, renamed-copy staging (`ci.integration-test.binding-abi-conformance.yml`) | LAWFUL | RE-ANCHOR | staging retires when the loader lands (:536 (2)); stage the server file under its own name |
| T48 | `scripts/check_dotnet_packages.sh:113` "carries native assets, which only Eiva.Fletcher.Interop may" (+ `:74` four packages, `:119` dependency regex) | **FORBIDDEN** ("Interop is the only package with native assets", D22) in part | RE-ANCHOR | "only Interop and the server package, each exactly its two shims; GatewayClient depends on neither"; keep the D-BIND-157 half on Interop (F12, F13) |
| T49 | `scripts/check_shim_dependencies.sh`, `shim_floors.env`; `ci.dotnet.yml` `locate_shim.sh fletcher-c-abi variant=edge` | LAWFUL | KEEP (edge); EXTEND for the server shim per Q7's default | |
| T50 | `Fletcher.GatewayClient.Tests/BatchedSubscriptionTests.cs` (`:230` `ArgumentOutOfRange`, `:240` `GatewayBatchOptions.MaxTimeout`), `gateway-dotnet-end-to-end`, `gateway-dotnet-crosslang`, `AotSmoke` gateway batch | out of scope (domain 5; BC-8/BC-9 deferred copies) | none | they pin a cap and a pre-check d10 forbids for `SubscriberArrow`, but on domain 5's `GatewayBatcher` |
| T51 | `protoc-dotnet` | out of scope (domain 1) | none | |

**Must change: 6 DELETE, 38 RE-ANCHOR** (34 + T42, T46, T47, T48).

### d10 Q9: managed-batcher cases domain 7's suite lacks (must be ported, not lost)

Checked against the 63 `TEST`s in `pubsub-arrow/tests/test_pubsub_arrow*.cpp`. **4 cases:**
1. **`RowsSplitAcrossMessagesAreDroppedNotMisaligned`**: two messages, each malformed alone, valid
   together. `BatchDecoder` has the check ("buffer not fully consumed", `batch_decoder.cpp:310`) but no
   d7 test pins it at the batch level.
2. **`ARowWithAnEmptyAttachmentKeyArrives`** (batched path): an empty attachment key carried through a
   window. No d7 test.
3. **`AHandlerThatThrowsIsAbsorbedCountedAndRaised`**, d7 half: a callback that throws does not stop
   the next batch (d7 L10 has no test, d7 Findings c). The counting half stays C#'s (L9).
4. **`ARowThatWouldPassTheCeilingFlushesTheWindowFirst`**: an offset-budget split at a small size.
   d7's split tests are `heavy` only and d7 has no test-only budget; the port needs a d7 test hook (a
   domain-7 round) or an owner "drop".

Not ports (d7 has the case): `AFixedSizeListArrivesNonNull` (`FixedSizeListWithNamedItemArrivesNonNull`),
`ADictionaryColumnKeepsItsNulls`, `ACorruptRowDoesNotCountTowardMaxRows`,
`UnsubscribeWaitsForATimerHandlerStillRunning`, `AWindowAtTheMaximumTimeoutWaits`,
`OnACarryingTransport…` (mock late-schema cases), `ARowLargerThanTheCeiling…` (heavy).

## D. The round's driving-document set (the brief's "Part C")

| Document | Mode | Role |
|---|---|---|
| `docs/domains/d10-csharp-binding.md` | **round** (survey for what exists, round for the switch) | the domain; laws L1–L15, D1–D27 |
| `plans/RIX-rulings.md` | **round** (tier 1, append-only) | :174 to :566; R-1 to R-6 answers go here |
| `plans/D10C-pre-round-audit.md` (this file) | **round** | B1, F1–F27, R-1–R-6, the test sort, Q9, the census |
| `docs/domains/bounded-context-register.md` | **round** (rows added or amended at close) | BC-2, -8, -9, -10, -12, -13, -14; new BC-16, BC-17 |
| `docs/domains/d9-c-abi-shim.md` | **survey** (read-only; a domain-9 change needs a named exception) | the C surface, L2, L14, L18, §6, Q9 |
| `docs/domains/d7-batch-assembly.md` and `pubsub-arrow/tests` | **survey** (read-only) | batching semantics (L14), the Q9 target |
| `docs/domains/d2-row-encoding.md` | **survey** | dictionary answers of the two codecs (D24, R-5) |
| `plans/BIND-locked-decisions.md` | **survey** (tier 2); **round** only for the owed annotations (B1, F11) | inherited decisions D18 |
| `plans/BIND-csharp-bindings.md` | **survey** (tier 2); **round** for the BIND-5 annotation (B1) | |
| `c-abi/include/fletcher/abi/binding.h` | **survey** | what C# mirrors (L2) |
| `dotnet/README.md`, `README.package.md`, `PREVIEW-TESTERS.md`, SA/FE/NL doc comments, `Fletcher.Interop.csproj` | **round** (tier 3, owed) | F15–F17, F27 |
| `docs/domain-map.md` | **survey** | row 10, planned order 6 |
| `docs/archive/D9S/D9S-pre-round-audit.md` | **survey** (precedent) | format, R-3/R-4 origins |

## E. Bounded-context census (Gate 4b; the brief's "Part D")

Searched by operation in `dotnet/src`, not by name: native loads (`NativeLibrary.Load`/`TryLoad`,
`SetDllImportResolver`, `[LibraryImport(...)]`: one resolver, `NL:56-87`; every import names
`NativeMethods.LibraryName`); C Data imports (`CArrowArrayImporter.ImportRecordBatch` at
`FletcherCodec.cs:272, 325`; `CArrowSchemaImporter.ImportSchema` at `SchemaHandle.cs:129`); range checks
and clamps on options (`SA:228-237`, `GatewayBatch.cs`); `TimeSpan` → ms conversions
(`SchemaArrival.cs:216-232`, `SA:512-523` ticks); status/origin casts (`Errors.cs:58-67`, one throw
site; `SchemaArrival.cs:112-125`, outcomes as values); `catch` that counts (`Subscriber.cs:770`,
`SA:335-350` duplicate `ReportAbsorbed`); frame checks (`InDeliveryOnThisProvider`, `SA:310`);
threads/timers/clocks (`SA:355-596` only); attachment copies (`SA:468-485` `AppendDelivered`);
dictionary type mapping (`DecodedSchema.cs:47-100`); package rules (`check_dotnet_packages.sh:74,
108-121`, `Fletcher.Interop.csproj:52-63`).

| # | Context (one right answer) | Disposition | Site / copies | Structural test | Register |
|---|---|---|---|---|---|
| D1 | **Which shim is loaded?** | **EXTEND-AND-NARROW** | Single site `NativeLoader.Resolve`, server candidates first in BOTH routes. Copies to delete: the runtime's default probing deciding by the edge name (`NL:73`, F14); CI's renamed-copy staging (:525, T47). Behaviour on a present-but-unloadable server shim per R-1. | Both files staged: `fl_binding_variant() == SERVER` and exactly one `fletcher-c-abi*` module in the process; edge only: `EDGE`; a scan finds no `NativeLibrary.Load*`/`TryLoad` outside `NativeLoader` | BC-10: deferred half → "d10 L5, `NativeLoader.Resolve`" |
| D2 | **Is this a server build?** (does a batched subscribe work) | **REUSE** BC-10 | The shim (`fl_binding_variant`, `RequireServerBuild`, `binding.cpp:228-238`). C# forwards the shim's `FL_NOT_SUPPORTED` (L3, L6); a public variant property may read the query, never refuse from it (R-2 (c) needs a ruling) | On the edge shim `SubscribeBatched` throws `FletcherException(NotSupported, Seam)` whose message contains the shim's text verbatim; `fl_binding_variant` is called from one site in `dotnet/src` | BC-10: add the C# note |
| D3 | **Are these batch options acceptable?** | **EXTEND-AND-NARROW** BC-8 | Domain 7's constructor. Copies to delete: `SA:228-235` (check + `MaxTimeout` cap), `SA:237` (clamp), `SA:157` (`MaxTimeout`), `SA:147` (doc). `GatewayBatch.cs` stays domain 5's | BC-8's 6 x 7 grid driven from C# through the server shim: refused ⇔ out of range, `FletcherException(InvalidArgument)` with domain 7's message byte-equal, nothing delivered, nothing left; a scan of `dotnet/src/Fletcher` finds no comparison or clamp on `MaxRows`/`Timeout` | BC-8: "C#: passes through (d10 L3, D22)" |
| D4 | **How does a C# timeout cross?** | **EXTEND-AND-NARROW** BC-14 | One internal site lifted from `SchemaArrival.ToMilliseconds`; for batch options it forwards negatives and never rounds a negative to 0 (today `(long)total` truncates: −1 tick would cross as 0 and be accepted); delete `SA:512-523` | −1 tick on a batched subscribe is refused by domain 7 (not accepted as 0); `InfiniteTimeSpan` → `INT64_MAX`; `TimeSpan.MaxValue` never flushes early; the arrival unchanged; one `TimeSpan`→ms site in `dotnet/src/Fletcher` | BC-14: "C#: one internal site, shared by the arrival and the batched subscribe" |
| D5 | **Is this batch the caller's to keep?** | **DEFINE** | One batch-thunk function moves array and schema (C Data move), imports with the batch's own schema, then calls the handler; never releases what it did not move (L8). Incumbents are other lifetimes: `FletcherCodec.cs:272, 325` (owned decode output), `SchemaHandle.cs:127-134` | A handler that keeps the batch past the callback reads it intact; the shim's "unmoved, released by the shim" count is 0 for every moved batch; allocation baseline after disposal | BC-13: "C#: the batch thunk's move, one site" |
| D6 | **How does a status, origin or reason number map to C#?** | **REUSE** (extend) | `Errors.ThrowIfFailed` and the `FletcherStatus`/`FletcherOrigin` enums; add `Handover = 4`, renumber `BatchReason` 1..3 (F2), mirror `fl_variant` | Q11: a test that reads `binding.h`'s enums and `#define`s and compares every C# value by name and number | none (L2) |
| D7 | **How does a failed hand-over surface?** | **DEFINE** | One branch of the batch thunk: null batch, `RowsDropped` as given, reason kept, `BatchStatus` failure {status, origin 4, message copied by length via `Errors`' decoder}; never `fl_error_dispose` (borrowed) | d9's test-only hand-over fault shim driven from C#: Σ rows + Σ dropped = rows offered; origin `Handover`; message; reason | BC-12: add the C# half |
| D8 | **Per-row Arrow: is this row delivered or dropped?** | **DEFINE** (pending R-3) | One classify-and-count function in `SubscriberArrow`'s per-row handler. Copies to delete: the managed batcher's drop paths (`SA:398-465`, `Resolve`). C++'s per-row gap (BC-2) is not reused | Conservation on the per-row path, edge and server shims: good, malformed and schema-less rows; rows offered = delivered + Σ `RowsDropped` | **BC-16 (new)** |
| D9 | **Which .NET package may carry native assets?** | **EXTEND-AND-NARROW** | One rule list in `check_dotnet_packages.sh` (`:74`, `:108-113`, `:119`); delete the "only Interop" text in `Fletcher.Interop.csproj:6-10` and the READMEs | The check over five packages, with controls: a `runtimes/` entry injected into `Eiva.Fletcher` fails; a GatewayClient dependency on the server package fails; the server package with other than its two shims fails | **BC-17 (new)** |
| D10 | **What limits does the server shim meet?** (DLL list, floors, ceiling) | **DEFER-BUT-REGISTER** (Q7 default) | `check_shim_dependencies.sh` and `check_dotnet_packages.sh`, per package | Same lists and floors fail by name; ceiling from the first measured pack | in BC-17's text |
| D11 | **What Arrow type does a dictionary column arrive as?** | **REUSE** domain 2 | `BatchDecoder` re-folds (batches), `NanoarrowCodec` gives the value type (per-row). C# converts neither; batches never go through `DecodedSchema.cs:100` | Batched: `DictionaryArray` (mirror of d7's `DictionaryColumnRefoldedToDictionaryArray`); per-row: value type (R-5) | none (d2) |
| D12 | **When, on which thread and in what order does a batch reach the handler?** | **REUSE** BC-1; **EXTEND-AND-NARROW** BC-9 | Domain 7. Copy to delete: the managed `Batcher` (`SA:353-827`) | L4's scan of `dotnet/src/Fletcher` (no `System.Threading.Timer`, `Stopwatch` deadline, row buffer in `SubscriberArrow`); C# replay of `EveryCallbackRunsOnTheBatchThreadInWindowOrder` | BC-9: the C# copy deleted |
| D13 | **Is a handler failure counted?** | **REUSE** (narrow) | D-BIND-19 rule 5: the per-object count and `Diagnostics`. Copy: `SA:335-350` duplicates `Subscriber.cs:770`; one helper | A throwing batch handler: count +1, event raised, next batch arrives; the reflection test | BC-7: "C# counts (d10 L9)" |
| D14 | **May a `SubscriberArrow` be disposed or closed here?** | **REUSE** BC-6 | `Subscriber.InDeliveryOnThisProvider` (`SA:310`); a batch callback pushes no frame, so it is served (L11) | Dispose from a batch callback: queued batches then one `Closing`; dispose from a row delivery: refused | BC-5: C# note |
| D15 | **Which close applies to this subscription id?** | **REUSE** d9's id space | Bit 63 marks a batched id (`h:1121-1127`); C# keeps no parallel flag that could disagree | Closing a row id with the batched close (and the reverse) is refused by the shim, not by C# | none |
| D16 | **How is a borrowed row's attachment set copied?** | **EXTEND-AND-NARROW** | One copy function (today `SA:468-485`, `AppendDelivered`, empty keys cross) shared by the per-row and batch thunks | `ARowWithAnEmptyAttachmentKeyArrives` on both paths | none |
| D17 | **Is another shim already loaded?** | **REUSE** BC-15 | The shim's marker scan; the loader loads one file (D1) | D1's single-module assertion | BC-15: C# note |
| D18 | **Can this binding use this shim's version?** | **REUSE** | `NativeLoader.VerifyAbiVersion`; one version for both builds (d9 D27) | `NativeLoaderTests` handshake rows | none |

### Proposed register rows (text for `bounded-context-register.md`; not edited)

| ID | Question | Single site | Structural test | Status | Domain |
|---|---|---|---|---|---|
| BC-16 | On C#'s per-row Arrow subscribe, is this row delivered or dropped, and is the drop reported? | One classify-and-count function in `SubscriberArrow`'s per-row handler (over `fl_subscriber_subscribe` and `fl_decode_rows`); answer per R-3 (default: a null batch with `RowsDropped = 1`) | Per-row conservation over good, malformed and schema-less rows on both shims | `[designed-not-built]` (D10C); owner R-3 owed | d10 (L7, L9) |
| BC-17 | Which .NET package may carry a native shim, and what limits does each shim meet? | `scripts/check_dotnet_packages.sh` (one rule list: Interop and the server package, each exactly its two shims; every other package none; GatewayClient depends on neither) with `check_shim_dependencies.sh` per shim; D-BIND-157 on Interop; the server ceiling per Q7 | The check over five packages with three red controls | `[built]` for four packages under the old rule; five `[designed-not-built]` | d10 (L15) |

Amendments: BC-2 (drop the C# premise; point at BC-16), BC-8, BC-9, BC-10 (deferred half settled),
BC-12, BC-13, BC-14 (C# site text), as in D1–D7, D12.

## Owner-level items (one line each)

- **B1 (BLOCKER)**: annotate D-BIND-25 (per-row half), -63, -66 (batched half), -68 (Scope, B3) and
  BIND-5's acceptance as retired for `SubscriberArrow` (tier 2, approval), before the deletion stage.
- **R-1**: server shim chosen by presence (fail loudly if unloadable) or by reference. Default (a).
- **R-2**: the edge refusal = shim message + one sentence naming the package. Default (a).
- **R-3**: per-row Arrow reports an undecodable row (null batch, `RowsDropped = 1`). Default (a).
- **R-4**: bad batch option → `FletcherException(InvalidArgument)`; :536 (3) supersedes :235's "as it
  does today". Default (a).
- **R-5**: per-row value type vs batched `DictionaryArray` accepted. Default (a).
- **R-6**: same-topic row + batched mix in one `SubscriberArrow`: document (a) or refuse (b).
  Recommended (b), default (a).
- **Q9**: four cases to port to domain 7 (or drop by ruling), before the deletion stage.

## Tier-2 corrections needing approval (suggested, not applied)

- **F1**: d10 L12 names who refuses a negative; BC-14's C# site text.
- **F2**: d10 L2's mark lists `BatchReason` (0-based vs `FL_BATCH_*` 1..3) with the visible renumbering.
- **F3**: d10 L11's Tests line (no `SubscriberArrow` test today).
- **F4, F5, F6**: d10 D3 (:258 (2)), D22 (drop "the broad reading of :226" or cite R-4's answer), L8
  (cite d9 D32).
- **F7**: d10 D8 and register BC-2 drop the amended premise (after R-3).
- **F8**: d10 §8 rule 6 takes part C's sort.
- **F10**: d9 §6 / Q5a / Q9 / Q11 annotated as settled by :536, :551, :566 (or left to d9's next round).
- **F11**: D-BIND-146 annotated (outcome now domain 7's).
- **F12, F13, F23**: packaging-stage notes (five packages; the dependency regex; L15's line cite).
- **F18**: ledger ingestion lines for 2026-11-15; stale line numbers in older ingestion notes.
- Tier 3, owed by the round (no approval needed): **F14** (loader's ordinary route), **F15** (READMEs,
  csproj), **F16** (SA doc comments), **F17** (FE doc comments), **F27** (consumer and snippet).
