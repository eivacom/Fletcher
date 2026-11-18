# PDA-DEC-3 — Stage Brief (2026-09-01)

**In one sentence:** the pub/sub boundary can now carry payload bytes it did not allocate
— a protocol's own receive memory reaches the subscriber untouched — and learning a
topic's shape and reporting a failure become things a C, C# or Rust caller can do without inventing anything.
**Forcing test:** `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` — a protocol hands over sidecar bytes **where they lie**, no copy; plus the copy guard, whose "exactly one copy" pin drops to zero.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Sidecar (attachment) payload | CHANGED | an owned span, so bytes a protocol already holds cross without being duplicated |
| Waiting for a topic's shape | CHANGED | a deadline-bearing wait replaces a C++-only future, and now reports four distinct answers — arrived, not yet, this transport has no shapes, and no shape will ever arrive |
| Failure reporting | NEW | one error carrying a stable numbered cause, so every protocol reports the same cause the same way |
| Converting a topic's shape to an Arrow schema | NEW | the one safe conversion becomes public, so no caller writes the unsafe one |
| In-process loopback | CHANGED | gains a shape-carrying form alongside today's "client brings its own"; see Decision 1 for the live-subscription rule |

## Deleted
- The C++-future way of learning a topic's shape, in all three layers → the deadline-bearing wait. **Narrowed, disclosed:** waiting forever silently is gone, and the Arrow-facing layer now hands back the raw shape plus the new conversion rather than an Arrow schema directly.
- The old sidecar-payload type and every place that built one → the owned span (deliberately source-breaking, so no site is missed); the copy guard's "exactly one copy" assertion → "no copies".

## Corner cases forbidden vs handled
**Forbidden:** payload bytes with no owner keeping them alive; mutable bytes after they
cross; a shape announcement made twice or never (an abandoned subscription is told "no
shape will arrive" rather than hanging); **reporting that as "this transport has no
shapes"**, which demands the opposite handling; a subscription whose shape mode changes
mid-stream; a failure with no numbered cause, or one silently renumbered; an empty topic name; any C boundary.
**Handled:** a callback that throws (unforbiddable in the language; the protocol absorbs it rather than dying); a shape not yet arrived, and a subscription torn down before one arrives (both legitimate, so both are reported, not refused); one per-message copy in Fast DDS's borrowed-sample read path.

## Decisions for you
1. **A gateway client that subscribes before anyone announces a topic's shape gets no
   shape — and today, once someone announces it, that same live subscription silently
   starts receiving one, which the contract forbids. What should happen instead?**
   (a) hold: that subscription keeps getting no shape until the client resubscribes ·
   (b) allow the switch and loosen the contract to permit it.
   **Recommendation:** (a); under (b) a client decodes one stream two ways with no
   signal — silent wrong data, not an error. **Default:** (a).
   *Background: the loopback caches a `CreateTopic` schema and hands it to whatever
   subscription is live; §7 clause 1's "never mix" becomes per-subscription.*
2. **What does an application see when a publish or subscribe fails?**
   (a) one error carrying a stable numbered cause, identical across protocols ·
   (b) today's assorted standard errors plus a prose map.
   **Recommendation:** (a); (b) cannot be mirrored by two independent language bindings
   without drift, which is what this round exists to stop. **Default:** (a) — messages
   unchanged, branching on error *type* moves to the code.
3. **Keep the C++-only way of waiting for a topic's shape as a convenience?**
   (a) no — one waiting mechanism, the same one a C#/Rust app uses · (b) both.
   **Recommendation:** (a); two mechanisms drift, and (b) leaves the binding path
   untested. **Default:** (a) — ~10 call sites updated here, and Arrow-facing callers also convert the shape themselves via the new public conversion.

## Risks accepted / debt carried
- One per-message copy remains in Fast DDS's borrowed-sample read path, in the gap the spec assigns to the later zero-copy-receive stage — a reduction, not a regression, and the copy guard's "no copies" claim stays scoped to the interface, never a transport.
- Largest non-guard stage of the round, landing as one change: splitting it would leave two ways to wait for a shape coexisting for a stage.
- Two false-green traps this round already hit are named for the implementer: a stale build setting that drops two test subjects, and a cached package that makes a "run the tests" flag a no-op.

## Numbers
Declared net lines: +950 / −350 · new public surface: 7 (5 retired simultaneously) · design cycles used: 2/2

---
*As landed (2026-09-01):* +3338 / −1113 code (excl. `plans/`) vs declared +950/−350;
the code reviewer counted it and found ~1071 genuinely new code lines — real scope,
not padding. Public surface **8**, ratified: `PubSubStatusName` and
`EnvelopeAttachmentCount` were pushed to `internal/` at step 4, the latter deleting an
unchecked invariant with it. 2 design cycles, 2 fix cycles, 3 implementer launches,
6 owner touches. Full suite green; `pubsub-conformance` 62/62 with XRCE on.
