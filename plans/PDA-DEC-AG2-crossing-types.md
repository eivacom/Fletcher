# PDA-DEC-AG2 — The two crossing types with no form

*Design (step 1). Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
§3.2 and §5.1 — cited, not restated. Rulings ledger:
[plans/PDA-DEC-rulings.md](PDA-DEC-rulings.md), 57 entries, all binding —
revision 3 is written to rulings **53, 54, 55, 56 and 57** (all 2026-09-06).
Locked decisions: [plans/PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md).*

## Summary

`Attachments` (A6) and a seam error's **message** (A7) cross the seam with no published form:
entries are enumerated — and encoded onto the wire — in hash order, and a refusal's message may
arrive truncated or not at all, though §4's answer lives only in it. This item gives each a form
a boundary can reproduce, and makes the unreproducible form unrepresentable.

## Design

### 1. The class, stated once

A published form must be **a function of the value alone**: anything a separately built binary —
or a language binding — cannot reproduce from the value is not a form. That is the sentence A6
and A7 share, and why they are one item. Both pass ruling 46's test (*"does this data drill all
the way up to the language ABI?"*) — attachments reach a C#/Rust application, and a refusal's
message is all a mistyped name gives an operator. **No C declaration is written here** (locked
decision 14): the form is rules over the C++ value plus a stand-in harness boundary.

### 2. `Attachments` becomes a sealed, ordered container

`core/include/fletcher/core/types.hpp`. The alias `using Attachments =
std::unordered_map<std::string, Blob>` is **retired**; the name rebinds to a class:

```
class Attachments {                        // private vector<Entry>, kept sorted and unique by key
 public:
  Attachments() noexcept = default;              // empty; `Publish(..., {})` keeps working
  void Set(std::string key, Blob value);         // inserts or replaces; one entry per key
  void Clear() noexcept;                         // empties, keeps capacity (reused decode out-param)
  size_t size() const noexcept;  bool empty() const noexcept;
  std::string_view KeyAt(size_t i) const;        // i in [0,size()); else kInvalidArgument
  const Blob&      ValueAt(size_t i) const;      // same
  const Blob*      Find(std::string_view key) const noexcept;   // null when absent
};
```

Load-bearing choices:

- **One enumeration, and it is positional.** Deliberately **no `begin()/end()`** and no map
  API. The *unordered container*, not the range-`for`, is what made the wire order a build
  artefact; sealing buys *one mechanism, not two* (2026-09-01) for ~a dozen indexed loops, so
  no producer can walk the set in an order the published form does not fix.
- **The order is part of the form:** **ascending unsigned-byte order of the key bytes** (UTF-8
  is a stated convention; nothing in the tree validates it). A boundary never sorts — it reads
  the sequence Fletcher publishes — so no binding has to reproduce a collation (C#'s ordinal
  comparison is UTF-16 code-unit order, and would differ).
- **`Find` returns a pointer, not a reference or an exception.** Absent is a value, not a
  failure; no `at()` to throw, no `operator[]` to insert by accident. `Blob` is unchanged
  — its five §3.2 clauses are **inherited, not re-proved** here.

**Wire consequence, deliberate:** `SerializeEnvelope` (`envelope.hpp:63-70`) and the Fast DDS
codec (`envelope_codec.hpp:40`) enumerate attachments to produce the wire bytes — `std::hash`'s
order today, key order after. The *format* is untouched and every decoder matches by key, never
position, but the bytes of a ≥2-attachment message change where hash order was not ascending.
Locked decisions 11 and 13 reserve that to the owner and **ruling 53 is the authorisation** —
*"attachment ordering only"*. Any other byte movement, here or later, is a fresh stop-and-ask.

### 3. A key that cannot survive the trip is refused at the door

`Set` refuses a key containing a **NUL** with `kInvalidArgument`. A key is bytes-plus-length at
the seam, but a boundary marshalling a NUL-terminated string truncates it silently, so `"a\0b"`
and `"a"` become one attachment and one overwrites the other — the silent-wrong-answer class
this round refused three times on 2026-09-04, and why the provider selector already refuses a
NUL (`provider_registry.cpp:107-113`).

This writes a **new normative refusal into frozen §3.2**, where §12.1's *who may act* is "nobody
alone". **Ruling 54 is that word**, specific by its own terms — *"for this refusal, which no
other change may treat as a general licence to amend frozen text."* §3.2 gains exactly the
NUL-key sentence; the enumeration-order text of §2 rides on ruling 53, not on this one.

**And refused again on arrival — ruling 57.** Ruling 54 said *"when attached"*; three decode
paths insert **wire-supplied** keys: `envelope_codec.hpp:100` in `ParseEnvelopeBody`, whose two
callers are Fast DDS's `deserialize()` (`fletcher_sample_pub_sub_type.hpp:166,177`) and
`on_data_available` (`data_reader_listener.hpp:207`), plus `core/envelope.hpp:160` in
`DeserializeEnvelope`. Routing those through `Set` would put an exception into a transport frame
and label a wire fault a caller fault. So the check sits **with the other five wire checks**:
`ParseEnvelopeBody` gains one `memchr(key, 0, key_len)` → `return false` beside its bounds
checks (the sample drops as malformed down the existing warn path), and `DeserializeEnvelope`
throws `std::invalid_argument`, the wire-problem type its own `:116-118` comment reserves —
never `PubSubError`, which that comment reserves for caller faults. `Set`'s throw is thereby
**unreachable from every decode path**. Forbidding, not handling.

**Not proposed:** a key-length bound, and an empty-key refusal. A5's 246-byte bound was
*measured*, not reasoned; nothing here has been measured, and an empty key survives a
pointer-plus-length boundary intact. Inventing either is over-forbidding without evidence.

### 4. A message is part of the error's value

`core/include/fletcher/core/status.hpp`. `PubSubError` already carries the message via `what()`;
the published rule is what is missing. §5.1 gains it, and the header states it beside the type:

1. A refusal's message is retrievable **from the error instance that produced it** — never a
   global or thread-local `errno`-style slot, which would not survive multiple provider
   instances in one process. The sibling spec states exactly this
   (`docs/protocol-driver-abi-spec.md:229-232`), so the two boundaries derive one rule, not two.
2. It is **bytes plus length** (UTF-8 by convention) and **contains no NUL**.
3. A boundary must convey **the number and the message**. For a §4 configuration refusal the
   message *is* the answer: `kInvalidArgument` is shared with many other refusals, so a boundary
   forwarding only the number hands an operator a bare "invalid argument" for a typo'd name.

Rule 2 is true by construction: `PubSubError`'s constructor escapes any NUL as `\x00`. **The NUL
routes into a message are exactly two** (CORRECTED, review cycle 2): `segments.hpp:103-113`'s
`"…" + seg`, and a factory constructing `PubSubError(status, std::string)`, rethrown unchanged
(`status.hpp:145`). `e.what()` is **not** one — `const char*` truncates at the NUL, before and after.

**§5.1 is amended, and the class that changes is named — ruling 56.** Revision 2 claimed no
in-tree message holds a NUL. **That is false:** `segments.hpp:103-106` (`__` refusal) and
`:110-113` (`/` refusal) concatenate the **raw segment** into the message and both fire *before*
the per-byte NUL check at `:115`, so `{"__a\0b"}` and `{"a/b\0c"}` produce NUL-bearing messages
today. The escape therefore changes exactly **one class of message: a topic refusal built from a
segment that itself contains a zero byte** — and §5.1's *"Messages are unchanged"* (`spec:679`)
is amended to name that class rather than asserted to survive. Ruling 56 is that word: a
**second** frozen-text authorisation, separate from ruling 54's §3.2 licence and equally
non-generalising — this one message class, nothing else. `seg` is deliberately **not** routed
through `Quoted`, which also escapes every byte ≥ 0x7f: a wider change than authorised.

### 5. The §4 answer is a function of the registry, and stays one — with no new surface

**Ruling 55 declined the typed query.** `RegisteredNames()` is **not added** — not deferred, not
renamed, not hidden behind `internal/`: the capability was declined, not its placement. Both
properties the ruling still names are delivered with no new API.

**Determinism.** `factories_` is a `std::map` (`provider_registry.hpp:285`) — *"Ordered, so the
'available providers' in a refusal is stable … rather than hash-order"*; `std::less<std::string>`
compares through `char_traits<char>`, ascending unsigned-byte order, **the same total order §2
publishes for attachment keys**, so the item ships one order rule, not two. Determinism is thus
an existing **rung-1 seal** AG2 *pins*: same names, different insertion orders, same bytes.

**Reconstructibility** — the leg genuinely broken today — is §4's rules 1–3 plus the NUL escape,
all in `core/status.hpp`. The registry needs no product change; its two sites gain a comment
naming ruling 55 and the seal (so no later reader "optimises" the container and reopens A7), and
`Register`/`Create` are untouched, leaving a path selector admissible (locked decision 3).

### 6. Published text this change falsifies

Three non-`frozen` documents publish the retired form; each is an *edit*, not an amendment:

- `docs/wire-format-specification.md:165` publishes `Attachments | unordered_map<string, Blob>` —
  false after the retirement, with no machine check to redden it; the row becomes the sealed
  container. Its **Envelope Wire Format** section (`:142-158`) describes the very bytes ruling 53
  moves, and that ruling's words are *"a fixed order **stated in the spec**"* — so the
  ascending-key-order sentence is stated **there**, beside the layout, as well as in seam §3.2.
- `fastdds-pubsub-provider/README.md:529,535` are rationale rows premised on `Attachments` being
  an `unordered_map` allocating a sentinel node on default construction (52 ns, ~110 ns): both
  re-measured or struck, since a stale number reads as current.
- `docs/protocol-driver-abi-spec.md:21-23` and §0.2 `:79-81` publish the multi-language driver
  licence ruling 46 superseded; both are amended to say the driver side is C++.

### 7. What is published, and what it claims

`integration-tests/pubsub-conformance/README.md` gains one short section in the round's
narrow-claim form: green proves *this tree's* attachments and refusals are reproducible from
their published form by a stand-in boundary, and nothing about a real C#/Rust binding.

## Corner cases forbidden

**Rung 1 — unrepresentable:**

1. **Build-dependent enumeration.** No unordered enumeration exists to reach: order is a total
   order on key bytes, fixed by the type. The §4 name list was **already** sealed (§5); AG2 pins it.
2. **Two enumeration mechanisms.** No `begin()/end()`, no map API — positional only, so
   there is one sequence and it cannot drift from itself.
3. **Duplicate keys.** `Set` replaces; there is one entry per key by construction.
4. **A message that truncates at a language boundary.** No `PubSubError` holding a NUL in its
   message exists, because the constructor escapes it.
5. **A "view-only" attachment value** — inherited from `Blob`'s sealed constructor (§3.2).
6. **A refusal whose cause lives in a global slot.** No slot exists to add to; the message is a
   member of the thrown value.

**Rung 2 — refused typed at the door:**

7. **An attachment key that cannot survive a language boundary** (embedded NUL) —
   one check in `Set`, `kInvalidArgument`, no partial mode. *Authorised by ruling 54.*
8. **The same key arriving from the wire** — refused by the wire checks, not by `Set`:
   `ParseEnvelopeBody` returns `false` (sample dropped as malformed), `DeserializeEnvelope` throws
   `std::invalid_argument`. No exception in a transport frame. *Authorised by ruling 57.*
9. **An out-of-range index** — `KeyAt`/`ValueAt` refuse with `kInvalidArgument`. One check, no
   clamping, no sentinel entry, no "valid sometimes" index.

**Handled residue — each with why it is not forbidden:**

10. **A message longer than a boundary's buffer.** Not bounded. *Why not forbidden:* no bound
    has been measured, and A5's precedent is that a ceiling is measured, not reasoned; the
    message is bytes-plus-length, so a boundary truncating its own copy chooses that itself.
11. **Non-printable, non-NUL bytes in a message.** Left as they are; `Quoted`
    (`provider_registry.cpp:52`) escapes them at that one site, `segments.hpp` does not. *Why not
    forbidden:* forbidding arbitrary bytes mangles legitimate non-ASCII text, and only NUL truncates.

## Premises and stop conditions

- **P1 — no in-tree decoder is order-sensitive.** Every attachment decoder matches by key
  (`envelope.hpp:160`, `envelope_codec.hpp:100`, `legacy_fletcher_topic_type.hpp:127`, and the
  shipped `gateway-client-ts/src/envelope.ts:104`). *If false* — a decoder depending on
  position — **STOP-AND-ASK**: ordering changes interop, and the remedy is the owner's call.
- **P2 — `Attachments`' map-ness is not load-bearing in any public API.** Migration is
  mechanical (`operator[]`/`at`/`count`/`find`/`emplace`/`insert_or_assign`/`clear`→`Clear`).
  *If false* — a public surface outside `core`/`pubsub` (most likely `pubsub-arrow`'s
  `SubscribeResult`) exposing map semantics a binding is expected to use — **STOP-AND-ASK**
  before adding a compatibility accessor: a shim is a coexistence window in an item whose
  whole point is that there is one form.
- **P3 — `what()` is the only message channel.** *If false* — a provider stashing diagnostics
  in a side channel — **STOP-AND-ASK**; do not publish a second channel.
- **P4 — ruling 46 tripwire.** If expressing either form starts to need a C declaration,
  an `extern "C"` symbol, or a struct shared with either ABI round, **STOP** — that is ABI
  work this round may not do (locked decision 14).
- **P5 — machine checks, not hand-composed facts.** Migration completeness is proved by the
  compiler (the alias is gone, so every map-API site fails to build) and by the existing
  envelope round-trip tests. No caller ledger is written.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `SeamVocabulary.AnAttachmentSetIsReconstructibleFromItsPublishedFormAlone` | §2 — a stand-in boundary using only `size/KeyAt/ValueAt` serialises the set to flat bytes and rebuilds it through `Set`; the rebuilt set publishes a byte-identical form, and the same entries built in several construction orders publish the identical form. | **Does not compile:** the published form does not exist. Written against the map's own iteration it would green *by luck* on a build whose hash order happened to agree — which is the finding, not a workaround. |
| `SeamVocabulary.AnAlteredAttachmentSetPublishesADifferentForm` *(live negative control)* | §2 — one key byte changed, one value byte changed, two values swapped between keys, one entry dropped: each must publish a **different** form. | Does not compile (same reason). Proves the comparison in the positive test is not inert. |
| `Registry.ARefusalIsReconstructibleFromItsNumberAndMessageAlone` *(re-specified, rev 2)* | §4 — a stand-in boundary copies `{int32 status, bytes+length message}` across a byte-only, NUL-terminated channel and rebuilds a refusal indistinguishable from the original. **Ruling 55 removed its second leg**, which compared the rendered list against `RegisteredNames()`; the property is now asserted **through the message alone** — two registries holding the same names but built in different insertion orders must produce byte-identical refusal messages. Same property, no query. | **Deterministically red on the reconstruction leg** (CORRECTED, review cycle 2 — the `std::runtime_error` stand-in first named here could not redden): the factory throws `PubSubError(kTransportFailure, std::string("a\0b", 3))`, rethrown unchanged, so the NUL reaches the message and everything after it is lost across the channel today. The insertion-order leg is **green at base** — it guards the existing `std::map` seal (§5) and goes red if that container is re-typed. Stated as a seal guard, not claimed as forcing. |
| `Registry.ANumberAloneCannotTellTwoRefusalsApart` *(live negative control — unchanged)* | §4 rule 3 — a number-only boundary renders a typo'd provider name and an empty topic-segment list identically; with the message they are distinct. Never depended on the query: it reads two messages and compares them. | Asserts the message is load-bearing rather than decorative; it breaks if a later change folds the cause into the number. **Not weakened by ruling 55.** |
| `SeamVocabulary.AnAttachmentKeyThatWouldTruncateIsRefused` — **two legs, rev 3** | §3 — the attach leg: `Set` refuses a NUL-bearing key with `kInvalidArgument` (**ruling 54**). The arrival leg: a hand-built envelope body carrying a NUL in a key makes `ParseEnvelopeBody` return `false` **without throwing at all**, and `DeserializeEnvelope` throw `std::invalid_argument` — **never** `PubSubError` (**ruling 57**). | Today both are accepted: the key is stored and silently collides at a boundary. The arrival leg is additionally red *on the exception type* if the implementer routes decode through `Set` — the mistake it exists to catch. |

One sentence, false if any row breaks: **two builds handed the same attachments publish the same
form; a boundary holding only a refusal's number and message reproduces it exactly.**

## Risks / Unknowns

- **Every authorisation this item needs is in hand, and none generalises.** Wire bytes (locked
  decisions 11/13) by **ruling 53**, attachment ordering only; frozen §3.2 by **ruling 54**, the
  NUL-key sentence only; frozen §5.1 by **ruling 56**, one message class only; the receive-side
  refusal by **ruling 57**. Any further frozen sentence or moved byte is a fresh stop-and-ask.
- **The typed name query — DECLINED by ruling 55**, and gone from the design, the surface count
  and the tests. Residual cost: an application wanting "did you mean…" parses Fletcher's sentence
  or keeps its own list. The §4 list comes from a `std::map`, so one test leg is a seal guard.
- **Oracle check, redone by search rather than by assumption** — the practice that failed in
  revision 2. Every `PubSubError(` carrying a concatenation in `core/`, `pubsub/` and the three
  providers was read: all of them are downstream of `RequireSegments`, which refuses a NUL-bearing
  segment — *except* `segments.hpp:103-113`, which fire **before** its own check at `:115`. That
  is exactly the class ruling 56 names, and no other in-tree message changes. Third-party text
  (an SDK `what()`, `schema_import.cpp:24`'s Arrow status) is escaped by the same constructor —
  the mechanism's purpose, not an in-tree message change. §3.2's `unordered_map` sentence is what
  A6 may amend, not a contradiction. **No other spec contradiction; no other frozen text touched.**
- **Coexistence windows: none** — no conversion, no map-compatible accessor, no shim.
**Declared net lines (revision 3): +920 / −270, band +700 / +1600 adds.** Revision 2 declared
+830/−250; ruling 57's decode check and its two-leg test add ~35, B2's four files ~35, ruling
56's §5.1 amendment ~20. Of the total, ~400 are tests, ~180 the container, ~170 spec and doc
text, the rest call-site migration. **File counts reconciled (DEBT-7):** 54 source files name
`Attachments`; ~15 hold a map *expression* and need real edits, the rest only a `const
Attachments&` in a signature — the ceiling is pegged to that 15. **What would make the band
wrong:** P2 failing; the envelope and Fast DDS encode loops needing restructuring rather than
re-indexing; or the decode refusal needing a third call site. Past +1600 the honest move is a
stage split (container plus migration first, the message rule second), not a quiet overrun —
AG1 declared +1050 and landed +2028.

**New public surface: 1** — `fletcher::Attachments` (the class, whose members replace the map
API the retired alias published; `Clear()` is one of them, not a second item). No registry
method; no `PubSubProvider` method added, removed or reordered; no `PubSubStatus` value appended.

## Files-to-touch

- `core/include/fletcher/core/`: `types.hpp` (the class), `status.hpp` (message rule, NUL escape),
  `envelope.hpp` (positional encode; the arrival refusal in `DeserializeEnvelope`); `core/README.md`;
  `core/tests/{test_envelope,test_status_taxonomy}.cpp`.
- `pubsub/`: `include/fletcher/pubsub/{provider,publisher,subscriber,in_process_provider,delivery_channel}.hpp`,
  `src/{in_process_provider,subscriber,delivery_channel}.cpp`, `tests/test_publisher_subscriber.cpp:388-393`
  — migration; `{include/fletcher/pubsub,src}/provider_registry.{hpp,cpp}` — **comment only**
  (ruling 55: no method added, no composition rewritten).
- `fastdds-pubsub-provider/`: `src/internal/{envelope_codec,data_reader_listener,ordered_delivery}.hpp`
  (`envelope_codec` also gains the arrival check), `src/fast_dds_pubsub_provider.cpp`, `benchmarks/*`,
  `tests/*`, `README.md:529,535`.
- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`, `tests/*`, `test_package/src/example.cpp`;
  `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`, `pubsub-arrow/tests/*`,
  `gateway/src/ws_session.cpp`, `integration-tests/protoc-arrow-bridge/tests/test_pubsub.cpp`.
- `integration-tests/pubsub-conformance/src/{seam_vocabulary,registry,copy_accounting,copy_clauses,caller_tier}.cpp`,
  `include/fletcher/conformance/copy_accounting.hpp`, `README.md`.
- Docs: `docs/pubsub-interface-spec.md` §3.2 (A6; rulings 53, 54) and §5.1 (A7; ruling 56);
  `docs/wire-format-specification.md` `:165` and `:142-158` (the stated order, where ruling 53 says
  it belongs) — **not `frozen`: an edit**; `docs/protocol-driver-abi-spec.md` `:21-23`, §0.2 `:79-81`
  (ruling 46); `plans/PDA-decouple-interface.md` — the AG2 row.

## Files-to-delete

- `Attachments = std::unordered_map<std::string, Blob>` (`core/include/fletcher/core/types.hpp:119`)
  and every member it published — `operator[]`, `at`, `count`, `find`/`end`, `emplace`,
  `insert_or_assign`, `clear`, `begin`/`end`. *Replacement:* the sealed form
  (`size`/`empty`/`KeyAt`/`ValueAt`/`Find`/`Set`/`Clear` — `Clear` because the reused decode
  out-param needs it and nothing else can empty the container).
- `docs/wire-format-specification.md:165`'s `unordered_map` Key-Types row, and
  `docs/protocol-driver-abi-spec.md:21-23` + §0.2 `:79-81`'s any-language driver licence.
  *Replacement:* the sealed container for the first; for the second, *no replacement — licence
  gone, disclosed*, ruling 46 superseding it with the owner looking at that text.
- No test file is deleted; test **bodies** migrate off the map API in place — the compiler, not
  a ledger, proves none was dropped. `RegisteredNames()` and its conformance test
  `Registry.TheAvailableNamesAreAnsweredWithoutReadingProse` are deleted before being written:
  *no replacement — capability declined, disclosed*, ruling 55.
