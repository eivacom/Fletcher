# D10C-7 — design review (step 2)

## Cycle 1 — APPROVE-WITH-DEBT(3), no BLOCKER
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **658 lines, 51 `## ` entries** (counted), last heading "## 2026-11-16 —
D10C-7 pre-design: T30 in d10; the preview-consumer red window" (:650). No tier-1 collision; R-6's chosen text read for D5.
**Tree claims verified:** `h:874-884, 1020-1023, 1071, 1077-1081, 1102-1129`; `SA:175, 254-280, 379-385, 438-448, 471, 592`;
`Sub:283, 425-454, 569-574, 717-742` (`_disposed` set before destroy); `subscriber.cpp:720-745` (quiet only under
`InsideDeliveryOn(provider)`); `subscriber_arrow.cpp:529-534` (a batched close calls the seam's `Unsubscribe` on the caller's
thread, so `quiet` covers both kinds); `FletcherCodec.cs:266-279`; `DecodedSchema.cs:74,100`; `Directory.Build.props:26-28`.

**DEBT-1 (PM, brief)** — Decision 1 lacked (d) "merge D10C-8's deletion" (net ≈ −160 to −310, no split, no dead-code window;
"the PM ruled it separate" is not owner authority under the lean default); "(a) — the round purpose lands whole" was false while
`Batcher` stayed; (c)'s :643 citation omitted that the ruling kept 250 for later stages. The C# design was unaffected.
**DEBT-2 (step 3)** — the `GCHandle` free and the claim release must key on "native refused", not a catch-all: a post-OK throw
(OOM building `Subscription`/`SchemaArrival`, or `_claimed[id]`) would free a `ctx` native still holds (use-after-free; batches
silently misrouted or swallowed) and drop a live subscription's claim. One frame frees; null-check `delivery.Subscription` at Closing.
**DEBT-3 (PM)** — 7a priced ≈ +215-245 net (comparable: `RowDelivery`, `SA:490-589`) vs ~+170-200 declared; counting rule unstated.

**Answers.** (1) Dead code, not a bridge: nothing constructs `Batcher`, no option, build or error path reaches it; config :143 does
not fire. Merging is the cheaper shape and the owner's call (DEBT-1). (2) (a)'s cut is sound, consequences paired; my order
(d) ≥ (a) > (c) > (b). (3) Memory sound: move-then-import, a `Free` that releases only an unreleased struct (P2), Closing the one
post-OK free (P3); residue DEBT-2. (4) D24 as the first step-3 action is adequate (a capability, not a hot-path cost; red stops at
the owner, L3); probe the thunk's own import path. (5) FWD-1c lawful: R-6's "a clear exception instead of silent loss" covers the
quiet transport subscription; disclosed (brief). (6) Edge sentence: one site, after the shim refused; it refuses nothing (L3, L6,
BC-10). (7) BC-14: one site; `floor` sends −1 tick as −1, never 0.

## Cycle 2 — APPROVE-WITH-DEBT(1), no BLOCKER (narrow: D7, the `accepted` flag, the lane, size)
**Proof of read:** ledger re-read: **667 lines, 52 entries**, last "## 2026-11-16 — D10C-7: the switch and the deletion are one
stage" (:660). DEBT-1..3 resolved: (d) ruled and the brief rewritten; D2's `accepted` and D3 step 6; the counting rule (brief :51-52).
**(1) D7 against ladder rung 8 and audit MUST-DELETE: matches.** Deleted set = rung 8's (`Batcher`, the ceiling constant/ctor/field,
`DecodeBatchFramed`, the framed import; d9 keeps the entry point, d10 Q8); six tests = audit's five + T40 (audit :23); BC-9 narrowed.
Verified: no test, script or integration test calls `DecodeBatchFramed` or the import; `Batcher`'s other dependencies
(`AppendDelivered`, `FletcherCodec`, `ReportAbsorbed`, `BatchStatus`) keep live callers, so nothing is orphaned; afterwards the only
`Stopwatch` in `dotnet/src/Fletcher` is `SchemaArrival.cs:183`, and no `Timer` remains. Both scans are red today.
**(2) `accepted`: sound.** Two free sites, exclusive by construction: `catch when (!accepted)` in `SubscribeBatched` (native never
accepted, so it never delivers, `h:1081`) and Closing (only after acceptance, exactly once, P3). A post-OK throw frees nothing and
keeps the claim; the live subscription ends at `Dispose`, whose Closing frees: no double free, no leak past the object (disclosed,
D2). A captured local passed `ref` inside the lambda is legal C#. NIT fixed in D2: `SubscribeClaimed` read "unchanged".
**(3) The lane in-stage: right.** Audit M2: the batched options half proves nothing on edge. It copies the binding-abi lane's server
pass; its YAML is proven only by a CI run, so that run ID joins D10C-4 DEBT-5's owed list (:408 (2)).
**(4) Size: honest.** The sums reconcile; the switch is cycle 1's pricing; the deletion is ≈ −505 counted (`Batcher` holds ~30 `///`
lines). The ceiling binds net, and the owner's (d) text disclosed "~300 written, ~600 deleted": no note beyond reporting adds
against +320-380 at close, separately from net.
**C2-1 (DEBT, step 3) — scope the scans to the class, not to one file.** L4 is binding-wide; both scans see only `SubscriberArrow`,
so a batcher re-grown in another file passes. Cheaper and wider: no `System.Threading.Timer` reference anywhere in `Eiva.Fletcher.dll`
(none remains); `Stopwatch` refused in every type but a named allow-list {`SchemaArrival`}; the source scan over `dotnet/src/Fletcher`
with the same list. The packed leg takes its input from the pack step, the build output by default: never a `Skip` (config :145).
