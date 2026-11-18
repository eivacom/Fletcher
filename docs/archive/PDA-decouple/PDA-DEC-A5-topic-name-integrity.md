# PDA-DEC-A5 — Topic name integrity: the segment list is the topic

*Step-1 design, cycle 2. Amendment A5 of the nine authorised for PR #126 (owner
rulings 2026-09-03, 2026-09-04). Oracle: `docs/pubsub-interface-spec.md` §2, §3.5
(frozen; amended here under the 2026-09-03 ruling), §5.1, §12.1. Cycle-1 review:
`plans/reviews/PDA-DEC-A5-design-review.md` — 2 BLOCKERs (both in the evidence
apparatus) and A5-DEBT-1/2/3, all resolved below. `NUL` is the zero byte;
`"a<NUL>b"` is the three-byte segment `a`, zero, `b`.*

## Summary

The seam identifies a topic by a **segment list**; every provider identifies it by a
**single joined byte string**. Nothing today makes that map injective or faithful, so
distinct Fletcher topics collide into one provider topic — silently. This item makes
the map injective **by refusing, at the one door all providers already pass through,
the segment shapes that break it**, and amends §3.5 so the join is the seam's, not the
provider's discretion. No new public surface.

## Design

### The class, named

Reported defects, **one class**:

| Reported | What actually fails |
|---|---|
| embedded NUL truncates a **topic** name (XRCE `name.c_str()`, `xrce_dds_pubsub_provider.cpp:679-681`) | `name` to wire bytes is not faithful |
| the **participant** name truncates too (`:673-675`) | *the same string at a second sink* — not a second defect |
| `{"a/b"}` and `{"a","b"}` are one topic in all three providers (`segments.hpp:36-40`) | list to `name` is not injective |

Root cause, stated once: **`JoinSegments` is a map from segment lists to bytes that
is neither injective nor faithfully carried, and nothing anywhere requires it to be.**
Verification's fourth finding (an empty segment: `{""}` yields `""`, `{"a",""}` yields
`"a/"`) is the same map producing a degenerate name. Review cycle 1 found a fifth
(**A5-DEBT-1**): both DDS providers derive a companion topic `name + "/__schema"`
(`fast_dds_pubsub_provider.cpp:331,494`; `xrce_dds_pubsub_provider.cpp:720,881`), so
the accepted list `{"a","__schema"}` lands on the schema channel of `{"a"}` — a
Fletcher topic colliding with a **provider-derived** name. Same class, same door.

**The class argument is already in the tree:** `PeerSubject::RejectUnsendableTopic`
(`peer_subject.cpp:73-85`) is a *second, independently invented door* refusing exactly
the empty and `/`-bearing segment — *"nothing forbade it, so this does"*. A downstream
consumer had to write the rule the seam lacks. Per the 2026-09-04 ruling the fix is
aimed at **the map**, not at either sink or any single instance.

### The invariant

> For every accepted segment list `L`: `Join(L)` contains no NUL,
> `Split(Join(L)) == L`, and `Join(L)` is not a provider-derived companion name.
> So two distinct accepted lists are two distinct topics **in every provider**, no
> accepted name collides with a provider-derived one, and the name each provider
> hands its transport is the whole name.

### The mechanism — one door, four refusals

`internal::RequireSegments` (`pubsub/include/fletcher/pubsub/internal/segments.hpp`)
already sits on **every** provider entry point: both `JoinSegments` and
`JoinSegmentsInto` call it first, and review cycle 1 verified all twelve provider
methods plus the caller tier, `pubsub-arrow` and the conformance peer route through
those two. It gains four per-segment refusals beside the existing empty-list one,
all `PubSubError(kInvalidArgument, ...)` — **no new status; the owner alone
allocates those** (2026-09-01 ruling):

1. **a segment containing NUL** — the name would not reach the wire whole.
2. **a segment containing `/`** — the joined name would not split back to the list
   it came from.
3. **an empty segment** — a segment that names nothing; §3.5's existing rule, one
   level down (`{""}` reproduces the empty name the empty-*list* refusal forbids).
4. **a segment beginning `__`** — the namespace providers derive companion names in.
   Refusing the **prefix**, not the literal `__schema`, is designing to the class: it
   puts every present and future derived companion name out of reach.

No trimming, no case folding, no normalisation, no escaping — deliberately the same
words §4 already uses for the selector, so a binding learns one rule.

*On the `__schema` shadow:* today the collision is loud within one Fast DDS
participant (`create_topic` returns null on a type mismatch, surfacing as
`kTransportFailure`) and a silent non-match across participants. XRCE uses
`UXR_REPLACE` and **its behaviour was not resolvable from the tree** — itself a
reason to forbid rather than characterise. Nothing here asserts what XRCE does.

### Why rung 2 and not rung 1 (A5-DEBT-3)

§2 publishes the four signatures over `const std::vector<std::string>&`
(`docs/pubsub-interface-spec.md:96-101`) and §12.1 freezes §2. §2's carve-out —
*"this round may change the **types** in those signatures, and only where a type has
**no C-expressible form** (§3)"* (`:104-106`) — **does not reach** a sealed
`TopicName`, because §3.5 already gives topic segments a C form. Nobody may later
cite that carve-out as proof rung 1 was available.

The stronger point, independent of the freeze: across a boundary of
pointer-and-length pairs a bad name is **always constructible**, so a sealed type
relocates the check into a constructor rather than making anything unrepresentable —
rung 2 wearing rung 1's name. **Rung 2 at the single door is the correct rung, not a
settlement.** It is also what makes both XRCE `c_str()` sinks safe:
`uxr_buffer_create_*_bin` take `const char*` with no length form, so truncation
cannot be fixed at the sink, only prevented at the door.

### The spec amendment (frozen §3.5, authorised)

Deleted, verbatim: *"`std::vector<std::string>`, so the provider may join with any
separator."* — the sentence that licenses a provider under which `{"a/b"}` and
`{"a","b"}` are two topics. Replaced by:

- the seam computes the name once, `/`-joined, and that name **is the topic's
  identity**;
- the four refusals, stated beside the empty-list refusal;
- the invariant above;
- **(A5-DEBT-2)** a driver may map the seam-computed name into its own transport
  namespace **only injectively**. PDA-ABI may not change the seam (§1, locked
  decision 1), and a future driver on a transport where `/` is not a legal topic
  character would otherwise be non-conforming. This narrows rather than widens and
  cannot reopen the aliasing hole.

§12.1's freeze-list phrase *"§3.5 including the empty-segment refusal"* still reads
true and is **not** edited.

**Cost on the publish path.** `JoinSegmentsInto` runs per sample, so the refusal adds
one linear pass over bytes the join is about to copy anyway. It stays unconditional at
every entry point — validating only on `CreateTopic` would be a partial mode, and
`Publish` to an undeclared topic is reachable. Watched by the existing
`bench_pubsub_fanout` `BM_Publish`/`BM_CreateTopic_Redeclare`; an unexplained
regression is a report, not a redesign.

### Wire bytes

**No accepted name's wire bytes change, in any provider** — verified independently
in review across all twelve provider methods plus the caller tier. All three
providers derive the wire name from `JoinSegments`, every derived name is a pure
function of that string (`name + "/__schema"`; XRCE's participant name `= name`),
and the join itself is untouched. What changes is the accepted *set*:

| list | on the wire today | after |
|---|---|---|
| `{"a<NUL>b"}` | `a` (XRCE only; Fast DDS and loopback carry it whole) | refused |
| `{"a/b"}` | `a/b`, aliased with `{"a","b"}` (all three) | refused |
| `{"a",""}`, `{""}` | `a/`, empty (all three) | refused |
| `{"a","__schema"}` | `a/__schema`, the schema channel of `{"a"}` | refused |
| `{"a","b"}` | `a/b` | **`a/b`, unchanged** |

Pinned by `Segments.AcceptedNamesJoinToTheSameBytesAsBefore` — a byte table over
accepted lists, **including the two derived forms** (`name + "/__schema"` and the
XRCE participant name), green today and required to stay green. It is the negative
control against over-reach: any design that *escaped* rather than refused reddens it.
The in-round precedent for a pure narrowing is PDA-DEC-3's empty-**list** refusal,
which turned `JoinSegments({})` from the legal topic key `""` into a throw and landed
as frozen §3.5 text with no wire-byte stop-and-ask. Remote clients lose nothing to
rules 2 and 3 — `WsSession::SplitTopic` (`gateway/src/ws_session.cpp:149-162`) splits
on `/` and drops empty pieces — but can carry a NUL or a `__`, so rules 1 and 4 are
remotely visible; see brief decision 3.

## Corner cases forbidden

**Rung 2 — refused typed at the door, one check, no recovery, no partial mode:** the
four segment shapes above. NUL removes both XRCE truncation sinks at once; `/`
removes segment-list aliasing in all three providers and the provider-dependent
identity §0.1(2) forbids; empty removes the degenerate and trailing-separator names;
`__` removes the whole provider-derived companion namespace from reach, present and
future — not just `__schema`. Plus **a provider joining its own way**, removed by
deleting §3.5's "any separator" licence (a driver may still map the name into its
transport, but only injectively).

**Rung 1 — made unrepresentable:** **normalisation divergence** (case folding,
Unicode forms, trimming) — there is no normalisation step anywhere on the path, so
there is no place for two providers to disagree; identity is bytes, and this is
absence rather than a rule. And **these shapes over the conformance peer pipe**:
`RejectUnsendableTopic` already makes empty and `/`-bearing segments unsendable by
construction, and rule 1's shape is added there so the peer door becomes total.

**Handled residue:**

- **A name the transport itself rejects** (charset, length limits). *Why not
  forbidden?* Not an injectivity failure, and the limits differ per transport, so
  forbidding their union at the seam would refuse names that work. It already fails
  loudly through §5.1's translation.
- **The XRCE participant name being the topic name.** *Why not forbidden?* A naming
  choice inside one provider, with no divergence and no wrong answer once the door
  holds; changing it would move wire bytes for names that work today.

## Premises and stop conditions

- **P1 — every provider's wire name comes from `JoinSegments`/`JoinSegmentsInto`.**
  Verified in review: five files, fourteen call sites, twelve of them provider entry
  points. **STOP-AND-ASK if a provider derives a topic or participant name any other
  way** — the door is then not a door.
- **P2 — `pubsub_tests` compiles `segments.hpp` from the working tree**
  (`pubsub/tests/CMakeLists.txt:10-12` links the in-tree `fletcher-pubsub`), while
  the conformance harness links the **packaged** target (`CMakeLists.txt:7,116`),
  where a working-tree edit is inert (PDA-DEC-3 finding B3). Every mutation below is
  labelled with which it needs. **STOP if a mutation on `pubsub_tests` fails to fire:
  report the inert mutation, never claim a red you did not get.**
- **P3 — the gateway cannot emit an empty or `/`-bearing segment.** It *can* emit a
  NUL-bearing or `__`-prefixed one, which is why rules 1 and 4 are remotely visible.
- **P4 — Fast DDS and the loopback do not truncate** (no `c_str()` in
  `fastdds-pubsub-provider/src`, verified in review). If one appears the door already
  covers it — one line in the log, no design change.
- **P5 — no in-tree caller or test uses a refused shape.** Review searched `*.cpp`
  for empty and `/`-bearing segments and found none. **The implementer repeats the
  search for a `__`-prefixed segment; if one exists, report it before landing.**
- **P6 — platform.** Per §12.4 every green here will be a local Windows run. **A
  Linux-only difference in this refusal's behaviour is a question for the owner, not
  a local fix** (2026-09-03 ruling).

## Forcing-test mapping

Cycle 1's `TEST_P(ProviderConformance, …)` is **withdrawn (BLOCKER 1)**: it would also
run on `FastDdsCrossProcess`/`XrceCrossProcess`, where `RejectUnsendableTopic` returns
`Reply::HarnessFailure` for the empty and `/` shapes (so `refused()` is false and the
case reddens for a *harness* reason), while the NUL shape falls through to the
harness's own `JoinSegments` (`peer_subject.cpp:55,63`), which after this item throws —
a clause catching that would be green on the harness's door. **The cheaper forbid is
taken:** cross-provider evidence moves to plain `TEST` cases in the two provider
subject binaries, which already host this shape (`Registry.FastDdsResolvesAsABuiltIn`,
`Registry.XrceResolvesAsABuiltIn`) and construct a real provider directly — no
`Subject`, no `Reply`, no peer. `RejectUnsendableTopic` gains the NUL shape so the
peer door is total.

| Test | Binary | Header source | Turned green by | Red today because |
|---|---|---|---|---|
| `Segments.SegmentsThatAliasOrTruncateAreRefused` | `pubsub_tests` | **in-tree** | the four refusals in `RequireSegments` | `RequireSegments` tests `segs.empty()` and nothing else |
| `Segments.JoinIsInvertible` | `pubsub_tests` | **in-tree** | rule 2 | `Join({"a/b"})` equals `Join({"a","b"})` |
| `Segments.RefusalReachesAllFourEntryPoints` | `pubsub_tests` | **in-tree** | the door on a real `InProcessPubSubProvider`'s four methods | all four accept every refused shape today |
| `TopicNames.AmbiguousSegmentsAreRefused` | `conformance_fastdds`, `conformance_xrce` | packaged | one shared door, so all three providers agree (2026-08-31 divergence ruling) | both providers accept every refused shape today |
| `SeamVocabulary.AmbiguousTopicSegmentsAreRefusedAtEveryEntryPoint` | `conformance_seam_vocabulary` | packaged | the same door, asserted where §3.5's sibling case lives | all four in-process methods accept every refused shape today |
| `Segments.AcceptedNamesJoinToTheSameBytesAsBefore` | `pubsub_tests` | **in-tree** | — **green today, must stay green** | — it is the wire-bytes control |

**Mutations, each with the control it must redden and what it costs to run.** A4's
bar applies: every control mutation-verified, and each mutation reddens **only** its
own case. A mutation that cannot bite is labelled, never counted.

| Mutation | Reddens | Cost |
|---|---|---|
| M1 drop the NUL check | `SegmentsThatAliasOrTruncateAreRefused` | in-tree, free |
| M2 drop the `/` check | `SegmentsThatAliasOrTruncateAreRefused` **and** `JoinIsInvertible` (two independent controls) | in-tree, free |
| M3 drop the empty-segment check | `SegmentsThatAliasOrTruncateAreRefused` | in-tree, free |
| M4 move the check out of `RequireSegments` into `CreateTopic` only | `RefusalReachesAllFourEntryPoints` on the other three methods | **in-tree, free** — this is the fix for cycle 1's inert M4 |
| M5 apply the refusal in the loopback only | `TopicNames.AmbiguousSegmentsAreRefused` on both provider binaries | **requires a package rebuild of `fletcher-pubsub` and both provider packages before the mutation; record the rebuild with the result, or report it inert** |
| M6 escape `/` instead of refusing it | `AcceptedNamesJoinToTheSameBytesAsBefore` | in-tree, free |
| M7 drop the `__` prefix check | `SegmentsThatAliasOrTruncateAreRefused` (the `{"a","__schema"}` row) | in-tree, free |

**Inner loop is whole-suite, never scoped to the forcing test.** Three build trees,
each `ctest -R '.' --output-on-failure` (every entry): the `pubsub` tree, the
`gateway` tree (it splits topics) and the conformance harness — the last configured
`-DFLETCHER_CONFORMANCE_XRCE=ON`, since with it OFF the XRCE half of the
cross-provider evidence silently does not exist and the run still passes.

## Risks / Unknowns

- **The wire-bytes tripwire does not fire** — confirmed independently in review across
  all twelve provider methods, with PDA-DEC-3's empty-list refusal as in-round
  precedent that narrowing the accepted set is not a wire-byte change. The narrowing
  is still behaviour, so it goes to the owner as brief decisions 1-3; if the owner
  prefers escaping, the tripwire **does** fire and the item stops.
- **Frozen text is edited.** One sentence of §3.5 is deleted (quoted above), inside the
  2026-09-03 ruling's authorisation. Nothing else frozen is touched; §12.1 is not edited.
- **XRCE's `UXR_REPLACE` behaviour on a `__schema` collision is unresolved** and
  nothing here asserts it. The refusal removes the question rather than answering it.
- **Size.** Re-declared below, up from cycle 1's +430: the in-tree four-entry-point
  control, the second provider-binary case and the `__` refusal are all new. Roughly
  80% is test apparatus and docs; the mechanism is four `if`s in one function.
- **No coexistence window**, no shim, no dual path, no deprecation — and no debt:
  A5-DEBT-1/2/3 are all folded into this revision.

## Files-to-touch

- `pubsub/include/fletcher/pubsub/internal/segments.hpp` — the four refusals in
  `RequireSegments`; doc comment rewritten to state the invariant.
- `docs/pubsub-interface-spec.md` §3.5 — the amendment above.
- `pubsub/tests/test_segments.cpp` — **new**; the refusal table, the invertibility
  oracle, the byte table, and the four-entry-point control on a real
  `InProcessPubSubProvider`. Plus one source line in `pubsub/tests/CMakeLists.txt`.
- `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp` — the
  four-entry-point case, beside the empty-list one; `src/peer_subject.cpp` — add NUL
  to `RejectUnsendableTopic` so the peer door is total;
  `subjects/fastdds_main.cpp` and `subjects/xrce_main.cpp` —
  `TopicNames.AmbiguousSegmentsAreRefused`; `README.md` — the new rows and the peer
  exclusion, recorded rather than implied.
- `plans/PDA-decouple-progress-log.md` — the A5 entry with the mutation results,
  including whether M5's package rebuild was run.

## Files-to-delete

- **§3.5's sentence** *"so the provider may join with any separator"* — replaced by
  the normative seam-owned join plus the injective-driver-mapping clause.
- **`RequireSegments`'s current doc comment** — replaced.
- **No source file, type, shim, config key or test is retired.** Justification: this
  item tightens a check that already lives in one place on every entry point, so there
  is no second mechanism, old path or bridge to delete; and
  `EmptyTopicSegmentListIsRefusedAtEveryEntryPoint` is kept, because the empty *list*
  is a different rule from the empty *segment*.

## Numbers

Expected **+535 / -15** (net +520), re-declared from cycle 1's +430/-15. Composition:
`segments.hpp` +30; spec §3.5 +35/-10; `test_segments.cpp` +180; the in-tree
four-entry-point control +60; the seam-vocabulary case +90; the two provider-binary
cases +45 each; `peer_subject.cpp` +5; README and progress log +45. Contingency,
unchanged: +120 if the provider-binary cases need wiring of their own. **New public
surface: 0** — `internal/` is not public surface, no new `PubSubStatus`, no signature
change.
