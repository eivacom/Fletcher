# D10C-7 — compliance review (step 4a)

## Cycle 1 — ISSUES: 2 blocking (F1, F2), 3 non-blocking (F3-F5)
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **667 lines, 52 `## ` entries**, last "## 2026-11-16 — D10C-7: the switch
and the deletion are one stage" (:660). No tier-1 collision; no tier-2-vs-tier-1 conflict. Base `4542e78`, working tree + untracked scan file.

**Held, not findings.** D1-D7 shape. DEBT-2: `accepted` is set only after FL_OK, before any allocation; `catch when (!accepted)` is in
exactly two frames; Closing is the only free after OK; `Subscription` is null-checked. C2-1: binding-wide metadata scan (no timer;
`Stopwatch` only in {SchemaArrival}); source scan of `dotnet/src/Fletcher`; input from an env var; nothing `Skip`ped. U1 dropped,
U2 kept. **L4 by grep:** no `Batcher`, ceiling, framed decode/import, `_batchers`, timer or row buffer in the `dotnet/src/Fletcher`
source; its one `Stopwatch` is `SchemaArrival.cs:183`. **No bridge:** the variant is asked at one site (`Subscriber.cs:521`), only
after a non-OK status, only to word the error. **Public surface:** unchanged (an `unsafe` modifier, docs). No edit to `c-abi`,
`pubsub-arrow`, d7 or d9. **BC-14:** one TimeSpan-to-ms site (`SchemaArrival.ToNativeMilliseconds`, floored below 0), the arrival's
refusal in front of it. **Deleted:** exactly T20, T30, T34, T38, T39, T40. **Rulings:** :575 (2) (shim text plus one sentence,
Edge only; T7's server arm has none); :575 (5) and :551 (2) (`DictionaryArray`, nothing converted); :484 (one Closing, one free);
:551 (1); :551 (3) (a null array passes through; `failure` unread, rung 11); :536 (3) (refusal and clamp gone, `MaxRows` int64);
:650 (1) (L3 Tests line, V12); :650 (2) (T46 untouched); :660. Every changed d10 mark reads "Windows, by hand; CI and Linux unrun".
**Implementer's items:** (1) the NULL written at the fixed offsets is D3 step 2's move, not a hand-rolled release (P2), and
`Marshal.OffsetOf` cross-checks the offsets. (2) A null idle Closing is d7 L6's "null if there is no decoder"; the test accepts
either form (doc: F4). (3) The order-test fix keeps all three proofs. (4) The lane widening matches Risks (pack step: F2). **`MaxTimeout`
kept: right under the plan** (rung 10 removes it; Files-to-delete "Kept"; D22/:536 (3)'s cap is the refusal, gone); its doc is not (F4).

### F1 BLOCKING: residue (a) can end in silence (design D3 step 3; d10 L9; :484)
`SubscriberArrow.cs:606-607` calls `CArrowArray.Create()` and `CArrowSchema.Create()` (AllocHGlobal, throws on out of memory)
outside the `try`. That exception escapes `Deliver` and the thunk's outer catch swallows it: the handler never gets the window. This
includes a Closing: its `GCHandle` is still freed, but the app never sees its last delivery. A failure in the second `Create` also
leaks the first struct. The design names out of memory as residue (a): a null batch, its rows counted, the reason kept.
**Fix:** null-initialise both pointers, allocate inside the `try`, free only non-null ones in `finally`; the catch hands `[]` (~4 lines).

### F2 BLOCKING: D7's packed-assembly proof cannot run as written (`ci.dotnet.yml:598-603`)
The new step runs `dotnet test … --no-restore` in a fresh `docker run --rm`. The restore ran in the Pack step's container, and its
`/root/.nuget/packages` was discarded with it (no `NUGET_PACKAGES`, no cache mount, nothing in the image). The build fails with
NETSDK1064 before the scan reads anything. Every other step restores and builds in one container (`:205-206`). The evidence ran a
different command (Windows, `--no-build`): this YAML has never run. **Fix:** add `dotnet restore tests/Fletcher.Tests --locked-mode &&`
inside the same `bash -c`, or drop `--no-restore`.

### F3: BC-10's rung-7 variant scan is missing (register BC-10; ladder rung 7)
BC-10 reads "`[built]` … except the variant-keyed-refusal scan (rung 7)"; ladder rung 7 says "`fl_binding_variant` called from one
site in `dotnet/src`". The design assumed the scan exists ("its scan must allow it"), but none does, and d10's "Variant check" row
is now `[built]` "One call site" with nothing proving it. **Fix:** a source-scan arm (exactly one `fl_binding_variant(` call, in
`Subscriber.SubscribeBatches`) with a control, about 10 test lines; or the PM moves the item to a later rung in BC-10 and the ladder.

### F4: two public docs state what the switch made false
`SubscriberArrow.cs:59` says "Zero rows … for an idle Closing", but an idle Closing before any row is null (d7 L6). `:113` says
`MaxTimeout` is "the longest finite Timeout: the most a timer can wait", but there is no timer now and longer timeouts are accepted
(:536 (3), D22). **Fix:** ":59: zero rows, or NULL if no row has carried a schema"; ":113: no longer a limit (:536 (3)); removed at rung 10".

### F5: brief :47's Linux devcontainer run did not happen (evidence: "Not run"); the marks say so. Run it, or note it in As-landed.

## RECORD
- `ci.pr.yml` was edited but is not in Files-to-touch; the transport lane's same-commit rule requires the edit, so it is correct.
- d10 stale citations: L9 Holds-at `SA:968-975` (deleted Batcher); "Window (C#)" `SA:604-607`; L12 Holds-at `SchemaArrival.cs:216-228`
  (`ToMilliseconds`, now `ToNativeMilliseconds`); the "Thunk" glossary row lacks the batch thunk.
- d10 L14 is a bare `[built]`, with V5-V8 (rung 10) and the hand-over (rung 11) carved out only in prose; L3's options clause is
  `[built]` but its only test, the grid, is rung 10's. Both marks should say so.
- Register: BC-13's C# half is still `[designed-not-built]` although d10 L8 is `[built]`; BC-14 still names `ToMilliseconds`; BC-9
  still says "Until the switch, the C# copy violates L9 and L12". `README.package.md:153` (timer thread) is rung 12's (F15). Nit (4b):
  the edge message reads "…for batched subscriptions Batched delivery…" (the shim's text has no full stop).
