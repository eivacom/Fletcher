# D10C-7 — compliance review (step 4a)

## Cycle 1 — ISSUES: 2 blocking (F1, F2), 3 non-blocking (F3-F5)
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **667 lines, 52 `## ` entries**, last "## 2026-11-16 — D10C-7: the switch
and the deletion are one stage" (:660). No tier-1 collision; no tier-2-vs-tier-1 conflict. Base `4542e78`, working tree + untracked scan file.
**Held, not findings.** D1-D7; DEBT-2 (`accepted` set only after FL_OK; `catch when (!accepted)` in two frames; Closing the only free
after OK; `Subscription` null-checked); C2-1 (binding-wide metadata scan, {SchemaArrival} allow-list, source scan of `dotnet/src/Fletcher`,
env-var input, never `Skip`); U1 dropped, U2 kept. L4 by grep: no `Batcher`, ceiling, framed decode/import, `_batchers`, timer or row
buffer; one `Stopwatch` (`SchemaArrival.cs:183`). No bridge: the variant is asked once, after a non-OK status, only to word it. Public
surface unchanged. No `c-abi`/`pubsub-arrow`/d7/d9 edit. BC-14: one site (`ToNativeMilliseconds`, floored below 0). Exactly T20, T30,
T34, T38, T39, T40 deleted. Rulings :575 (2), (5); :551 (1)-(3) ((3)'s `failure` is rung 11); :484; :536 (3); :650 (1), (2); :660 held.
Every changed d10 mark reads "Windows, by hand; CI and Linux unrun". Implementer's items: (1) NULL at fixed offsets is D3 step 2's move,
not a hand-rolled release (P2); (2) a null idle Closing is d7 L6's "null if there is no decoder" (doc: F4); (3) the order-test fix keeps
all three proofs; (4) the lane widening matches Risks (pack step: F2). **`MaxTimeout` kept: right under the plan** (rung 10 removes it;
Files-to-delete "Kept"; D22/:536 (3)'s cap is the refusal, which is gone); its doc is not right (F4).
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
**RECORD.** `ci.pr.yml` is outside Files-to-touch but the transport lane's same-commit rule requires it: correct. d10 cites stale lines:
L9 `SA:968-975`, "Window (C#)" `SA:604-607`, L12 `SchemaArrival.cs:216-228`, and "Thunk" lacks the batch thunk. L14 is a bare `[built]`
(V5-V8 rung 10, hand-over rung 11, in prose only); L3's options clause is `[built]` with its only test at rung 10. Register: BC-13's
C# half is still `[designed-not-built]`; BC-14 names `ToMilliseconds`; BC-9 says "Until the switch … violates". `README.package.md:153`
is rung 12's (F15). Nit (4b): the edge message reads "…for batched subscriptions Batched delivery…" (no full stop in the shim's text).

## Cycle 2 — PASS (no blocking item; diff base `4542e78`, working tree)
**Proof of read:** ledger re-read: **667 lines, 52 entries**, last :660, unchanged. L4 grep, public surface and the d7/d9 scope re-checked: unchanged.
- **F1 closed.** `SubscriberArrow.cs:602-658`: both pointers start null; `Create`, the list and the move run inside the `try`; `finally`
  frees only non-null; the catch delivers `Array.Empty`. The leg `AWindowTheThunkCannotTakeOverArrivesAsANullBatchWithItsRowsCounted`
  (a Closing with more than int.MaxValue rows) gets a null batch, its rows counted, its reason kept, and nothing absorbed.
- **F2 closed in YAML** (`ci.dotnet.yml:603-606`): the step restores `--locked-mode` in its own container and sets `TreatNoTestsAsError`.
  Only the owed CI run proves it (:408 (2)).
- **F3 closed:** `NativeLoaderTests.NoNativeLoadOutsideTheResolver` requires exactly one `fl_binding_variant(` call, in `Subscriber.cs`;
  red against a planted second call (evidence). **F4 closed** (`:57-59`, `:113`). **F5 closed** (evidence row 23, before the fix
  cycle; a re-run is in progress).
- RECORD items fixed: d10 citations, the L3/L14 marks split three ways, register BC-9, -10, -13 (shim count "not observed from C#"), -14.
**New RECORD.** (a) The thunk now finds `release` with `Marshal.OffsetOf`, which is still D3's move, but the P2 leg's cross-check uses the
same call and is no longer independent; no AOT lane runs a batched subscribe (D-BIND-81, d10 D18), so 4b should note it. (b) The timing
control is widened from 2 s to 5 s (4b), which departs from the ladder ground rules and design :122; it is disclosed, and the PM amends the text or accepts it.
