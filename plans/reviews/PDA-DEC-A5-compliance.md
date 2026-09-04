# PDA-DEC-A5 — step-4a compliance review (independent, adversarial)

**Verdict: PASS-WITH-FINDINGS(1).** Ledger `grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **42**.
Diff reviewed: `git diff 722cc6b 5f04e2c` (one commit, `5f04e2c`; working tree clean before and
after this review). Mechanism, spec amendment, rulings and the wire-bytes claim all conform. One
blocking item: the sentence the item deleted from frozen §3.5 **survived verbatim in the public
seam header**.

---

## BLOCKING

### B1 — the deleted §3.5 licence survived in `provider.hpp`; the pre-change contract is still shipped

`pubsub/include/fletcher/pubsub/provider.hpp:50-53`:

> `/// Topic names are represented as a list of string segments so that`
> `/// the provider can join them with any separator it prefers. **An empty segment`
> `/// list is illegal** and is refused with PubSubStatus::kInvalidArgument by every`
> `/// method that takes one — there is no default topic and no recovery (§3.5).`

This is the item's `Files-to-delete` entry — *"§3.5's sentence **so the provider may join with
any separator**"* — alive, in the one public header that mirrors §3.5, on the abstract interface
the 2026-09-01 ruling says the language bindings read (*"bindings interface to the abstract
interface, not the underlying ABI"*). It is now **false**: amended §3.5 states *"The join is the
seam's, not the provider's discretion"*, and the header states the opposite in the same words the
spec retired. Two failures in one block:

1. the retired licence is still published;
2. the block is the header's statement of §3.5's refusal set and still names **only the
   empty-list** rule, so the four `kInvalidArgument` refusals every method on this interface now
   performs are undocumented at the interface that declares them.

Grep confirms it is the **only** surviving instance in the tree
(`grep -rn 'any separator' --include=*.md --include=*.hpp --include=*.cpp`, excluding `plans/`);
`fastdds-pubsub-provider/README.md:137` and `xrcedds-pubsub-provider/README.md:25` already say
"joined with `/`" and are consistent with the amendment.

This is the converse-check class ("what survived that should not"), and the round has already
treated a stale header contract as a defect worth its own fix, not as maintenance (`a8a955c`,
*"correct two stale header contracts"*).

**Fix (one paragraph, same PR):** in `provider.hpp`, delete *"so that the provider can join them
with any separator it prefers"*, state the seam-owned `/` join and that the joined name is the
topic's identity, and list the five §3.5 refusals (or name them by reference to §3.5) instead of
the empty-list rule alone. No signature change, no new surface.

---

## Explicit calls on the three claims I was asked to adjudicate

### Claim 2 — the `__` rule is a PREFIX rule: **CONFIRMED**

`segments.hpp:70` — `if (seg.size() >= 2 && seg[0] == '_' && seg[1] == '_')`. Not a literal
`__schema` match; `{"__"}`, `{"__anything"}` and `{"__anything_at_all"}` are all refused and all
three are pinned as rows (`test_segments.cpp:123-126`, `seam_vocabulary.cpp:419-420`,
`fastdds_main.cpp:179-180`, `xrce_main.cpp:1023-1024`). Matches the 2026-09-04 ruling
(*"Reject any part starting with `__` — closes the whole reserved namespace"*) and §3.5's
*"The **prefix** is reserved, not any one literal name"*. Verified live: mutation M7 (drop the
check) reddens `SegmentsThatAliasOrTruncateAreRefused` + `RefusalReachesAllFourEntryPoints` and
**nothing else** — I ran it (2/4 red, `JoinIsInvertible` and the byte table stay green).

Also verified the reservation does not bite its own providers: both DDS companion names are built
by **string concatenation** (`name + "/__schema"`, `fast_dds_pubsub_provider.cpp:331,494`;
`xrce_dds_pubsub_provider.cpp:723,885`), never by re-entering `JoinSegments`, so no provider can
refuse its own channel. P5's `__` half re-checked independently: no `__`-prefixed topic segment
exists anywhere in the tree outside A5's own rows (the `__rba.fletcher.rs` hits are generated
Rust filenames; the chrono/zerocopy hits are Cargo fingerprints).

### Claim 3 — the §3.5 replacement carries BOTH driver obligations, and narrows: **CONFIRMED**

`docs/pubsub-interface-spec.md:270-276` carries injectivity **and** the reciprocal confinement:

> *"…but **only injectively**, and every companion name it derives from a topic's name must lie
> in the reserved `__` namespace below. Both obligations are needed: injectivity alone would still
> let a driver derive `name + ".meta"` and land on an accepted topic…"*

That is A5-DEBT-2 and **A5-DEBT-4** both discharged, in the wording the debt asked for, including
the `.meta` counter-example. **It narrows on every clause**: the old text licensed *any* separator
(under which `{"a.b"}` and `{"a","b"}` could alias on a `.`-joining provider); the new text fixes
the join at the seam and constrains a driver to an injective, `__`-confined mapping. Nothing the
old §3.5 said was dropped — the C-form sentence, the empty-list refusal, "no default topic and no
recovery", and "the check lives once in `internal::RequireSegments`" are all retained. The one
genuinely new prohibition beyond the ten authorised amendments is *"No trimming, no case folding,
no Unicode normalisation, no escaping"*, which forbids provider behaviour rather than permitting
it — a narrowing, inside the 2026-09-03 licence to narrow without asking, and the same words §4
line 335 already uses for the selector.

**§12.1 is untouched** — the spec diff is a single hunk at `@@ -261,12 +261,44 @@`, entirely
inside §3.5. §12.1's freeze-list phrase *"§3.5 including the empty-segment refusal"* still reads
true (and is now literally true, since there is an empty-*segment* rule as well as an
empty-*list* one).

### Claim 4 — the accepted set narrowed with no accepted name's bytes moving: **CONFIRMED, by execution**

Two independent lines of evidence, neither taken from the implementer:

1. **Structural.** The diff to `segments.hpp` touches the doc comment and `RequireSegments` only.
   Both join bodies are byte-identical to `722cc6b`. Every provider's wire name and both derived
   forms are pure functions of that join (`fast_dds…:303,335,439,496`;
   `xrce…:675,680,723,855,859,885` — including the participant name, which **is** the joined
   topic name). A function that only ever throws earlier cannot move a byte of a name it still
   returns.
2. **Executed.** I reverted `RequireSegments` to its pre-A5 body in the working tree, rebuilt
   `pubsub_tests` and ran `Segments.*`:
   `AcceptedNamesJoinToTheSameBytesAsBefore` **PASSED** on the pre-A5 body and passes on the
   post-A5 body; the other three failed. So the control is green on both sides of the change and
   the three forcing cases are genuinely red-first. The tree was restored (`git status` clean,
   23/23 green on rebuild).

The design's "non-stop-and-ask" premise therefore holds on evidence, not on assertion.

---

## Other verifications (no findings)

- **Claim 1 — four refusals, one door, `kInvalidArgument`, no new status: CONFIRMED.**
  `core/include/fletcher/core/status.hpp` is **not in the diff**; the enum is 0–9 with ten
  one-value-at-a-time `static_assert`s, unchanged. `kReentrantCall` appears nowhere in the tree —
  A5 did not touch A3's allocation. All five throws in `RequireSegments` are
  `PubSubStatus::kInvalidArgument`, and every assertion checks the **number**, not merely that
  something was thrown (`RefusedAsInvalid`, `test_segments.cpp:62-73`).
- **The door is the door.** All twelve provider entry points reach a topic only through
  `JoinSegments`/`JoinSegmentsInto` (in-process 180/256/287/321, Fast DDS 289/383/429/533, XRCE
  634/774/829/967), plus the caller tier, `pubsub-arrow` and the peer. No path constructs a topic
  or participant name any other way — **P1 holds**; no stop-and-ask is owed.
- **The participant-name sink is covered** — the "wider than claimed" half of the original
  finding. `uxr_buffer_create_participant_bin(..., name.c_str(), ...)` at
  `xrce_dds_pubsub_provider.cpp:674,853` takes the *same* joined string, so the NUL refusal at the
  door removes both `c_str()` sinks at once. Confirmed by reading, not by the design's claim.
- **Claim 5 — no legacy test was retired, and none needed to be: CONFIRMED.** Searched the tree
  for topic literals in the four refused shapes (`Topic{…"…/…"}`, `{""}`, `"__…"`, NUL) across
  `*.cpp/*.hpp/*.ts/*.js/*.json`: the only hits are A5's own rows.
  `EmptyTopicSegmentListIsRefusedAtEveryEntryPoint` is kept beside the new sibling, as
  `Files-to-delete` required, and the seam-vocabulary binary now reports **8 entries** — matching
  the README's updated count (I ran it: 8/8 green).
- **Claim 6 — mutations.** I re-ran four in-tree mutations myself rather than accept the table:
  **M1** (drop NUL) → 2/4 red, exactly the two recorded; **M4** (per-segment checks moved to
  `CreateTopic` only) → `RefusalReachesAllFourEntryPoints` red with **13 × Publish, 13 × Subscribe,
  13 × Unsubscribe accepted and zero CreateTopic** — the per-method signature recorded in the log,
  and proof that cycle-1's inert M4 is genuinely fixed; **M6** (faithful escape of `/`, escaping
  its own `%` marker) → the byte control **reddens**, so the over-reach control can fail; **M7**
  (drop `__`) → 2/4 red, discriminating. All four bite, none is inert. **M5** I could not
  reproduce (it needs a `conan create` chain plus a live Agent), but its premise is corroborated
  independently: the Conan cache holds **14 copies of `segments.hpp` carrying the A5 refusals**,
  timestamped 2026-09-04 11:46 and 11:55, against 107 pre-A5 copies — i.e. packages really were
  rebuilt during implementation. `TopicNames.AmbiguousSegmentsAreRefused` is compiled into **both**
  subject binaries (`conformance_xrce --gtest_list_tests` shows it, 28 cases total, matching the
  CMakeLists comment) and the Fast DDS case passes when run directly (81 ms, domain 155).
- **Corner-case ladder.** Rung 2 is a refusal with no recovery: no `catch (const PubSubError&)`
  anywhere on the topic path converts a refusal into a fallback (the only two catch sites are
  `TranslateSeamFailure`'s re-throw and `gateway/src/main.cpp:261`, which exits 2). Rung 1's peer
  door is now total — `RejectUnsendableTopic` gained the NUL via `std::string(" \t/\0", 4)`, an
  explicit length, so the search does not stop one character early.
- **Above the seam.** `WsSession::SplitTopic` (`gateway/src/ws_session.cpp:149-162`) drops empty
  pieces and splits on `/`, so **P3 holds**: the gateway cannot emit shapes 2 or 3, can emit 1 and
  4, and those now surface as an error reply rather than a wrong delivery. No in-tree gateway or
  TypeScript test uses a `__`-prefixed or NUL-bearing topic. Nothing above the seam assumes the old
  accepted set.
- **Files-to-touch.** Every named file was touched and nothing outside it except
  `integration-tests/pubsub-conformance/CMakeLists.txt` (**comments only** — the ten-domain count,
  the 28-case count and the ctest-filtering note; no build logic moved) and
  `plans/reviews/design-debt.md` (the PM's own census correction). Neither is scope creep.
  `Files-to-delete` is fully honoured **except** B1 above. Declared +535/−15, landed **+737/−19**
  excluding `plans/` — I re-derived it (`git diff --numstat … | awk`), it matches the log exactly,
  and the overrun is test apparatus, as the log states.
- **PM correction (a) is right.** `conformance_fastdds` uses `gtest_discover_tests` (one ctest
  entry per case) while `conformance_xrce` is `add_test(NAME conformance_xrce COMMAND
  conformance_xrce)` — a single entry wrapping the binary. `ctest -R 'TopicNames\.'` therefore
  matches the Fast DDS entry only and reports all-passed. The `|conformance_xrce` alternation is
  the right repair.
- **PM correction (b) is right.** `integration-tests/gateway-end-to-end/test/end-to-end.test.ts:360`
  sets `domainId: '154'`. 155 is free (the in-tree census at `fastdds_main.cpp:279` enumerates
  151–154 and nothing in 161–167). A5 taking 155 (Fast DDS) and session base `0x55000000` on the
  Agent's own domain 153 (XRCE) discharges A5-DEBT-5 correctly.

---

## Non-blocking

- **N1 — the two published cause-lists for `kInvalidArgument` now omit four causes.**
  `core/README.md:32` and `core/include/fletcher/core/status.hpp:37-38` both read *"an empty
  topic-segment list, a blob with bytes and no owner, a negative timeout"*. The phrasing is
  illustrative (*"something the seam refuses to interpret: …"*), so neither is false, but they are
  the only published cause lists and the four new refusals are absent from both. One clause each
  if the PM wants them aligned; not a §12.1 append, so no ruling is implicated.
- **N2 — the `rule N` labels in `test_segments.cpp:113-126` are off by one against §3.5 and
  `segments.hpp`.** The test numbers NUL/`/`/empty/`__` as rules 1–4 (the design's mechanism
  numbering); the spec and the header number them 2–5, with the empty **list** as rule 1. A failure
  message therefore names the wrong rule number. Cosmetic, and the test follows the approved
  design's numbering, so it is drift between two documents rather than a deviation from either.

## RECORD (PM's to fix in place — never blocking)

- `.claude/runbook.PDA-DEC.config.md:75` configures the harness `-DFLETCHER_CONFORMANCE_XRCE=OFF`
  while its own note six lines below says ON *"is MANDATORY for it: OFF silently drops the two
  subjects that carry the defect, so an OFF run is not evidence for A5"*. The command as written
  cannot produce A5's cross-provider evidence.
- `integration-tests/pubsub-conformance/subjects/fastdds_main.cpp:283` (pre-existing) cites
  `end-to-end.test.ts:350` for the domain-154 fact; the new comment at `:47` cites `:360`, which is
  the correct line. Two citations of one fact now disagree inside one file.
