# PDA-DEC-AG2 — architecture review, design cycle 2 of 2 (final; no cycle 3)

**Verdict: NEEDS-REWORK — 1 BLOCKER, 3 new DEBT.**
**Oracle-wins tripwire: NONE remains.** B3's tripwire is discharged; no design
sentence now contradicts frozen `docs/pubsub-interface-spec.md` §3.2 or §5.1.
**The single BLOCKER is not an owner question** — it needs no ruling, no
locked-decision relief and no redesign. See "The one line for the PM".

## Read receipt — the rulings ledger

`grep -c '^## 2026' plans/PDA-DEC-rulings.md` → **57**. Read in full.
Last two entries:

- **56** — *2026-09-06, "Frozen §5.1 is amended to name the one message class that changes" (selection).*
- **57** — *2026-09-06, "A zero-byte label is refused on arrival as well as on attachment" (selection).*

Rulings 46, 53, 54, 55, 56, 57 checked line by line against revision 3. **All six
are honoured as written.** No ruling deviation, no locked-decision deviation, no
STOP-AND-ASK on authorisation.

---

## The three cycle-1 BLOCKERs — verified against the tree, not against the report

### B3 — DISCHARGED, and the named class is exactly right

Verified in `pubsub/include/fletcher/pubsub/internal/segments.hpp`:

| Site | Concatenates | Fires before the NUL check at `:115`? | NUL-bearing message reachable? |
|---|---|---|---|
| `:91-95` (joined-length) | `std::to_string(joined)` only | yes | **no** — no raw segment in the text |
| `:99-100` (empty segment) | nothing | yes | no |
| `:103-106` (`__` prefix) | raw `seg` | **yes** | **yes** |
| `:110-113` (`/` separator) | raw `seg` | **yes** | **yes** |
| `:116-118` (NUL) | nothing | — | no |

So the class that changes is exactly the two `seg`-concatenating refusals, and both
are instances of the design's phrase *"a topic refusal built from a segment that
itself contains a zero byte"*. **Not narrower, not wider.** The joined-length
refusal, which also fires before `:115`, is correctly excluded because it does not
carry the segment. Residue #11 is corrected: `Quoted` is at
`provider_registry.cpp:52` only, `segments.hpp` does not use it, and the design now
says so and refuses `Quoted` as the route (it also escapes ≥ 0x7f — wider than
ruling 56 authorises). Ruling 56 is cited as non-generalising, in its own terms.

**And the claim is true for a stronger reason than the design gives** — see the
BLOCKER, which turns on the same fact.

### B2 — DISCHARGED, and the sweep is complete

I searched the whole tree for the retired form. `unordered_map<string, Blob>` /
`unordered_map<std::string, Blob>` appears in exactly two live published places —
`docs/wire-format-specification.md:165` and `docs/pubsub-interface-spec.md:186` —
and both are in Files-to-touch. Everything else is `docs/archive/HARD/**` or a
`plans/` document. `docs/wire-format-specification.md` is correctly treated as an
**edit**, not an amendment (it carries no `frozen` marker), and it is correctly
named as where ruling 53's *"a fixed order stated in the spec"* lives, beside the
Envelope Wire Format layout at `:142-158`. The two `fastdds-pubsub-provider/README.md`
rationale rows (`:529`, `:535`) and `pubsub/tests/test_publisher_subscriber.cpp`
are all in. Three weaker stale statements remain — DEBT below, not a re-open.

**AG2-DEBT-7 is also discharged, and I checked the number rather than taking it.**
`Attachments` is named in exactly **54** `.hpp/.cpp` files. The files holding a real
**map expression** (`[]`, `at`, `count`, `find`/`end`, `emplace`, `insert_or_assign`,
`clear`) are exactly twelve:

`core/include/fletcher/core/envelope.hpp`, `core/tests/test_envelope.cpp`,
`fastdds…/src/internal/envelope_codec.hpp`, `fastdds…/benchmarks/legacy_fletcher_topic_type.hpp`,
`fastdds…/tests/test_fast_dds_pubsub_provider.cpp`, `fastdds…/tests/test_fletcher_sample_pub_sub_type.cpp`,
`xrce…/tests/test_xrce_provider.cpp`, `xrce…/test_package/src/example.cpp`,
`pubsub/tests/test_publisher_subscriber.cpp`, `pubsub-arrow/tests/test_pubsub_arrow.cpp`,
`integration-tests/pubsub-conformance/src/copy_accounting.cpp`,
`integration-tests/protoc-arrow-bridge/tests/test_pubsub.cpp`.

**Every one of them is covered by Files-to-touch**, directly or by an explicit
`tests/*` / `benchmarks/*` wildcard. The "~15" figure is honest and the ceiling
pegged to it is defensible. Buildability: no hidden cross-cutting change —
`protoc/src/generator.cpp` emits `fletcher::Attachments` only as a by-value
parameter and a `const&` (`:974, :1021, :1027, :1044, :1051`), never a map
expression, so the change does **not** reach generated code beyond the type name.

**P2 will not fire.** The most likely trigger the design names — `pubsub-arrow`'s
`SubscribeResult` — publishes `std::vector<Attachments>` and indexes the *vector*
(`subscriber_arrow.hpp:89`, `test_pubsub_arrow.cpp:366`), not the container. No
public surface depends on map-ness.

### B1 — DISCHARGED for every product decode path; one benchmark path is missed

I enumerated the decode paths myself rather than accepting the count:

| Path | After the change | Reaches a throwing `Set`? |
|---|---|---|
| Fast DDS `deserialize()` → `fletcher_sample_pub_sub_type.hpp:166,177` → `ParseEnvelopeBody` | `memchr` → `return false`, sitting with the eight existing bounds `return false`s (`envelope_codec.hpp:70,73,83,85,89,92,96,97`) | **no** |
| Fast DDS `on_data_available` → `data_reader_listener.hpp:207` → `ParseEnvelopeBody` | same; existing `EPROSIMA_LOG_WARNING` + `continue` | **no** |
| `DeserializeEnvelope` (`envelope.hpp:160`) — gateway `ws_session.cpp:279`, XRCE `OnTopic` `:353`, tests, test_packages | `std::invalid_argument`, joining the **five** existing ones at `:121,127,137,147,153`; matches the `:116-118` contract verbatim | **no** |
| `fastdds…/benchmarks/legacy_fletcher_topic_type.hpp:106,127` | writes wire-decoded keys into a real `fletcher::Attachments` (`transport_data.hpp:59`) from inside a `TopicDataType::deserialize()` override; migrates to `Set`; **no check proposed** | **yes** |

The XRCE receive path is safely covered though the design does not name it: it
routes through `DeserializeEnvelope` inside `try { … } catch (...) { return; }`
(`xrce_dds_pubsub_provider.cpp:352-362`), so the new `std::invalid_argument` is
already absorbed and the sample dropped — no exception crosses the XRCE client's C
frames. The gateway's behaviour is unchanged in class: `DeserializeEnvelope`
already throws `std::invalid_argument` on a malformed frame today.

The benchmark decoder is DEBT, not a BLOCKER: it is not shipped, it decodes only
bytes the same benchmark encoded, and the consequence is loud rather than silent.
But the design's own P1 lists that file as a decoder while §3 counts three — an
internal inconsistency worth one line.

**`Clear()` — necessary, and no hole.** `envelope_codec.hpp:76` calls
`attachments.clear()` as the *first act* of `ParseEnvelopeBody` on a reused
out-param, and `fastdds-pubsub-provider/README.md:529,535,536` record the measured
reason for that reuse. Nothing else in the sealed API can empty the container.
`Clear()` cannot produce an unsorted or duplicate-key state — the two invariants
the form rests on — so it opens no hole, and counting it inside the one new type
rather than as a second surface item is right.

---

## BLOCKER

### C2-B1 — A7's forcing test is **green at base**: the named red-at-base mechanism cannot produce a NUL-bearing message, because `what()` is a `const char*`

Design `:110-111`:

> Rule 2 is true by construction … the untrusted entry is `TranslateSeamFailure`'s
> `catch (const std::exception& e) → e.what()`.

and the forcing-test mapping, row 3:

> **Deterministically red on the reconstruction leg:** a factory throwing a
> `std::runtime_error` whose `what()` contains a NUL loses everything after it
> across the channel today.

**That entry is the one route through which a NUL provably cannot pass.**
`std::exception::what()` returns `const char*`. `TranslateSeamFailure`
(`core/include/fletcher/core/status.hpp:160-161`) does
`throw PubSubError(PubSubStatus::kInternal, e.what())`, and `std::string` from a
`const char*` **stops at the first NUL**. A factory throwing
`std::runtime_error(std::string("before\0after", 12))` therefore yields a
`PubSubError` whose message is already `"before"` — *before* the constructor escape
exists, and *after* it exists, identically. The stand-in boundary copies `"before"`
and rebuilds `"before"`: **indistinguishable from the original, both at base and
after the fix.** The leg is green at base and the fix is a no-op for it.

The loss the row attributes to the channel has in fact already happened one frame
earlier, at `e.what()`, where nothing in this item can reach it.

Consequence: as specified, **A7 has no red-at-base leg at all.** Row 3's other leg
is declared a seal guard by the design's own words; row 4 is a live negative
control. So half the item — the message rule, which is A7's whole substance — ships
behind a guard that cannot fail. That is the unfalsifiable-guard shape this round
has already shipped twice, and my own cycle-1 review wrongly certified this leg as
"deterministically red" (see the PM line).

**Acceptable fix — two lines, no owner input, nothing redesigned.** Forbidding is
not available here: the fix is to name the route that actually carries a NUL.

1. §4: replace the untrusted-entry sentence. The routes that can hand the
   constructor a NUL-bearing `std::string` are (i) `segments.hpp:103-113`'s
   `"…" + seg`, and (ii) a provider factory constructing
   `PubSubError(status, std::string)` directly — which `TranslateSeamFailure`
   **rethrows unchanged** (`status.hpp:145-146`), so the escape must live in the
   constructor exactly as designed. `e.what()` is not a NUL route and should be
   named as such.
2. Mapping row 3: the stand-in factory throws
   `PubSubError(kTransportFailure, std::string("a\0b", 3))`, **not** a
   `std::runtime_error`. That is red at base (the message truncates at the
   NUL-terminated channel today) and green after (the constructor escapes it).

Note this same fact **strengthens** B3's discharge rather than weakening it: since
no exception's `what()` can carry an embedded NUL, no third-party text — an SDK
message, `sample_writer.hpp:87`'s `transport.serialize_error`, or
`schema_import.cpp:24`'s Arrow status — can be altered by the escape. §5.1's
amendment naming one class is therefore **exhaustively true**, and the design
should say why in one clause so the amendment's exhaustiveness is checkable
without redoing this analysis.

---

## DEBT (3 new) — appended to `design-debt.md`, handed to the implementer

**C2-DEBT-1 — the unreachability claim omits a fourth decoder.**
`fastdds-pubsub-provider/benchmarks/legacy_fletcher_topic_type.hpp:106,127` decodes
wire bytes into a real `fletcher::Attachments` (`transport_data.hpp:59`) from inside
a `TopicDataType::deserialize()` override, and after migration reaches `Set`. The
design's P1 lists this file as a decoder; §3 counts three. Either give it the same
`memchr` guard, or narrow the sentence to *"every decode path that can receive
foreign bytes"* and record that the benchmark decodes only bytes it produced.

**C2-DEBT-2 — three further published statements go stale and are not in
Files-to-touch.** `docs/data-flow-diagrams.md:155` ("EncodedRow + Attachments
**map**"); `fastdds-pubsub-provider/benchmarks/README.md:28` ("an empty
`Attachments` against what `PublishData` costs"); and
`fastdds-pubsub-provider/README.md:536`, whose "listener reusing one `Attachments`"
claim stays true only because `Clear()` exists. One-word edits each. This is not
the `unordered_map` class B2 named, which is complete.

**C2-DEBT-3 — a sorted-vector `Set` makes decode quadratic in a wire-supplied
count.** `ParseEnvelopeBody` bounds `att_count` only by `(total - pos) / 8`
(`envelope_codec.hpp:83`), so a 1 MiB sample may claim ~131k attachments. Today
that is O(k) hash inserts; after sealing, `Set` is the only insertion point, and a
producer emitting **descending** keys forces O(k²) element moves on the receive
path whose cost `fastdds-pubsub-provider/README.md:536` publishes as 3.2 ns/sample.
Ascending input — which conforming Fletcher now always emits — is O(1) amortised
per key, so this bites only a non-conforming or hostile producer, and it is loud
rather than silent. Either give the decode paths an ordered-append fast path, or
state the complexity beside `Set` and re-check the published number. Consistent
with the round's refusal to invent unmeasured bounds (§3 "Not proposed"): measure
before capping.

### Cycle-1 DEBT status (for the register)

Discharged in revision 3: **AG2-DEBT-1** (`Clear()` added, surface still 1),
**AG2-DEBT-5** (order published as ascending unsigned-byte order of the key bytes;
UTF-8 demoted to a stated convention), **AG2-DEBT-6** (rung-1 item 2 restated as
"one mechanism, not two"), **AG2-DEBT-7** (counts reconciled, and verified above).
Still open and owed by the implementer: **AG2-DEBT-2** (nothing tests the *wire*
order ruling 53 was given for), **AG2-DEBT-3** (the seal guard cannot see the
variation ruling 55 names), **AG2-DEBT-4** (the escape is non-injective, unlike
`Quoted`).

---

## Budget, numbers, and the other checks I was sent to make

- **Design 299 / 300; brief 58 / 60.** Both within cap. New public surface **1**
  (`fletcher::Attachments`); no registry method, no `PubSubProvider` method, no
  `PubSubStatus` append. Files-to-delete present, real, and every entry carries a
  replacement or a **disclosed** no-replacement. No coexistence bridge. No
  hand-composed post-change ledger anywhere; P5 refuses one explicitly, and the
  only counts in the document are **pre**-change file counts used to size a band,
  which is legitimate — and correct, as verified above.
- **Band +700 / +1600 against a declared +920 / −270: defensible.** 1.7x headroom
  against AG1's realised 1.9x overrun, with the three band-breakers named, the
  stage-split shape named for the overrun case, and P2 — the biggest of them —
  verified above as unlikely to fire. The +90 delta over revision 2 is accounted
  for line-item by line-item.
- **The oracle check "verified by search, not assumption" — spot-checked, and it
  holds, though the prose overstates the mechanism.** I read every `PubSubError(`
  carrying a concatenation in `core/`, `pubsub/` and the three providers. The
  design's blanket *"all of them are downstream of `RequireSegments`"* is not
  literally accurate — XRCE's `agent_host` and `QuoteEntry(entry)` messages are
  NUL-free because `xrce_document.cpp:71-76` refuses a NUL anywhere in the document
  up front; Fast DDS's `profile_document.hpp` values are NUL-free because XML
  parsing terminates at one; `sample_writer.hpp:87` and `schema_import.cpp:24` are
  NUL-free because their text arrives through a `const char*` or an Arrow status.
  **But the conclusion is correct**: no in-tree message other than
  `segments.hpp:103-113` can carry a NUL. The claim is right; the reason given for
  it is not the reason it is right. That is what C2-B1 fixes.
- **Ruling 53** used for attachment ordering only, with the sentence landing in the
  wire-format document as the ruling's words require, and any other byte movement
  named a fresh stop-and-ask. **Ruling 54** used for the §3.2 NUL-key sentence
  only. **Ruling 55** honoured — `RegisteredNames()` deleted before being written,
  the capability declined not relocated, and the registry gains comment only.
  **Ruling 56** cited as a second, separate, non-generalising authorisation.
  **Ruling 57** implemented at the door the owner specified. **Ruling 46** honoured
  — no C declaration, P4 is a live tripwire, and the `protocol-driver-abi-spec.md`
  edits are real. **Locked decisions 3, 7, 11, 13, 14** all hold: the path selector
  stays admissible; `Blob` values move as `shared_ptr` moves, never bytes, so
  zero-copy on the attachment path is untouched; the wire-byte movement is
  authorised and scoped; nothing ABI is started.

## NITs

None returned, none fixed. The one formatting slip I found (the missing blank line
before *"Declared net lines"* at `:248`, which folds that paragraph into the
preceding bullet) would cost the design its last line of budget, so I left it.

---

## The one line for the PM

**The recurring class, named as the runbook asks:** across both cycles the same
defect has recurred with the specific items differing — **a claim about what a
value can hold, asserted from prose rather than read off the type.** Cycle 1: *"no
message in the tree contains a NUL"* (false — `segments.hpp` concatenates a raw
segment). Cycle 2: *"the untrusted entry is `e.what()`"* (false in the opposite
direction — a `const char*` cannot carry one). It is the same class with opposite
sign, and **I propagated it too**: my cycle-1 review certified the very leg that
cannot redden. The premise that needs correcting is procedural, not architectural —
NUL-reachability claims in this item must be read off the type of the channel, not
argued.

**This is not a principle-level disagreement and must not go to the owner.** C2-B1
needs no ruling, no locked-decision relief, no spec change and no redesign: it is
one sentence in §4 and one clause in the forcing-test mapping, both of which fit
inside the 299-line budget by replacing text already there. If the process cannot
open a revision, bind it on the implementer as a mandatory correction with the red
demonstrated before the fix lands — but it cannot ship as written, because as
written A7 has nothing that can fail.
