# PDA-DEC-AG2 — The two crossing types with no form

*Design (step 1). Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
§3.2 and §5.1 — cited, not restated. Rulings ledger:
[plans/PDA-DEC-rulings.md](PDA-DEC-rulings.md), 55 entries, all binding —
revision 2 is written to rulings **53, 54 and 55** (2026-09-06).
Locked decisions: [plans/PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md).*

## Summary

`Attachments` (A6) and a seam error's **message** (A7) cross the seam with no published
form. Attachment entries are enumerated, and encoded onto the wire, in `unordered_map`
hash order; a refusal's message has no published form at all, so a boundary may convey it
truncated or not at all — and §4's answer lives only in the message. This item gives each
a form a boundary can reproduce, and makes the unreproducible form unrepresentable.

## Design

### 1. The class, stated once

A published form must be **a function of the value alone**. Anything a separately
built binary — or a language binding — cannot reproduce from the value is not a form.
That is the single sentence A6 and A7 share, and it is why they are one item.

Ruling 46 removed the driver-side C-expressibility proof and left the test *"does this
data drill all the way up to the language ABI?"* — which both pass: attachments reach a
C#/Rust application, and a refusal's message is all a mistyped protocol name gives an
operator. So both still owe a language-agnostic form. **No C declaration is written
here** (locked decision 14); the form is rules over the C++ value plus a stand-in
boundary in the harness.

### 2. `Attachments` becomes a sealed, ordered container

`core/include/fletcher/core/types.hpp`. The alias
`using Attachments = std::unordered_map<std::string, Blob>` is **retired** and the name
rebinds to a class:

```
class Attachments {
 public:
  Attachments() noexcept = default;              // empty; `Publish(..., {})` keeps working
  void Set(std::string key, Blob value);         // inserts or replaces; one entry per key
  size_t size() const noexcept;
  bool   empty() const noexcept;
  std::string_view KeyAt(size_t i) const;        // i in [0,size()); else kInvalidArgument
  const Blob&      ValueAt(size_t i) const;      // same
  const Blob*      Find(std::string_view key) const noexcept;   // null when absent
 private:
  std::vector<Entry> entries_;   // Entry is private; kept sorted and unique by key
};
```

Load-bearing choices:

- **One enumeration, and it is positional.** Deliberately **no `begin()/end()`** and no
  map API: a range-`for` is exactly what made the wire order a build artefact, and
  removing it forces every producer — the envelope encoder included — through the
  published form. Cost: ~a dozen loops become indexed. The *"one mechanism, not two"*
  shape (2026-09-01).
- **The order is part of the form:** **ascending unsigned-byte order of the key's UTF-8
  bytes**. A boundary never sorts — it reads the sequence Fletcher publishes — so no
  binding reproduces a collation (C#'s ordinal comparison is UTF-16 code-unit order and
  would differ; it never has to be used).
- **`Find` returns a pointer, not a reference or an exception.** Absent is a value, not a
  failure; no `at()` to throw, no `operator[]` to insert by accident. `Blob` is unchanged
  — its five §3.2 clauses are **inherited, not re-proved** here.

**Wire consequence, deliberate:** `SerializeEnvelope` (`envelope.hpp:63-70`) and the Fast
DDS codec (`envelope_codec.hpp:40`) enumerate attachments to produce the wire bytes —
`std::hash`'s order today, key order after. The envelope *format* is untouched and every
decoder matches by key, never position, but the bytes emitted for a ≥2-attachment message
change on builds whose hash order was not ascending.

**Authorisation, at the point the bytes move:** locked decisions 11 and 13 reserve any
wire-byte change to the owner, and **ruling 53 (2026-09-06) is that authorisation** —
given *"for attachment ordering only"* and expressly not generalising. Any other byte
movement, here or later, is a fresh stop-and-ask.

### 3. A key that cannot survive the trip is refused at the door

`Set` refuses a key containing a **NUL** with `kInvalidArgument`. A key is
bytes-plus-length at the seam, but a boundary marshalling a NUL-terminated string
truncates it silently, so `"a\0b"` and `"a"` become one attachment and one overwrites the
other — the silent-wrong-answer class this round refused three times on 2026-09-04, and
the reason the provider selector already refuses a NUL (`provider_registry.cpp:107-113`).

This writes a **new normative refusal into frozen §3.2**, where §12.1's *who may act* is
"nobody alone". **Ruling 54 (2026-09-06) is that word**, and its licence is specific by
its own terms — *"for this refusal, which no other change may treat as a general licence
to amend frozen text."* So §3.2 gains exactly the NUL-key sentence; the enumeration-order
text amended in §2 rides on ruling 53, not on this one.

**Not proposed:** a key-length bound, and an empty-key refusal. A5's 246-byte bound was
*measured*, not reasoned; nothing here has been measured, and an empty key survives a
pointer-plus-length boundary intact. Inventing either would be over-forbidding without
evidence.

### 4. A message is part of the error's value

`core/include/fletcher/core/status.hpp`. `PubSubError` already carries the message via
`what()`; what is missing is the published rule. §5.1 gains it, and the header states
it beside the type:

1. A refusal's message is retrievable **from the error instance that produced it** —
   never a global or thread-local `errno`-style slot, which would not survive multiple
   provider instances in one process. The sibling spec already states exactly this
   (`docs/protocol-driver-abi-spec.md:229-232`); the seam adopts it, so the two
   boundaries derive one rule, not two.
2. It is **bytes plus length**, UTF-8, and **contains no NUL**.
3. A boundary must convey **the number and the message**. For a §4 configuration
   refusal the message *is* the answer: `kInvalidArgument` is shared with many other
   refusals, so a boundary that forwards only the number hands an operator a bare
   "invalid argument" for a typo'd protocol name.

Rule 2 is true by construction, not asserted: `PubSubError`'s constructor escapes any NUL
as `\x00` — the *coerce-don't-throw* shape §5.1 already mandates for a refused status. The
untrusted entry is `TranslateSeamFailure`'s `catch (const std::exception& e) → e.what()`,
a third-party SDK's message. **Only** NUL is escaped, so §5.1's *"Messages are unchanged"*
stays true of every message in the tree — none contains one.

### 5. The §4 answer is a function of the registry, and stays one — with no new surface

**Ruling 55 declined the typed query.** `ProviderRegistry::RegisteredNames()` is **not
added** — not deferred, not renamed, not moved behind `internal/`: the owner declined the
capability, not its placement. An application displays only the sentence Fletcher composed.
The A7 defect stands, so both properties the ruling names are delivered with no new API.

**Determinism.** Revision 1 said the list at `provider_registry.cpp:167-180` is composed
from an `unordered_map`. **That is false in the tree as found:** `factories_` is a
`std::map` (`provider_registry.hpp:285`), whose comment gives the reason — *"Ordered, so
the 'available providers' in a refusal is stable … rather than hash-order."*
`std::less<std::string>` runs through `char_traits<char>::compare`: ascending unsigned-byte
order, **the same total order §2 publishes for attachment keys**, so the item ships one
order rule, not two. Determinism is an existing **rung-1 seal** that AG2 *pins* rather than
builds: same names, different insertion orders, byte-identical message. Green at base, red
if anyone re-types the container — a seal guard, and the mapping table calls it one.

**Reconstructibility** — the leg genuinely broken today — is §4's rules 1–3 plus the NUL
escape, all in `core/status.hpp`. The registry needs no product change; its two sites gain
one comment each naming ruling 55 and the seal, so a later reader does not "optimise" the
container and silently reopen A7. `Register`/`Create` are untouched, so a path selector
remains admissible (locked decision 3).

### 6. The one extra line

`docs/protocol-driver-abi-spec.md` `:21-23` and §0.2 `:79-81` still publish the
multi-language driver licence ruling 46 superseded; both are amended to say the driver
side is C++. That spec is `proposed`, not `frozen` — an edit, not an authorised amendment.

### 7. What is published, and what it claims

`integration-tests/pubsub-conformance/README.md` gains one short section, worded to the
round's standing narrow-claim preference: green proves *this tree's* attachments and
refusals are reproducible from their published form by a stand-in boundary, and nothing
about any real C#/Rust binding — none exists to measure. Same limit `encode_copies` carries.

## Corner cases forbidden

**Rung 1 — unrepresentable:**

1. **Build-dependent enumeration of attachments.** No unordered enumeration exists to
   reach; order is a total order on key bytes fixed by the type. The §4 name list is
   sealed the same way and **already was** — `factories_` is ordered, so there is no hash
   order to compose from (§5); AG2 pins that seal rather than adding one.
2. **Two enumeration mechanisms.** No `begin()/end()`, no map API — positional only, so
   there is one sequence and it cannot drift from itself.
3. **Duplicate keys.** `Set` replaces; there is one entry per key by construction.
4. **A message that truncates at a language boundary.** No `PubSubError` with a NUL in
   its message exists, because the constructor escapes it.
5. **A "view-only" attachment value** — inherited from `Blob`'s sealed constructor
   (§3.2), not re-proved here.
6. **A refusal whose cause lives in a global slot.** There is no slot to add to; the
   message is a member of the thrown value.

**Rung 2 — refused typed at the door:**

7. **An attachment key that cannot survive a language boundary** (embedded NUL) —
   one check in `Set`, `kInvalidArgument`, no partial mode. *Authorised by ruling 54.*
8. **An out-of-range index** — `KeyAt`/`ValueAt` refuse with `kInvalidArgument`. One
   check, no clamping, no sentinel entry, no "valid sometimes" index.

**Handled residue — each with why it is not forbidden:**

9. **A message longer than a boundary's buffer.** Not bounded. *Why not forbidden:* no
   bound has been measured in the tree, and A5's precedent is that a refusal ceiling is
   measured, not reasoned; the rule makes the message bytes-plus-length, so a boundary
   that truncates its own copy is making that choice itself.
10. **Non-printable, non-NUL bytes in a message.** Escaped only where caller data is
    composed in (`Quoted`, `provider_registry.cpp:52`). *Why not forbidden:* the message
    is a human diagnostic in UTF-8; forbidding arbitrary bytes would mangle legitimate
    non-ASCII text, and only NUL truncates.

## Premises and stop conditions

- **P1 — no in-tree decoder is order-sensitive.** Every attachment decoder in the tree
  matches by key (`envelope.hpp:160`, `envelope_codec.hpp:100`). *If false* — if any
  decoder, in-tree or on the wire, depends on attachment position — **STOP-AND-ASK**:
  ordering the container would change interop, and the remedy is the owner's call, not
  "leave the order unspecified".
- **P2 — `Attachments`' map-ness is not load-bearing in any public API.** Migration is
  mechanical (`operator[]`/`at`/`count`/`find`/`emplace`/`insert_or_assign`/`clear`).
  *If false* — if a public surface outside `core`/`pubsub` (e.g. `pubsub-arrow`'s
  `SubscribeResult`) exposes map semantics a binding is expected to use — **STOP-AND-ASK**
  before adding a compatibility accessor; a shim is a coexistence window in an item whose
  whole point is that there is one form.
- **P3 — `what()` is the only message channel.** *If false* — if any provider stashes
  diagnostics in a side channel — **STOP-AND-ASK**; do not publish a second channel.
- **P4 — ruling 46 tripwire.** If expressing either form starts to need a C declaration,
  an `extern "C"` symbol, or a struct shared with either ABI round, **STOP** — that is ABI
  work this round may not do (locked decision 14).
- **P5 — machine checks, not hand-composed facts.** The migration's completeness is
  proved by the compiler (the alias is gone, so every site that used map API fails to
  build) and by the existing envelope round-trip tests. No caller ledger is written.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `SeamVocabulary.AnAttachmentSetIsReconstructibleFromItsPublishedFormAlone` | §2 — a stand-in boundary using only `size/KeyAt/ValueAt` serialises the set to flat bytes and rebuilds it through `Set`; the rebuilt set publishes a byte-identical form, and the same entries built in several construction orders publish the identical form. | **Does not compile:** the published form does not exist. Written against the map's own iteration it would green *by luck* on a build whose hash order happened to agree — which is the finding, not a workaround. |
| `SeamVocabulary.AnAlteredAttachmentSetPublishesADifferentForm` *(live negative control)* | §2 — one key byte changed, one value byte changed, two values swapped between keys, one entry dropped: each must publish a **different** form. | Does not compile (same reason). Proves the comparison in the positive test is not inert. |
| `Registry.ARefusalIsReconstructibleFromItsNumberAndMessageAlone` *(re-specified, rev 2)* | §4 — a stand-in boundary copies `{int32 status, bytes+length message}` across a byte-only, NUL-terminated channel and rebuilds a refusal indistinguishable from the original. **Ruling 55 removed its second leg**, which compared the rendered list against `RegisteredNames()`; the property is now asserted **through the message alone** — two registries holding the same names but built in different insertion orders must produce byte-identical refusal messages. Same property, no query. | **Deterministically red on the reconstruction leg:** a factory throwing a `std::runtime_error` whose `what()` contains a NUL loses everything after it across the channel today. The insertion-order leg is **green at base** — it guards the existing `std::map` seal (§5) and goes red if that container is re-typed. Stated as a seal guard, not claimed as forcing. |
| `Registry.ANumberAloneCannotTellTwoRefusalsApart` *(live negative control — unchanged)* | §4 rule 3 — a number-only boundary renders a typo'd provider name and an empty topic-segment list identically; with the message they are distinct. Never depended on the query: it reads two messages and compares them. | Asserts the message is load-bearing rather than decorative; it breaks if a later change folds the cause into the number. **Not weakened by ruling 55.** |
| `SeamVocabulary.AnAttachmentKeyThatWouldTruncateIsRefused` | §3 — `Set` refuses a NUL-bearing key with `kInvalidArgument`. Lands: authorised by **ruling 54**. | Today the key is accepted and silently collides at a boundary. |

The item's one sentence, false if any of the above breaks: **two Fletcher builds handed
the same attachments set publish the same form, and a boundary holding only a refusal's
number and message can reproduce that refusal exactly.**

## Risks / Unknowns

- **Both of revision 1's stop-and-asks are DISCHARGED.** Wire-byte movement (locked
  decisions 11/13) by **ruling 53**, for attachment ordering only; new normative text in
  frozen §3.2 by **ruling 54**, for the NUL-key sentence only. Neither licence extends
  anywhere else in this item.
- **The typed name query — DECLINED by ruling 55**, against the recommendation, and gone
  from the design, the surface count and the tests. Residual cost, stated: an application
  offering "did you mean…" must parse Fletcher's sentence or keep its own list. Accepted
  trade, not a gap to reopen.
- **Revision-1 premise correction.** The §4 list is composed from a `std::map`, not an
  `unordered_map` (`provider_registry.hpp:281-285`). A7 is unaffected — its verified
  substance is the message's *missing published form*
  (`PDA-DEC-9-bind-stopask-verification.md:257-278`), not hash order — but one leg of one
  forcing test is consequently a seal guard, and the mapping table says so.
- **Oracle check performed:** §5.1's *"Messages are unchanged"* remains true, because no
  message in the tree contains a NUL; §3.2's `unordered_map` sentence is the sentence A6
  is authorised to amend, not a contradiction. **No other spec contradiction found.**
- **Coexistence windows: none.** The alias is retired in the same change that introduces
  the class, with no conversion and no map-compatible accessors — the same reasoning
  §3.2 already records for the retired `shared_ptr` alias.
**Declared net lines (revision 2): +830 / −250, band +650 / +1300 adds.** Ruling 55 removes
~70 adds — the method, its comment, its implementation, its conformance test. Of the rest,
~370 are tests, ~150 the two spec amendments and the harness README, ~180 the container,
the remainder call-site migration. **The ceiling did not move with the estimate:** the top
of the band is set by the attachments migration across ~15 files (premise P2), which ruling
55 did not touch. **What would make the band wrong, named:** P2 failing — a public surface
(most likely `pubsub-arrow`'s `SubscribeResult`) needing reshaping rather than a rename — or
the envelope and Fast DDS codec loops needing restructuring rather than re-indexing. Either
takes this past +1300, where the honest move is a stage split, not a quiet overrun: AG1
declared +1050 and landed +2028.

**New public surface: 1** — `fletcher::Attachments` (the class; it *retires* the
`unordered_map` alias and every map member that alias published). No registry method is
added; no `PubSubProvider` method is added, removed or reordered; no `PubSubStatus` value
is appended.

## Files-to-touch

- `core/include/fletcher/core/types.hpp` (the `Attachments` class), `status.hpp` (the
  message rule; NUL escape), `envelope.hpp` (encode/decode through the positional form).
- `core/README.md` (one line beside the taxonomy table), `core/tests/test_envelope.cpp`,
  `core/tests/test_status_taxonomy.cpp`.
- `pubsub/include/fletcher/pubsub/provider_registry.hpp`, `pubsub/src/provider_registry.cpp`
  — **comment only** (ruling 55: no method added, no composition rewritten).
- `pubsub/include/fletcher/pubsub/{provider,publisher,subscriber,in_process_provider,delivery_channel}.hpp`,
  `pubsub/src/{in_process_provider,subscriber,delivery_channel}.cpp` — migration.
- `fastdds-pubsub-provider/src/internal/{envelope_codec,data_reader_listener,ordered_delivery}.hpp`,
  `src/fast_dds_pubsub_provider.cpp`, `benchmarks/*`, `tests/*` — migration.
- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`, `tests/*`, `test_package/src/example.cpp`.
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`, `pubsub-arrow/tests/*`,
  `gateway/src/ws_session.cpp`, `integration-tests/protoc-arrow-bridge/tests/test_pubsub.cpp`.
- `integration-tests/pubsub-conformance/src/{seam_vocabulary,registry,copy_accounting,copy_clauses,caller_tier}.cpp`,
  `include/fletcher/conformance/copy_accounting.hpp`, `README.md`.
- `docs/pubsub-interface-spec.md` — §3.2 (A6), §5.1 (A7).
- `docs/protocol-driver-abi-spec.md` — `:21-23`, §0.2 `:79-81` (ruling 46).
- `plans/PDA-decouple-interface.md` — the AG2 row.

## Files-to-delete

- `Attachments = std::unordered_map<std::string, Blob>` (`core/include/fletcher/core/types.hpp:119`)
  and, with it, every member that alias published to callers — `operator[]`, `at`,
  `count`, `find`/`end`, `emplace`, `insert_or_assign`, `clear`, `begin`/`end`.
  *Replacement:* the sealed container's published form (`size`/`KeyAt`/`ValueAt`/`Find`/`Set`).
- `docs/protocol-driver-abi-spec.md:21-23`'s *"implementable by anyone in any language"*
  and §0.2 `:79-81`'s Rust/C#-driver paragraph. *No replacement — the licence is gone,
  disclosed:* ruling 46 superseded it deliberately, with the owner looking at that text.
- No test file is deleted; test **bodies** migrate off the map API in place, each still
  asserting a fact that survives — the compiler, not a ledger, proves none was dropped.
- **Revision 2 also deletes, before it was written:** `RegisteredNames()` and its
  conformance test `Registry.TheAvailableNamesAreAnsweredWithoutReadingProse`.
  *No replacement — capability declined, disclosed:* ruling 55.
