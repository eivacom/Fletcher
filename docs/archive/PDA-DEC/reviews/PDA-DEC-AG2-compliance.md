# PDA-DEC-AG2 — compliance review (step 4a)

**Read receipt.** `plans/PDA-DEC-rulings.md` read in full. `grep -c '^## 2026'` = **57**.
Last two entries: *"Frozen §5.1 is amended to name the one message class that changes
(selection)"* and *"A zero-byte label is refused on arrival as well as on attachment
(selection)"*.

**Diff base** `102e56d`, working tree, uncommitted: 28 files, +1278 / −106 (band +700/+1600 —
inside it).
**Judged against** `plans/PDA-DEC-AG2-crossing-types.md` (rev 3), `-brief.md`, both design
reviews, `plans/reviews/design-debt.md` (AG2-DEBT-1…10, C2-DEBT-1…3), and
`docs/pubsub-interface-spec.md` as oracle.

**Verdict: FAIL — 4 blocking. One is an oracle-wins tripwire (B1) and is the PM's to
escalate, not a fix cycle.**

Built and ran, this box, `Windows-msvc194-x86_64-Release`: `core` 42/42, `pubsub` 24/24,
`fastdds-pubsub-provider` 88/88, `xrcedds-pubsub-provider` 16/16; conformance harness
configured with `-DFLETCHER_CONFORMANCE_XRCE=ON` **explicitly** (141 entries discovered),
`-R 'SeamVocabulary\.|Registry\.|CopyAccounting\.|conformance_xrce'` = 52/52 passed
including all seven new entries (#54–57, #77–79). Nothing here is a red test; every finding
below is a claim the tree does not support.

---

## B1 — BLOCKING · oracle-wins tripwire · frozen §5.1 publishes an exclusivity claim the tree falsifies

`docs/pubsub-interface-spec.md:716-720`:

> **Messages are unchanged, with one named exception**: a refusal built from a topic segment
> that itself contains a zero byte now shows that byte written out as `\x00` rather than ending
> at it. **That is the only class of message this seam renders differently** […]

Read off the type rather than off the prose — the practice this item's own revision 2 failed
on twice:

- `PubSubError::PubSubError(PubSubStatus, std::string)` (`core/include/fletcher/core/status.hpp:137`)
  routes **every** message through `Escape` (`:156`), which rewrites **every** zero byte in
  **any** string it is handed. There is no discrimination of origin, and there cannot be — the
  constructor does not know one.
- `TranslateSeamFailure` (`:190`) does `catch (const PubSubError&) { throw; }`, so a message a
  factory or a provider composed reaches the caller with the escape already applied.

The design says so itself. Revision 3 §4, **marked CORRECTED at review cycle 2**, reads: *"The
NUL routes into a message are exactly **two** […] `segments.hpp:103-113`'s `"…" + seg`, and **a
factory constructing `PubSubError(status, std::string)`, rethrown unchanged** (`status.hpp:145`)."*
Route (ii) is not a topic refusal.

And **this change's own forcing test is route (ii)**. `Registry.ARefusalIsReconstructibleFrom`
`ItsNumberAndMessageAlone` (`integration-tests/pubsub-conformance/src/registry.cpp:1051-1078`)
registers a factory that throws `PubSubError(kTransportFailure, <"a", NUL, "b">)` and asserts
the seam now publishes `a\x00b` where it published `a` at base. That is, verbatim, a class of
message this seam renders differently which is not the named class. The test is green; the
sentence is false; both shipped in the same change.

The same §5.1 paragraph contradicts itself two bullets later: rule 2 states the general
mechanism — *"`PubSubError`'s constructor rewrites each zero byte as the four characters
`\x00`"* — with no class restriction at all.

**Where it came from.** The cycle-2 design review closed with *"§5.1's amendment naming one
class is therefore **exhaustively true**"* (`PDA-DEC-AG2-design-review-c2.md:174-176`). That
conclusion reasons **only** about third-party text arriving through `e.what()` (a `const char*`,
which cannot carry an embedded NUL). It does not carry its own route (ii) — named eleven lines
earlier in the same review, in the acceptable-fix that created the forcing test — into the
conclusion. The implementation copied the conclusion into frozen text.

**Why this is escalation, not a wording fix.** Ruling 56 authorises naming *"the single class of
message that now changes"*. If the mechanism as built changes two classes, then either the
mechanism exceeds the ruling or the ruling's premise was wrong. §3 and §5.1 are `frozen` and
§12.1's *who may act* is **"nobody alone"**; the owner has twice this month (rulings 54, 56)
been asked for an explicit word before a sentence went into frozen text. This is the same
shape.

**Acceptable fix (PM's call which):**
(a) escalate — put to the owner that the escape sits on the constructor and therefore reaches
    any message carrying a zero byte, and ask whether §5.1's sentence should name that; or
(b) if the PM judges the second route outside §5.1's subject matter, say so **in the sentence**:
    name the exception as the only *Fletcher-composed* message that changes, and state plainly
    that a message supplied to `PubSubError` by a provider or an application is escaped by the
    same rule. Nothing in `core/README.md` or `status.hpp` needs changing — both already
    describe the mechanism correctly; it is only §5.1's exclusivity clause that over-reaches.

---

## B2 — BLOCKING · `docs/wire-format-specification.md:165` publishes two statements the same section's own evidence refutes

> **A key containing a zero byte is malformed.** An encoder cannot produce one
> (`Attachments::Set` refuses it), and a decoder that receives one drops the message as
> malformed […]

Seven lines above (`:163`) the same section names `gateway-client-ts/src/envelope.ts` as
**"a second, independent encoder"**. Both halves are false for that encoder/decoder pair:

- `serializeEnvelope` (`gateway-client-ts/src/envelope.ts:26-33,50-57`) writes
  `textEncoder.encode(key)` for each `Map<string, Uint8Array>` key. **Measured, not reasoned**
  — `node`, this box: `new TextEncoder().encode("a\u0000b")` → `[97, 0, 98]`. That encoder can
  and will produce a zero-byte key.
- `deserializeEnvelope` (`:93-104`) accepts it and round-trips it: `TextDecoder` returns
  `"a\u0000b"`. That decoder does not drop the message.

This is not an obscure corner. **Ruling 57's own premise is that file** — *"Reachable today from
the shipped client (`gateway-client-ts/src/envelope.ts:104`)"*. The ruling exists **because** a
second encoder can produce such a key; the document written under it asserts none can.

The declared deviation (AG2-IMPL-1) was assessed as asked: for **ordering**, the narrowing is
honest and sufficient — the layout block states the rule, the prose names the one encoder that
does not emit it, and every decoder matches by key so nothing breaks. For the **zero byte** the
same discipline was not applied, and the unqualified sentence is what a second implementer
reads.

**Acceptable fix:** narrow `:165` exactly as `:158-163` was narrowed — no Fletcher **C++**
encoder can produce such a key (`Attachments::Set` refuses it) and every Fletcher **C++**
decoder drops such a message; the TypeScript client neither emits the canonical order nor
refuses the byte, and a C++ peer drops what it sends. One sentence, same shape as the
paragraph above it.

---

## B3 — BLOCKING · frozen §3.2 clause A1 carries normative API rules no ruling authorises

`docs/pubsub-interface-spec.md:227-231`, introduced as *"Three clauses replace it, **and they
are normative**"*:

> **A1 — enumeration is positional, and there is only one.** […] `Find(key)` **answers absence
> with a null pointer rather than a failure**. There is deliberately **no iterator pair and no
> map API** — one mechanism, not two […]

§12.1 lists **§3 entire** as `frozen`; *who may act* is **"nobody alone."** The ledger's
authorisations over §3.2 are exactly two:

- **Ruling 53** — *"Side data goes out in a fixed order **stated in the spec**."* Applies to
  §3.2's `Attachments`, **"for attachment ordering only"**.
- **Ruling 54** — *"authorising **new normative text in frozen §3.2** for this refusal, **which
  no other change may treat as a general licence to amend frozen text**."*

Neither reaches "no iterator pair and no map API", and neither reaches `Find`'s absence
semantics. Both are new normative obligations on every later round.

Nor is A1 entailed by ruling 53. **AG2-DEBT-6 — raised in cycle 1, accepted, and closed as
discharged in revision 3 — established the opposite**: *"once `entries_` is sorted,
`begin()/end()` over that vector cannot drift from positional order. The unordered container
made the wire order a build artefact, not the range-`for`."* By the item's own accepted debt,
sealing the API is a design preference, not what the order rule requires. Preferences belong in
the design and in the header — and both already carry them, fully argued
(`core/include/fletcher/core/types.hpp:139-146`).

The design scoped this correctly and the implementation went past it: rev 3 §3 says *"**§3.2
gains exactly the NUL-key sentence**; the enumeration-order text of §2 rides on ruling 53, not
on this one."* Neither design review examined whether A1's non-order content is authorised.

**On the PM's related ruling, assessed as asked:** *retiring §3.2's `unordered_map` sentence is
covered by ruling 53* — **that reading holds**, and I would not have raised it. §3.2 cannot
state a fixed enumeration order while the same paragraph declares the type an unordered
container; the sentence cannot survive ruling 53 being carried out in the section ruling 53
names. The same reasoning carries **A2** cleanly. It does **not** stretch to A1's second half.
Keep the two apart in the register: the type sentence and A2 are covered; A1 is not.

**Acceptable fix, no code change:** keep in A1 only the order's vehicle — `size()`, `KeyAt(i)`,
`ValueAt(i)` are the enumeration and it is positional. Move "no iterator pair and no map API"
and the `Find`-returns-null rule up into the **non-normative narration** that already sits
immediately above the clause list (`:220-226`), which is where this section's history is told.
Alternatively carry it to the owner as a fresh word — the route rulings 54 and 56 both took.

---

## B4 — BLOCKING · the converse check: three source comments asserting the retired shape survived, one in the public seam header

The design's Files-to-delete retires the alias *"and every member it published"*. C2-DEBT-2
exists precisely because a stale published statement reads as current, and
`fastdds-pubsub-provider/README.md:522-524` states the rule in the project's own words: *"a stale
figure in a source comment reads as current."* Three such comments survive, all asserting, in
the present tense, that `Attachments` **is** an `unordered_map` that allocates a sentinel node:

1. **`pubsub/include/fletcher/pubsub/provider.hpp:129-133`** — the **public seam header**:
   *"An empty `Attachments` **is** an `unordered_map`, and MSVC allocates a sentinel node in its
   default constructor, so every delivery on every topic built and destroyed one […]
   **~110 ns per sample against 1.4 ns for the call itself**, more than the whole rest of the
   delivery path."*
   `provider.hpp` **is named in the design's Files-to-touch** and was not touched at all. Worse,
   `fastdds-pubsub-provider/README.md:535` — which this change *did* correct, to
   *"**The allocation is gone** […] so this number is historical"* — cites this file **by path**
   as where the decision lives. The document was fixed and the source it points at was not.
2. **`fastdds-pubsub-provider/src/internal/transport_data.hpp:52-56`** — *"Attachments **is** an
   unordered_map, and MSVC allocates a sentinel node in its default constructor […] **See README
   'Measured decisions'**."* The README row it cites (`:529`) was rewritten in this very change
   to say the opposite.
3. **`fastdds-pubsub-provider/benchmarks/bench_pub_sub_type.cpp:238-241`** — the rationale
   comment sitting directly on `BM_AttachmentsConstruct`. `benchmarks/README.md:28` was
   corrected for exactly this benchmark; this file **was edited by this change** (`:439`); the
   comment two hundred lines above the edit was left.

Premise P5 (*"the compiler, not a ledger, proves none was dropped"*) cannot reach prose, which
is why the converse sweep has to be done by hand — and it was not. Note the code sweep itself
is **clean**: I re-measured and found no surviving `unordered_map<…, Blob>`, no
`operator[]`/`at`/`count`/`find`/`emplace`/`insert_or_assign`/`clear` on an `Attachments`, and
no range-`for` over one, anywhere outside `build/`. It is only the rationale text that survived.

**Acceptable fix:** the same one-sentence edit already applied to the two README rows, at each
of the three sites. Site 1 is the one that matters — it is what a language-binding author reads.

---

## Non-blocking findings

**N1 — the registry comment the design ordered was not written.** Design §5 and Files-to-touch:
*"`{include/fletcher/pubsub,src}/provider_registry.{hpp,cpp}` — **comment only** […] its two
sites gain a comment naming ruling 55 and the seal (so no later reader 'optimises' the container
and reopens A7)."* Neither file is in the diff; `provider_registry.hpp:280-285`'s comment is the
pre-existing PDA-DEC-4 one and names neither ruling 55 nor AG2. Not blocking: the property is
now guarded by `Registry.TheRefusalListIsAFunctionOfTheRegistrysContents`, which asserts the
rendered list literally and is a stronger guard than a comment. A Files-to-touch deviation, not
a lost property.

**N2 — rung-2 item 9 ships unguarded.** *"An out-of-range index — `KeyAt`/`ValueAt` refuse with
`kInvalidArgument`. One check, no clamping, no sentinel entry, no 'valid sometimes' index"* is
implemented (`types.hpp:232-238`) but **nothing anywhere calls `KeyAt(size())`**. Every other
rung-2 item in this item got a two-legged test with its bound. Not a deviation — the approved
forcing-test mapping has no row for it — but the ladder ships one rung whose removal would
redden nothing.

**N3 — this change introduces a contradiction inside `docs/protocol-driver-abi-spec.md`.**
`:4` still reads *"the authoritative spec for the **pure-C** protocol driver ABI"*; `:22-23` now
reads *"**Both sides of it are C++**"*. Ruling 46's own words are *"both sides of the protocol
ABI will be **pure C++**"*. The design scoped the ruling-46 amendment to `:21-23` and §0.2
`:79-81` and the implementation obeyed it exactly, so this is a design gap faithfully
reproduced rather than a deviation; the document is `proposed`, not frozen, and ruling 46 wins
regardless. One line, addressed to PDA-ABI or whoever next opens that spec.

**N4 — AG2-IMPL-1 discloses half the TS-client consequence.** The carried-debt entry covers
ordering (*"correct but not canonical"*). The other half of the change also lands there: a TS
client publishing an attachment key containing `U+0000` now has its whole frame refused by the
gateway (`gateway/src/ws_session.cpp:279` → `DeserializeEnvelope` → `std::invalid_argument`,
caught at `:132` and returned as an error) and its DDS samples dropped. Ruling 57 authorised the
refusal, so this is **disclosure, not a violation** — but it is a live behaviour change at the
one boundary the item deliberately does not touch, and it belongs in the same debt entry.

---

## Checked and conformant — no finding

Recorded because these are the places a deviation would most likely hide, not to pad depth.

- **Ruling 57's shape, exactly.** All three `Attachments::Set` call sites reachable from wire
  bytes are guarded by a `memchr` immediately above: `core/.../envelope.hpp:168` → `:182`,
  `fastdds/.../envelope_codec.hpp:105` → `:116`, `benchmarks/legacy_fletcher_topic_type.hpp:128`
  → `:138` (C2-DEBT-1 discharged in code, not by footnote). **`Set`'s throw is genuinely
  unreachable from every decode path** — enumerated: those three are the only `.Set(` call sites
  in non-test code, and `xrce_dds_pubsub_provider.cpp:353` and `ws_session.cpp:279` both reach
  the wire through `DeserializeEnvelope`. `ParseEnvelopeBody` returns `false`;
  `DeserializeEnvelope` raises `std::invalid_argument`, never `PubSubError`. Both are asserted
  on the exception **type**, and `FletcherSamplePubSubTypeTest.ASampleWithAZeroByteAttachment`
  `KeyIsRejected` asserts it through `deserialize()` itself — the frame the ruling is about.
  Forbidding, not handling.
- **Ruling 53 was not widened.** No byte other than attachment order moved: `SerializeEnvelope`
  and `EncodeEnvelopeBody` changed only their enumeration, and the layout is untouched. The
  claim is asserted **on the bytes** at both encoders
  (`SeamVocabulary.TheSameMessagePublishesTheSameWireBytes`,
  `EnvelopeCodecTest.TheSameBodyIsEncodedToTheSameBytes`), not on the container — AG2-DEBT-2
  discharged as specified.
- **Ruling 55 was not widened or quietly restored.** No `RegisteredNames` anywhere in the tree;
  no `internal/` variant; `Register`/`Create` untouched; the property is asserted through the
  message alone, with the rendered list pinned to the literal `"alpha, bravo, mike, zulu"`
  (AG2-DEBT-3 discharged).
- **New public surface is 1.** `fletcher::Attachments` only. `PubSubError::Escape` is `private`
  (`status.hpp:142` … `:156`). `PubSubStatus` still 0–10. `PubSubProvider`'s five virtuals
  unchanged. No `extern "C"`, no `dlopen`, no C header, no vtable — locked decision 14 and
  premise P4 hold.
- **The re-anchored control still controls.** `Registry.ANumberAloneCannotTellTwoRefusalsApart`
  uses the real `inprocess` built-in for both legs and `ASSERT`s each is `kInvalidArgument`
  *before* comparing the two messages, so the re-aim did not turn a control into a tautology.
  The design's *"(unchanged)"* marker is stale — the entry is new to the tree — but its shape
  and its property are those the design specified.
- **A7's forcing test is red at base**, on the collapse leg rather than the reconstruction leg:
  at base both causes publish `"a"` and `EXPECT_NE(ab.message, ac.message)` fails.
  (The reconstruction leg is idempotent over an already-truncated message and is green either
  side — the design's row attributes the redness to the wrong leg. Not a finding: the test
  reddens deterministically, which is what C2-B1 demanded.)
- **Counts re-measured independently.** **56** source files name `Attachments` outside `build/`
  — the implementer's figure, correcting revision 3's 54 — and I get the same 56.
  Thirteen `TEST(` in `seam_vocabulary.cpp` and twenty-two in `registry.cpp`, matching the
  harness README's "thirteen entries" / "22 entries" / "27 `Registry.` ctest entries in all".
  AG2-DEBT-4 pinned by `Taxonomy.TheZeroByteEscapeIsDeliberatelyNotInjective`; AG2-C2-DEBT-3
  stated beside `Set` in `types.hpp:157-163`.
- **P1, P2, P3 hold.** No decoder matches by position; `pubsub-arrow`'s `SubscribeResult`
  exposes `std::vector<Attachments>` and no map semantics, so the stop-and-ask P2 guards was not
  reached; no second message channel was added.

---

## RECORD (paperwork only — PM corrects in place, never a fix cycle)

- `plans/reviews/design-debt.md`, implementation-pass note: *"Two files outside Files-to-touch
  also needed real edits […] `xrcedds-pubsub-provider/test_package/src/example.cpp` (**which is
  in Files-to-touch**) and `fastdds-pubsub-provider/benchmarks/bench_pub_sub_type.cpp` (**which
  is**, under `benchmarks/*`)."* The sentence says "outside" and then says both are inside. The
  arithmetic is right (11 + 2 = the 13 files with real map-expression edits); only the lead-in
  is wrong.
- `plans/PDA-DEC-AG2-crossing-types.md`, forcing-test mapping row 4, marks
  `Registry.ANumberAloneCannotTellTwoRefusalsApart` *"(live negative control — **unchanged**)"*.
  It is new to the tree at this base; nothing was unchanged.
- Same document, row 3, calls A7's forcing test *"deterministically red on the **reconstruction**
  leg"*. It is red on the collapse leg. The test is red at base either way.
