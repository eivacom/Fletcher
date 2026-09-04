# PDA-DEC-A5 — Topic name integrity: the segment list is the topic

*Step-1 design. Round PDA-DEC, amendment A5 of the nine authorised for PR #126
(owner rulings 2026-09-03, 2026-09-04). Oracle: `docs/pubsub-interface-spec.md`
§3.5 (frozen; amended here under the 2026-09-03 absorption ruling), §5.1, §12.1.*

*Notation: `NUL` is the zero byte; a segment written `"a<NUL>b"` is the
three-byte segment `a`, zero, `b`.*

## Summary

The seam identifies a topic by a **segment list**; every provider identifies it by
a **single joined byte string**. Nothing today makes that map injective or
faithful, so distinct Fletcher topics collide into one provider topic — silently.
This item makes the map injective **by refusing, at the one door all providers
already pass through, the three segment shapes that break it**, and amends §3.5 so
the join is the seam's, not the provider's discretion. No new public surface.

## Design

### The class, named

Three reported defects, **one class**:

| Reported | What actually fails |
|---|---|
| embedded NUL truncates a **topic** name (XRCE `name.c_str()`, `xrce_dds_pubsub_provider.cpp:679-681`) | `name` to wire bytes is not faithful |
| the **participant** name truncates too (`:673-675`) | *the same string at a second sink* — not a second defect |
| `{"a/b"}` and `{"a","b"}` are one topic in all three providers (`segments.hpp:36-40`) | list to `name` is not injective |

Root cause, stated once: **`JoinSegments` is a map from segment lists to bytes that
is neither injective nor faithfully carried, and nothing anywhere requires it to
be.** Verification's fourth finding (an empty individual segment: `{""}` yields
`""`, `{"a",""}` yields `"a/"`) is the same map producing a degenerate name, and
belongs here too. Per the 2026-09-04 ruling, the fix is aimed at **the map** — the
object the guarantee is about — not at either sink and not at either instance.

### The invariant

> For every accepted segment list `L`, `Join(L)` contains no NUL, and
> `Split(Join(L)) == L`. Therefore two distinct accepted lists are two distinct
> topics **in every provider**, and the name each provider hands its transport is
> the whole name.

### The mechanism — one door, three refusals

`internal::RequireSegments` (`pubsub/include/fletcher/pubsub/internal/segments.hpp`)
already sits on **every** provider entry point, because both `JoinSegments` and
`JoinSegmentsInto` call it first and all twelve provider methods plus the caller
tier, `pubsub-arrow` and the conformance peer route through those two. It gains
three per-segment refusals beside the existing empty-list one, all
`PubSubError(kInvalidArgument, ...)` — **no new status; the owner alone allocates
those** (2026-09-01 ruling):

1. **a segment containing NUL** — the name would not reach the wire whole.
2. **a segment containing `/`** — the joined name would not split back to the list
   it came from.
3. **an empty segment** — a segment that names nothing; §3.5's existing rule, one
   level down (`{""}` reproduces the empty name the empty-*list* refusal forbids).

No trimming, no case folding, no normalisation, no escaping — deliberately the
same words §4 already uses for the selector, so a binding learns one rule.

### Why rung 2 and not rung 1

Rung 1 (a sealed `TopicName` type, so a bad name cannot be constructed) would
change the parameter types of §2's method set. §12.1 freezes §2, and §11 names
"any change to the interface's method set" as frozen. **Rung 2 at the single door
is the highest rung available**, and it is what makes both XRCE `c_str()` sinks
safe: XRCE's `uxr_buffer_create_*_bin` take `const char*` with no length form, so
truncation cannot be fixed at the sink — only prevented at the door.

### The spec amendment (frozen §3.5, authorised)

Deleted, verbatim: *"`std::vector<std::string>`, so the provider may join with any
separator."* — the sentence that licenses a provider under which `{"a/b"}` and
`{"a","b"}` are two topics. Replaced by: the seam computes the name once,
`/`-joined, and every provider uses it; plus the three refusals stated beside the
empty-list refusal; plus the invariant above. §12.1's freeze-list phrase *"§3.5
including the empty-segment refusal"* still reads true and is **not** edited.

### Cost on the publish path

`JoinSegmentsInto` runs per sample. The refusal adds one linear pass over bytes the
join is about to copy anyway; it is unconditional at every entry point (validating
only on `CreateTopic` would be a partial mode — `Publish` to an undeclared topic is
reachable). Checked, not asserted, by the existing `bench_pubsub_fanout`
`BM_Publish` / `BM_CreateTopic_Redeclare`; a regression the implementer cannot
explain is a report, not a redesign.

### Wire bytes

**No accepted name's wire bytes change, in any provider.** All three derive the
wire name from `JoinSegments`, and the join itself is untouched. What changes is
the accepted *set*: three shapes that today reach the wire stop reaching it at all
and are refused with `kInvalidArgument` instead —

| list | on the wire today | after |
|---|---|---|
| `{"a<NUL>b"}` | `a` (XRCE only; Fast DDS and loopback carry it whole) | refused |
| `{"a/b"}` | `a/b`, aliased with `{"a","b"}` (all three) | refused |
| `{"a",""}`, `{""}` | `a/`, empty (all three) | refused |
| `{"a","b"}` | `a/b` | **`a/b`, unchanged** |

Pinned by `Segments.AcceptedNamesJoinToTheSameBytesAsBefore` — a byte table over
accepted lists, green today and required to stay green. It is the negative control
against over-reach: any design that *escaped* rather than refused reddens it.

Remote clients lose nothing: `WsSession::SplitTopic`
(`gateway/src/ws_session.cpp:149-162`) drops empty pieces and splits on `/`, so it
cannot emit a `/`-bearing or empty segment. It **can** emit a NUL-bearing one (a
JSON string may carry an escaped zero byte), so rule 1 closes a remotely reachable
silent-collision path and rules 2-3 narrow only in-process C++ callers and future
bindings.

## Corner cases forbidden

**Rung 2 — refused typed at the door, one check, no recovery, no partial mode:**

- A segment containing NUL. Removes both XRCE truncation sinks at once.
- A segment containing the separator. Removes segment-list aliasing in all three
  providers, and removes the provider-dependent identity §0.1(2) forbids.
- An empty segment. Removes the degenerate empty and trailing-separator names.
- **A provider joining its own way** — removed by deleting §3.5's "any separator"
  licence and by the fact that all five join sites already call the shared helper.
  A provider that invented its own join would have to duplicate the helper; the
  cross-provider case below runs on every subject, so it would be caught.

**Rung 1 — made unrepresentable:**

- **Normalisation divergence** (case folding, Unicode forms, trimming): there is no
  normalisation step anywhere on the path, so there is no place for two providers
  to disagree. Identity is bytes. This is absence, not a rule.

**Handled residue:**

- **A name the transport itself rejects** (charset, length limits). *Why not
  forbidden?* Because it is not an injectivity failure and the limits differ per
  transport; forbidding a union of them at the seam would refuse names that work.
  It already fails loudly through §5.1's translation, and stays that way.
- **The XRCE participant name being the topic name.** *Why not forbidden?* It is a
  naming choice inside one provider with no divergence and no wrong answer once the
  door check holds; changing it would move wire bytes for names that work today,
  which is the tripwire this item must not trip.

## Premises and stop conditions

- **P1 — every provider's wire name comes from `JoinSegments`/`JoinSegmentsInto`.**
  Verified across the five product call sites. **STOP-AND-ASK if the implementer
  finds a provider deriving a topic or participant name any other way** — the door
  is then not a door, and this design does not cover it.
- **P2 — `pubsub_tests` compiles `segments.hpp` from the working tree** (it links
  the in-tree `fletcher-pubsub` target). PDA-DEC-3 finding B3 recorded that the
  *conformance* harness links the **packaged** header from the Conan cache, so a
  working-tree edit there is inert. **STOP if a mutation on `pubsub_tests` also
  fails to fire: report the inert mutation, do not claim a red you did not get**,
  and say the guard is falsifiable only after a package rebuild.
- **P3 — the gateway cannot emit an empty or `/`-bearing segment.** If that changes,
  the narrowing becomes remote-visible and is an owner question, not a local fix.
- **P4 — Fast DDS and the loopback do not truncate** (zero `c_str()` on a name path
  in `fastdds-pubsub-provider/src`). If one appears, the door check already covers
  it — no design change, one line in the log.
- **P5 — platform.** Per spec §12.4 every green here will be a local Windows run.
  **A Linux-only difference in this refusal's behaviour is a question for the owner,
  not a local fix** (2026-09-03 ruling).

## Forcing-test mapping

All new cases are behaviour names, not `ClauseN` names, so §7's frozen clause
wording is untouched.

| Test | Binary | Turned green by | Red today because |
|---|---|---|---|
| `Segments.SegmentsThatAliasOrTruncateAreRefused` | `pubsub_tests` | the three refusals in `RequireSegments` | `RequireSegments` tests `segs.empty()` and nothing else |
| `Segments.JoinIsInvertible` | `pubsub_tests` | rule 2 | `Join({"a/b"})` equals `Join({"a","b"})` |
| `SeamVocabulary.AmbiguousTopicSegmentsAreRefusedAtEveryEntryPoint` | `conformance_seam_vocabulary` | the door sitting on all four methods | all four in-process methods accept all three shapes today |
| `ProviderConformance.AmbiguousTopicSegmentsAreRefused/<subject>` | `conformance_inprocess`, `conformance_inprocess_carrying`, `conformance_fastdds`, `conformance_xrce` | one shared door, so all three providers agree (2026-08-31 divergence ruling) | every subject accepts all three shapes today |
| `Segments.AcceptedNamesJoinToTheSameBytesAsBefore` | `pubsub_tests` | — **green today, must stay green** | — it is the wire-bytes control |

**Mutations that must redden a named control** (the implementer applies and reverts
each and records the result — A4's standard):

| Mutation | Reddens |
|---|---|
| M1 drop the NUL check | `SegmentsThatAliasOrTruncateAreRefused`; the conformance cases after a package rebuild |
| M2 drop the `/` check | `SegmentsThatAliasOrTruncateAreRefused` **and** `JoinIsInvertible` (two independent controls) |
| M3 drop the empty-segment check | `SegmentsThatAliasOrTruncateAreRefused` |
| M4 move the check out of `RequireSegments` into `CreateTopic` only | `SeamVocabulary....AtEveryEntryPoint` on the other three methods |
| M5 apply the refusal in the loopback only | `ProviderConformance..../FastDdsLocal` and `/XrceLocal` |
| M6 escape `/` instead of refusing it | `AcceptedNamesJoinToTheSameBytesAsBefore` |

**Inner loop is whole-suite, never scoped to the forcing test.** Three build trees,
each run with `ctest -R '.' --output-on-failure` (that is, every entry): the
`pubsub` component tree, the `gateway` component tree (it splits topics), and the
conformance harness. **The harness must be configured with
`-DFLETCHER_CONFORMANCE_XRCE=ON`** — with it OFF the cross-provider case silently
loses the two subjects that carry the original defect, and the run still passes.

## Risks / Unknowns

- **The wire-bytes tripwire does not fire, but it comes close.** No accepted name's
  bytes change and a test pins that; three shapes stop being emitted at all. That
  narrowing is routed to the owner as Brief decision 1 rather than designed through.
  If the owner prefers escaping, the tripwire **does** fire and the item stops.
- **Frozen text is edited.** One sentence of §3.5 is deleted (quoted above). This is
  inside the 2026-09-03 absorption ruling's authorisation for this amendment.
  Nothing else frozen is touched; §12.1 is not edited.
- **Size.** Verification called this "two or three checks in `RequireSegments`";
  A4 was called "an in-flight count in `Fanout`" and landed at +1657. The product
  change here really is small; the estimate below is roughly 80% test apparatus and
  docs, which is where growth would come from. If the cross-provider case turns out
  to need subject wiring of its own, add about +120.
- **No coexistence window.** No shim, no dual path, no deprecation: the refusal is
  live the moment it lands. Nothing here is scheduled for deletion in a later stage.
- **Debt carried:** none new. The `JoinSegmentsInto`/`JoinSegments` duplication
  (an existing design-debt line, kept for a measured 2.5%) is untouched — the
  *check* lives once, in `RequireSegments`, which both call.

## Files-to-touch

- `pubsub/include/fletcher/pubsub/internal/segments.hpp` — the three refusals in
  `RequireSegments`; doc comment rewritten to state the invariant.
- `docs/pubsub-interface-spec.md` §3.5 — the amendment above.
- `pubsub/tests/test_segments.cpp` — **new**; the three refusal cases, the
  invertibility oracle, the byte table.
- `pubsub/tests/CMakeLists.txt` — one source line.
- `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp` — the
  four-entry-point case, beside the empty-list one.
- `integration-tests/pubsub-conformance/src/clauses.cpp` — the cross-provider case.
- `integration-tests/pubsub-conformance/README.md` — two rows in the case table.
- `plans/PDA-decouple-progress-log.md` — the A5 entry with the mutation results.

## Files-to-delete

- **§3.5's sentence** *"so the provider may join with any separator"* — replaced by
  the normative seam-owned join.
- **`RequireSegments`'s current doc comment** — replaced.
- **No source file, type, shim, config key or test is retired.** Justification: this
  item tightens a check that already lives in exactly one place and is already on
  every entry point; there is no second mechanism, no old path and no bridge to
  delete. `EmptyTopicSegmentListIsRefusedAtEveryEntryPoint` is kept rather than
  absorbed — it guards the empty *list*, a different rule from the empty *segment*,
  and deleting it would lose the four-entry-point routing check.

## Numbers

Expected **+430 / -15** (net +415). Composition: product `segments.hpp` about +25;
spec §3.5 about +30/-10; `pubsub/tests/test_segments.cpp` about +180; the
conformance seam-vocabulary case about +90; the cross-provider case about +60;
README plus progress log about +45.
**New public surface: 0** (`internal/` is not public surface; no new
`PubSubStatus` value; no signature change).
