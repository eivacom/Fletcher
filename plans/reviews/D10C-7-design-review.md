# D10C-7 — design review (step 2), cycle 1 of 2

**Verdict: APPROVE-WITH-DEBT(3). No BLOCKER.** Design `plans/D10C-7-switch.md` (150/150), brief `plans/D10C-7-brief.md` (55/60).
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **658 lines, 51 `## ` entries** (counted), last heading "## 2026-11-16 —
D10C-7 pre-design: T30 in d10; the preview-consumer red window" (:650). No tier-1 collision. R-6's chosen text read for D5
(`plans/D10C-pre-round-audit.md:313-319`).
**Tree claims verified:** `h:874-884, 1020-1023, 1071, 1077-1081, 1102-1129`; `SA:175, 254-280, 379-385, 438-448, 471, 592`;
`Sub:283` (`InDeliveryOnThisProvider`), `:425-454`, `:569-574`, `:717-742` (`_disposed` set before destroy: D4's in-destroy no-op
holds); `subscriber.cpp:720-745` (quiet only under `InsideDeliveryOn(provider)`); `subscriber_arrow.cpp:529-534` (a batched close
calls the seam's `Unsubscribe` on the caller's thread, so D4's `quiet` covers both kinds); `FletcherCodec.cs:266-279`;
`DecodedSchema.cs:74,100`; T40 is the ceiling ctor's only caller once T38/T39 go; `fl_binding_variant` has no `dotnet/src`
caller today; `Directory.Build.props:26-28` (TWAE + EnforceCodeStyleInBuild), so P5 is rightly a stop, not a bet.

## DEBT-1 (PM, before the owner reads the brief) — Decision 1 lacks its cheapest option and misstates the purpose
- Missing **(d) merge D10C-8's deletion into this stage**: net ≈ −160 to −310 (7a + 7b + rung 8's −470/−520), under the ceiling
  with margin; no split; no dead-code window (D7 and P5 vanish); the purpose's "C#'s own copy is gone" (:566) lands with the
  switch. Cost: one large, mostly-deletion review; rung 8's two scans and T40 move in; audit 0a's T40 line re-points. The design
  names it (:133) but the owner is not offered it. "The PM ruled it separate" is not owner authority; the lean default makes a
  construct whose deletion is already scheduled the owner's plan-shape question.
- "(a) — the round purpose lands whole" and (a)'s "serves the purpose whole" are false while `Batcher` is in source: say "the switch".
- (c) cites :643 as precedent; that ruling also kept 250 "for later stages". Say so.
- The C# design (D1-D6) is unchanged under any option; these are brief edits.

## DEBT-2 (step 3, code) — key the GCHandle free and the claim release on "native refused", never on a catch
The claim sits outside the inner `try` (D10C-6 DEBT-1), so the free for a claim refusal is a catch in `SubscribeBatched`, and that
catch also sees anything thrown after native OK (OOM building `Subscription`/`SchemaArrival`, or `_claimed[id]` in
`SubscribeClaimed`). Freeing `ctx` then is a use-after-free in the thunk: a reused handle slot routes batches to another delivery,
or the outer catch swallows them, silently; and `SubscribeClaimed`'s catch drops the claim of a live subscription. Name ONE frame
that frees, on the status or an `accepted` flag (`catch when (!accepted)`); after OK only Closing frees and only `Unsubscribe`
releases the claim. Null-check `delivery.Subscription` in step 6: a racing `Dispose` can deliver Closing before it is set.
Reachable only under OOM, hence DEBT.

## DEBT-3 (PM, at close) — 7a is priced low; agree the remedy now
Priced from what D1-D6 commission at this tree's comment density (comparable: `RowDelivery`, `SA:490-589`, ~100 lines for a
simpler job): thunk + `BatchDelivery` ~95-120; `SubscribeBatches`/`UnsubscribeBatches` + shared options ~60-85; `FlBatch` +
imports ~30-35; Errors, BC-14, D5, routing ~45-55; rewrites ~30; minus ~60-70 → **net ≈ +215-245** vs declared ~+170-200.
Inside 250, so not a breach, but D10C-5/-6's overruns put it over unless nothing grows. State in Numbers whether doc comments
count (D10C-3 excluded them; D10C-5's +260 did not); the `SA:4-94` header rewrite is ~100 lines of churn. If 7a passes 250, the
remedy is pulling rung 8's deletion forward (cheap at any point: it deletes code 7a made unreachable), not another overrun ruling.

## The PM's eight questions
1. **Bridge or dead code:** dead code. Nothing constructs `Batcher`; no option, build, fallback or error path reaches it (ladder
   :84-85 says the same). Config stop-and-ask :143 does not fire; :203 (1) is met at round end and nothing ships mid-round (:278).
   Merging is still the cheaper shape → owner, as Decision 1 (d), not a separate stop (DEBT-1).
2. **Split:** (a) is sound: the refusal stays with the first batched call, no red window, (b) rightly shown to buy ~10 lines for a
   red window. Two consequence lines per option: yes. Grounding needs DEBT-1. My order: (d) ≥ (a) > (c) > (b).
3. **Memory:** sound. Move-then-import plus a `Free` that releases only an unreleased struct (P2, with a NULL-`release` leg)
   cannot double-release; Closing is the one post-OK free, d9 promises exactly one (P3); the kept batch owns its buffers. Rest: DEBT-2.
4. **D24:** adequate as the first step-3 action. The "measure a flagged cost" rule targets hot-path costs; a red D24 changes only
   D3 step 2 and stops at the owner either way (L3). Run the probe through `ImportSchema` + `ImportRecordBatch` with a dictionary
   child (the thunk's own path), not a standalone array import.
5. **FWD-1c:** lawful. R-6's chosen text is "a clear exception instead of silent loss"; the quiet transport subscription is the
   very collision :575 (6) exists for, so D5 is the same rule held for the transport's true lifetime, not a new re-answer (L3).
   Disclosed in user terms (brief :15); "until disposed" is conservative (a same-kind re-subscribe and a normal close lift it).
6. **Edge refusal:** one site (`SubscribeBatches`), asked only after the shim refused; it refuses nothing (L3, L6, BC-10); a
   server provider's `NotSupported` gets no sentence (T7).
7. **BC-14:** one site; the arrival keeps its refusal in front; batched forwards; `floor` sends −1 tick as −1, never 0. 8. **Size:** DEBT-3.
