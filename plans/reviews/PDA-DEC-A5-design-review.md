# PDA-DEC-A5 — design review, cycle 1 of 2

Design: `plans/PDA-DEC-A5-topic-name-integrity.md` (252 lines, `22ad8b3`).
Oracle: `docs/pubsub-interface-spec.md` §2, §3.5, §5.1, §7, §12.1.
Rulings ledger read in full first: `grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **39**.

**Verdict: NEEDS-REWORK — 2 BLOCKERs, 3 DEBT.** Both BLOCKERs are in the evidence,
not the mechanism. The mechanism, the class argument, the spec amendment and the
rung choice all survive independent checking; the two controls that carry the
item's distinguishing claims cannot redden as specified.

---

## BLOCKER 1 — the cross-provider forcing case cannot run on the cross-process subjects, and on one shape it can pass vacuously

`ProviderConformance` is parameterised over **two** subjects per DDS provider:
`FastDdsLocal` + `FastDdsCrossProcess` (`subjects/fastdds_main.cpp:52,57`) and
`XrceLocal` + `XrceCrossProcess` (`subjects/xrce_main.cpp:811,818`). A new
`TEST_P(ProviderConformance, AmbiguousTopicSegmentsAreRefused)` therefore runs on
the peer subjects too. It cannot work there:

- `PeerSubject::RejectUnsendableTopic` (`src/peer_subject.cpp:77-85`) **already**
  refuses any segment that is empty or contains `/`, space or tab, returning
  `Reply::HarnessFailure`. `Reply::refused()` is false for a harness failure by
  construction (`include/fletcher/conformance/subject.hpp:69-71,86`), and
  `subject.hpp:126-127` says a clause asserting a refusal "must test `refused()`,
  never merely 'not ok'". So the `/` row and the empty-segment row go **red on
  every cross-process subject for a harness reason**, whatever the provider does.
- The NUL row is worse. `RejectUnsendableTopic` does not test for NUL, so
  `DeclareTopic`/`PublishRow` fall through to `internal::JoinSegments(topic)` at
  `peer_subject.cpp:55` / `:63` — *the harness's own call* — which after this item
  **throws** out of a method declared to return a `Reply`. A clause that catches
  that and reads it as a refusal is green on the harness's door, not the
  provider's: a vacuous pass on the one case that carries the 2026-08-31
  divergence ruling.

**Acceptable fix (cheapest):** assert the cross-provider case on the **local**
subjects only, and say in the design and the README that the peer protocol makes
these three shapes unrepresentable by construction — `RejectUnsendableTopic` is
already that forbid, and forbidding here is cheaper than teaching the pipe to
carry them. If the case must also run on peers, make the harness door total first
(add the NUL test to `RejectUnsendableTopic`) and assert `kHarnessFailure` there,
never `refused()`. Do **not** widen the peer line protocol.

## BLOCKER 2 — M4 and M5, the two mutations that carry "one door, all four entry points" and "all three providers agree", cannot redden

P2 is correct and I verified both halves:

- `pubsub_tests` links the **in-tree** `fletcher-pubsub`: `pubsub/CMakeLists.txt:11-25`
  builds it from `src/*.cpp` with a `BUILD_INTERFACE` include dir on the working
  tree, and `pubsub/tests/CMakeLists.txt:10-12` links that target. A working-tree
  edit to `segments.hpp` reaches `pubsub_tests` **and** `in_process_provider.cpp`.
- The conformance harness links the **packaged** target:
  `integration-tests/pubsub-conformance/CMakeLists.txt:7` `find_package(fletcher-pubsub
  CONFIG REQUIRED)`, `:116` `fletcher-pubsub::fletcher-pubsub`. PDA-DEC-3 finding B3
  (`plans/reviews/PDA-DEC-3-compliance.md:368-394`) stands.

The design knows this — M1's row says "the conformance cases **after a package
rebuild**". But **M4** ("move the check out of `RequireSegments` into `CreateTopic`
only") names only `SeamVocabulary...AtEveryEntryPoint`, and **M5** ("apply the
refusal in the loopback only") names only `ProviderConformance/FastDdsLocal` and
`/XrceLocal` — all conformance binaries, all inert against a working-tree edit.
P2's stop condition only fires on a `pubsub_tests` miss, so the honest outcome is
that the item ships with **no live control** on either of the two claims that
distinguish it from a one-provider patch. A4's bar is "every control
mutation-verified; each mutation reddens only its own case"
(`plans/PDA-decouple-progress-log.md:992`), and a control that cannot redden is
the failure this round has logged repeatedly.

**Acceptable fix (cheapest):** give M4 an **in-tree** control —
`src/in_process_provider.cpp` is inside the in-tree `fletcher-pubsub` and
`pubsub/include/fletcher/pubsub/in_process_provider.hpp` is already on
`pubsub_tests`' include path, so a `Segments.RefusalReachesAllFourEntryPoints`
case in `pubsub_tests` driving a real `InProcessPubSubProvider` reddens M4 with no
package rebuild (it is the existing `SeamVocabulary.EmptyTopicSegmentListIsRefusedAtEveryEntryPoint`
body, `src/seam_vocabulary.cpp:328-358`, moved one tree over). For M5, annotate
the row "requires a package rebuild of `fletcher-pubsub` and the provider packages
before the mutation; record the rebuild with the result, or report it inert".
Budget +≈60 lines, already inside the declared contingency.

---

## Rulings requested

### Claim 1 — "one class, not three": holds, with one correction the design already makes

Verified at the tree. `xrce_dds_pubsub_provider.cpp:634` assigns
`std::string name = internal::JoinSegments(topic_segments)`; `:673-675`
(`uxr_buffer_create_participant_bin(..., name.c_str(), ...)`) and `:679-681`
(`uxr_buffer_create_topic_bin(..., name.c_str(), ...)`) are `c_str()` on **that
same local**, in the same function. The participant case is genuinely the same
string at a second sink, not a second defect; the class argument does not weaken.

The NUL case and the `/` case are one property — non-injectivity of *segment list
→ wire name* (two distinct accepted lists landing on one wire name). The
empty-segment case is **not** an injectivity failure (`{"a",""}` → `"a/"` collides
with nothing accepted), and the design says exactly that and routes it separately
as brief decision 2. That is honest bundling, not a tidy story.

Supporting evidence the design does not use, and should:
`PeerSubject::RejectUnsendableTopic` (`src/peer_subject.cpp:73-85`) is a **second,
independently invented door** refusing precisely the empty and `/`-bearing segment,
with the comment "nothing forbade it, so this does". A downstream consumer already
had to write the rule the seam lacks. That is the class argument, in the tree.

### Claim 2 — the wire-bytes tripwire does NOT fire. Verified independently; the PM does not need to carry it

Every wire name derives from `JoinSegments`/`JoinSegmentsInto`, checked by search,
not by argument: `in_process_provider.cpp:180,256,287,321`;
`fast_dds_pubsub_provider.cpp:289,383,429,533`;
`xrce_dds_pubsub_provider.cpp:634,774,829,967` (twelve provider methods);
`publisher.cpp:36`; `subscriber.cpp:445`; `pubsub-arrow` ×4. Every *derived* name
is a pure function of that string and is untouched: `name + "/__schema"`
(`fast_dds_pubsub_provider.cpp:331,494`; `xrce_dds_pubsub_provider.cpp:720,881`)
and XRCE's participant name (`= name`). The join is unchanged and no escaping or
normalisation is proposed. **No accepted name's bytes move on any provider.**

The change is a pure narrowing of the accepted set, and the round already has a
precedent for that shape: PDA-DEC-3's empty-**list** refusal turned
`JoinSegments({})` from the legal topic key `""` into a throw
(`src/seam_vocabulary.cpp:318-323`, `README.md:328`) and landed as frozen §3.5 text
with no wire-byte stop-and-ask. Locked decisions 11 and 13 are not tripped.
Routing the narrowing to the owner as brief decisions 1 and 2 is still right,
because the *accepted set* is behaviour.

One implementer note, not a finding: `AcceptedNamesJoinToTheSameBytesAsBefore`
pins `JoinSegments` output only. Two rows for the derived forms (`name +
"/__schema"`, the participant name) cost nothing and close the last inch.

### Claim 3 — deleting §3.5's frozen sentence is inside the 2026-09-03 authorisation. No separate ruling needed

The verification pass the owner ruled on names that exact sentence as part of A5's
defect: `plans/reviews/PDA-DEC-9-bind-stopask-verification.md:226-231` — "§3.5's
frozen first sentence … licenses a future provider under which they are **two**" —
and its *Contract gap* section files A5 against "§3.5, named explicitly in the
freeze list". The ruling was "Amend the spec and land all eight before merge, so
the seam ships correct rather than shipping frozen-and-known-wrong". Deleting the
"any separator" licence **is** that amendment, not a second decision on top of it.

It is also a **narrowing** of provider discretion, and the 2026-09-03 licence
permits inferring the owner's narrow-claim preference without asking — narrowing
only, never widening (as the 2026-09-04 carve-out ruling restates). §12.1's
freeze-list phrase "§3.5 including the empty-segment refusal" still reads true and
the design correctly leaves §12.1 unedited. **Carry nothing.** One wording caveat
is filed as A5-DEBT-2.

### Claim 4 — rung 1 really is unavailable, and for a stronger reason than the design gives

§2 publishes the four signatures over `const std::vector<std::string>&`
(`docs/pubsub-interface-spec.md:96-101`) and §12.1 freezes §2; §11's "any change to
the interface's method set (§2)" is frozen too. §2 *does* carry a carve-out the
design does not cite — "What this round *may* change is the **types** in those
signatures, and **only where a type has no C-expressible form (§3)**"
(`:104-106`) — and it does not reach a sealed `TopicName`, because §3.5 already
gives topic segments a C form (pointer-and-count of pointer-and-length pairs). So
the conclusion stands on the spec's own terms, and BIND is building against those
signatures right now.

Independently of the freeze: across a C boundary of pointer-and-length pairs, a
bad name is **always constructible**, so a sealed type relocates the check into a
constructor rather than making anything unrepresentable — it would be rung 2 wearing
rung 1's name. **Rung 2 at the single door is the correct rung, not a settlement.**
Filed as A5-DEBT-3 so nobody later cites §2:104-106 as proof rung 1 was available.

### Claim 5 — refusal is right for all four shapes; the brief's behaviour-visible cost is accurate, with one gap

`WsSession::SplitTopic` (`gateway/src/ws_session.cpp:149-162`) splits on `/` and
drops empty pieces, so no remote client can hold a working `/`-bearing or
empty-segment topic — the brief's claim is true. It **can** carry a NUL, which the
design states and the brief does not spell out; that case is a silent wrong-slot
delivery on XRCE today, so refusing it needs no decision and the tree already
refuses NUL in three sibling strings (`in_process_provider.cpp:75-79`,
`xrce_document.cpp:71-75`, `provider_registry.cpp:107-113`). No in-tree caller or
test uses an empty or `/`-bearing segment (searched `*.cpp`). So the cost is
exactly what the brief says: in-process C++ callers and future bindings, loud, no
migration path.

The gap: decision 3's option (a) reads "reject exactly the three shapes that cause
a collision or truncation", and there is a fourth collision shape it does not
cover — A5-DEBT-1, the `__schema` shadow. If the owner is to close that, it belongs
in decision 3. I did not add it to the brief: my licence there was a trim to cap.

### Claim 6 — P2's inertness

P2 is true and now verified from the build files (BLOCKER 2). The design is honest
about it and stops rather than claiming a red; the defect is that two of its six
mutations name only inert targets, and the item's two distinguishing claims are the
ones that lose their control. See BLOCKER 2 for the cheap fix.

### Claim 7 — budget is defensible; not a finding

+430 composes exactly (25+30+180+90+60+45) and each line checks out against the
tree: the existing four-entry-point case is 31 lines including its comment
(`seam_vocabulary.cpp:328-358`), so +90 for three shapes × four methods plus a
positive control is generous rather than thin. Undeclared: BLOCKER 2's in-tree
control (+≈60) and, if A5-DEBT-1 is taken, +≈10 — both inside the declared +120
contingency. This is not A4's failure mode returning: A4's 4.6× came from three fix
cycles on a mechanism whose scope kept moving, and here the mechanism is three
`if`s in one function that no reviewer has proposed moving. Design doc 252 ≤ 300;
new public surface 0 ≤ 3; net lines declared.

---

## Checks that passed, so the implementer does not re-derive them

- **P1 verified.** Twelve provider methods + caller tier + `pubsub-arrow` + the
  conformance peer all route through `JoinSegments`/`JoinSegmentsInto`; the door
  really is a door. (Design §P1 says "five product call sites"; that is five
  *files*, fourteen sites — harmless looseness, not worth a cycle.)
- **P3 verified** — `ws_session.cpp:149-162`, as above.
- **P4 verified** — no `c_str()` anywhere in `fastdds-pubsub-provider/src`.
- **`Files-to-delete` is present and real**, and the justification for retiring
  nothing is sound: there is no second mechanism, no bridge, no coexistence window
  scheduled for later deletion. Keeping
  `EmptyTopicSegmentListIsRefusedAtEveryEntryPoint` rather than absorbing it is
  correct — empty *list* and empty *segment* are different rules.
- **No hand-composed post-change ledger.** The wire-bytes table is a small
  pre/post behaviour table pinned by a named test, not a survival ledger.
- **No new `PubSubStatus`**; `kInvalidArgument` reused, per the 2026-09-01 ruling.
  Nothing here touches `kReentrantCall = 10` or A3.
- **Scope test (2026-09-01 split ruling) passes** — no ABI development; this is
  seam preparation only.

## NITs

Fixed silently in `plans/PDA-DEC-A5-brief.md` (trimmed 72 → 60 lines against the
HARD cap; every load-bearing fact retained — reflow only, no scope cut needed).
Nothing edited in the design doc.
