# PDA-DEC-3 — Stage Brief (2026-09-01)

**In one sentence:** the pub/sub boundary can now carry payload bytes it did not
allocate — a protocol's own receive memory reaches the subscriber untouched — and
learning a topic's shape and reporting a failure become things a C, C# or Rust
caller can do without inventing anything.
**Forcing test:** `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` — a
protocol hands over sidecar bytes **where they lie**, no copy; plus the copy guard, whose "exactly one copy" pin drops to zero.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Sidecar (attachment) payload | CHANGED | an owned span, so bytes a protocol already holds cross without being duplicated |
| Waiting for a topic's shape | CHANGED | a wait with a stated deadline replaces a C++-only future; the same wait works from any language |
| Failure reporting | NEW | one error carrying a stable numbered cause, so every protocol reports the same cause the same way |
| In-process loopback | CHANGED | can be created in a shape-carrying form alongside today's "client brings its own" form |

## Deleted
- The C++-future way of learning a topic's shape, in all three layers → the deadline-bearing wait. **Narrowed, disclosed:** waiting forever silently is gone.
- The old sidecar-payload type and every place that built one → the owned span (deliberately source-breaking, so no site is missed); the copy guard's "exactly one copy" assertion → "no copies".

## Corner cases forbidden vs handled
**Forbidden:** payload bytes with no owner keeping them alive; mutable bytes after
they cross; a shape announcement made twice or never (an abandoned subscription
reports "no shape will arrive" rather than hanging); a transport that sometimes
carries shapes and sometimes not; a failure with no numbered cause, or one
silently renumbered; an empty topic name; any C boundary.
**Handled:** a callback that throws (unforbiddable in the language; the protocol
absorbs it rather than dying); a shape not yet arrived (asynchrony is the contract); one per-message copy in Fast DDS's borrowed-sample read path.

## Decisions for you
1. **Does the gateway's in-process loopback start carrying topic shapes?**
   (a) no — today's "the client brings its own shape" behaviour stays, carrying is
   opt-in · (b) yes, always — clients must declare a topic before publishing.
   **Recommendation:** (a); (b) breaks every gateway client. **Default:** (a).
   *Background: the carrying form is the new sixth conformance subject.*
2. **What does an application see when a publish or subscribe fails?**
   (a) one error carrying a stable numbered cause, identical across protocols ·
   (b) today's assorted standard errors plus a prose map.
   **Recommendation:** (a); (b) cannot be mirrored by two independent language
   bindings without drift, which is what this round exists to stop. **Default:**
   (a) — messages unchanged, branching on error *type* moves to the code.
3. **Keep the C++-only way of waiting for a topic's shape as a convenience?**
   (a) no — one waiting mechanism, the same one a C#/Rust app uses · (b) both.
   **Recommendation:** (a); two mechanisms drift, and (b) leaves the binding path
   untested. **Default:** (a), at ~10 call sites updated here.

## Risks accepted / debt carried
- Largest non-guard stage of the round; the design carries a split shape, and a
  recommendation not to use it.
- One per-message copy remains in Fast DDS's borrowed-sample read path, in the gap
  the spec assigns to the later zero-copy-receive stage. Raised as a tripwire.
- Two false-green traps this round already hit are named for the implementer: a
  stale build setting that drops two test subjects, and a cached package that makes a "run the tests" flag a no-op.

## Numbers
Declared net lines: +900 / −330 · new public surface: 6 (5 retired to pay) · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
