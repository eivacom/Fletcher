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

---
---

# RE-CHECK after fix cycle 1 — `git diff 335b016 f526acb` (2026-09-04)

*The cycle-1 review above is the cycle-1 record and is left intact. This section is appended.*

**Verdict: PASS-WITH-FINDINGS(1).** Ledger `grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **43**.
Cycle diff `+268 / −53`; cumulative `722cc6b → f526acb` = **`+967 / −34`** excluding `plans/`
(re-derived, matches). Working tree clean before and after; every mutation I applied was reverted
and the tree re-verified green.

**Cycle-1 B1 is CLOSED, and closed everywhere.** `grep -rn 'any separator'` outside `plans/`
returns nothing. `provider.hpp:50-74` now states the seam-owned `/` join, that the joined name IS
the identity, both driver obligations, and all six refusals. I checked for a *third* place that
still disagrees: no other public header, no per-entry-point doc comment in `provider.hpp`
(`CreateTopic`/`Publish`/`Subscribe`/`Unsubscribe` restate the delivery contract, never the naming
rule), `publisher.hpp:48` and both provider READMEs say "joined with `/`", which the amendment
makes true rather than false, and `docs/protocol-driver-abi-spec.md:51` inherits §3.5 **by
reference**. Cycle-1 N2 (the `rule N` off-by-one) is also fixed, correctly, to §3.5's numbering.

**§12.1 is byte-identical across the whole item.** I extracted `§12.1 … §12.2` from `722cc6b` and
from `f526acb` and diffed: identical, `md5 e66e71c53dcc201467cca8cbe2cd8c17`. The spec diff for the
entire item is **one hunk**, `@@ -261,+56 @@`, wholly inside §3.5; no section header moved.

## BLOCKING

### B2 — the gateway behaviour change is pinned by nothing

`WsSession::SplitTopic` (`gateway/src/ws_session.cpp:163-176`) stopped dropping empty pieces, so
`"a//b"`, `"/a/b"`, `"a/b/"` and `"//a//b//"` — WebSocket topic strings that **work today** and all
name `{"a","b"}` — now reach §3.5 rule 4 and come back as error frames. The mechanism is right
(all three `SplitTopic` call sites at `:189`, `:194`, `:271` sit inside a
`catch (const std::exception&) { SendError(...) }`, so it is a frame, not a crash), but **no test
asserts any of it**. There is no gateway unit case, no `gateway-end-to-end` TypeScript case, and
no conformance clause: I built and ran `gateway_tests` — **20/20, the same 20 as before the
change** — and `git diff 335b016 f526acb` touches no gateway test. A refactor that restores the two
`if (!seg.empty())` guards goes unnoticed, and the log's claim that this was "the last place on the
path still tidying a name" has no control behind it. This is a behaviour change at a **shipped,
remotely-visible** caller with a red-on-regression story of zero.

**Fix:** one `gateway_tests` case — `SplitTopic("a//b")` yields `{"a","","b"}` and the handler
answers with an error frame, while `"a/b"` still creates the topic. Two assertions.

## Explicit calls

### The §3.5 narrowing — **honest and complete**

The invariant is now scoped at the head (*"for every **accepted** segment list `L` — and accepted
is bounded, rule 6 being what makes the rest of this sentence true rather than aspirational"*) and
closed at the foot (*"**Nothing is claimed for a list this section refuses**"*). I read every
surviving sentence of §3.5, of `segments.hpp`'s block and of the rewritten `provider.hpp` block
looking for one that still says "always injective": there is none. `Split(Join(L)) == L` is scoped
to accepted lists in both the spec and the header; the driver clauses are *obligations*, not
claims; and `provider.hpp`'s *"the same list names the same topic on every transport"* is true for
the accepted set. The amendment stays inside the authorised eleven — the only content beyond
ruling 43 is the *scoping* of a claim that was previously unscoped, which is a narrowing.

One sentence deserves naming and then survives: *"a transport whose own ceiling is lower than 246
bytes refuses on its own terms through §5.1, which is a loud failure rather than a silent one"*
asserts that other transports **refuse** rather than truncate — which is precisely what Fast DDS
did not do at 255. It is true as written only because the same section already binds a driver to
map **injectively**: a driver that silently truncates is non-conforming under that clause, so the
residual case is "non-conforming driver", not "conforming driver, silent collision". It is also
inherited in substance from the approved design's Handled-residue entry. No change asked.

The six-refusal set is exactly the authorised one: the eight of 2026-09-03 are elsewhere, the
ninth (idempotence) is A4's, the tenth is rule 5, the eleventh is rule 6. **No new
`PubSubStatus`:** `core/include/fletcher/core/status.hpp` is not in the cycle diff, the enum is
0–9 with all ten one-value-at-a-time `static_assert`s intact, and `kReentrantCall` appears nowhere
in the tree. All six refusals throw `kInvalidArgument`.

Rule 6's numbers conform to the ruling and are structural, not literal:
`kMaxJoinedTopicBytes = kFastDdsAnnouncedTopicBytes(255) - kDerivedCompanionSuffixBytes(9)`, the
bound is on the **joined** length (`joined = segs.size() - 1 + sum of seg.size()`, separators
included), and I verified the header's `UXR_BINARY_SEQUENCE_MAX is 512` citation against
`microxrcedds_client-src/include/uxr/client/core/type/xrce_types.h:37`. The 9-byte headroom is
also *complete*: I enumerated every name either DDS provider derives from the joined name —
`name + "/__schema"` (`fast_dds…:331,494`; `xrce…:720,885`) and XRCE's participant name `= name` —
and there is no third derivation and no longer suffix.

### M9 — **CONFIRMED by execution, and it is the strongest evidence in the item**

I mutated `kMaxJoinedTopicBytes` to `kFastDdsAnnouncedTopicBytes` — literally the 255 the owner
**rejected** — rebuilt and ran `Segments.*`:

```
[  FAILED  ] Segments.NamesThatWouldTruncateOnTheWireAreRefused
[  PASSED  ] 4 tests   (SegmentsThatAliasOrTruncate…, JoinIsInvertible,
                        RefusalReachesAllFourEntryPoints, AcceptedNamesJoinToTheSame…)
```

Seven assertions fired, and they are the right seven: `247 bytes is the shortest REFUSED name`,
`the separators are part of the joined length` (the 3x82 = 246-summing, 248-joining shape), the
`JoinSegmentsInto` row, and all four provider entry points. The owner's specific 246-vs-255 choice
is therefore pinned by a test that goes red the moment anyone takes the rejected option. I also ran
**M8** (delete the length check outright): same isolation — that case alone, the other four green.
Keeping rule 6 out of `RefusedCases()` is what buys that isolation, and it is deliberate and
documented.

### The gateway change — **NOT inside ruling 41; the owner has not been told, and the PM carries it**

Ruling 41's *Applies-to* is exactly one line: *"`internal::RequireSegments` refuses an empty
segment."* Nothing above the seam. More decisively, the **premise the owner was given** for both
2026-09-04 topic rulings is now false:

- ruling 41: *"Neither Fletcher's gateway nor any code in the tree uses one."*
- ruling 40: *"No remote client loses a working topic: Fletcher's own gateway can never produce
  such a part."*

Both were true **because `SplitTopic` dropped empty pieces** — and the approved design says so in
as many words, citing `gateway/src/ws_session.cpp:149-162` as *evidence* under "Wire bytes"
(*"Remote clients lose nothing to rules 2 and 3 — `WsSession::SplitTopic` splits on `/` and drops
empty pieces"*). The design lists `ws_session.cpp` nowhere in `Files-to-touch`. Removing the
dropping makes the gateway able to produce an empty part, so remote clients **do** now lose working
topics, which is the one cost the owner was told could not arise.

It is **not a ruling violation**: it is a narrowing, and the 2026-09-04 carve-out ruling states the
2026-09-03 licence *"permits **narrowing** without asking, never **widening**"*. So I do not ask for
a revert, and the direction is the one the owner has now chosen eight consecutive times. But it is a
**disclosure owed before PR #126 merges**, because the owner priced two rulings on a sentence this
change falsifies. **Verdict: the latter — you carry it.**

## Other verifications this cycle (no findings)

- **The peer door is total, and no `Reply`-returning method can throw.** Only `DeclareTopic` and
  `PublishRow` return `Reply` and reach `internal::JoinSegments`; both call
  `RejectUnsendableTopic` first, which now wraps `RequireSegments` in a `try` and converts to
  `Reply::HarnessFailure`. `Subscribe`/`Unsubscribe` return `SubscriptionResult`/`void` and
  delegate straight to the provider, so they are outside the claim. Delegation also strictly
  widens the harness door: NUL is still covered (rule 2, since `isspace('\0')` is false), and the
  pipe half moved from the literal `" \t"` to `isspace`, picking up `\n \r \v \f`.
- **Nothing bypasses `RequireSegments`, and nothing still documents the pre-change contract.**
  Re-swept for join-licence wording, for "only the empty list is refused" wording, and for any
  claim that topic names are unbounded — all empty outside `plans/` and `docs/archive/`.
- **No test was retired and none needed to be.** The only removed assertion is
  `EXPECT_EQ(joined, "telemetry/depth")`, replaced by `EXPECT_EQ(companion,
  "telemetry/depth/__schema")` plus a length and a prefix relation — which entails the removed
  pin and is strictly stronger, so the XRCE participant-name control survives.
- **Both record items landed:** `fastdds_main.cpp:283` `:350` → `:360`; `CMakeLists.txt` "ten
  domains" → "eleven".
- Local runs: `pubsub_tests` **24/24**, `gateway_tests` **20/20**,
  `conformance_seam_vocabulary` 8/8 and `conformance_fastdds --gtest_filter=TopicNames.*` green
  (the latter two from the cycle-1 pass; the fix cycle does not touch their sources beyond the
  README). The Conan cache shows a further `fletcher-pubsub` rebuild carrying the fix-cycle header,
  so the packaged side is current.

## Non-blocking

- **N3 — `RequireSegments` lost its doc comment.** The three constants were inserted **between**
  the ~60-line `///` block and the function with no blank line (`segments.hpp:71-77`), so the block
  — the header's statement of §3.5 — now documents `kFastDdsAnnouncedTopicBytes`, and
  `RequireSegments` is undocumented. Introduced by this cycle's own fix. One blank line, or move
  the three constants above the block.
- **N4 — the rule-6 headroom assertion cannot see the header drift.**
  `EXPECT_LE((longest + "/__schema").size(), kFastDdsAnnouncedCeiling)` uses the *test's* 246/255
  literals, so it stays green even if `kMaxJoinedTopicBytes` moves. Harmless — the `247 is the
  shortest REFUSED name` assertion is what actually catches drift, and M8/M9 prove it does — but
  the headroom row asserts arithmetic rather than the product.
- **N5 — `fastdds-pubsub-provider/README.md` has a `### Topic name` section that does not mention
  the 246-byte ceiling**, which exists *because of* this provider's `fastcdr::string_255`
  announcement. Not false and not in `Files-to-touch`; it is simply the first place a Fast DDS user
  would look for the limit.
- Cycle-1 **N1** (the two illustrative `kInvalidArgument` cause lists in `core/`) stands, left
  deliberately and recorded in the log. Cycle-1 **N2** is fixed.

## RECORD (PM's, never blocking)

- The re-check brief states the headroom is pinned by `static_assert(246 + 9 == 255)` in the header
  form; the `static_assert` is in **`pubsub/tests/test_segments.cpp:115`**, not in `segments.hpp`,
  which carries the derivation but no assert. The progress log states this correctly.
  Behaviourally the pin is real (M8 and M9 both bite), so this is wording only.
- The cycle-1 RECORD item about `.claude/runbook.PDA-DEC.config.md:75` (`XRCE=OFF` in the command
  vs *"ON is MANDATORY for it"* in its own note six lines below) is unchanged and still stands.

## LIVE CACHE HAZARD — not a defect in `f526acb`, but act before close

While writing this section I checked the Conan cache and found that the **current** cached
`fletcher-pubsub` package carries a **mutated** `segments.hpp`:

```
conan list 'fletcher-pubsub/*:*#*'
  fletcher-pubsub/0.5.0-alpha#376f2faef9183d7244d902b73e6e9abb   (2026-09-04 13:46:32 UTC)
```

is the only recipe revision, and its header reads
`constexpr size_t kMaxJoinedTopicBytes = kFastDdsAnnouncedTopicBytes;  // M9: the REJECTED 255`
(`/c/Users/CTM/.conan2/p/fletce7550719a534e/...`, built 15:46 local). The 15:32 build
(`fletc81a0d11ccc907`) carries the correct derived 246 and has been superseded.

Two reviewers were mutating the same working tree in the same window — a `conan create` captured a
mutation in flight. The **working tree is correct** (246, derived; `pubsub_tests` 24/24 green, and
I reverted every mutation I applied), so `f526acb` itself is unaffected. But **any packaged-target
run started from this cache now links a 255-bounded seam**, which would make
`TopicNames.*`/`SeamVocabulary.*` and any harness green mean less than it appears to. Re-run
`conan create pubsub` (and the two provider packages) from the clean tree before trusting any
packaged-target result taken after 15:46 today.

---
---

# FINAL CHECK after fix cycle 2 — `git diff 445b4ea b946798` (2026-09-04)

*The cycle-1 record and the fix-cycle-1 re-check above are left intact. This section is appended.*

**Verdict: PASS — 0 blocking.** Ledger `grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **43**
(re-grepped). Cycle `+99 / −12`; cumulative `722cc6b → b946798` = **`+1055 / −35`** excluding
`plans/` (re-derived, matches). Working tree clean before and after; every mutation I applied was
reverted, every affected build tree rebuilt, and the suites re-verified green.

## B2 is CLOSED — I reproduced the whole red/green cycle myself

**The mutation-only red is sufficient here, and it is the only honest form available.** Red-first
needs the defect present in the diff base; the two `if (!seg.empty())` guards were deleted in fix
cycle 1 at `f526acb`, so no state of `445b4ea` can fail this case. The implementer said exactly
that rather than claiming a red it did not get — which is the standard this round has enforced
since PDA-DEC-1 (*"report the inert mutation, never claim a red you did not get"*). What makes the
mutation *evidence* rather than assertion is that it is the **exact inverse** of the change, and I
ran it:

| step | result |
|---|---|
| `b946798`, harness gateway rebuilt against the current packages | **2 of 31 pass** (the new case, both provider contexts) |
| both `if (!seg.empty())` guards restored verbatim in `ws_session.cpp`, gateway rebuilt | **2 of 31 FAIL** — `gateway over 'inprocess'` and `gateway over 'fastdds'`, *"promise resolved undefined instead of rejecting"*, at `end-to-end.test.ts:649`. Nothing else moved: 29 passed |
| guards restored to `b946798`, rebuilt | **31 of 31 pass** |

Note the failure *mode*: `promise resolved undefined` means the gateway **accepted** `emptypart//bad`
— the aliasing behaviour itself — not a timeout, not a harness error. And the assertion pins the
seam's own message (`/empty segment/`, which is `RequireSegments`' rule-4 text arriving through
`SendError(e.what())` and `client.ts:84`'s `throw new Error(resp.message)`), so a rejection for any
*other* reason would not satisfy it. That is a discriminating red on the exact claim.

**The placement is right, and better than what I asked for.** I asked for a `gateway_tests` case;
that was the weaker suggestion. `SplitTopic` is `private static` (`ws_session.hpp:110`, under the
`private:` at `:80`) and `gateway_tests` links **`gateway_codec` alone**, which is
`schema_codec.cpp` + `publish_frame.cpp` — `ws_session.cpp` is compiled into the `gateway`
*executable* target (`gateway/CMakeLists.txt:48-64`). Taking my suggestion would have meant putting
Boost.Beast and a provider on the test link line and widening a private member: changing production
structure to admit a test. The claim is about what a **client** sees, and `gateway-end-to-end` is
the only harness that drives the shipped WebSocket surface. It is also **CI-wired for the right
trigger**: `ci.pr.yml`'s `integration-gateway-e2e` path filter includes `gateway/**`, so an edit to
`ws_session.cpp` runs this workflow. The positive row is placed **first**, so a red on the negative
rows cannot be read as "this gateway rejects everything".

## The cache purge — COMPLETE for everything Conan can resolve

I did not take the report on trust; I enumerated the cache.

- **No poisoned artifact exists anywhere.** Zero hits for the M9 form
  (`kMaxJoinedTopicBytes = kFastDdsAnnouncedTopicBytes;`) across the whole of `~/.conan2/p`, and
  zero for the M8 form (a rule-6-era header with the length check missing). The poisoned recipe
  revision I found last round, `fletcher-pubsub#376f2fae…`, **is gone**; the only recipe revision
  now is `#b3ba0519…`.
- **Every resolvable `fletcher-pubsub` package revision is clean.** I resolved all four through
  `conan cache path` and read the header each one ships: 4/4 **CLEAN246** (`kMaxJoinedTopicBytes =
  kFastDdsAnnouncedTopicBytes - kDerivedCompanionSuffixBytes`, with `if (joined >
  kMaxJoinedTopicBytes)` present).
- **The compiled provider binaries are clean too, established by provenance rather than by
  grepping a header they do not contain.** For every resolvable provider package revision I read
  its own build folder's generated `fletcher-pubsub` config to find the exact pubsub package folder
  it compiled against, then classified that header: Fast DDS **4/4 CLEAN246** (built 16:20 and
  16:40, `libs=2`/`libs=1`), XRCE **6/6 CLEAN246** (built 16:04–16:42, `libs=3`). Widened to every
  build folder in the cache: **26 CLEAN246, 0 POISONED**; the rest consumed pre-fix-cycle headers
  and are all dated before 12:03 today.
- **No lockfiles** anywhere in the tree or the cache.

So the packages a consumer resolves today carry the 246 refusal, and nothing compiled against 255
survives. The coordinator's point that header-checking alone was insufficient is correct, and the
provenance chain above is what closes it.

### Three build-tree residues — not poisoned, but they decide whether a number means anything

None is a defect in `b946798`. All three matter for close-gate evidence, so they are listed here
rather than buried.

1. **`pubsub-arrow/build` silently resolves a fully pre-A5 seam.** Its generators point at
   `…/p/b/fletc62b281d1d34ee/p`, an **orphaned** package folder (not among the four Conan resolves)
   whose `RequireSegments` is the 2026-09-03 version — `segs.empty()` and *nothing else*, none of
   the six refusals. The folder still exists, so `cmake --build pubsub-arrow/build` does **not**
   error; it compiles and goes green against a seam with no A5 rules at all. This is the silent
   kind. Re-run `conan install` there before any `pubsub-arrow` number is taken.
2. **`fastdds-pubsub-provider/build` and `xrcedds-pubsub-provider/build` reference purged package
   folders** (2 missing each). These fail *loudly* at configure — the safe mode — but they cannot
   produce a number until `conan install` is re-run. `gateway/build` was in the same state; I
   re-ran `conan install` + configure + build there myself to run the mutation above, so it is
   current again.
3. **The nested `build/build/generators` is back at the conformance harness** (re-created 16:50),
   so `integration-tests/pubsub-conformance/CMakeUserPresets.json` again includes **two** preset
   files each declaring a preset named `conan-default`, with different `binaryDir`s
   (`…/build` and `…/build/build`). That is the exact condition reported as discarded. Both
   currently resolve the same CLEAN246 pubsub package, so nothing is wrong today — but
   `cmake --preset conan-default` is ambiguous, and a run could configure one tree while a reader
   inspects the other.

## The three nits

- **N3 — fixed correctly.** The three constants moved **above** the doc block
  (`segments.hpp:21-27`), so the ~60-line `///` block documents `RequireSegments` again. Verified in
  the packaged header a consumer resolves, not just in the working tree.
- **N4 — the product assertion genuinely holds; one comment overstates its role.** The product
  half is now real and is what I asked for: it takes the longest **accepted** name
  (`JoinSegments(JoinedLength(246))`), derives the companion **as both DDS providers derive it**
  (`name + "/__schema"`), applies the sink's own silent truncation
  (`substr(0, min(size, 255))`), and asserts `announced == companion` — nothing lost — plus
  `announced != longest`, still distinct from the data topic. Both hold (246 + 9 = 255, so the
  truncation is a no-op, and 255 ≠ 246). The counterfactual `EXPECT_GT((std::string(247,'x') +
  "/__schema").size(), 255)` also holds: 256 > 255, so 247 is refused *because of the companion*,
  not because the data name would overrun. That is the right claim and it is what makes 246 the
  right number.
  **The inaccuracy:** its comment says *"Bounded at 255 instead, this assertion is what fails."*
  It does not. I re-ran **M9** on `b946798` and listed every failing assertion: lines 402, 408,
  442, 446, 450, 454, 455 — the 247-refusal, the separator-counting shape, `JoinSegmentsInto`, and
  the four provider entry points. The counterfactual and both product assertions use the *test's*
  own `kMaxJoinedBytes`/`kFastDdsAnnouncedCeiling` literals, so they are insensitive to the header
  constant and stay green under any bound mutation. The mutation is still caught — twice over, by
  a different assertion in the same case, and M9 still reddens **that case alone** (4 of 5
  `Segments.*` green). So the evidence is sound and only the sentence is wrong. **Non-blocking, and
  explicitly not worth a third cycle**: one clause, at the PM's convenience, or leave it.
- **N5 — fixed, and more than asked.** `fastdds-pubsub-provider/README.md`'s `### Topic name`
  section now carries the 246 cap, names the three `string_255` sites and
  `fixed_size_string.hpp:83,331` as `noexcept`, records the measured aliasing, states *why* 255 was
  rejected, and points at `Segments.NamesThatWouldTruncateOnTheWireAreRefused`. It also adds "the
  join is the **seam's**, not this provider's choice", which is the B1 correction reaching the
  provider docs.

## Final conformance sweep — all clean

- **Six refusals, one status.** `segments.hpp` contains 6 `PubSubStatus::` references and all 6 are
  `kInvalidArgument`. `core/include/fletcher/core/status.hpp` is **absent from the cumulative diff
  `722cc6b..b946798`**, its 10 one-value-at-a-time `static_assert`s are intact, and `kReentrantCall`
  appears **nowhere** in the tree — A3's allocation is untouched. The six map exactly onto the
  authorised eleven amendments (tenth = rule 5, eleventh = rule 6).
- **§12.1 byte-identical**, re-measured at `b946798`: `md5 e66e71c53dcc201467cca8cbe2cd8c17`,
  the same value as at `722cc6b`. The spec diff for the **whole item** is still exactly **one
  hunk**, inside §3.5; this cycle touches `docs/pubsub-interface-spec.md` **not at all** (0 lines).
- **§3.5's invariant is still scoped**: one occurrence of *"for every **accepted** segment list"*
  and one of *"Nothing is claimed for a list this section refuses"*.
- **Converse.** `grep -rn 'any separator'` outside `plans/` is still empty. All twelve provider
  entry points still route through `internal::JoinSegments` (4 + 4 + 4); no new bypass; the only
  refused-shape literal outside test and harness code is a doc-comment example in `segments.hpp:58`.
  `SplitTopic` still keeps every piece (0 `if (!seg.empty())`, 2 unconditional `push_back`s).
- Local runs at `b946798`: `pubsub_tests` **24/24**, `gateway-end-to-end` **31/31** (both provider
  contexts), plus the full mutation cycle above.

## Your two corrections — both right, and one addition

- **The runbook's XRCE flag:** you are right and I was wrong. `.claude/runbook.PDA-DEC.config.md:75`
  is prose about the conanfile requiring the xrce package unconditionally; both `cmake` sites carry
  `ON`. Withdraw that RECORD item.
- **The brief and the `static_assert`:** right — the brief predates ruling 43 and still describes
  four refusals, so it never claimed the `static_assert`. My RECORD line was about the wording of
  the re-check brief you sent me, not the Stage Brief; either way your As-landed delta covers it.
  Withdraw.
- **The gateway disclosure** is yours and I have not re-filed it. For the brief's Deleted/Risks
  lines, the concrete cost is: `"a//b"`, `"/a/b"`, `"a/b/"` and `"//a//b//"` are WebSocket topic
  strings that worked before this item and now return an error frame.
