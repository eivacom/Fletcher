# PDA-DEC-AG2 — The two crossing types with no form

*Design (step 1). Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md)
§3.2 and §5.1 — cited, not restated. Rulings ledger:
[plans/PDA-DEC-rulings.md](PDA-DEC-rulings.md), 52 entries, all binding.
Locked decisions: [plans/PDA-decouple-locked-decisions.md](PDA-decouple-locked-decisions.md).*

## Summary

`Attachments` (A6) and a seam error's **message** (A7) are the two things that cross
the seam with no published form. Both fail the same way today: **their published
content is a function of the build, not of the value** — attachment entries are
enumerated (and encoded onto the wire) in `std::unordered_map` hash order, and the §4
refusal that lists the registered provider names composes that list the same way.
This item gives each a form that a boundary can reproduce, and makes the build-
dependent form unrepresentable rather than documented.

## Design

### 1. The class, stated once

A published form must be **a function of the value alone**. Anything a separately
built binary — or a language binding — cannot reproduce from the value is not a form.
That is the single sentence A6 and A7 share, and it is why they are one item.

Ruling 46 removed the driver-side C-expressibility proof: both sides of the protocol
ABI are C++. Its replacement test — *"does this data drill all the way up to the
language ABI?"* — both of these pass. Attachments reach a C#/Rust application; a
refusal's message is the only thing a mistyped protocol name gives an operator. So
both still owe a language-agnostic form. **No C declaration is written here** (locked
decision 14; charter ruling 2026-09-01). The form is published as rules over the C++
value plus a stand-in boundary in the harness.

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

- **One enumeration, and it is positional.** There is deliberately **no `begin()/end()`**
  and no map API. A range-`for` over the container is exactly what made the wire order
  a build artefact; removing it forces every producer — including the envelope
  encoder — through the published form. Cost: roughly a dozen loops become indexed
  loops. This is the 2026-09-01 *"one mechanism, not two"* shape.
- **The order is part of the form:** entries enumerate in **ascending unsigned-byte
  order of the key's UTF-8 bytes**. A boundary never sorts — it reads the sequence
  Fletcher publishes — so no binding has to reproduce a collation (C#'s ordinal
  comparison is UTF-16 code-unit order and would differ; it never has to be used).
- **`Find` returns a pointer, not a reference or an exception.** Absent is a value, not
  a failure; there is no `at()` to throw and no `operator[]` to insert-by-accident.
- `Blob` is unchanged. Its five clauses (§3.2) are **inherited, not re-proved** here.

**Wire consequence, deliberate:** `SerializeEnvelope`
(`core/include/fletcher/core/envelope.hpp:63-70`) and the Fast DDS codec
(`fastdds-pubsub-provider/src/internal/envelope_codec.hpp:40`) enumerate attachments
to produce the wire bytes. Today that order is `std::hash`'s; after this it is the
key order. The envelope *format* is untouched and every decoder in the tree matches by
key, never by position — but the bytes emitted for a ≥2-attachment message change on
builds whose hash order was not ascending. See **Risks** (locked decisions 11 and 13)
and Stage-Brief decision 1.

### 3. A key that cannot survive the trip is refused at the door

`Set` refuses a key containing a **NUL** with `kInvalidArgument`. A key is
bytes-plus-length at the seam, but a language boundary that marshals a NUL-terminated
string silently truncates it, so `"a\0b"` and `"a"` become one attachment and one
overwrites the other — the silent-wrong-answer class this round has refused three
times (2026-09-04, twice for topic segments and once for the provider selector, which
already refuses a NUL for exactly this reason:
`pubsub/src/provider_registry.cpp:107-113`).

This writes a **new normative refusal into frozen §3.2** that the BIND stop-and-ask
verification did not name, so it follows the `__`-prefix precedent: Stage-Brief
decision 2, **no default on silence**. Nothing else in the design depends on it.

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
   provider instances in one process. (The sibling spec already states this rule at
   `docs/protocol-driver-abi-spec.md:229-232`; the seam adopts the same one so the two
   boundaries derive one rule, not two.)
2. It is **bytes plus length**, UTF-8, and **contains no NUL**.
3. A boundary must convey **the number and the message**. For a §4 configuration
   refusal the message *is* the answer: `kInvalidArgument` is shared with many other
   refusals, so a boundary that forwards only the number hands an operator a bare
   "invalid argument" for a typo'd protocol name.

Rule 2 is made true by construction rather than asserted: `PubSubError`'s constructor
escapes any NUL in the message as `\x00`, the same *coerce-don't-throw* shape §5.1
already mandates for a refused status. The untrusted entry is
`TranslateSeamFailure`'s `catch (const std::exception& e) → e.what()` — a third-party
SDK's message, which Fletcher does not compose. **Only** NUL is escaped: §5.1's
existing sentence *"Messages are unchanged"* stays true of every message in the tree,
because no message in the tree contains one.

### 5. The §4 answer stops being a build artefact

`pubsub/src/provider_registry.cpp:167-180` composes `"available: …"` by iterating
`factories_`, an `unordered_map`. `ProviderRegistry` gains
`std::vector<std::string> RegisteredNames() const` — ascending byte order — and the
refusal message renders exactly that sequence. Two effects: the message becomes a
function of the registry's contents, and an application can list what *is* available
without parsing Fletcher's prose (Stage-Brief decision 3).

Nothing about `Register`/`Create` changes; the round's registry signature is untouched,
so a path selector remains admissible (locked decision 3).

### 6. The one extra line

`docs/protocol-driver-abi-spec.md` `:21-23` and §0.2 `:79-81` still publish the
multi-language driver licence that ruling 46 superseded. Both are amended to say the
driver side is C++. That spec is `proposed`, not `frozen`, so this is an edit, not an
amendment requiring authorisation.

### 7. What is published, and what it claims

`integration-tests/pubsub-conformance/README.md` gains one short section, worded to the
round's ninth-consecutive narrow-claim preference (2026-09-04): green proves that *this
tree's* attachments container and *this tree's* refusals are reproducible from their
published form by a stand-in boundary. It proves nothing about any real C#/Rust
binding, because none exists to measure — the same limit the `encode_copies` claim
already carries.

## Corner cases forbidden

**Rung 1 — unrepresentable:**

1. **Build-dependent enumeration of attachments.** The container has no unordered
   enumeration to reach; order is a total order on key bytes fixed by the type.
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
   one check in `Set`, `kInvalidArgument`, no partial mode. *Owner-gated (decision 2).*
8. **An out-of-range index** — `KeyAt`/`ValueAt` refuse with `kInvalidArgument`. One
   check, no clamping, no sentinel entry, no "valid sometimes" index.

**Handled residue — each with why it is not forbidden:**

9. **A message longer than a boundary's buffer.** Not bounded. *Why not forbidden:* no
   bound has been measured anywhere in the tree, and A5's precedent is that a refusal
   ceiling is measured, not reasoned. A boundary that truncates its own copy is that
   boundary's choice, and the rule says the message is bytes-plus-length so it need not.
10. **Non-printable, non-NUL bytes in a message.** Escaped only where caller data is
    composed in (`Quoted`, `provider_registry.cpp:52`). *Why not forbidden:* the
    message is a human diagnostic in UTF-8 and forbidding arbitrary bytes wholesale
    would mangle legitimate non-ASCII text; only NUL truncates.

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
  before adding a compatibility accessor; a shim here is a coexistence window in an item
  whose whole point is that there is one form.
- **P3 — `what()` is the only message channel.** *If false* — if any provider stashes
  diagnostics in a side channel — **STOP-AND-ASK**; do not publish a second channel.
- **P4 — ruling 46 tripwire.** If expressing either form starts to require a C
  declaration, an `extern "C"` symbol, or a struct shared with either ABI round,
  **STOP** — that is ABI work this round may not do (locked decision 14), and ruling 46
  already removed the driver-side proof that would have motivated it.
- **P5 — machine checks, not hand-composed facts.** The migration's completeness is
  proved by the compiler (the alias is gone, so every site that used map API fails to
  build) and by the existing envelope round-trip tests. No caller ledger is written.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `SeamVocabulary.AnAttachmentSetIsReconstructibleFromItsPublishedFormAlone` | §2 — a stand-in boundary using only `size/KeyAt/ValueAt` serialises the set to flat bytes and rebuilds it through `Set`; the rebuilt set publishes a byte-identical form, and the same entries built in several construction orders publish the identical form. | **Does not compile:** the published form does not exist. Written against the map's own iteration it would green *by luck* on a build whose hash order happened to agree — which is the finding, not a workaround. |
| `SeamVocabulary.AnAlteredAttachmentSetPublishesADifferentForm` *(live negative control)* | §2 — one key byte changed, one value byte changed, two values swapped between keys, one entry dropped: each must publish a **different** form. | Does not compile (same reason). Proves the comparison in the positive test is not inert. |
| `Registry.ARefusalIsReconstructibleFromItsNumberAndMessageAlone` | §4 + §5 — a stand-in boundary copies `{int32 status, bytes+length message}` across a byte-only, NUL-terminated channel and rebuilds a refusal indistinguishable from the original; the message renders exactly `RegisteredNames()`. | **Deterministically red:** a factory throwing a `std::runtime_error` whose `what()` contains a NUL loses everything after it across the channel today. `RegisteredNames()` does not exist, so the ordering leg does not compile. |
| `Registry.ANumberAloneCannotTellTwoRefusalsApart` *(live negative control)* | §4 rule 3 — a number-only boundary renders a typo'd provider name and an empty topic-segment list identically; with the message they are distinct. | Asserts the message is load-bearing rather than decorative; it breaks if a later change folds the cause into the number. |
| `SeamVocabulary.AnAttachmentKeyThatWouldTruncateIsRefused` *(decision 2 only)* | §3 — `Set` refuses a NUL-bearing key with `kInvalidArgument`. | Today the key is accepted and silently collides at a boundary. Not landed if decision 2 is unanswered. |
| `Registry.TheAvailableNamesAreAnsweredWithoutReadingProse` *(decision 3 only)* | §5 — `RegisteredNames()` equals, in order, the names the refusal message renders. | Does not compile. Not landed if decision 3 is declined. |

The item's one sentence, false if any of the above breaks: **two Fletcher builds handed
the same attachments set publish the same form, and a boundary holding only a refusal's
number and message can reproduce that refusal exactly.**

## Risks / Unknowns

- **STOP-AND-ASK (locked decisions 11 and 13) — attachment order on the wire.**
  Decision 13: *"A wire-byte change is a stop-and-ask"*; decision 11 says the same of a
  divergence fix. Ordering the container changes the bytes emitted for a ≥2-attachment
  message on any build whose hash order was not ascending. The argument that it is not a
  format change: the envelope layout is untouched, every decoder matches by key, and
  today's bytes are already **not** a function of the input — two Fletcher builds can
  already emit different bytes for the same envelope. The argument it *is* one: the
  emitted bytes for some inputs change. Raised as **Stage-Brief decision 1** with a
  recommendation and a default; flagged here so the PM sees it before implementation
  rather than after a red test.
- **New normative refusal in frozen §3.2** (the NUL key) is authorised by nobody yet —
  §12.1's *who may act* is "nobody alone". Carried as decision 2 with **no default**,
  per the 2026-09-04 idempotence ruling.
- **New public method on the frozen §4 surface** (`RegisteredNames`) — decision 3.
  Declining it costs one test and nothing else.
- **Oracle check performed:** §5.1's *"Messages are unchanged"* remains true, because no
  message in the tree contains a NUL; §3.2's `unordered_map` sentence is the sentence A6
  is authorised to amend, not a contradiction. **No other spec contradiction found.**
- **Coexistence windows: none.** The alias is retired in the same change that introduces
  the class, with no conversion and no map-compatible accessors — the same reasoning
  §3.2 already records for the retired `shared_ptr` alias.
- **Size risk.** The declared band assumes the migration is mechanical. It is wrong if
  P2 fails, or if `pubsub-arrow`'s per-row attachment vector needs reshaping rather than
  a rename. AG1 landed at ~1.9x its declaration; the band below is set accordingly.

**Declared net lines: +900 / −250, band +700 / +1300 adds.** Of the adds, ~400 are
tests, ~150 are the two spec amendments and the harness README, ~200 the container and
the registry, the rest call-site migration.

**New public surface: 2** — `fletcher::Attachments` (the class; it *retires* the
`unordered_map` alias and every map member that alias published) and
`ProviderRegistry::RegisteredNames()` (decision 3). No `PubSubProvider` method is added,
removed or reordered; no `PubSubStatus` value is appended.

## Files-to-touch

- `core/include/fletcher/core/types.hpp` — the `Attachments` class.
- `core/include/fletcher/core/status.hpp` — the message rule; NUL escape in `PubSubError`.
- `core/include/fletcher/core/envelope.hpp` — encode/decode through the positional form.
- `core/README.md` — one line on the message's form beside the taxonomy table.
- `core/tests/test_envelope.cpp`, `core/tests/test_status_taxonomy.cpp`.
- `pubsub/include/fletcher/pubsub/provider_registry.hpp`, `pubsub/src/provider_registry.cpp`.
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
- No test file is deleted; test **bodies** in `core/tests/test_envelope.cpp` and the
  provider suites migrate off the map API in place, because each still asserts a fact
  that survives — the compiler, not a ledger, proves none was silently dropped.
