# PDA-DEC-AG2 — compliance review, cycle 2 (step 4a re-review)

**Read receipt.** `plans/PDA-DEC-rulings.md` read in full. `grep -c '^## 2026'` = **58**.
Last two entries: *"A zero-byte label is refused on arrival as well as on attachment
(selection)"* (ruling 57) and *"§5.1 states the general escape rule, not one class
(selection)"* (ruling 58 — new since cycle 1, committed as `291dd19`, and the authority for
cycle-1 B1).

**Scope.** Re-review of the cycle-1 fix increment only. Base `102e56d`, working tree,
uncommitted: 37 files, +2232 / −142. **Not re-audited, as directed and as cleared in cycle 1:**
the ordering mechanism, §3.2 clauses A2/A3, the 56-source-file / 11-map-expression counts,
ruling 55's non-restoration, premises P1–P5, and the new-public-surface-is-1 check.

**Verdict: PASS-WITH-FINDINGS — 0 blocking.** B1–B4 all discharged, verified against the tree
rather than against the report. **No oracle-wins tripwire remains**: the one that existed
(cycle-1 B1) was escalated, answered by ruling 58, and the landed sentence sits inside what that
ruling authorises. Four new non-blocking findings — one of them a converse-check survivor about
ruling 58 itself — and six RECORD lines follow.

---

## The four cycle-1 blockers — verified closed

### B1 — CLOSED. §5.1's third attempt is true, and no wider than ruling 58 authorises.

`docs/pubsub-interface-spec.md:729-741`. Read off the mechanism, not the report:

- **The general rule is stated, and it is the rule the code implements.** `PubSubError::Escape`
  (`core/include/fletcher/core/status.hpp:156-168`) is reached from the single
  `PubSubError(PubSubStatus, std::string)` constructor (`:137`) and rewrites every zero byte in
  whatever string it is handed, with no discrimination of origin — there is none available to
  it. `TranslateSeamFailure` (`:190-192`) rethrows a `PubSubError` unchanged, so a
  factory-composed or provider-composed message reaches a caller with the escape already
  applied. §5.1's *"this seam escapes in ANY message it publishes … whatever composed the
  message"* is that mechanism, described correctly for the first time.
- **The self-falsifying exclusivity claim is gone.** *"That is the only class of message this
  seam renders differently"* is replaced by *"that is an **example of the rule and not a limit
  on it**"*. The item's own forcing test
  `Registry.ARefusalIsReconstructibleFromItsNumberAndMessageAlone` uses the factory route, which
  the sentence no longer excludes — the contradiction that made cycle-1 B1 a tripwire is
  dissolved rather than papered over.
- **The named in-tree example is still accurate, re-derived rather than trusted.**
  `pubsub/include/fletcher/pubsub/internal/segments.hpp`: the `__`-prefix refusal (`:102-107`)
  concatenates `seg` and fires before the per-char loop; the `/` refusal (`:110-113`)
  concatenates `seg` and sits *above* the `c == '\0'` check (`:115-119`) in the same loop body.
  For a segment whose first offending byte is the NUL, the zero-byte refusal fires first and its
  message carries no segment — so it is genuinely those two routes and no third.
- **No second in-tree class was introduced by this increment.** Re-checked, because the fix
  added two new throw sites: `Attachments::Set`'s refusal reports the NUL's *offset* and never
  the key (`core/include/fletcher/core/types.hpp:207-213`), and `Attachments::RequireInRange`
  carries only integers (`:266-271`). The registry's refusals route every raw string through
  `Quoted`, which renders a zero byte as `\x00` (`pubsub/src/provider_registry.cpp:52-68`).
- **Ruling-58 scope.** The landed wording tracks the selected option almost verbatim: general
  rule stated; topic refusals named as *"the class that arises in Fletcher's own code today"*,
  explicitly as an example. It reaches into no other frozen section, and the attribution line
  names both rulings and says which widened which — which is what ruling 56's surviving half
  (the fact of the amendment, non-generalising to any other frozen section) requires.

### B2 — CLOSED. The wire-format zero-byte sentence is narrowed to the shape of the ordering sentence above it.

`docs/wire-format-specification.md:167` now binds *"Fletcher's **C++** encoders and decoders"*,
names `gateway-client-ts/src/envelope.ts` as the exception, states the `TextEncoder` /
`TextDecoder` behaviour **as measured**, and publishes the actual outcome — a Fletcher C++ peer
drops the sample, the gateway refuses the publish frame — instead of asserting the byte cannot be
produced. `:174`'s `Key Types` row no longer says `unordered_map`. This is acceptable-fix as
specified, plus the outcome sentence, which was not asked for and is right.

### B3 — CLOSED. Frozen §3.2 clause A1 is reduced to the order's vehicle.

`docs/pubsub-interface-spec.md:249-251`: A1 now reads *"enumeration is positional. `size()`,
`KeyAt(i)` and `ValueAt(i)` are the enumeration, and a boundary reads the published sequence
through them"* — and nothing else. The sealing rules ("no iterator pair and no map API",
`Find`-returns-null) moved into the narration block at `:227-241`, are labelled *"narration
rather than obligation"*, and that block now states plainly that the sealing **is not what the
order rule requires** — *"a sorted range walked by a range-`for` would satisfy it equally"* — and
is *"deliberately not imposed on a later round or on a second implementation of this seam"*.
That is acceptable-fix (a), applied exactly, and it retires the unauthorised normative
obligation rather than relocating it. A2 and A3 re-read line by line: unchanged from what cycle 1
cleared.

### B4 — CLOSED, and the sweep re-run rather than accepted.

Four sites corrected — all past tense, number marked historical:
`pubsub/include/fletcher/pubsub/provider.hpp:129-137` (the public seam header, the one that
mattered), `fastdds-pubsub-provider/src/internal/transport_data.hpp:52-59`,
`fastdds-pubsub-provider/benchmarks/bench_pub_sub_type.cpp:238-244`, and
`fastdds-pubsub-provider/src/internal/data_reader_listener.hpp:160-165` — the fourth, which
cycle 1 did not list and the implementer found.

**Sweep re-run independently**, case-insensitive `unordered_map` over `*.hpp *.cpp *.h *.md *.ts
*.txt` outside `build/`: every remaining occurrence naming `Attachments` is past-tense narration
(`types.hpp:130,189,228,295`, `spec:220`, `test_envelope.cpp:248`, `seam_vocabulary.cpp:525`,
both `fastdds-pubsub-provider/README.md` rows, `benchmarks/README.md:28`); every other occurrence
is an unrelated live container (`topics`, `callbacks_`, `codecs_`, `subscriptions_`, …).
**No surviving present-tense `unordered_map` claim about `Attachments` anywhere in the tree.**
The implementer's claim holds.

---

## The new mechanism — `internal::AttachmentsWireBuilder`

**Ruling 57's shape survives, and by a stronger construction than the design's.** The ruling
required the refusal to sit with the existing wire checks, `Set`'s throw unreachable from any
decode path, the caller-fault / wire-fault type split intact, and *"forbidding, not handling"*.
All four verified.

**Decode paths enumerated exhaustively.** `grep '\.Set('` over the tree returns **no**
product-code call site on an `Attachments` at all — every remaining one is in a unit test, a
benchmark driver or the conformance harness, i.e. the local-attachment door the ruling leaves
alone:

| Decode path | Refusal | Can reach a throwing `Set`? |
|---|---|---|
| `core/envelope.hpp:171-192` `DeserializeEnvelope` (3-arg; the 2-arg and vector forms delegate) | `build.Append` false → `throw std::invalid_argument`, beside the four existing truncation throws | no — no `Set` call remains on the path |
| `fastdds…/src/internal/envelope_codec.hpp:98-121` `ParseEnvelopeBody` (callers: `fletcher_sample_pub_sub_type.hpp` `deserialize()`, `data_reader_listener.hpp` `on_data_available`) | `build.Append` false → `return false`, beside the eight existing bounds `return false`s | no |
| `fastdds…/benchmarks/legacy_fletcher_topic_type.hpp:119-142` `deserialize()` | `build.Append` false → `return false` | no |
| `xrce_dds_pubsub_provider.cpp`, `gateway/src/ws_session.cpp` | reach the wire only through `DeserializeEnvelope` | no |

- **The type split is intact and asserted on the type, not on the message.**
  `SeamVocabulary.AnAttachmentKeyThatWouldTruncateIsRefused` (`seam_vocabulary.cpp:806-816`)
  keeps the disjoint pair — `catch (const PubSubError&) { ADD_FAILURE("…decode was routed
  through Attachments::Set"); }` against `catch (const std::invalid_argument&) { SUCCEED(); }` —
  with the note that `std::runtime_error` and `std::logic_error` are disjoint hierarchies so it
  cannot pass by accident, plus the clean-key bound so a decoder refusing everything is not
  green. `EnvelopeCodecTest.AnArrivingAttachmentKeyWithAZeroByteIsDroppedAsMalformed` and
  `FletcherSamplePubSubTypeTest.ASampleWithAZeroByteAttachmentKeyIsRejected` cover the Fast DDS
  `deserialize()` frame the ruling is actually about.
- **Forbidding, not handling — the refusal did not become a recovery path.** `Append` returns
  `false` *and appends nothing*; it is `[[nodiscard]]`; every one of the three callers refuses
  the whole sample. No path skips the offending entry and keeps the rest, which is the shape
  that would have turned ruling 57's refusal into tolerance.
- **The published form cannot be violated through the new door.** `Append`'s `ascending_` latch
  clears on `!(entries.back().key < key)`, so a duplicate or any out-of-order key forces the
  `Finish()` path; that sort is `stable_sort` and the dedupe keeps the last of each equal run,
  reproducing `Set`'s and the retired `insert_or_assign`'s last-wins exactly — pinned by
  `EnvelopeTest.ADuplicateAttachmentKeyOnTheWireResolvesLastWins`. `entries_` is private with the
  builder as its only `friend`, so `Set` and `Append` are the only two doors into an
  `Attachments` and **both** refuse a NUL key. Rung 1 (unrepresentable) and rung 2 (refused typed
  at the door) of the corner-case ladder therefore still hold as stated.
- **No behavioural regression at the two early returns.** `ParseEnvelopeBody`'s `total < 4` and
  `row_len > total - 4` returns sit *above* the builder — exactly where the retired
  `attachments.clear()` sat *below* them. The listener's reused container was not cleared on
  those two paths before this change either.

**The two flagged design deviations — both in the design's direction, neither a drift.**

1. **The benchmarks' legacy decoder routed through the builder.** Its map expression had to
   migrate regardless (the alias is retired; `benchmarks/*` is in Files-to-touch). Routing it
   through the builder rather than `Set` is what makes the design's own §3 sentence — *"`Set`'s
   throw is thereby unreachable from every decode path"* — structurally true rather than true by
   a hand-copied guard, and discharges AG2-C2-DEBT-1 on its stronger limb rather than its
   footnote limb. The benchmark comparison is unaffected: both arms already held the same
   container, and the legacy arm measures the retired `TopicDataType` wiring, not the container.
2. **`Attachments::Clear()` now called by the builder.** The design's Files-to-delete justifies
   `Clear` as existing because *"the reused decode out-param needs it and nothing else can empty
   the container"*. That reason is intact — the builder *is* the decode out-param's clearing
   mechanism, and `types.hpp:226-231` says so in those words (*"the builder moved WHERE that call
   is made, not what makes it"*). The declared public surface is unchanged, which is what
   conformance requires; making `Clear` private would itself have been a deviation from the
   approved class shape.

**Is the builder itself surface the design did not declare?** It is `fletcher::internal::` in an
installed header, which is the tree's existing convention for non-seam helpers
(`internal::EnvelopeAttachmentCount` in the same file family, `internal::status_name`), and it is
stated as such at `types.hpp:315-316`. Locked decision 14 / premise P4 are untouched: no C
declaration, no `extern "C"`, no struct shared with either ABI round. §3.2's mention of it sits
inside the narration block and is load-bearing there — without it the narration's *"no map API —
one mechanism, not two"* would be incomplete now that a second mutating door exists. Not a
finding.

**Converse check — what survived that should not.** Nothing did, in code.

- The three hand-copied `memchr` NUL guards are gone. `grep memchr` over `*.hpp`/`*.cpp` outside
  `build/` returns **zero** product-code hits; the only survivors are prose (`types.hpp:308`, the
  design doc, the review register).
- **No test asserts the deleted guards.** Both arrival-leg tests assert the *observable* refusal
  (`false` / `std::invalid_argument` / not-`PubSubError`), never the mechanism, so they survived
  the mechanism swap correctly and would still redden if decode were re-routed through `Set`.
- `RegisteredNames()` and `Registry.TheAvailableNamesAreAnsweredWithoutReadingProse`: absent
  everywhere, including under `internal/`. Re-checked because new registry text landed this
  cycle.
- The retired map API: no `at` / `count` / `find` / `emplace` / `insert_or_assign` / `clear` /
  `begin` / `end` and no range-`for` on an `Attachments` anywhere outside `build/`.
- `docs/protocol-driver-abi-spec.md`'s any-language driver licence: withdrawn at `:87-91`
  (*"withdrawn, not relocated"*), and `:3-7`'s contradicting "pure-C" line is amended too —
  cycle-1 N3 closed.
- `docs/wire-format-specification.md:165`'s `unordered_map` Key-Types row: gone, replaced by the
  sealed container as Files-to-delete ordered.

**One survivor, and it is about ruling 58 itself — see C2-N1 below.**
`core/tests/test_status_taxonomy.cpp:266-269` still says *"the owner's 2026-09-06 ruling names
exactly **one class** of message that may change — a topic refusal built from a segment carrying
a zero byte."* Ruling 58 superseded ruling 56 as to exactly that scope three commits before this
increment, and §5.1 as landed says the opposite eleven lines away. The assertion it annotates
(a message with no zero byte is unchanged) is still correct; only the attribution is stale. It is
the one place the implementer's stale-statement sweep did not reach, and it is the sentence this
cycle exists to have got right.

---

## Non-blocking findings

**C2-N1 — a test comment still asserts ruling 56's superseded one-class scope.**
`core/tests/test_status_taxonomy.cpp:266-269`, quoted in full above. *Acceptable fix, one
sentence:* "…the owner's rulings of 2026-09-06 permit the escape to change **any** message that
carries a zero byte; what is pinned here is that a message carrying none is untouched." Not
blocking — it is a source comment in a test, it misstates a ruling rather than the code, and the
assertion beneath it is sound. Flagged as a named finding rather than a RECORD line because it is
in the tree, not in a register, and because the implementer's claimed tree sweep should have
found it.

**C2-N2 — §5.1 lists "a third party's `what()`" among the composers whose zero bytes are
escaped, and that one can never carry a zero byte.** True as written — the constructor does
escape every zero byte in whatever it is handed — but a binding author reading only §5.1 could
conclude that a NUL inside a third-party exception's message survives. It does not: `what()`
returns a `const char*` and has truncated before Fletcher sees the tail, which the design records
(`plans/PDA-DEC-AG2-crossing-types.md` §4: *"`e.what()` is **not** one"*) and on which the
cycle-2 design review's conclusion rested. Not blocking: the sentence is not false, and ruling
58's own selected text is deliberately general. *Acceptable fix, one clause, PM's call whether it
is worth reopening frozen text for:* after "a third party's `what()`", add "— though a `what()`
string has already truncated at any zero byte it held, before Fletcher receives it".

**C2-N3 — `docs/wire-format-specification.md:167` gives one reason where there are now two
doors.** *"`fletcher::Attachments` is the only way a key reaches either encoder and
`Attachments::Set` refuses it"*. Since this increment there is a second mutating door,
`internal::AttachmentsWireBuilder::Append`, which also refuses a NUL key — so the **conclusion**
holds without qualification (no `Attachments` can hold such a key), but the stated reason names
one of two mechanisms. Non-normative prose in a non-frozen document. *Acceptable fix:* "…and both
doors into it — `Set` and the decoders' bulk builder — refuse it".

**C2-N4 — `core/tests/CMakeLists.txt` is edited and is not in Files-to-touch.** The edit is
comment-only: the 30 s ctest `TIMEOUT` rationale, re-derived for the 44th row, which now depends
on that timeout being the thing that reddens if the decode goes quadratic again. Correct and
necessary. A Files-to-touch omission of the class this register has booked before (PDA-DEC-6
DEBT-5, PDA-DEC-3 DEBT-4); no property is at risk.

**Cycle-1 non-blocking items — disposition, each stated explicitly.**

| | Status |
|---|---|
| **N1** — the ruling-55 registry comment the design ordered was never written | **CLOSED.** `provider_registry.hpp:280-291` and `provider_registry.cpp:171-176` both carry it now, naming the 2026-09-06 ruling, the `std::map` seal, and `Registry.TheRefusalListIsAFunctionOfTheRegistrysContents` as the guard that catches a re-typing. Files-to-touch satisfied. |
| **N2** — rung-2 item 9 (out-of-range index) shipped unguarded | **CLOSED.** `seam_vocabulary.cpp:670-702` walks `{size(), size()+1, ~size_t{0}}` through both `KeyAt` and `ValueAt`, asserts `kInvalidArgument` on each, and carries both bounds — `size()-1` still valid, and every index outside on an empty set. The ladder rung is now removable-and-red. |
| **N3** — `protocol-driver-abi-spec.md:4` contradicted `:22-23` | **CLOSED**, and better than asked: `:3-7` amends the line *and says why it read otherwise*, rather than silently deleting the contradiction. |
| **N4** — AG2-IMPL-1 disclosed only the ordering half of the TS-client consequence | **STILL OPEN, still non-blocking, now largely moot.** The debt entry is unchanged, but `wire-format-specification.md:167` publishes the refusal half in the document a TS-client author actually reads, which was the substance of the complaint. Left to the PM, as cycle 1 said. |

---

## RECORD (paperwork only — PM corrects in place; never blocking, never a fix cycle)

- `plans/reviews/design-debt.md:724` — AG2-C2-DEBT-1's discharge still reads *"the benchmark
  decoder got the same `memchr` guard (two lines)"*. There is no `memchr` guard anywhere in the
  tree; the discharge is the builder, described accurately eleven lines below under
  AG2-C2-DEBT-3. Only this row is stale, and it understates the discharge.
- `plans/reviews/design-debt.md:706-708` — carried from cycle 1, uncorrected: *"Two files outside
  Files-to-touch also needed real edits"* followed by both files being marked as inside it.
  Arithmetic right, lead-in wrong.
- `plans/PDA-DEC-AG2-crossing-types.md`, forcing-test mapping row 4 — still marks
  `Registry.ANumberAloneCannotTellTwoRefusalsApart` *"(live negative control — unchanged)"*; it
  is new to the tree at this base.
- `plans/PDA-DEC-AG2-crossing-types.md`, forcing-test mapping row 3 — still attributes A7's
  redness to the *reconstruction* leg; it is red on the collapse leg. Red at base either way.
- `plans/PDA-DEC-AG2-crossing-types.md` §3 and Files-to-touch still describe the arrival refusal
  as *"`ParseEnvelopeBody` gains one `memchr(key, 0, key_len)`"*. Superseded by the builder. The
  implementation-pass note in `design-debt.md` records the change, so this is the design doc
  lagging its own recorded amendment, not an undeclared deviation.
- `plans/PDA-DEC-AG2-crossing-types.md:5` still says the ledger holds *"57 entries"*; it is 58,
  and revision 3 predates ruling 58.

---

## Build and test evidence — this box, `Windows-msvc194-x86_64-Release`

- `core` — **44/44 passed** (1.06 s), including `EnvelopeTest.ADescendingAttachmentKeyOrder`
  `DoesNotMakeDecodeQuadratic`, `EnvelopeTest.ADuplicateAttachmentKeyOnTheWireResolvesLastWins`,
  `Taxonomy.AMessageCarryingAZeroByteSurvivesTheBoundary`,
  `Taxonomy.TheZeroByteEscapeIsDeliberatelyNotInjective`.
- `fastdds-pubsub-provider` — **88/88 passed**, including
  `FletcherSamplePubSubTypeTest.ASampleWithAZeroByteAttachmentKeyIsRejected` (#24),
  `EnvelopeCodecTest.TheSameBodyIsEncodedToTheSameBytes` (#63),
  `EnvelopeCodecTest.AnArrivingAttachmentKeyWithAZeroByteIsDroppedAsMalformed` (#64).
- `pubsub` — **24/24 passed**. `xrcedds-pubsub-provider` — **16/16 passed**. `pubsub-arrow` —
  **16/16 passed**.
- Conformance harness — `conan install` + `cmake --preset conan-default
  -DFLETCHER_CONFORMANCE_XRCE=ON` (**explicit**, as instructed) + `ctest --preset conan-release`:
  **141/141 passed**, 26 `SeamVocabulary.` and 54 `Registry.` log lines, all seven of this item's
  new entries green — `SeamVocabulary.AnAttachmentSetIsReconstructibleFromItsPublishedFormAlone`
  (#54), `AnAlteredAttachmentSetPublishesADifferentForm` (#55),
  `AnAttachmentKeyThatWouldTruncateIsRefused` (#56), `TheSameMessagePublishesTheSameWireBytes`
  (#57), `Registry.ARefusalIsReconstructibleFromItsNumberAndMessageAlone` (#77),
  `ANumberAloneCannotTellTwoRefusalsApart` (#78),
  `TheRefusalListIsAFunctionOfTheRegistrysContents` (#79). No `CreateProcess error 2`: the peer
  binary is present in this build tree, so the `FastDdsCrossProcess` cases ran and passed.

Nothing below is a red test. Every finding above is a claim about the tree, established by
reading the tree.
