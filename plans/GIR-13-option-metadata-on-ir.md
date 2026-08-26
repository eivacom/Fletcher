# GIR-13 — Option metadata on the IR schema visitor

Reconciles PR #121 (`--fletcher_opt=metadata_from_option=...`, custom proto
options → Arrow schema metadata) with the IR schema visitor built in GIR-5.

**Status:** ⚪ not-started. This is the sole blocker between round GIR being
functionally complete (GIR-1..GIR-11 all 🟢) and `feature/generator-ir-rewrite`
being mergeable.

> **Why 13 and not 12.** `GIR-12` named the GeoArrow-CRS item, dropped in
> `b787893` as a domain concern (locked decision #6). Reusing the number would
> collide with live config: `.claude/runbook.GIR.config.md` still lists
> "GIR-12 removed 2026-07-10 per maintainer directive" under `out_of_scope`, so a
> `GIR-12` tracker row would contradict the config the runbook agents read.
> `GIR-12` stays retired.

## Why this exists

#121 landed on `main` on 2026-08-04, *after* `feature/generator-ir-rewrite`
branched at `5b36534` (2026-06-29). It threads an `OptionMetadataResolver`
through the **flat** generator — precisely the machinery GIR-5 deleted when it
unified schema emission and IPC building onto one IR visitor.

This is not a merge conflict to resolve. `git merge-tree` reports four
conflicted files, but two are mechanical (an include and a test-list union) and
the other two are the real work: the resolver has to be **re-threaded into the
IR schema visitor**, which is a different structure from the one #121 was
written against.

Nothing about #121's user-facing contract changes. `docs/fletcher-options.md`
stays authoritative and is not edited by this item.

## The two structures

**On `main`** — resolver threaded through two hand-synchronised copies:

```
SchemaMetadataPairs(msg, resolver)  -> builtins + resolver->ForMessage(msg)
FieldMetadataPairs(fi, resolver)    -> builtins + resolver->ForField(fi.descriptor,
                                                                    fi.flatten_chain)
        |                                   |
        v                                   v
EmitMetadataBlock()                 SetMetadataPairs()
  (C++ source text)                   (in-process nanoarrow)
```

`generator.cpp:1094-1097` carries the comment that these two "both consume the
identical pair vector, so key order and content cannot drift between the two
paths" — an invariant maintained **by hand**.

**On the GIR branch** — one visitor, two sinks:

```
SchemaVisitor
  :441  sink_.SetMetadata(root,  {{proto_package, ...}, {proto_message, ...}})
  :454  sink_.SetMetadata(child, {{field_number, ...}, {field_id, ...}})
        |
        +-- CppSchemaSink::SetMetadata       (:226, emits source text)
        +-- NanoarrowSchemaSink::SetMetadata (:301, executes in-process)
```

Both metadata call sites are hardcoded to the builtin pairs.

**This reconciliation strengthens #121's central invariant.** After GIR the two
paths agree *by construction* — one pair vector, one call site, two renderers —
so the hand-kept lockstep #121 documented becomes structural. That is the
argument for doing this on the IR rather than reverting GIR-5.

## Scope

### 1. Re-add the option plumbing (absent on the branch)

`metadata_rules` / `metadata_from_option` do not exist anywhere under `protoc/`
on this branch. Re-add:

- `ParseMetadataRules` into the `--fletcher_opt` token loop (`generator.cpp:1793`).
- `OptionMetadataResolver::Create` against `file->pool()` at the `GenerateFile`
  entry, mirroring `main`'s `generator.cpp:2983-2987`.
- Thread `const OptionMetadataResolver*` (nullable, default `nullptr`) into
  `GenerateFile` (`:1281`) and on into `SchemaVisitor`.

`protoc/include/option_metadata.hpp` and `protoc/src/option_metadata.cpp`
(652 lines together) port across **unchanged** — they are descriptor-level and
know nothing about emitters. Do not rewrite them.

### 2. Restore the `BuildMessageSchema` resolver parameter

GIR-5 dropped it. Restore
`BuildMessageSchema(msg, const OptionMetadataResolver* resolver = nullptr)` in
`schema_builder.hpp`; `test_schema_builder` and #121's `test_ipc_parity` both
depend on the parameter.

### 3. Thread the flatten chain through the visitor's own walk — the real work

`ForField(leaf, flatten_chain)` needs the outer→inner chain of
`(fletcher.flatten_field)` wrapper fields the leaf was inlined through. #121 got
this by adding `flatten_chain` to `FieldInfo`.

GIR-5 extracted its own flatten walk. `BuildFlattenedFieldListImpl`
(`cpp_backend_schema_visitor.cpp:63`) **discards the wrapper** — on a
field-level flatten it recurses into `fd->message_type()` and `continue`s
without recording `fd`:

```cpp
if (... && HasFieldFlatten(fd)) {
    BuildFlattenedFieldListImpl(fd->message_type(), out, path);
    continue;                    // <-- fd is unrecoverable from the leaf
}
```

So `SchemaFieldRecord` (declared in `cpp_backend_schema_visitor.hpp:44`) needs a
`flatten_chain` member, accumulated by that walk.
The change is structurally parallel to something the walk already does: it
threads `id_prefix` to build the dotted `field_id`, and the chain is exactly the
**descriptor-level counterpart of that same numeric path**. Thread the chain
next to the prefix.

`rec.source_field` already holds the leaf `FieldDescriptor*` (set in the
innermost recursion), so `ForField`'s first argument is available. Note its
existing comment says "the schema paths ignore it" — that stops being true and
should be updated.

### 4. Escape resolver-supplied bytes in the C++ sink — latent correctness bug

`CppSchemaSink::SetMetadata` (`:226`) writes keys and values **raw** into
`ArrowCharView("...")` string literals.

That is safe for the four builtin keys (package/message names, integers, dotted
paths — all ASCII-safe) but **not** for resolver output, which is arbitrary
option bytes, verbatim and uninterpreted by design. An embedded quote breaks the
generated header; an embedded NUL truncates the value. Caller-named `arrow_key`
needs it too — keys legitimately contain colons (e.g. `ARROW:extension:name`).

#121 already solved this: apply `EscapeCppStringLiteral` (`option_metadata.hpp`)
at this sink. Its octal-not-hex reasoning must be preserved verbatim — C++ hex
escapes consume an unbounded digit run, so a `0x01` byte followed by a literal
`A` would emit a two-character-looking escape and read back as one character.

Applying it uniformly to all pairs is preferred over branching on
builtin-vs-resolver: the builtins are already escape-invariant, so uniform
application is a no-op for them and removes a conditional that could rot.

### 5. Preserve ordering and reserved keys

Builtins first, resolver extras appended — at both call sites. The reserved-key
rejection (`proto_package` / `proto_message` / `field_number` / `field_id`) lives
in `ParseMetadataRules` and survives automatically, but the **append order** is
an emitter-side property and must be re-established in the visitor.

## Forcing tests

**The forcing tests already exist on `main`** — this item does not author new
red-first tests, it makes #121's suite pass against the IR emitter. Port and run:

| Suite | Size | Guards |
|---|---|---|
| `protoc/tests/test_option_metadata.cpp` | 32 tests | resolver semantics + all 8 `EscapeCppStringLiteralTest` cases |
| `integration-tests/.../test_metadata_options.cpp` | 176 lines | end-to-end option → metadata |
| `integration-tests/.../test_metadata_nodrift.cpp` | 243 lines | no-drift guard |
| `integration-tests/.../test_ipc_parity.cpp` | 34 lines | C++ path vs IPC path agreement |

The four that specifically force item 3 (flatten chain) — these fail if the
chain is dropped, which is the red-for-the-right-reason gate:

- `OptionMetadataTest.FlattenFieldWrapperContextReachesEachInlinedLeaf`
- `OptionMetadataTest.InlinedLeavesInheritTheFlattenFieldWrapper`
- `OptionMetadataTest.FlattenedWrapperFieldCarriesTheWrapperMessagesMetadata`
- `OptionMetadataTest.FieldScopeOverridesInheritedFieldTypeScopePerKey`

Forcing item 5: `OptionMetadataTest.BuiltinKeysSurviveAndMappedKeysAreAppended`,
`OptionMetadataTest.NullResolverEmitsExactlyTheFourBuiltinKeys`.

Plus, unchanged from GIR-5: `SchemaVisitor.CppAndIpcByteIdentical` must stay
green, now *with* a resolver active — the strongest single assertion that the
two-sink unification holds under option metadata.

### Golden re-baselining

GIR's 10 `.ipc` goldens (`protoc/tests/golden/*.ipc`) are generated **without**
`metadata_from_option`, so they must stay **byte-identical**. A changed golden
here means the resolver leaked into the no-rules path — a bug, not a re-baseline.
Locked decision #2 governs: wire bytes do not move.

## Risks & unknowns

1. **Escaping is the one place a silent wrong answer is possible.** Every other
   item fails loudly (missing key, compile error). A mis-escaped value produces a
   header that compiles and carries subtly wrong metadata bytes. The
   `EscapeCppStringLiteralTest` cases are the guard — do not weaken them.

2. **`ForField` candidate order is load-bearing.** #121 tries leaf-first, then
   the chain innermost→outermost, "so the most specific declaration wins". The
   chain must be accumulated **outer→inner** to match; reversed, the tests still
   pass on single-level flatten and fail only on nested wrappers.

3. **`test_metadata_nodrift.cpp` may need re-baselining** for generated *source*
   (GIR changed generated source bytes throughout — locked decision #2 permits
   this under review). Confirm at design time whether it asserts on source or on
   schema bytes; only the former may move.

4. **Interaction with GIR-10's backend guard.** `ValidateBackendsSupportFields`
   runs front-end, before emission. Confirm rule-compilation ordering relative to
   it, so a scalar-leaf-nested-list proto that also carries metadata rules reports
   the backend error rather than a confusing resolver error.

## Files to touch

| File | Change |
|---|---|
| `protoc/include/option_metadata.hpp` | port from `main` unchanged |
| `protoc/src/option_metadata.cpp` | port from `main` unchanged (555 lines) |
| `protoc/include/cpp_backend_schema_visitor.hpp` | `flatten_chain` member on `SchemaFieldRecord` (`:44`); resolver on the visitor ctor |
| `protoc/src/cpp_backend_schema_visitor.cpp` | accumulate the chain in `BuildFlattenedFieldListImpl` (`:63`); `ForMessage`/`ForField` at `:441`/`:454`; escape in `CppSchemaSink::SetMetadata` (`:226`) |
| `protoc/src/generator.cpp` | `ParseMetadataRules` in the opt loop; `Create`; thread through `GenerateFile` |
| `protoc/include/schema_builder.hpp` | restore the resolver parameter |
| `protoc/include/generator_internal.hpp` | include union (`option_metadata.hpp` + `ir.hpp`) |
| `protoc/tests/CMakeLists.txt` | test-list union (add `test_option_metadata.cpp`) |
| `protoc/tests/test_option_metadata.cpp` | port from `main` |
| `integration-tests/protoc-arrow-bridge/` | port #121's 3 test TUs + `CMakeLists.txt` / `conanfile.py` deltas |
| `docs/fletcher-options.md` | **no change** — contract is unaltered |

RBA emitter: untouched (locked decision #3).

## Out of scope

- Any change to #121's user-facing option grammar or semantics.
- Migrating the RBA accessor onto the IR — that is **RIR**
  ([RIR-rba-onto-ir.md](RIR-rba-onto-ir.md)).
- Wire-format change of any kind (locked decision #2).
- Re-litigating GIR-5's two-sink unification.

## Base and sequencing

**Base: `hard/3-7-consolidated` (PR #124) — NOT `main`.** Rebase
`feature/generator-ir-rewrite` onto that branch and do GIR-13 there. Do not wait
for #124 to merge. This is a locked base; the full rationale and the consequence
to accept are in
[GIR-generator-ir-rewrite.md](GIR-generator-ir-rewrite.md#gir-13-base--hard3-7-consolidated-not-main).

The short form, verified 2026-08-26:

- HARD-3..7 and GIR change **disjoint file sets** — no overlap at all.
- Trial-merging GIR onto `hard/3-7-consolidated` conflicts in exactly the same
  four files as onto `main`, all of them #121 — i.e. this item's own work. HARD
  contributes **zero** additional conflicts.
- `hard/3-7-consolidated` fast-forwards from `main`, so the base also carries
  #111, #121 and #122.

Until #124 merges, a PR from this branch shows HARD's 9 commits alongside GIR's.
After it merges, only GIR's changes remain in the diff.

Downstream chain is unchanged: **GIR → BIND-C# → BIND-Rust → RIR**.
