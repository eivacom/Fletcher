# DICT-1 — Option surface + reader (design)

Round: **DICT** · Item: **DICT-1** · Branch: `feature/dictionary-option` (stacked on
`feature/generator-ir-rewrite`, POST-GIR tree)

- User story / acceptance: [plans/DICT-dictionary-option.md § DICT-1](DICT-dictionary-option.md)
- Locked decisions: [plans/DICT-locked-decisions.md](DICT-locked-decisions.md) (#1–#5, #10)
- Spec: [docs/dictionary-option-spec.md](../docs/dictionary-option-spec.md) §2, §7
- Forcing test: `TypeMapperTest.ReadsDictionaryOption`

---

## Summary

Add the `(fletcher.dictionary)` surface to `protoc/include/fletcher/options.proto`
(extension **50001** on `FieldOptions`) and a **typed reader** that turns it into
`{index_kind, ordered}`. The reader is built on the **reflection/`DynamicMessage`
mechanism PR #121 already proved** — extracted into one shared primitive both
readers call — not on a new unknown-field walker. Its output lands on the IR's
canonical carrier `ir::FieldFacts` (locked #5). No mapper projection, no schema
emission, no accessor/Rust change.

**Revision 2 (2026-08-28)** — reworked against the Step-2 review appended at the
end of this file. The mechanism choice did not change: the review confirmed D0/D2,
the `FieldFacts` correction, the per-call factory, the probe's necessity for the
genuinely-malformed case, 50001 being free, and the RBA/Rust scope guard. What did
change:

| Review item | Resolution | Where |
|---|---|---|
| R1 presence probe is a silent false-positive channel | probe narrowed to "extension resolved **and** blob unreadable"; the `fletcher.DictionaryOptions` full-name gate dropped; extension-not-in-pool → `nullopt` | D3 steps 2 / 5 + edge table |
| R2 `flatten_field` is a second, unmentioned drop site | new **D4b**: names both walk sites, assigns enforcement (reject) to DICT-2, adds two DICT-1 pinning sub-cases | D4b, hand-off, tests |
| R3 spec §7 still mandates the walker | §7 amended **in this item**; added to Files-to-touch | D0 "Spec §7", Files |
| R4 by-name claim vs by-number decode | `index_type` is now decoded by **enum symbol name** (precedent `option_metadata.cpp:99-106`), so D0 reason 2 is true as written | D3 step 4 |
| R5 four sub-cases unreachable through the harness | unknown-field **injection** route pinned (`test_ir.cpp:56-61` pattern), `inline` header, one-pool-per-fixture 50000 trap | Forcing-test mapping |
| R6 factory/message destruction order; partial snippet | invariant stated as a rule the refactor must not break; **full** `ParsedOptions` body incl. the retained `cache.find` hit | D2 + D3 step 3 |
| R7 "leaf wins" untestable; no conflict policy | test declares **different** index types; policy = leaf wins, silently accepted but **warned** through the existing `facts.warning` channel | D4 |
| R8 purity claim is wrong | corrected: benign cached-size write on the shared default instance; single-threadedness is the real guarantee | D5 |

Non-blocking notes 1–4 are folded in (suite-name rationale, WKT-wrapper sub-case,
SPDX/clang-format obligation, `HasFieldDictionary` usage note).

---

## Design

### D0 — Option-reading mechanism: REUSE the PR #121 reflection reader (decision)

**Chosen: reflection over a `DynamicMessage` built from the `DescriptorPool`
protoc populates from the `CodeGeneratorRequest`.** Locked decision #10's
constraint ("do not link `options.pb.cc`") is honoured; its *mechanism*
(hand-rolled unknown-field walker) **retires**.

Evaluation, concretely:

* The mechanism lives in `protoc/src/option_metadata.cpp`,
  `OptionMetadataResolver::Impl::ParsedOptions` (lines 247–260): serialize the
  linked-in options message (whose custom options sit in its `UnknownFieldSet`
  precisely because the plugin does not link the declaring `.proto`), then
  re-parse those bytes into a `DynamicMessage` whose descriptor comes from the
  pool — which *does* know `fletcher.dictionary`. The extension then reads as a
  real field by reflection.
* It already covers a **message-typed** extension, not just scalar/enum ones:
  `option_metadata.cpp:276-282` does `GetMessage(*cur_opts, st.ext, &factory)` and
  descends sub-fields by descriptor, and the fixture at
  `test_option_metadata.cpp:199-260` declares exactly that shape
  (`extend google.protobuf.FieldOptions { ColOpts col = 60100; }`). So
  `DictionaryOptions` needs no new capability — only a typed decode.
* **The `OptionMetadataResolver` *class* is not reusable as-is** and must not be
  bent into service: its public API is rule-driven
  (`--fletcher_opt=metadata_from_option=` tokens) and returns
  `vector<pair<string,string>>` of *stringified* values. Reading
  `(fletcher.dictionary)` through it would mean synthesising internal rules and
  re-parsing strings back into an enum — worse than either alternative.
* Therefore: **reuse the mechanism, not the class.** Extract `ParsedOptions`'
  round-trip into one shared free function (D2) that both `option_metadata.cpp`
  and the new dictionary reader call. One implementation of the trick, two typed
  consumers. No second bespoke option parser exists after this item.

Why reflection beats a walker here (recorded reasons, not preference):

1. A message-typed option needs a *nested* parse; a walker would hand-decode
   varints inside a length-delimited payload — the exact bespoke-parser drift GIR
   spent a round removing.
2. It is **forward-compatible with the pool's `options.proto`, not the plugin's**:
   a consumer pinning an older/newer `fletcher/options.proto` still works, because
   sub-fields are located **by name** and enum values are resolved to their
   **symbol names** (D3 step 4) on the pool's descriptor, and anything the plugin
   does not recognise is skipped. A walker hard-codes field numbers *and* enum
   numbering.
3. It survives someone later linking a generated `options.pb.cc`: the
   serialize/re-parse round-trip is agnostic to whether the extension arrived as a
   known field or an unknown one. An `UnknownFieldSet` walker silently returns
   "absent" in that case.

The existing `FindBoolOption` walker (flatten, #50000) is **left untouched** —
migrating it is not DICT-1's job and would put a working, tested path at risk.
Recorded as follow-up in Risks.

**Spec §7 amendment (required in this item).**
[docs/dictionary-option-spec.md](../docs/dictionary-option-spec.md) §7 currently
mandates the walker ("The reader must walk that payload's inner fields … done with
a small unknown-field walker"). Locked #10 (revisited) authorises the switch, so
this is not a stop-and-ask — but the authoritative spec must not contradict the
implementation, and leaving it to DICT-5 would let a stale mandate govern DICT-2/3
review. DICT-1 therefore rewrites §7 to describe: the reflection/`DynamicMessage`
mechanism and where it is shared from; the **narrowed** presence probe (D3 step 5)
and that it decodes nothing; the fail-soft-to-defaults contract and its limit; and
an explicit restatement that the plugin still does **not** link `options.pb.cc`.
The "linking the generated descriptor" alternative stays recorded as the fallback
if the option set ever outgrows reflection.

### D1 — Option surface (`protoc/include/fletcher/options.proto`, spec §2)

`50001` on `FieldOptions` verified free: the file currently declares only
`flatten = 50000` (MessageOptions) and `flatten_field = 50000` (FieldOptions), and
no in-repo fixture uses 50001 (the option fixtures sit at 60100+). Append (proto2
file, so `optional` = explicit presence):

```proto
enum DictionaryIndexType {
    DICTIONARY_INDEX_UNSPECIFIED = 0;   // resolves to int32
    DICTIONARY_INDEX_INT8        = 1;
    DICTIONARY_INDEX_INT16       = 2;
    DICTIONARY_INDEX_INT32       = 3;
    DICTIONARY_INDEX_INT64       = 4;
}

message DictionaryOptions {
    optional DictionaryIndexType index_type = 1;
    optional bool               ordered     = 2;  // v1: rejected at codegen (DICT-2)
}

extend google.protobuf.FieldOptions {
    // 50000 is taken by flatten_field on FieldOptions.
    optional DictionaryOptions dictionary = 50001;
}
```

Purely additive to a shipped file: new enum, new message, new extension number.
No public C++/ABI surface changes, no major bump.

Also add the **one registry row** (`50001 | FieldOptions | dictionary`) to
`docs/fletcher-options.md` per locked #2 — cheap insurance that a later round
cannot re-take the number. The user-facing prose section stays DICT-5's.

### D2 — The shared mechanism: new `option_reader` module

New files `protoc/include/option_reader.hpp` / `protoc/src/option_reader.cpp`.
Header comment must state the split against the neighbouring module:
`option_metadata` copies **third-party** options into Arrow metadata as strings;
`option_reader` reads **Fletcher's own** options into typed values; both share one
re-parse primitive.

```cpp
// option_reader.hpp
namespace fletcher {

// Re-parse `opts` (a linked-in descriptor.pb options message; custom options the
// plugin does not link live in its UnknownFieldSet) as a DynamicMessage of
// `pool_options_descriptor` — the SAME options type as seen in the DescriptorPool
// protoc built from the CodeGeneratorRequest, which does know the extensions.
// Returns nullptr if the blob does not re-parse.
//
// LIFETIME (load-bearing): the result is created from a `factory` prototype and
// MUST be destroyed BEFORE `factory`. Callers therefore declare the factory
// FIRST and the returned message (or its owner) after it.
std::unique_ptr<google::protobuf::Message> ReparseOptionsWithPool(
    const google::protobuf::Message& opts,
    const google::protobuf::Descriptor* pool_options_descriptor,
    google::protobuf::DynamicMessageFactory* factory);

}  // namespace fletcher
```

**`OptionMetadataResolver::Impl::ParsedOptions` becomes exactly this** — the cache
hit path is retained verbatim; only the two round-trip lines move out. Dropping the
cache would be a #121 behaviour change (it is what makes the shared default options
instance parse once per resolver):

```cpp
const Message* ParsedOptions(const Message& opts, const Descriptor* pool_opts_desc) const {
    auto it = cache.find(&opts);
    if (it != cache.end()) return it->second.get();

    std::unique_ptr<Message> dyn = ReparseOptionsWithPool(opts, pool_opts_desc, &factory);
    if (!dyn) return nullptr;
    const Message* raw = dyn.get();
    cache.emplace(&opts, std::move(dyn));
    return raw;
}
```

**Member-order invariant (add as a code comment on the struct).** The cached
messages are created from `factory`'s prototypes, so they must be destroyed before
it. That holds today *only* because `factory` (`option_metadata.cpp:241`) is
declared **before** `cache` (`option_metadata.cpp:245`) — members destruct in
reverse declaration order. D2 is a diff into that neighbourhood, so the ordering
must be pinned by a comment: reordering those two members is silent
use-after-free, not a style change.

`protoc/tests/test_option_metadata.cpp` must stay green **unchanged** — that is the
regression proof for this refactor.

### D3 — The typed dictionary reader

```cpp
// option_reader.hpp (continued) — includes ir.hpp for ir::DictionaryIndexKind
struct DictionaryOption {
    ir::DictionaryIndexKind index_kind = ir::DictionaryIndexKind::INT32;
    bool ordered = false;
};

// Typed read of [(fletcher.dictionary) = {...}] on `field`. nullopt == option absent.
// Never fails: a DECLARED but unreadable option resolves to defaults (see contract).
std::optional<DictionaryOption> ReadFieldDictionaryOption(
    const google::protobuf::FieldDescriptor* field);

// Presence only. Equivalent to ReadFieldDictionaryOption(field).has_value().
// Prefer `node.facts.dictionary` when you already hold an IR node: this overload
// re-does the reflection read and, being descriptor-based, cannot see a dictionary
// that reached a node through flatten propagation (D4). It exists for callers that
// have no IR node (e.g. a raw-descriptor validation walk).
bool HasFieldDictionary(const google::protobuf::FieldDescriptor* field);
```

**Placement note (deliberate deviation from the story's letter).** The story says
"declare the public reader in `type_mapper.hpp`". It goes in the new
`option_reader.hpp` instead, because (a) `type_mapper.hpp:13-15` deliberately only
forward-declares `ir::IrNode` and must not start including `ir.hpp` — which the
`DictionaryIndexKind` return type would force; (b) `type_mapper.cpp` is already
465 lines and post-GIR is a *projection* layer, not the option layer; (c) the
reader's only production consumers are `ir.cpp` (DICT-1) and `generator.cpp`
(DICT-1.5), neither of which needs `type_mapper.hpp` for it. No locked decision
names a file. `type_mapper.hpp`/`.cpp` are **not touched** by DICT-1.

**Algorithm** (`kExtName = "fletcher.dictionary"`, `kExtNumber = 50001`):

1. **Fast path.** If `field->options().ByteSizeLong() == 0` → `nullopt`. An
   options-less field shares the default instance; this makes the common field
   free and keeps `BaseFacts` cheap (see D5).
2. **Resolve the extension — this is the only evidence the reader will act on.**
   `pool = field->file()->pool()`, `ext = pool->FindExtensionByName(kExtName)`.
   Accept only if **all** hold:
   * `ext != nullptr`
   * `!ext->is_repeated()`
   * `ext->containing_type()->full_name() == "google.protobuf.FieldOptions"`
   * `ext->number() == kExtNumber`
   * `ext->cpp_type() == CPPTYPE_MESSAGE`

   If any fails → **`nullopt`** (no dictionary). Note the gate deliberately does
   **not** require `ext->message_type()->full_name() == "fletcher.DictionaryOptions"`:
   the sub-field decode is by name (step 4), so a renamed-but-compatible option
   message reads correctly, and demanding the name would silently downgrade it to
   defaults. Residual limit, recorded: a fork that **renumbers** the extension away
   from 50001 is unreadable — and by locked #2 it is not Fletcher's option.
3. **Re-parse.** Declare the factory first, then the message (D2 lifetime rule):
   ```cpp
   google::protobuf::DynamicMessageFactory factory;   // MUST outlive `dyn`
   std::unique_ptr<google::protobuf::Message> dyn =
       ReparseOptionsWithPool(field->options(), ext->containing_type(), &factory);
   ```
   On `nullptr` → step 5.
4. **Decode.** If `!refl->HasField(*dyn, ext)` → `nullopt` (**option genuinely
   absent**; presence is the trigger, not truthiness — locked #1). Otherwise
   `const Message& d = refl->GetMessage(*dyn, ext, &factory)` and read sub-fields
   **by name** off `d.GetDescriptor()`:
   * `index_type` — accepted when present, singular and `CPPTYPE_ENUM`. Resolve
     the **symbol name**, not the raw number:
     `f->enum_type()->FindValueByNumber(refl->GetEnumValue(d, f))->name()`
     (precedent: `option_metadata.cpp:99-106`), then map
     `DICTIONARY_INDEX_UNSPECIFIED→INT32`, `…_INT8→INT8`, `…_INT16→INT16`,
     `…_INT32→INT32`, `…_INT64→INT64`. Unknown name, null `EnumValueDescriptor`,
     absent/missing/wrong-typed field → `INT32`. **This is what makes D0 reason 2
     true**: a fork that renumbers the enum values is still read correctly, and
     the shipped numbering is separately pinned by the forcing test's
     hand-encoded bytes.
   * `ordered` — present, singular, `CPPTYPE_BOOL` → `GetBool`; else `false`.

   Return the `DictionaryOption`. All values are copied out **before** `factory`
   dies at scope exit.
5. **Degraded presence probe — narrow.** Reached **only** from step 3, i.e. the
   extension resolved and passed the gate but the blob did not re-parse. Probe
   `field->options()`' `UnknownFieldSet` for a `TYPE_LENGTH_DELIMITED` field
   numbered 50001; if found → `DictionaryOption{}` (int32, unordered), else
   `nullopt`. This is a *presence probe*, not a payload parser — it decodes
   nothing — so it does not reconstitute the walker D0 rejected.

   **Why narrow (R1).** In the earlier draft step 5 was also reached when the
   extension was absent from the pool, which trusted a bare field number that
   step 2 had just refused to trust on stronger evidence. A pool that does not
   import `fletcher/options.proto` but carries some third-party message-typed
   `FieldOptions` extension at 50001 — entirely plausible in the collision-prone
   50000-range that PR #121 exists to serve — would have silently produced a
   dictionary column nobody declared. Narrowed, the probe can only ever fire for
   an option Fletcher's own declaration backs.

**Fail-soft contract (why, and its limit).** A **declared** but unreadable option
resolves to **defaults**, never to "absent" and never to a hard error: the reader
is called from `ir::BuildFieldIr`, which has no error channel, and dropping a
declared dictionary would emit a *value-typed* schema for a field the author
declared dictionary — the worst outcome. The story's acceptance requires exactly
this ("a malformed/empty payload resolves to defaults"). The reachable-only-by-
malformation paths are unreachable from protoc-compiled input (protoc serialises a
valid `DictionaryOptions` or fails the compile), so this is a robustness floor, not
a routine path. If DICT-2 later wants a hard error for a corrupt payload it has the
error channel; DICT-1 does not.

Edge cases the design pins (all reachable via injected unknown fields, hence
tested — see the harness section):

| State of `FieldOptions` / pool | Result |
|---|---|
| no options at all | `nullopt` (step 1) |
| extension declared, empty payload (`= {}`) | present, `{INT32, false}` — a zero-length submessage still sets presence |
| extension declared, `08 02` | `{INT16, false}` |
| extension declared, `10 01` | `{INT32, true}` |
| extension declared, `08 02 10 01` | `{INT16, true}` |
| extension declared, `08 09` (undeclared enum number) | present, `{INT32, false}` — closed proto2 enum → value lands in unknowns → sub-field absent → default |
| extension declared, truncated/garbage (`08`) | present, `{INT32, false}` (step 5) |
| extension declared, **varint** at 50001 | present, `{INT32, false}` — wire-type mismatch on a known field → unknowns → step 4 `HasField` false… **see note** |
| **extension NOT in the pool** (or gate mismatch), any bytes at 50001 | **`nullopt`** — no false positive (R1) |

> Note on the varint row: a varint at 50001 makes step 4's `HasField(ext)` false, so
> the reader returns `nullopt` *without* reaching step 5. That is the correct answer
> (the bytes are not a `DictionaryOptions`), and it is asserted as such. The row is
> kept in the table because the earlier draft answered it from step 5.

### D4 — Where the option lands: `ir::FieldFacts` is the carrier (locked #5)

Locked #5 names `ScalarFacts.dictionary`; **the post-GIR type is
`ir::FieldFacts.dictionary`** (`protoc/include/ir.hpp:133-149`) — there is no
`ScalarFacts` type. Same intent (a field-level modifier on the IR, never a
structural kind), so this is a naming correction, not a deviation. `FieldFacts` is
in fact the *better* home: its comment (`ir.hpp:129-132`) already declares itself
the single canonical home of `nullable` **and `dictionary`**, and
`test_ir.cpp:217-232` already asserts a dictionary-modified scalar stays
`NodeKind::SCALAR`.

`FieldFacts` currently carries only `bool dictionary`, which is not enough to reach
DICT-3 — the index type would otherwise have to live on `FieldMapping`, making the
flat projection a second source of truth (exactly what locked #5 forbids). So
DICT-1 adds the missing two fields:

```cpp
// ir.hpp — new, next to the existing (unused) DictionaryModifier
enum class DictionaryIndexKind { INT8, INT16, INT32, INT64 };

struct FieldFacts {
    ...
    bool dictionary = false;                       // existing — now populated
    DictionaryIndexKind dictionary_index_kind = DictionaryIndexKind::INT32;  // new
    bool dictionary_ordered = false;                                          // new
    ...
};
```

*Why a dedicated 4-value enum rather than reusing `LogicalKind::INT8/16/32/64`:*
those slots exist but have **no `cpp_backend_type_table` entry** (no proto type maps
to int8/int16), so reuse buys nothing and invites someone to feed the index kind to
`LookupScalar`. Four values make DICT-3's mapping an exhaustive `switch` with no
`default`. The pre-existing unused `ir::DictionaryModifier` is **left untouched** —
adding a second presence representation would be drift (see Risks).

**Population** — `protoc/src/ir.cpp`:

* `BaseFacts(field)` (line 23): `if (auto d = ReadFieldDictionaryOption(field))
  { f.dictionary = true; f.dictionary_index_kind = d->index_kind;
  f.dictionary_ordered = d->ordered; }`. One place, so every node built from a
  field gets it identically.
* `BuildFlattenedSingular` (line 261) **must propagate the outer field's option**.
  It returns the *inner* field's IR (`inner_ir`), whose facts come from the inner
  field, so an option on the wrapper field would otherwise be silently dropped and
  DICT-3 would emit a value-typed column for a field the author declared
  dictionary. Mirror the existing `nullable` propagation on line 274, **leaf wins**
  when both declare it (same precedence as `OptionMetadataResolver::ForField`'s
  leaf-first scan, `option_metadata.cpp:485-491`):

  ```cpp
  const FD* inner = msg->field(0);
  IrNode inner_ir = BuildFieldIr(inner);
  if (IsFieldNullable(field)) inner_ir.facts.nullable = true;
  if (auto outer = ReadFieldDictionaryOption(field)) {
      if (!inner_ir.facts.dictionary) {
          inner_ir.facts.dictionary = true;
          inner_ir.facts.dictionary_index_kind = outer->index_kind;
          inner_ir.facts.dictionary_ordered = outer->ordered;
      } else if (inner_ir.facts.dictionary_index_kind != outer->index_kind ||
                 inner_ir.facts.dictionary_ordered != outer->ordered) {
          // Conflict policy (see below): leaf wins, and say so.
          AppendWarning(inner_ir.facts, "(fletcher.dictionary) on flatten wrapper '" +
                        field->name() + "' overridden by the inner field's own declaration");
      }
  }
  ```

  **Conflict policy (decision, R7).** A wrapper and its inlined leaf may both carry
  the option. **Leaf wins**, accepted as *valid* — it mirrors `ForField`'s "most
  specific declaration wins" and it is the only choice that keeps a shared-library
  wrapper from dictating a consumer's column — and a *disagreeing* pair records a
  `facts.warning`. `AppendWarning` sets the warning when empty and appends with
  `"; "` otherwise, so it cannot clobber the flatten/nesting warnings. No existing
  fixture declares the option at all, so no golden can move.

  **Correction (step-2 re-review, cycle 2): the warning is currently a CARRIER,
  not user-visible output.** `facts.warning` is copied into `FieldMapping.warning`
  by `ProjectIrToFieldMapping` (`type_mapper.cpp:159` for `SCALAR`), but **no
  emitter reads it any more**: GIR-5 dropped the per-field warning comments, and
  says so at `generator.cpp:816-824` ("…dropped per-field warning comments"). A
  whole-`protoc/` search finds no consumer, so the header comment at
  `type_mapper.hpp:42` ("Non-empty → emit as a comment in generated code") is
  **stale post-GIR**. The decision stands on its own merits — the resolution is
  deterministic, the shape is pathological, and `facts.warning` is still the right
  carrier — but do NOT claim the user sees it. Consequently the conflict is
  *silent to the user* in DICT-1; if that is unacceptable, escalating a disagreeing
  pair to an error is DICT-2's call (it has the channel). Restoring warning
  rendering is out of scope here and is not a DICT deliverable.

* **Known, documented consequence:** for a `repeated` field, `BaseFacts(field)` is
  called for both the `LIST` node and its element node with the *same* descriptor,
  so `facts.dictionary` is true on both. That is correct — presence is a
  field-level fact — and harmless in DICT-1 (nothing reads it yet). **DICT-2 must
  gate on the top-level node's kind** (`NodeKind::SCALAR`), not on "some node has
  dictionary=true". `BuildFlattenedRepeated` needs no propagation for the same
  reason: DICT-2 rejects it as non-`SCALAR`.
* **WKT wrappers work already.** `TryBuildWkt` (`ir.cpp:235-244`) builds its node
  from `BaseFacts(field)` and keeps `NodeKind::SCALAR`, so a
  `google.protobuf.StringValue` field carrying the option gets populated facts on a
  nullable scalar node — locked #9's "wrapper is a valid nullable dictionary" is
  therefore satisfied by DICT-1 with no extra code. DICT-2's forcing test depends
  on it, so DICT-1 pins it with a sub-case.

### D4b — The `flatten_field` hole: a second drop site, owned by DICT-2 (R2)

`BuildFlattenedSingular` handles **message-level** `(fletcher.flatten)` only
(`ir.cpp:458`). **Field-level** `(fletcher.flatten_field)` is expanded elsewhere,
by walks that `continue` past the wrapper field so the wrapper's own IR is
**never built**:

| Site | Role | Behaviour |
|---|---|---|
| `cpp_backend_schema_visitor.cpp:66-109` (`BuildFlattenedFieldListImpl`) | schema field list | recurses into `fd->message_type()` and `continue`s at line 89 — the wrapper `fd` never reaches `BuildFieldIr` |
| `generator.cpp:598-610` (`GatherFieldsImpl`) | row/edge field list | same shape, `continue` at line 609 |
| `generator.cpp:129-142` (`CollectCrossFileEnumIncludesFromMessage`) | header collection | same walk shape; **not** a drop site for the option (it collects includes only) — listed so the "descend exactly as the field walk does" family is complete |

Consequence: `[(fletcher.flatten_field) = true, (fletcher.dictionary) = {...}]` on a
wrapper field is **silently ignored today** — no node carries the fact, so DICT-2's
`FieldKind`-based rejection can never fire either. Per spec §4 a singular struct
message field carrying the option must be **rejected**, so the silence is also
spec-non-conformant. This is a known hazard of that walk shape, not a one-off:
GIR-13 fixed the *same* walk for dropping wrapper context (hence the
`flatten_chain` threading and the ordering comment at
`cpp_backend_schema_visitor.cpp:80-85`).

Also note the earlier draft's citation of `ForField`'s leaf-first scan as precedent
for D4's propagation was loose: that scan **is** the `flatten_field` chain
(`option_metadata.cpp:485-491` fed from `cpp_backend_schema_visitor.cpp:86`), not
the message-level flatten path. The precedent for *precedence* still stands; the
precedent for *propagation* does not, which is exactly why the two flatten flavours
get different answers below.

**Decision and ownership:**

* **Intended semantics: reject.** Unlike message-level flatten (which resolves to
  exactly *one* inner field, so propagation is well-defined), a `flatten_field`
  wrapper inlines **N** columns — there is no single column to dictionary-encode,
  and silently applying it to all N would be a fabricated schema. Reject with a
  clear reason, consistent with spec §4's treatment of a struct message field.
* **Owner: DICT-2**, which has the error channel. **It must be a front-end
  descriptor walk, NOT a `MapField` / `ProjectIrToFieldMapping` rejection** — that
  projection is never invoked for a wrapper field at all (`GatherFieldsImpl`
  `continue`s at `generator.cpp:609`, *before* the `BuildFieldIr` +
  `ProjectIrToFieldMapping` at `:614-615`), so an `UnsupportedReason`-style check
  there can never fire. The actionable shape: a validation pass (the
  `ValidateNoUnsupportedIr` / `ValidateBackendsSupportFields` family in
  `generator.cpp`, or a check at the two `continue` sites themselves) that, for
  each field with `HasFieldFlatten(fd)`, errors when `HasFieldDictionary(fd)`.
  (This is also why `HasFieldDictionary`'s descriptor-based API is kept in D3
  despite D4 preferring IR facts.)
* **DICT-1 pins today's behaviour with a sub-case** so the hole is a test, not a
  note: a wrapper field carrying both options yields no dictionary anywhere in the
  emitted field list, while `HasFieldDictionary(wrapper) == true`. When DICT-2 adds
  the rejection it flips that sub-case deliberately, with the diff visible.
* This adds no new deferral outside the round (locked #11's constraint): the owner
  is DICT-2, inside DICT.

**Hand-off, explicitly:**

* **DICT-1 (this item):** surface + spec §7 + `ReadFieldDictionaryOption` /
  `HasFieldDictionary` + `ir::FieldFacts` population + message-level-flatten
  propagation. Nothing else consumes it yet.
* **DICT-1.5:** `generator.cpp`'s existing `ValidateBackendsSupportFields` pass.
  It should reason over the **IR facts**, not raw descriptors — a raw
  `HasFieldDictionary(top_level_field)` scan misses a dictionary declared on the
  *inner* field of a message-level flatten wrapper, which is a real reachable shape
  after D4. (For the `flatten_field` shape it needs the descriptor walk of D4b;
  until DICT-2 rejects that shape, DICT-1.5 should treat it the same way — a
  dictionary anywhere in the message blocks `accessor`/`rust`.)
* **DICT-2:** `ProjectIrToFieldMapping` reads `node.facts.dictionary` /
  `.dictionary_index_kind` and projects onto the new `FieldMapping.is_dictionary` /
  `dict_index_type_expr`; rejects non-`SCALAR` kinds, rejects
  `facts.dictionary_ordered == true`, **and closes the D4b `flatten_field` hole**.
  FieldMapping stays a projection.
* **DICT-3:** the single `cpp_backend::SchemaVisitor` branches on
  `facts.dictionary`; the `DictionaryIndexKind → "arrow::int16()"` /
  nanoarrow-type mapping belongs in `cpp_backend_type_table` (GIR locked #1: no
  backend strings on IR nodes) and must **not** go through `LookupScalar`.

### D5 — Lifetime, cost and concurrency contract

* **No process-global cache and no `static` `DynamicMessageFactory`.** Both would
  be keyed on `Descriptor*`/`Message*` addresses and would hold raw pointers into
  pools they do not own; the protoc unit suites build and destroy many
  `DescriptorPool`s, so stale-address reuse and use-after-free are both reachable.
  The factory is therefore **function-local per call, declared before the message
  it creates**, and all values are copied out before it dies. This is the one
  non-obvious rule in this item — state it in a code comment.
* Cost: `BaseFacts` runs once per node and `BuildFieldIr` runs several times per
  field across emitters, so the step-1 fast path (`ByteSizeLong() == 0`) is
  load-bearing: only fields that carry *some* option pay for a `DynamicMessage`
  construction (one `FieldOptions` prototype plus sub-message prototypes), which is
  microseconds at codegen scale. If this ever shows up in a profile the fix is a
  per-generation reader object threaded from `generator.cpp`, **not** a global
  cache.
* Concurrency: none required, and the reader is **not** quite a pure function —
  `ByteSizeLong()` and `SerializeAsString()` update the *cached size* on
  `field->options()`, which for an options-less field is protobuf's **shared
  default instance**. The write is benign (same value every time; protobuf itself
  relies on this) but it is a write, so the honest guarantee is the plugin's
  single-threadedness (one `CodeGeneratorRequest`, sequential `Generate`), not
  purity. Nothing else in the reader is shared or mutable.

### D6 — Scope guard (hard)

Untouched by DICT-1, and any diff there is a stop-and-ask:
`protoc/src/recordbatch_accessor_emitter.*` (RBA, read-only until RIR — GIR locked
#3), all Rust accessor emission, `type_mapper.hpp`/`.cpp`, `cpp_backend_*`
(DICT-3), `generator.cpp` (DICT-1.5), and every runtime component (`arrow-bridge`,
`pubsub-arrow` — locked #6). No `FieldKind` member is added (locked #5).
`FindBoolOption`/flatten reading is unchanged.

---

## Forcing-test mapping

Forcing test: **`TypeMapperTest.ReadsDictionaryOption`** in
`protoc/tests/test_type_mapper.cpp` — one `TEST` with `SCOPED_TRACE`d sub-cases.
The suite name is retained verbatim for tracker fidelity (it is the round's named
forcing test); the code actually under test is `option_reader` + `ir`, since
`type_mapper.*` is untouched. Say that in a comment above the test so the next
reader is not confused.

**Two harness routes are needed; both are existing in-tree patterns.**

1. **Source-text compilation — the realistic route.** Reuse the fixture at
   `test_option_metadata.cpp:155-195`: a `DescriptorPool` seeded with the linked-in
   `descriptor.proto` (`DescriptorProto::descriptor()->file()->CopyTo`) plus
   in-process compilation of `.proto` **source text** via `io::Tokenizer` +
   `compiler::Parser` + `pool.BuildFile`. This reproduces production exactly —
   `BuildFile`'s option interpreter resolves `(fletcher.dictionary)` against the
   pool and stores it in the linked `FieldOptions`' `UnknownFieldSet`, because the
   C++ class does not know the extension. Extract it into a **header-only** test
   helper `protoc/tests/proto_text_pool.hpp` and add:

   ```cpp
   // Compiles the SHIPPED protoc/include/fletcher/options.proto TEXT (path from the
   // FLETCHER_OPTIONS_PROTO_DIR compile definition, mirroring SCHEMA_GOLDEN_DIR)
   // under the name "fletcher/options.proto", so test protos can
   // `import "fletcher/options.proto"`.
   inline void AddFletcherOptions(ProtoTextPool& pool);
   ```

   Compiling the **shipped** file (rather than an inline copy) makes the forcing
   test also assert spec §2 and locked #2 — a wrong extension number, a renamed
   sub-field or a renumbered enum value fails it. Only `descriptor.proto` is
   imported, so no `Importer`/`DiskSourceTree` is needed.

   *Two traps to respect:*
   * **Everything in `proto_text_pool.hpp` must be `inline`** (header-only).
     Otherwise the later optional refactor of `test_option_metadata.cpp` onto the
     shared header duplicates symbols in the single
     `fletcher_proto_plugin_tests` binary.
   * **Never build the shipped `options.proto` into the same pool as a fixture that
     declares its own 50000 `FieldOptions` extension** —
     `test_option_metadata.cpp:256` and `test_schema_visitor.cpp:456` both do, and
     `BuildFile` fails on a duplicate extension number. Sharing the *header* is
     fine; sharing a *pool* is not. Each test constructs its own pool (as the
     existing `test_type_mapper.cpp` helpers already do).

   *Sanctioned fallback if reading from the source dir proves CI-fragile:* an inline
   copy of the option definitions — but then note in the test that it no longer pins
   50001, and add a separate assertion that the shipped text declares
   `dictionary = 50001`.

2. **Unknown-field injection — for bytes source text cannot express.** The
   hand-encoded, undeclared-enum-number, truncated and wrong-wire-type sub-cases
   are not expressible as `.proto` source. The in-tree pattern that *is* is
   `test_ir.cpp:56-61`: mutate the `FieldDescriptorProto`'s options **before**
   `BuildFile` —
   ```cpp
   auto* opts = f->mutable_options();
   opts->GetReflection()->MutableUnknownFields(opts)
       ->AddLengthDelimited(50001, std::string("\x08\x02", 2));
   ```
   `DescriptorBuilder` copies options wholesale, so the injected unknown fields
   survive onto `FieldDescriptor::options()` — exactly how `HasMessageFlatten` sees
   `(fletcher.flatten)` in `test_ir.cpp`. Name this route explicitly in the test.
   For the *typed* sub-cases the pool must **also** contain `fletcher/options.proto`
   (route 1) so step 2's gate passes; for the "extension not in pool" sub-case it
   must deliberately **not**.

| Required sub-case (from the story) | Route | Design element that turns it green |
|---|---|---|
| hand-encoded `08 02` on #50001 → `{INT16, false}` | 2 (+1 for the pool) | D3 steps 2–4: the pool knows the extension, so the re-parse surfaces injected bytes as a real extension; enum number 2 → symbol `DICTIONARY_INDEX_INT16` → `INT16` |
| **`ordered=true`** (`10 01`, and `08 02 10 01`) → `ordered == true` | 2 and 1 | D1's proto2 `optional bool ordered = 2` (explicit presence) + D3 step 4's name-keyed `ordered` decode. Also asserted in source-text form (`[(fletcher.dictionary) = { ordered: true }]`), which is what a user actually writes |
| absence → no dictionary | 1 | D3 step 1 (options empty → `nullopt`) and step 4 (`HasField(ext)` false → `nullopt`); `HasFieldDictionary == false` |
| malformed / empty payload → defaults (int32, unordered) | 2 / 1 | Empty payload (`= {}`): step 4 presence + both sub-fields absent → defaults. Malformed (`08`): outer re-parse fails → **narrowed** step 5 probe → `DictionaryOption{}` |
| index round-trip `{unspecified→int32, int8, int16, int32, int64}` | 1 | D3 step 4's **symbol-name** map, driven from source text using the real enum names — which also pins D1's enum member set |

Additional sub-cases required by this design (class, not instance):

| Sub-case | Route | Design element |
|---|---|---|
| undeclared enum number (`08 09`) → present, defaults | 2 | closed proto2 enum → unknown field → sub-field absent → `INT32` |
| truncated payload (`08`) → present, defaults | 2 | narrowed step 5 |
| **varint** at #50001 → `nullopt` | 2 (`AddVarint`) | wire-type mismatch on a known field → step 4 `HasField` false; step 5 not reached |
| **extension NOT in the pool**, valid bytes at #50001 → **`nullopt`** | 2 without route 1 | D3 step 2 rejects; step 5 unreachable (R1) |
| **foreign** message-typed option at #50001 in a pool without `fletcher/options.proto` → `nullopt` | 2 without route 1 | the no-false-positive assertion (R1) |
| `ir::BuildFieldIr(field).facts.{dictionary, dictionary_index_kind, dictionary_ordered}` match the reader; a non-annotated sibling stays `false`/`INT32` | 1 | D4 `BaseFacts` population (and keeps `test_ir.cpp:232` green) |
| message-level `(fletcher.flatten)` wrapper: option on the **wrapper field** reaches the resolved inner node | 1 | D4 `BuildFlattenedSingular` propagation |
| …option on the **inner** field also works | 1 | inner field's own `BaseFacts` |
| …**both**, with **different** index types (wrapper `INT64`, leaf `INT16`) → result is `INT16`, **and** `inner_ir.facts.warning` mentions the override (an IR-fact assertion — the warning is not rendered anywhere today, see D4's correction) | 1 | D4 leaf-wins + conflict policy (R7 — distinct types make it observable) |
| `(fletcher.flatten_field)` wrapper carrying the option → no record produced by `cpp_backend::BuildFlattenedFieldList(msg)` (public, `cpp_backend_schema_visitor.hpp:72`) has `node->facts.dictionary`, while `HasFieldDictionary(wrapper) == true` | 1 | pins the D4b hole as a test; DICT-2 flips it. Assert at the **field-record/IR** level, not on generated text — DICT-1 emits nothing dictionary-shaped, so a text assertion would be vacuous |
| **scalar** field carrying both `flatten_field` and `dictionary` → dictionary applies | 1 | `HasFieldFlatten` requires `TYPE_MESSAGE` (`type_mapper.cpp:308`), so flatten is a no-op — spec §4's "document, do not error" |
| **WKT wrapper** (`google.protobuf.StringValue`) with the option → facts populated, node stays `SCALAR`, `nullable` | 1 | `TryBuildWkt` uses `BaseFacts` (`ir.cpp:235-244`); pins locked #9 for DICT-2 |
| node stays `NodeKind::SCALAR` (no container peer) | 1 | nothing structural changes; locked #5 / GIR locked #7 — mirrors `test_ir.cpp:217-232` |

No-drift proof obligations: `test_option_metadata.cpp` green **unchanged** (D2
refactor), `test_ir.cpp` green unchanged, and the RBA/accessor golden suites
untouched (D6).

---

## Risks / Unknowns

1. **`ByteSizeLong()` as the fast-path gate.** It counts unknown fields, so a field
   carrying *any* option (including `flatten_field` or a third-party one) falls
   through to the full path — correct, just not free. It also writes the shared
   default instance's cached size (D5); benign, and called out rather than papered
   over.
2. **Fail-soft vs. hard error on a corrupt payload.** The story's acceptance
   mandates defaults and `BuildFieldIr` has no error channel, so defaults it is.
   The residual risk is a user who writes `index_type: INT64` and gets int32 from a
   corrupt blob — unreachable from protoc-compiled input. If loud failure is
   preferred, the home is DICT-2 (which has the channel); it would not change this
   item's API.
3. **The narrowed probe trades one silence for another.** After R1, an option whose
   *declaration* is missing from the pool reads as absent. That is the right
   trade — a false positive fabricates a schema, a false negative for
   un-importable input cannot occur for genuine Fletcher protos (Risk 8) — but it
   is a deliberate asymmetry, recorded so DICT-2 does not "fix" it back.
4. **Locked #5 names `ScalarFacts.dictionary`; the real type is
   `ir::FieldFacts.dictionary`.** Treated as a naming correction (D4), not a
   deviation — intent, carrier semantics and the "projection, not second source of
   truth" rule are all honoured.
5. **Two new `FieldFacts` fields.** `dictionary_index_kind` / `dictionary_ordered`
   sit beside the locked-named `bool dictionary` instead of folding into a
   `DictionaryFacts` sub-struct, because locked #5 and `test_ir.cpp` both name
   `facts.dictionary`. A later tidy could fold all three; doing it here would churn
   a locked name.
6. **`ir::DictionaryModifier` (ir.hpp:103-106) stays dead.** Deliberate: using it
   as well as `bool dictionary` would create two presence representations. Removing
   it is a one-line cleanup for DICT-2 or RIR, not DICT-1.
7. **Touching `option_metadata.cpp` (a just-landed PR #121 file).** Mitigated by
   keeping the change to a mechanical delegation that preserves the cache and the
   member order (D2), with `test_option_metadata.cpp` as the regression gate. The
   alternative — copying the round-trip into `option_reader.cpp` — was rejected: it
   is precisely the duplication locked #10 tells us to avoid.
8. **Assumption: the plugin's pool always contains `fletcher/options.proto` when
   the option is used.** True by construction (a `.proto` cannot name
   `(fletcher.dictionary)` without importing the file, and protoc ships the whole
   transitive `FileDescriptorSet` in the `CodeGeneratorRequest`).
9. **The D4b hole is left open by DICT-1 on purpose.** A wrapper field with
   `flatten_field` + `dictionary` stays silently ignored until DICT-2. The risk is
   that DICT-2 forgets; mitigated by the pinning sub-case (a test that DICT-2 must
   consciously flip) plus the hand-off entry. If DICT-2 is descoped, this becomes a
   spec-§4 non-conformance to re-home — not a new out-of-round deferral.
10. **Test harness fragility (see the memory note on CI protoc/env gotchas).**
    Reading the shipped `options.proto` from the source dir follows the existing
    `SCHEMA_GOLDEN_DIR` precedent, so the risk is low; the inline-copy fallback is
    pre-authorised. The duplicate-50000 pool trap is the likelier stumble and is
    called out in the harness section.

**STOP-AND-ASK: none.** No locked decision is in tension, no public API breaks, and
no major version bump is implied. Locked #10's own text authorises the mechanism
switch made in D0 (and therefore the spec §7 amendment); the two file-placement
deviations (reader in `option_reader.hpp` rather than `type_mapper.hpp`; index kind
on `FieldFacts`) are from the *pre-GIR story text*, which the plan's own re-plan
note already declares partly dead, and both are recorded with reasons in D3/D4.

---

## Files-to-touch

**Modify**

| Path | Change |
|---|---|
| `protoc/include/fletcher/options.proto` | + `DictionaryIndexType`, `DictionaryOptions`, `extend FieldOptions { dictionary = 50001; }` (D1) |
| `docs/dictionary-option-spec.md` | **amend §7** to the reflection/`DynamicMessage` mechanism, the narrowed probe and the fail-soft contract; restate that `options.pb.cc` is still not linked (D0, R3) |
| `protoc/include/ir.hpp` | + `enum class DictionaryIndexKind`; + `FieldFacts::dictionary_index_kind`, `::dictionary_ordered`; extend the `FieldFacts` doc comment (D4) |
| `protoc/src/ir.cpp` | `BaseFacts` populates the three dictionary facts; `BuildFlattenedSingular` propagates the outer field's option (leaf wins, conflict warned); `#include "option_reader.hpp"` (D4) |
| `protoc/src/option_metadata.cpp` | `Impl::ParsedOptions` delegates to `ReparseOptionsWithPool` (cache hit path retained); + the factory-before-cache member-order comment (D2) |
| `protoc/CMakeLists.txt` | + `src/option_reader.cpp` to `fletcher_plugin_core` |
| `protoc/tests/CMakeLists.txt` | + `FLETCHER_OPTIONS_PROTO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../include"` compile definition (mirrors `SCHEMA_GOLDEN_DIR`) |
| `protoc/tests/test_type_mapper.cpp` | + `TypeMapperTest.ReadsDictionaryOption` and its sub-cases (both harness routes) |
| `docs/fletcher-options.md` | + the `50001 / FieldOptions / dictionary` registry row only (locked #2); prose section is DICT-5's |

**Add**

| Path | Contents |
|---|---|
| `protoc/include/option_reader.hpp` | `ReparseOptionsWithPool`, `DictionaryOption`, `ReadFieldDictionaryOption`, `HasFieldDictionary` (D2/D3) |
| `protoc/src/option_reader.cpp` | implementations, incl. the narrowed length-delimited presence probe and the per-call factory-before-message rule |
| `protoc/tests/proto_text_pool.hpp` | test-only, **all `inline`**: source-text `DescriptorPool` helper + `AddFletcherOptions` (extracted from `test_option_metadata.cpp`'s fixture) |

New files must carry the SPDX/copyright header used across the tree and satisfy the
whole-tree clang-format 18.1.3 and header scans, or PR-CI bounces on an env-only
failure (see the CI-gotchas memory).

**Explicitly NOT touched:** `protoc/src/recordbatch_accessor_emitter.*`, any Rust
accessor emission, `protoc/include/type_mapper.hpp`, `protoc/src/type_mapper.cpp`,
`protoc/src/generator.cpp`, `protoc/src/cpp_backend_*`, `arrow-bridge/*`,
`pubsub-arrow/*`.

---

## Step-2 review (2026-08-28)

**Verdict: NEEDS-REWORK.** No locked-decision deviation, no stop-and-ask. The
mechanism choice (D0/D2) is sound and verified against the tree; three findings
are load-bearing (R1 opens a silent false-positive channel and is internally
inconsistent with D3 step 2; R2 leaves half the "wrapper field" class unsolved on
a path the design does not mention; R3 is an un-flagged design-vs-spec conflict).
The rest are required clarifications that cost little and remove real traps.

### Confirmed against the tree (no change needed)

* **Mechanism, (a) message-typed options.** `option_metadata.cpp:276-282` already
  reads a *message-typed* `FieldOptions` extension through the re-parse
  (`GetMessage(*cur_opts, st.ext, &factory)` then descends sub-fields by
  descriptor), and the fixture at `test_option_metadata.cpp:199-260` declares
  exactly that shape (`extend google.protobuf.FieldOptions { ColOpts col = 60100; }`
  plus `EncOpts`/`TypOpts` message extensions). The design's claim that #121's
  mechanism covers a nested message option — not just scalar/enum — holds.
* **Mechanism, (c) locked #10.** Nothing in D2/D3 links `options.pb.cc`; the
  extension is reached only via `pool->FindExtensionByName` + reflection. Holds.
* **Locked #5 correction is coherent.** `ScalarFacts` does not exist;
  `ir::FieldFacts.dictionary` is real (`ir.hpp:140`), its doc comment
  (`ir.hpp:129-132`) already declares itself the single canonical home of
  `nullable` **and** `dictionary`, and `test_ir.cpp:217-232` is exactly as cited.
  D4's reading of locked #5 is right, and the two added facts fields are the
  correct way to avoid a second source of truth in `FieldMapping`.
* **Nothing consumes `facts.dictionary` yet.** A whole-`protoc/` search finds it
  only in `ir.hpp` and `test_ir.cpp` — so D4's population cannot move any golden,
  and `ir::DictionaryModifier` really is dead. "No drift" is verifiable.
* **Per-call `DynamicMessageFactory` (D5) — hazard is real, cost acceptable.** A
  `static` factory caches prototypes keyed on `const Descriptor*` and keeps raw
  pointers into a pool it does not own; the protoc unit suites build and destroy
  many `DescriptorPool`s, so both stale-address reuse and use-after-free are
  reachable. Per-call is the right call. Cost: only option-carrying fields pay,
  and each payment is one `FieldOptions` prototype (plus its sub-message
  prototypes) — fine at codegen scale. Keep the step-1 gate.
* **Scope (D6).** `recordbatch_accessor_emitter.*` and Rust emission are untouched
  and cannot drift: they consume the flat `FieldMapping`, which DICT-1 does not
  change. `50001` is genuinely free — `protoc/include/fletcher/options.proto`
  declares only `flatten = 50000` (MessageOptions) and `flatten_field = 50000`
  (FieldOptions); no in-repo fixture uses 50001 (the test fixtures sit at 60100+).
* **`type_mapper.hpp` placement deviation is justified.** `type_mapper.hpp:13-15`
  really does only forward-declare `ir::IrNode`, so returning
  `ir::DictionaryIndexKind` from there would force an `ir.hpp` include. Accepted.
* **Test-pool harness precedent is real.** `test_option_metadata.cpp:155-195` is as
  described, and `protoc/tests/CMakeLists.txt:21-22` has the `SCHEMA_GOLDEN_DIR`
  compile-definition precedent `FLETCHER_OPTIONS_PROTO_DIR` would mirror.
* **Presence probe is NOT a vestige (question 4).** Verified by construction: a
  malformed inner payload (e.g. `08`) is a *known* message-typed extension after
  step 2, so protobuf parses the length-delimited range as a `DictionaryOptions`
  and the truncated varint fails the **whole** `ParseFromString` → step 3 returns
  `nullptr`. Without a probe the reader would answer `nullopt`, i.e. silently drop
  a declared dictionary — which contradicts the story's acceptance ("a
  malformed/empty payload resolves to defaults (int32, ordered=false)"). So the
  fail-soft contract *and* the probe are required by acceptance, not preference.
  (This is why R1 narrows the probe rather than removing it.)

### Required changes

1. **Narrow the D3 step-5 presence probe to the "extension known, blob
   unreadable" case only — as written it is a silent false-positive channel and
   contradicts step 2.** Step 5 is currently reached from two very different
   states: (a) `FindExtensionByName("fletcher.dictionary")` failed or the gate
   mismatched, and (b) the re-parse failed. In state (a) the reader has *no*
   declaration backing the bytes, yet the probe trusts the bare number 50001 —
   weaker evidence than the declaration step 2 just refused to trust. Concrete
   failure: a pool that does **not** import `fletcher/options.proto` but carries
   some third-party message-typed `FieldOptions` extension at 50001 (50000/50001
   are in the collision-prone internal range, and PR #121 exists precisely to
   serve pools full of foreign options) makes that field a **dictionary column**
   nobody declared. The design's own Risk 7 argues state (a) is unreachable for
   genuine Fletcher input — so its only live effect is the false positive.
   Required:
   * Run the probe **only** when the extension resolved and passed the gate but
     `ReparseOptionsWithPool` returned `nullptr`. Extension-not-in-pool →
     `nullopt`.
   * Correspondingly **relax the gate** so a compatible option is read rather
     than degraded: keep `ext != nullptr`, `!is_repeated()`,
     `containing_type()->full_name() == "google.protobuf.FieldOptions"`,
     `number() == 50001`, `cpp_type() == CPPTYPE_MESSAGE`; **drop** the
     `message_type()->full_name() == "fletcher.DictionaryOptions"` requirement,
     which today converts a renamed-but-compatible message into silent defaults
     even though the by-name sub-field decode would have read it correctly.
   * Fix the two sub-case rows: "valid bytes, extension **not** in the pool" must
     expect **`nullopt`**, not "present, defaults"; add a new sub-case "foreign
     length-delimited option at #50001 with `fletcher/options.proto` **not**
     imported → `nullopt`" (the no-false-positive assertion).
   * State the residual limit explicitly: a fork that *renumbers* the extension
     away from 50001 is not readable and is not Fletcher's option.

2. **The "wrapper field" class is only half-solved: field-level
   `(fletcher.flatten_field)` is a second, unmentioned drop site.** The claim in
   D4 about `BuildFlattenedSingular` (`ir.cpp:261-276`) is **real and correctly
   diagnosed** — it returns `inner_ir` whose facts come from `inner`, and only
   `nullable` is propagated (line 274), so a wrapper-declared option would vanish.
   But that function serves *message-level* `(fletcher.flatten)` only
   (`ir.cpp:458`). **Field-level** flatten is expanded somewhere else entirely:
   `cpp_backend_schema_visitor.cpp:66-109` (`BuildFlattenedFieldListImpl`, which
   `continue`s past the wrapper and calls `ir::BuildFieldIr(fd)` on the *inner*
   fields), plus `generator.cpp:139` and `generator.cpp:607`. So
   `[(fletcher.flatten_field) = true, (fletcher.dictionary) = {...}]` on a wrapper
   field is **silently ignored** — the wrapper never reaches `BuildFieldIr`, so
   DICT-2's `FieldKind`-based rejection never fires either. Per spec §4 a singular
   struct message field carrying the option must be **rejected**, so today's
   behaviour is also spec-non-conformant, and the design's citation of
   `OptionMetadataResolver::ForField`'s leaf-first scan as the precedent is
   incoherent given that scan *is* the `flatten_field` chain
   (`option_metadata.cpp:485-491`, `cpp_backend_schema_visitor.cpp:86`). Required:
   * Record the `flatten_field` hole explicitly in D4 and in the hand-off list,
     naming the two walk sites above.
   * Assign enforcement to a named later item (DICT-2 is the natural owner: it has
     the error channel) and state the check must walk **raw descriptors / the
     flatten chain**, not the projected IR field list, because the wrapper is
     inlined away before projection. Decide and record the intended semantics
     (recommended: reject — a wrapper inlining N columns has no single column to
     dictionary-encode).
   * Add a DICT-1 sub-case pinning today's observable behaviour for that shape, so
     the hole is a test, not a note. Also add the spec-§4 companion sub-case that
     *does* already work: a **scalar** field carrying both `flatten_field` and
     `dictionary` resolves to dictionary (`HasFieldFlatten` requires
     `TYPE_MESSAGE`, `type_mapper.cpp:308`), matching spec §4's "document, do not
     error".

3. **Un-flagged design-vs-spec conflict: `docs/dictionary-option-spec.md` §7 still
   mandates the unknown-field walker.** §7 reads "The reader must walk that
   payload's inner fields … this is done with a small unknown-field walker". D0
   retires it. Locked #10 (revisited) authorises the switch, so this is *not* a
   stop-and-ask — but the authoritative spec must not keep contradicting the
   implementation. Required: amend §7 in this item (add
   `docs/dictionary-option-spec.md` to Files-to-touch) to describe the
   reflection/`DynamicMessage` mechanism, the narrowed presence probe, and the
   fail-soft contract, and note that the "not linking `options.pb.cc`" property is
   preserved. Do not leave it to DICT-5.

4. **Decide and record how `index_type`'s enum *value* is decoded — the current
   text claims by-name forward compatibility but implements by-number.** D0 reason
   2 justifies the mechanism as "fields are located **by name** on the pool's
   descriptor"; D3 step 4 then maps the raw `GetEnumValue` **number**
   (`0→INT32, 1→INT8, …`). A consumer pinning a fork that renumbered the enum
   values would be misread *silently* (INT64 read as INT16, etc.). Required:
   either (preferred, and it matches the in-tree precedent at
   `option_metadata.cpp:99-106`) resolve
   `f->enum_type()->FindValueByNumber(n)->name()` and map the **symbol name**
   (`DICTIONARY_INDEX_INT16` → `INT16`), falling back to `INT32` for an unknown
   name; or keep the number map and **delete the by-name forward-compat claim**,
   recording instead that spec §2's enum numbering is a hard contract pinned by
   the forcing test. Either way the sub-case table must say which.

5. **Pin the hand-encoded-bytes test mechanism — four of the design's own
   sub-cases are unreachable through the described harness.** `ProtoTextPool` +
   `AddFletcherOptions` compiles `.proto` *source text*, which cannot express
   `08 09`, a truncated `08`, a varint at #50001, or the extension-absent pool.
   The in-tree pattern that can is `test_ir.cpp:56-61`: inject into the
   `FileDescriptorProto` before `BuildFile` via
   `opts->GetReflection()->MutableUnknownFields(opts)->AddVarint(...)` /
   `AddLengthDelimited(50001, "\x08\x02")` — `DescriptorBuilder` copies options
   wholesale, so the unknown fields survive onto `FieldDescriptor::options()`
   (this is exactly how `HasMessageFlatten` sees `(fletcher.flatten)` in
   `test_ir.cpp`). Name that mechanism in the harness section, and note two
   traps: (a) `proto_text_pool.hpp` must be header-only `inline` — otherwise
   including it from `test_option_metadata.cpp` later (the "optional refactor")
   duplicates symbols in the single `fletcher_proto_plugin_tests` binary; (b) the
   shipped `options.proto` declares `flatten_field = 50000`, so its file must
   never be built into the **same pool** as the existing fixtures that declare
   their own 50000 on `FieldOptions` (`test_option_metadata.cpp:256`,
   `test_schema_visitor.cpp:456`) — `BuildFile` fails on a duplicate extension
   number. Sharing the header is fine; sharing a pool is not.

6. **State the factory/message destruction-order invariant as a rule the refactor
   must not break, and show the full delegated body.** A message created from a
   `DynamicMessageFactory` prototype must be destroyed **before** the factory. In
   `OptionMetadataResolver::Impl` this holds today only because `factory`
   (`option_metadata.cpp:241`) is declared **before** `cache`
   (`option_metadata.cpp:245`); moving either member is silent UB, and D2 is a
   diff to that struct's neighbourhood. Required: (a) add that member-order note
   as a code comment in `option_metadata.cpp`; (b) in D3 step 3 state that the
   per-call `factory` must be declared before the returned message; (c) show the
   *complete* replacement body for `ParsedOptions`, including the retained
   `cache.find(&opts)` hit path — the current ≈5-line snippet starts after it and
   invites dropping the cache, which would be a #121 behaviour change (it is what
   makes the shared default options instance parse once).

7. **Make "leaf wins" observable and decide the conflict policy.** The
   `BuildFlattenedSingular` sub-case "both → inner wins" is untestable if both
   declarations are `{}`. Required: the test must declare **different** index
   types on wrapper and leaf (e.g. wrapper `INT64`, leaf `INT16`) and assert
   `INT16`; and D4 must state whether a *conflicting* pair is acceptable silently
   (defensible, mirrors `ForField`) or should be flagged by DICT-2 — record the
   decision either way, since a shared library wrapper can now override a
   consumer's outer annotation.

8. **Correct the D5 concurrency sentence.** "the reader is a pure function of its
   argument with no shared mutable state" is not quite true: `ByteSizeLong()` and
   `SerializeAsString()` write the cached size on `field->options()`, which for an
   options-less field is the **shared default instance**. The race is benign
   (same value, and protobuf itself relies on it), but say that rather than
   claiming purity — the plugin's single-threadedness is the real guarantee.

### Non-blocking notes

* The forcing test keeps the tracker's name `TypeMapperTest.ReadsDictionaryOption`
  while `type_mapper.*` is untouched. Keeping the locked name is right; add one
  line saying the suite name is retained for tracker fidelity and the code under
  test is `option_reader` + `ir`.
* Add a sub-case for a **WKT wrapper** field (`google.protobuf.StringValue`) with
  the option: `TryBuildWkt` (`ir.cpp:235-244`) uses `BaseFacts(field)`, so the
  facts populate and the node stays `SCALAR` — locked #9's "wrapper is a valid
  nullable dictionary" is therefore already satisfied by DICT-1, and DICT-2's
  forcing test depends on it. Cheap to pin here.
* New files must carry the SPDX/copyright header and survive the whole-tree
  clang-format 18.1.3 / header scans (see the CI-gotchas memory) — worth one line
  in Files-to-touch so PR-CI does not bounce on an env-only failure.
* `HasFieldDictionary` builds a whole `DynamicMessage` just to answer a bool. Fine
  at this scale, but since D4 already argues DICT-1.5 should read IR facts
  instead, note in the header that the descriptor-based presence API is for
  callers that have no IR node — otherwise it will be reached for by default.

---

## Step-2 re-review, cycle 2 (2026-08-28)

**Verdict: APPROVE.** All eight required changes are genuinely discharged, not
merely acknowledged; each was re-verified against the tree rather than against the
resolution table. Three residual nits are already fixed inline above (D4 conflict
policy, D4b ownership bullet, two sub-case rows) — no further cycle needed.

**Re-verified, item by item**

* **R1 — discharged, and the two failure modes did not trade places.** Step 5 is
  now reachable only via step 3, which requires the step-2 gate to have passed,
  which requires `FindExtensionByName("fletcher.dictionary")` to resolve. A pool
  without `fletcher/options.proto` therefore returns `nullopt` regardless of what
  sits at 50001 → the false positive is gone. And it cannot reappear by another
  route: protobuf refuses two extensions of the same extendee at the same number in
  one pool, so once `fletcher.dictionary` resolves at 50001 no foreign extension
  can occupy it. The malformed path is intact: extension declared + injected
  `50001: "\x08"` → gate passes → the truncated submessage fails the whole
  `ParseFromString` → step 5 probes the untouched `UnknownFieldSet` → defaults.
  Acceptance ("malformed/empty payload resolves to defaults") still holds.
* **The new varint row is correct.** A wire-type mismatch on a *known* field is not
  a parse error in protobuf's reflection parser — `WireFormat::ParseAndMergeField`
  classifies it `UNKNOWN` and skips it into the `UnknownFieldSet` — so the parse
  succeeds, `HasField(ext)` is false, step 4 returns `nullopt`, and step 5 is never
  reached. The table and its note describe the real behaviour.
* **R2's third-walk exclusion is CORRECT.** `generator.cpp:129-149`
  (`CollectCrossFileEnumIncludesFromMessage`) inserts header paths into a
  `std::set<std::string>` and never touches `BuildFieldIr` or a field record, so it
  cannot drop the fact. The family is also **complete**: `HasFieldFlatten` has
  exactly three call sites in the tree — `generator.cpp:139`, `generator.cpp:607`,
  `cpp_backend_schema_visitor.cpp:79` — and all three are in D4b's table. The two
  real drop sites are confirmed: `GatherFieldsImpl` `continue`s at
  `generator.cpp:609`, *before* `BuildFieldIr`/`ProjectIrToFieldMapping` at
  `:614-615`; `BuildFlattenedFieldListImpl` `continue`s at
  `cpp_backend_schema_visitor.cpp:89`.
* **Deferring R2's fix to DICT-2 is sound, not a lossy hand-off.** DICT-1 leaves the
  wrapper case *detectably* wrong: `cpp_backend::BuildFlattenedFieldList` is public
  (`cpp_backend_schema_visitor.hpp:72`) and its records expose
  `node->facts.dictionary`, so the pinning sub-case is a real, non-vacuous
  assertion a different agent must consciously flip. Semantics (reject), owner
  (DICT-2, in-round per locked #11), the raw-descriptor requirement, and Risk 9's
  "if DICT-2 forgets" mitigation are all specific enough to act on.
* **R3** — spec §7 amendment is in D0 *and* in Files-to-touch. **R4** — decode is
  now by enum symbol name via `FindValueByNumber(...)->name()` with a null/unknown
  fallback to `INT32`, matching `option_metadata.cpp:99-106`, so D0 reason 2 is true
  as written. **R5** — both harness routes named, per-sub-case route column added,
  `inline` header and duplicate-50000 pool traps recorded; spot-checked that
  `test_type_mapper.cpp` really does build a fresh local `DescriptorPool` per test
  and declares no 50000 extension, so the trap does not fire in the forcing test's
  own file. **R6** — full `ParsedOptions` body with the retained `cache.find` hit,
  plus the factory-before-cache member-order rule. **R7** — distinct index types
  make leaf-wins observable. **R8** — purity claim replaced with the benign
  cached-size write plus single-threadedness.

**Residual nits (fixed inline above; no action beyond keeping the corrections)**

1. **The `facts.warning` channel was a void, and R7's justification claimed
   otherwise.** `facts.warning` → `FieldMapping.warning` still happens
   (`type_mapper.cpp:159`), but nothing renders it: GIR-5 removed the per-field
   warning comments and records that at `generator.cpp:816-824`, a whole-`protoc/`
   search finds no consumer, and `type_mapper.hpp:42`'s "emit as a comment" is
   stale. D4 now says so; the decision survives (deterministic resolution,
   pathological shape, right carrier) but the conflict is silent to the user in
   DICT-1, with escalation left to DICT-2. Restoring warning rendering is
   explicitly not a DICT deliverable.
2. **D4b named the wrong enforcement channel.** "`UnsupportedReason` /
   `MapField`-level rejection" cannot fire for a wrapper field, since the
   projection is never invoked for it. Rewritten to require a front-end descriptor
   walk (the `Validate*` family, or the two `continue` sites).
3. **The D4b pinning sub-case risked being vacuous.** "no dictionary in the emitted
   field list" has nothing to bite on in DICT-1 (no emitter consumes the fact yet);
   it now names `cpp_backend::BuildFlattenedFieldList` and
   `node->facts.dictionary`, which is assertable today.
