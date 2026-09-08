# PDA-DEC-AG2 — architecture review, design cycle 1 of 2

**Verdict: NEEDS-REWORK — 3 BLOCKERs, 7 DEBT.**
**Oracle-wins tripwire: YES, one** — B3, against frozen §5.1's *"Messages are unchanged"*.

## Read receipt — the rulings ledger

`grep -c '^## 2026' plans/PDA-DEC-rulings.md` → **55**. Read in full.
Last two entries:

- **54** — *2026-09-06, "A side-data label containing a zero byte is refused" (selection).*
- **55** — *2026-09-06, "A refused application gets Fletcher's sentence, not a list of names" (selection).*

Rulings 46, 53, 54, 55 checked line by line against the design. **All four are honoured
as written** — see "What the design gets right" below. No ruling deviation, no locked-decision
deviation, and therefore **no STOP-AND-ASK on authorisation**. Every BLOCKER below is inside the
authorisation the design already holds; each is a reach the design did not trace.

---

## BLOCKERs

### B1 — the NUL-key refusal's *decode-side* reach is unstated, and the obvious reading puts a throw inside Fast DDS's `deserialize()`

Design §2 makes `Set` the **only** insertion point ("removing it forces every producer …
through the published form"), and §3 makes `Set` throw `kInvalidArgument` on a NUL key. The
design traces this to the attach side only. It is not the only side.

Verified in the tree — three decode paths insert **wire-supplied** keys:

| Site | Today | After `Set` throws |
|---|---|---|
| `fastdds-pubsub-provider/src/internal/envelope_codec.hpp:100` (`insert_or_assign`, inside `ParseEnvelopeBody`) | `ParseEnvelopeBody` is `bool`-returning and **never throws**; every malformed case is `return false` (`:70,73,83,85,89,92,96,97`) | throws out of `ParseEnvelopeBody` |
| `…/internal/fletcher_sample_pub_sub_type.hpp:166,177` — the caller of the above | inside Fast DDS's **`deserialize()`**; `false` → `return false` | an exception into an SDK frame — the §5.3 hazard by name |
| `…/internal/data_reader_listener.hpp:207` — the other caller | inside `on_data_available`; `false` → `EPROSIMA_LOG_WARNING` + `continue` | an exception into a transport listener |
| `core/include/fletcher/core/envelope.hpp:160`, inside `DeserializeEnvelope` | header contract at `:116-118`: *"a malformed or truncated buffer is `std::invalid_argument` (a wire problem), a non-empty attachment with no owner is `PubSubError(kInvalidArgument)` (a caller problem)"* | a **wire** fault arrives as the **caller** type, contradicting that header |

**Reachable from a shipped client today**, not hypothetically: `gateway-client-ts/src/envelope.ts:104`
encodes any JS string as an attachment key (JS strings may hold `\x00`), and the gateway parses
that frame at `gateway/src/ws_session.cpp:279`. This is the same "a WebSocket client **can** send
such a part today" reasoning the owner used when authorising the `__` prefix (ruling, 2026-09-04).

Ruling 54's chosen option is worded **"Refuse it when attached."** The receive side was not put to
the owner, and the design's corner-case ladder has no rung for it — rung 2 items 7 and 8 are the
attach key and the index.

**Acceptable fix (forbidding is cheaper than handling):** put the wire-side NUL check where the
other five wire checks already are, not at `Set`. `ParseEnvelopeBody` gains one
`memchr(ptr, 0, key_len)` and `return false` (the existing "dropped a sample: malformed envelope"
path); `DeserializeEnvelope` throws `std::invalid_argument`, its own published wire-problem type.
`Set`'s throw then becomes **unreachable from every decode path**, so no exception is introduced
into a transport frame and no wire fault is mislabelled a caller fault. Add it to the design as a
rung-2 entry with one line in the forcing-test mapping. Do **not** solve it by making `Set`
non-throwing — that reopens the collision ruling 54 closed.

### B2 — Files-to-touch omits published contract text that this change falsifies, including the wire-format spec ruling 53 points at

Verified:

- **`docs/wire-format-specification.md:165`** publishes
  `| Attachments | unordered_map<string, Blob> | Key/value blob pairs |` in its *Key Types* table.
  After the alias is retired this published statement is **false**, with no machine check to
  redden. Not in Files-to-touch.
- The same document's **§"Envelope Wire Format" (`:142-158`)** is the published description of the
  exact bytes ruling 53 authorises moving, and ruling 53's chosen option reads *"Side data goes out
  in a fixed order **stated in the spec**"*. The design states the order in
  `pubsub-interface-spec.md` §3.2 only. The wire-format document says nothing about order and is
  not touched.
- **`fastdds-pubsub-provider/README.md:529` and `:535`** are two published rationale rows whose
  stated premise is that `Attachments` **is** an `unordered_map` that allocates a sentinel node on
  default construction (52 ns and ~110 ns, measured). Both become stale. Not in Files-to-touch.
- **`pubsub/tests/test_publisher_subscriber.cpp:388-393`** uses `emplace` and `count` on an
  `Attachments` and genuinely needs migration. Not in Files-to-touch.

This is the AG1 failure class exactly: the deliverable is published form, and the item leaves
published form contradicting the code.

**Acceptable fix:** add the four files to Files-to-touch; correct the Key Types row and add the
ordering sentence to `docs/wire-format-specification.md`'s envelope section (that document is not
`frozen`, so this is an edit, not an amendment); correct or delete the two README rationale rows.
Four lines of design, well under a page of work.

### B3 — the declared oracle check on frozen §5.1 rests on a tree fact that is false — **oracle-wins tripwire**

Design `:118-119`: *"**Only** NUL is escaped, so §5.1's 'Messages are unchanged' stays true of
every message in the tree — none contains one."* And `:245`: *"**Oracle check performed:** §5.1's
'Messages are unchanged' remains true, because no message in the tree contains a NUL."*

A NUL-bearing message is reachable **today**, from statically composed text:
`pubsub/include/fletcher/pubsub/internal/segments.hpp:103-106` (the `__`-prefix refusal) and
`:110-113` (the `/` refusal) both concatenate the **raw segment** into the message, and both fire
**before** the per-byte NUL check at `:115`. So `{"__a\0b"}` and `{"a/b\0c"}` produce messages
containing a raw NUL now. Under the design, `PubSubError`'s constructor rewrites those messages.

Two consequences, both material:

1. The design's handled-residue #10 justification — *"Escaped only where caller data is composed
   in (`Quoted`, `provider_registry.cpp:52`)"* — misdescribes the tree. `segments.hpp` composes
   caller data into a message **without** `Quoted`. That is a second false tree-claim resting on
   the same unchecked assumption.
2. §12.1 places **all of §5.1** in `frozen`, and `frozen` binds the *wording*. Shipping a mechanism
   that makes a frozen sentence false, while the design's stated basis for saying it does not is
   wrong, is a stop-and-ask against the spec under §1 — not something to leave for a red test.
   (I read `:679-680` in context as a record of PDA-DEC-3's migration rather than a forward
   guarantee, which is why this is a tripwire to resolve in wording rather than a ruling deviation.
   That reading has to be **stated and defended in the design**, not assumed.)

**Acceptable fix:** correct the premise. Name the one message class the escape actually changes
(`RequireSegments`' `__`-prefix and `/` refusals on a NUL-bearing segment), state whether that sits
inside A7's 2026-09-03 authorisation or needs the owner's word, and amend §5.1's sentence rather
than asserting it survives untouched. The cheaper alternative that would make the original claim
literally true — routing `seg` through `Quoted` at those two sites — is **not** recommended: it
also escapes every byte ≥ 0x7f, a strictly wider message change.

---

## What the design gets right (recorded, not padding — these were the checks I was sent to make)

- **Ruling 46.** No C declaration is written; §1 states the test as ruling 46 restates it ("does
  this data drill all the way up to the language ABI?") and P4 is a live tripwire on it. The
  §6 amendment to `docs/protocol-driver-abi-spec.md` is real: `:21-23` and `:79-81` say exactly what
  the design quotes, and that document is `proposed`, so it is an edit.
- **Ruling 53.** Used for attachment ordering and nothing else; the design says so at the point the
  bytes move and names any other movement a fresh stop-and-ask.
- **Ruling 54.** Used for the NUL-key sentence in §3.2 and nothing else, quoting its own limiting
  clause. The refusal of a key-length bound and an empty-key refusal ("A5's 246-byte bound was
  *measured*, not reasoned") is the right call and correctly grounded.
- **Ruling 55.** `RegisteredNames()` is not added, not renamed, not deferred, not hidden behind
  `internal/`; the method and its conformance test are in Files-to-delete with the ruling named.
  A7 is **not** treated as withdrawn — reconstructibility is still delivered through
  `core/status.hpp`.
- **The premise correction is correct.** `factories_` **is** a `std::map`
  (`provider_registry.hpp:285`), with the comment the design quotes verbatim. Revision 1's
  `unordered_map` claim was false, and nothing else in revision 2 still rests on it: A7's verified
  substance is §5.1's missing message form (`PDA-DEC-9-bind-stopask-verification.md:257-278`), and
  the reconstruction leg of the forcing test is genuinely red at base. **A7's remaining substance
  justifies the item.**
- **The re-specified forcing test IS falsifiable**, contrary to the worry that opened this review.
  Its reconstruction leg is deterministically red: `ProviderRegistry::Create` routes a throwing
  factory through `TranslateSeamFailure` (`provider_registry.cpp:186`), so a factory whose `what()`
  carries a NUL loses everything after it across a NUL-terminated channel today, and the
  constructor escape is what turns it green. That is red-for-the-right-reason, not a seal. The
  insertion-order leg is honestly declared a seal guard **and it can in fact redden**: all three
  mainstream `unordered_map` implementations link a new-bucket node into the front of their global
  node list, so re-typing `factories_` makes two insertion orders of the same names iterate
  differently. See DEBT-3 for the one thing it still cannot see.
- **Positional-only enumeration breaks no caller pattern the spec licenses.** §3.2's only mention
  of the container is the alias itself (`spec:186`), which A6 is authorised to change; §8 and §7
  say nothing about walking it. Every in-tree map use maps onto `Set`/`Find`/`KeyAt`/`ValueAt`
  **except `clear`** — see DEBT-1.
- **P1 verified, including off-tree.** All four attachment decoders match by key, not position:
  `core/envelope.hpp:160`, `fastdds …/envelope_codec.hpp:100`,
  `fastdds benchmarks/legacy_fletcher_topic_type.hpp:127`, and the shipped cross-language one,
  `gateway-client-ts/src/envelope.ts:104` (`attachments.set(key, blob)`). P1 does not fire.
- **Budget.** 300 lines exactly, at the cap, not over. New public surface **1** type. Files-to-delete
  is present, real, and carries a replacement or a disclosed no-replacement for every entry. No
  coexistence bridge. No hand-composed post-change ledger anywhere — P5 explicitly refuses one.

---

## DEBT (7) — registered in `design-debt.md`, handed to the implementer

**DEBT-1 — `clear` is deleted with no replacement, and the receive path needs it.**
`fastdds …/envelope_codec.hpp:76` (`attachments.clear()`, first act of `ParseEnvelopeBody`) and
`benchmarks/legacy_fletcher_topic_type.hpp:106` reset a **reused** `Attachments&` out-param;
`data_reader_listener.hpp` keeps one across the sample loop, and
`fastdds-pubsub-provider/README.md:529,535` record the measured reason not to build a fresh one per
sample. Files-to-delete removes `clear` and names the replacement as
`size/KeyAt/ValueAt/Find/Set`, none of which can empty a container. Add `void Clear() noexcept`
(capacity retained; public surface stays 1 type), or state that the codec assigns `Attachments{}`
and own the per-sample allocation in the band.

**DEBT-2 — nothing tests the wire order ruling 53 was given for.**
The mapping table's positive test asserts the **container's** published form. The ruling's own
sentence is about **bytes**. It follows by construction once `begin()/end()` are gone, but that is
the shape the round has twice shipped as an unfalsifiable guard. One cheap test closes it:
`SerializeEnvelope` (and `EncodeEnvelopeBody`) of the same set built in two insertion orders is
**byte-identical**, and the emitted key sequence is ascending.

**DEBT-3 — the seal guard cannot see the variation ruling 55 actually names.**
Two insertion orders in one process cannot distinguish "a function of the registry's contents" from
"a function of hash order", because hash order *is* a function of contents within one build; the
variation that matters is across builds and standard libraries. Strengthen by additionally asserting
the rendered `available:` list is in ascending byte order — that reddens under **any** re-typing,
regardless of whether that container happens to be insertion-order-sensitive.

**DEBT-4 — the constructor's NUL escape is non-injective, unlike `Quoted`.**
`provider_registry.cpp:43-51` records that this exact collision was measured and removed for
`Quoted` ("2 colliding pairs before, 0 after") by also escaping the backslash. The design
deliberately escapes **only** NUL so it changes fewer messages — a defensible trade, but a message
containing the literal characters `\x00` now renders identically to one containing a real NUL. Say
so in the header comment beside the escape, so a later reader does not "fix" it into a
message-changing escape.

**DEBT-5 — "UTF-8" is asserted where nothing validates it.**
§2 publishes *"ascending unsigned-byte order of the key's UTF-8 bytes"* and §4 rule 2 makes the
message *"bytes plus length, UTF-8"*. Keys and messages are `std::string` — arbitrary bytes, never
validated. Publish the order as **ascending unsigned-byte order of the key bytes** and let UTF-8 be
a stated convention rather than a guarantee the tree does not enforce.

**DEBT-6 — rung-1 item 2's justification is weaker than the choice it defends.**
Once `entries_` is sorted, a `begin()/end()` over that same vector cannot drift from the positional
order — they are one sequence. So *"a range-`for` is exactly what made the wire order a build
artefact"* is not right: the **unordered container** made it a build artefact, not the range-`for`.
The real case for sealing is "one mechanism, not two" hygiene bought for ~a dozen indexed loops,
which is a fine trade — state it that way rather than on a mechanism claim that does not hold.

**DEBT-7 — "~15 files" and the design's own Files-to-touch disagree by ~2x, and the band's ceiling
is pegged to that number.** P2 and the brief say "~15 files"; Files-to-touch names ~30 paths across
nine bullets (several as wildcards), and 54 source files reference `Attachments`. Most of those
need no edit (a `const Attachments&` in a signature), which is why the band may still be right —
but the ceiling is declared as *"set by the attachments migration across ~15 files"*, so reconcile
the two counts before implementation rather than after. On the declared numbers themselves: +830 /
−250 against a +650/+1300 band is defensible for the shape described, and the named band-breakers
(P2 failing on a public surface; the two encoder loops needing restructuring rather than
re-indexing) are the real ones — B1 adds a third (the decode-side check and its test), which is
small but should be named.

---

## NITs

None fixed. The three BLOCKERs open a revision anyway, and every editorial item I found is inside
text the revision will rewrite.

---

## The one line for the PM

Nothing here is a disagreement about direction. All three BLOCKERs are the same defect: **the
design traced each new rule to the door it was authorised at and stopped there** — `Set` was traced
to the attach side but not the decode side (B1), the retired alias was traced to `types.hpp` but not
to the two other documents that publish it (B2), and the NUL escape was traced to `Quoted` but not
to `segments.hpp` (B3). One revision that walks each rule to *every* site the tree reaches it from
discharges all three.
