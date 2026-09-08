# GIR-13 — step-4b independent code review

**Reviewer:** independent review agent (fresh context)
**Diff base:** `HEAD` = `9913274` (implementation staged; `git diff HEAD`)
**Scope:** `protoc/include/cpp_backend_schema_visitor.hpp`, `protoc/include/schema_builder.hpp`,
`protoc/src/cpp_backend_schema_visitor.cpp`, `protoc/src/generator.cpp`,
`protoc/tests/CMakeLists.txt`, `protoc/tests/test_schema_visitor.cpp`
(+ plan-doc churn in `plans/`, not reviewed line-by-line)

**Verdict:** no blocking findings. 4 should-fix, 10 nits.

## What I actually ran (not just read)

- Built and ran the full protoc unit suite: **88 tests, 87 pass, 1 skip** (the
  golden-capture guard). `test_option_metadata.cpp` now compiles and runs (32
  `TEST`s) — it was present in the tree since the #121 merge but **absent from
  `protoc/tests/CMakeLists.txt`**, i.e. it has not been executing on this branch
  until this diff. That wiring line is a real fix, not just plumbing.
- **Mutation-tested the four new guards.** Every mutation was applied to the real
  source, compiled, run, and reverted (`git checkout --`; tree restored, suite
  re-verified green afterwards):

  | # | Mutation | Caught by | T4 caught it? |
  |---|---|---|---|
  | M1 | pass `nullptr` at the **MAP** `DeepCopyMessageStruct` site | T2 rows 2 + 5 | **no** |
  | M2 | reverse chain orientation (`{fd}` + old chain) | T3 (`x:unit` = `"mid"`) | **no** |
  | M3 | remove `EscapeCppStringLiteral` from the C++ sink | T1 (a): all 4 source assertions | no |
  | M4 | move escaping into `SchemaVisitor::FieldMetadata` (sink raw) | T1 (b): in-process value `<missing>` | no |
  | M5 | pass `nullptr` at the **STRUCT** `DeepCopyMessageStruct` site | T2 rows 1, 3, 5, 6 | **no** |

  So T1/T2/T3 are genuinely toothed, and T1's layer assertion (the one silent-failure
  mode) is discriminating in **both** directions (M3 and M4 fail different assertions).
- **Probed the C++-language assumptions behind the escaping** with `cl.exe`
  17.14 `/std:c++20` on a hand-written literal mirroring `EscapeCppStringLiteral`'s output:
  - `\001 \177 \200 \201 \276 \302 \260 \377` decode to `1 127 128 129 190 194 176 255` —
    octal escapes >= `\200` round-trip **exactly** on MSVC (the pre-C++23
    "implementation-defined if not representable" clause is not a practical hazard).
  - `"\0011\0037\0007"` decodes to `01 '1' 03 '7' 00 '7'` — the 3-digit form is
    non-greedy as documented; the hex-escape hazard the header comment cites is real
    and correctly avoided.
  - `strlen("??/??!??=") == 9` — trigraphs are not processed (C++20).
  - **`/W4` emits C4125 on `\0011`** (see should-fix 1).

## Findings

### Blocking

None.

---

### Should-fix

**SF-1 — `EscapeCppStringLiteral` output can break a consumer's `/W4 /WX` build (MSVC C4125).**
*Confidence: high (empirically reproduced). Severity: should-fix.*

`EscapeCppStringLiteral` emits a 3-digit octal escape for any byte `< 0x20 || >= 0x7F`
and leaves printable ASCII raw. When such a byte is immediately followed by an ASCII
digit, the rendered literal is e.g. `"\3022"` (byte 0xC2 then `'2'`). MSVC diagnoses
this as:

```
warning C4125: decimal digit terminates octal escape sequence
```

Reproduced: `cl /nologo /std:c++20 /W4 /WX` on `static const char* seq = "\0011\0037\0007";`
-> `error C2220: the following warning is treated as an error`. At `/W1` it is silent, so
it is a level-4 warning — but a downstream consumer compiling the generated
`*.fletcher.pb.h` with `/W4 /WX` (plausible for a NaviSuite-style build) will fail to
compile. GCC/Clang have no equivalent diagnostic.

Trigger condition is plausible, not exotic: any non-ASCII label followed by a digit —
CJK/Cyrillic text then a digit, `"°2"` -> `\260` + `'2'`, etc. The **values are still
correct** on all three compilers (verified above), so this is a build-breakage risk, not
a wrong-bytes risk.

Fix is one line in the escaper: after emitting an octal escape, if the next input byte
is `'0'..'9'`, close and reopen the literal (`"\302" "2"`). Adjacent string-literal
concatenation happens after escape conversion, so `ArrowCharView("\302" "2")` is fine
— the test file's own `NastyValue()` already relies on exactly that trick.

*Caveat for triage:* `EscapeCppStringLiteral` itself is unchanged #121 code. GIR-13
broadens exposure (it now escapes **keys** too, and it is the change that puts the
escaper on the IR path), so this is the natural place to note it, but the PM may
legitimately log it against #121 instead.

**SF-2 — the octal branch of the escaper has never been through a real compiler.**
*Confidence: high. Severity: should-fix (coverage).*

T1 proves *which layer* escapes, but it compares the generated source against
`EscapeCppStringLiteral`'s own output — it cannot detect a bug *inside* the escaper.
The independent oracle for that is `EscapeCppStringLiteralTest.*` in
`test_option_metadata.cpp` (hardcoded expectations — good), but nothing **compiles** an
escaped literal. The one place that does — `integration-tests/protoc-arrow-bridge`,
whose `IpcParityTest.MappedMetadataIpcFileMatchesRuntimeSchemaBytes` byte-compares the
compiled `SampleSchema()` against `option_metadata.Sample.ipc` — uses a fixture value of
`ext_meta: "{\"crs\":\"EPSG:4326\"}"`, i.e. **pure ASCII JSON**. The octal branch is
therefore unexercised end-to-end.

Recommend adding a control byte and a non-ASCII byte to that fixture's `ext_meta`
(`integration-tests/protoc-arrow-bridge/proto/option_metadata.proto:99`). That single
fixture edit would have caught SF-1 and would close the last silent-wrong-answer gap in
the escaping path.

**SF-3 — nothing exercises the `Generate()` wiring itself.**
*Confidence: high. Severity: should-fix (coverage).*

T1–T4 all build their own resolver and call `GenerateSchemaFunctionFromIr` /
`BuildMessageSchema` directly. No unit test drives `ArrowRowGenerator::Generate`, so all
three new wiring points in `generator.cpp` are covered **only** by the Conan-gated
integration lane:

- `ParseMetadataRules(parameter, ...)` / `OptionMetadataResolver::Create(...)` error paths,
- `GenerateFile(file, schema_only, resolver.get())`,
- `BuildMessageSchema(msg, resolver.get())` in the `emit_ipc` loop.

Concretely: replacing `resolver.get()` with `nullptr` in the IPC loop is invisible to the
entire 88-test unit suite. (The integration lane's `IpcParityTest` would catch it, so
this is a fast-feedback gap rather than an uncovered defect.)

**SF-4 — the rule-error-ordering comment states an invariant the code does not have.**
*Confidence: high. Severity: should-fix (correctness of a documented property).*

`protoc/src/generator.cpp` (new comment above `ParseMetadataRules`):

> "…reporting them ahead of the shape validator keeps the diagnostic stable regardless
> of which file protoc happens to process first, and **neither validator can mask the
> other's root cause**."

But `Generate()` opens with `if (!ValidateNoUnsupportedIr(file, error)) return false;` —
*before* `ParseMetadataRules`. So a file containing an unsupported type does mask a
malformed `metadata_from_option` rule, which is exactly the masking the comment claims is
prevented; and the diagnostic is *not* file-order-independent. Only
`ValidateBackendsSupportFields` is ordered after the rule compilation.

Pick one: move the `ParseMetadataRules` + `Create` block above `ValidateNoUnsupportedIr`
(which actually delivers the stated property, since rule compilation reads only the flag
string and the pool), or narrow the comment to name `ValidateBackendsSupportFields`.

---

### Nits

**N-1 — NUL in a metadata value is silently truncated (both paths).**
*Confidence: high. Severity: nit.*
`NanoarrowSchemaSink::SetMetadata` uses `ArrowCharView(value.c_str())` (strlen) and the
generated code's `ArrowCharView("...\000...")` truncates at the same point, so the two
paths **agree** — no divergence, and
`EscapeCppStringLiteralTest.EmbeddedNulIsEscapedAsThreeDigitOctal` documents it. Still, a
`bytes`-typed option value containing 0x00 loses everything after it with no diagnostic.
Options: reject NUL-containing values in `OptionMetadataResolver` (a hard error is
defensible — Arrow metadata is length-prefixed, so this is purely a Fletcher-side
limitation), or switch both sinks to explicit-length `ArrowStringView`. GIR-13 is what
makes this reachable (before it, values were only the four ASCII builtins).

**N-2 — stale doc reference in `option_metadata.hpp`.**
*Confidence: high. Severity: nit.*
`OptionMetadataResolver::ForField`'s comment says "`flatten_chain` is
**FieldInfo::flatten_chain**". `FieldInfo` no longer has that member on this branch
(GIR-5 removed it; `grep flatten_chain protoc/include/generator_internal.hpp` is empty).
It is now `SchemaFieldRecord::flatten_chain`. This diff is the change that made the
reference stale and the natural place to fix it.

**N-3 — T4 is tautological for metadata and blind to both resolver-drop mutations.**
*Confidence: high (mutation-verified). Severity: nit.*
`CppAndIpcTracesAgreeWithResolverActive` compares two traces produced by the **same**
`SchemaVisitor`, and `RecordingSink::SetMetadata` logs the pair vector the visitor built —
so the metadata half of the comparison can essentially never fail.
`RecordingSink::DeepCopyMessageStruct` also drops the `resolver` argument from its log
line, so M1 and M5 both leave T4 green. The test's own comment is honest about this
("blind spots by construction… Do NOT present T4 alone as evidence"), so this is not a
misleading assertion — but a one-token strengthening would remove the blind spot:

```cpp
log.push_back("DeepCopy(" + std::string(d->full_name()) + "," + Id(dst) +
              (resolver ? ",R)" : ",-)"));
```

**N-4 — needless copies in the pair builders.**
*Confidence: high. Severity: nit.*
`const MetaPairs extra = resolver_->ForMessage(...)` followed by
`pairs.insert(pairs.end(), extra.begin(), extra.end())` copies two `std::string`s per
pair. Dropping the `const` and using `std::make_move_iterator` avoids it. Matches `main`'s
shape, so this is inherited, and the volume is trivial.

**N-5 — `?` is not escaped (trigraph, historical).**
*Confidence: high. Severity: nit (non-issue on C++17+).*
A value containing `??/` renders verbatim; in a pre-C++17 consumer that is a trigraph for
`\`. Verified MSVC C++20 does not process trigraphs (`strlen("??/??!??=") == 9`), and
trigraphs were removed by C++17. Recording for completeness only.

**N-6 — very long values will hit compiler literal limits.**
*Confidence: medium. Severity: nit.*
A large option value (e.g. a 64 KB JSON blob) renders as a single string literal on one
line. MSVC C2026 caps a string literal at 65535 bytes. No guard, no diagnostic — the
failure lands in the consumer's compiler. Worth a documented size limit if anyone cares;
not worth code today.

**N-7 — two newly-added lines deviate from `clang-format-18` + repo `.clang-format`.**
*Confidence: high. Severity: nit.*
`protoc/src/cpp_backend_schema_visitor.cpp:264` (the
`ArrowCharView(\"" << EscapeCppStringLiteral(value)` / `<< "\"));\n"` split) and `:274`
(`DeepCopyMessageStruct(..., SchemaRef dst,` wrap) would both be joined by clang-format.
**Important context:** the same file already has 5 pre-existing deviations at `HEAD`,
`main:protoc/src/generator.cpp` has 1, and the access-modifier indentation across
`protoc/` disagrees with `AccessModifierOffset` — so `ci.format-check-cpp` (which does
cover `protoc/`) appears not to be green on this branch regardless of this diff. Flagging
for whoever owns that, not as a gate on GIR-13.

**N-8 — dead code adjacent to the change.**
*Confidence: high. Severity: nit.*
`SetMetadataPairs` (`protoc/src/generator.cpp:887`) has no callers — GIR-5 removed the
last one. Pre-existing, but this diff is the last change that could plausibly have used
it. Delete.

**N-9 — struct-column metadata asymmetry is now user-visible through mapped keys.**
*Confidence: high. Severity: nit (pre-existing behaviour, newly consequential).*
For a singular struct field the field overlay **replaces** the deep-copied nested root
metadata, so the nested message's `message:`-scope keys (and `proto_package` /
`proto_message`) vanish from the column. The *same* struct reached as a list `item` or a
map `value` **keeps** them, because those branches apply only `SetName`. T2 rows 4/5/7 pin
both behaviours correctly (including the sharp
`EXPECT_FALSE(HasMeta(pos, "proto_message"))`, which is the right way to assert "replace,
not append"). Inherited from #121 and from the pre-GIR-13 generator, so no regression —
but a rule author will find their `message:`-scope key on `path.item` and missing on
`pos`. Worth a sentence in `docs/fletcher-options.md` if it is not already there.

**N-10 — resolver is rebuilt per file.**
*Confidence: high. Severity: nit (correct as written).*
`ParseMetadataRules` + `OptionMetadataResolver::Create` run once per `Generate()`, i.e.
once per file, discarding the memo cache and the `DynamicMessageFactory` between files.
O(files x rules) descriptor lookups. This matches `main` and is the **right** call given
the documented thread-safety constraint — hoisting it to a member would break exactly the
property the new comment promises. Noted so nobody "optimises" it later.

## Things I checked and found correct

- **Escaping is applied in `CppSchemaSink::SetMetadata` only.** The only other
  `ArrowMetadataBuilderAppend` emitter in the tree is the dead `SetMetadataPairs`;
  `NanoarrowSchemaSink::SetMetadata` correctly passes raw bytes. Mutation-verified in both
  directions (M3 / M4). Escaping both key and value is right: `arrow_key` is arbitrary
  argv bytes and `ParseMetadataRules` splits on the first two colons only, so
  `ARROW:extension:name` and `x:k"\q` are both legal keys — T1 uses the latter, which is
  what makes the key half of the assertion non-vacuous. Uniform (unconditional) escaping
  is byte-neutral for the four builtins, independently pinned by
  `EscapeCppStringLiteralTest.PrintableAsciiIsUnchanged`.
- **The two sinks cannot disagree by construction**: `RootMetadata()` / `FieldMetadata()`
  are the single build site per scope, both sinks consume the same vector, and the only
  transform is the C++ sink's escaping (which the compiler inverts). Byte parity with
  rules active is additionally proven end-to-end by
  `IpcParityTest.MappedMetadataIpcFileMatchesRuntimeSchemaBytes` / `...OnFlattenedFields...`.
- **Chain accumulation is byte-for-byte #121's algorithm.** `inner = flatten_chain;
  inner.push_back(fd)` on the way *down* is identical to `main`'s `GatherFieldsImpl`, so
  the orientation `ForField`'s `rbegin()` scan expects is preserved. `inner` outlives the
  recursive call it is passed to; the stored `const FieldDescriptor*` are pool-owned and
  outlive the whole walk (the pool is protoc's request pool, and `file->pool()` is the
  *same* object for every file in the invocation — which is also why cross-file
  `<Nested>Schema()` deep-copies cannot see a different rule set). Copy churn is one small
  vector per leaf. Orientation is mutation-verified by T3 (M2).
- **`source_field` is never null** on any `SchemaFieldRecord` (`rec.source_field = fd`
  unconditionally), and `ForField` defends against null anyway. The header comment change
  ("always the leaf") is a *comment* fix — the old "nullptr for inlined fields" claim was
  already false in code, so `ts_backend_visitor.cpp`'s `DeclaredWrapperFor` behaviour is
  unchanged.
- **Resolver lifetime is clean.** `std::unique_ptr<OptionMetadataResolver>` is a
  `Generate()` stack-local; it is handed down as a raw `const*` through a purely
  stack-based call chain (`GenerateFile` -> `GenerateSchemaFunction` ->
  `GenerateSchemaFunctionFromIr` -> `SchemaVisitor`, and `BuildMessageSchema` ->
  `BuildMessageSchemaIntoFromIr` -> `NanoarrowSchemaSink::DeepCopyMessageStruct` ->
  recurse). `resolver_` exists only as a `SchemaVisitor` member, and every `SchemaVisitor`
  is stack-local. No member, static, thread-local, lambda capture, or cross-file cache.
  Passing the resolver *through* `DeepCopyMessageStruct` rather than storing it as sink
  state is the right design choice for exactly this reason. The `mutable` factory + memo
  cache are therefore only ever touched by one thread.
- **Both deep-copy sites carry the resolver** (STRUCT and map-struct-value), and the LIST
  branch reaches STRUCT by recursion so `List<Struct>` / `List<List<...<Struct>>>` are
  covered. All three routes are separately asserted by T2 and separately mutation-verified
  (M1 -> map row only; M5 -> struct + list rows).
- **`CppSchemaSink::DeepCopyMessageStruct` ignoring the resolver is correct**, not a latent
  bug: it emits a *call* to `<Nested>Schema()`, which the same invocation generated under
  the same rules against the same pool. The `(void)resolver;` + comment is the right way to
  record that.
- **Null-resolver / empty-rule paths.** `metadata_rules.empty()` -> no resolver at all, so
  `resolver == nullptr` stays literally synonymous with "flag absent"; `RootMetadata` /
  `FieldMetadata` branch on `nullptr`; the pair vector is never empty so no new
  "skip `SetMetadata`" shape appears. Confirmed byte-identical:
  `SchemaVisitor.CppAndIpcByteIdentical` and the committed `tests/golden/*.ipc` pass
  unmodified.
- **No new throw paths.** The resolver returns `bool`/vectors and never throws; the C++
  sink only writes to an `ostringstream`. The nanoarrow path's existing `CheckNa` throws
  are still caught by the `try`/`catch` around `BuildMessageSchema` in the IPC loop.
  `Create` failure is converted to `*error` + `return false` as the plugin contract
  requires.
- **No de-duplication needed in the emitter.** `Upsert` already applies
  last-non-empty-wins with first-appearance ordering inside the resolver, and
  `IsReservedKey` rejects all four builtin keys at parse time, so a resolver key cannot
  collide with a builtin. The plain append is sufficient and the comment says so.
- **`map` fields.** `ForField` skips `cand->is_map()` for `field_type:` scope, so a
  synthetic `MapEntry` is never treated as a carrier. T2 row 9 pins it (`x:unit` and
  `x:meta` both absent on `byname`).
- **Unset sub-field -> no key** (proto3 implicit presence). T1 pins it on the same field
  that carries the nasty value (`Nasty.v` sets `nasty`, leaves `meta` unset -> no
  `x:meta`), which is a nice two-for-one.
- **Fixture realism.** Both new fixtures compile `.proto` *source text* into a private
  pool, so the custom options land in the linked-in options message's `UnknownFieldSet` —
  the exact shape the resolver's `DynamicMessage` re-parse must handle in production. The
  local `flatten`/`flatten_field` extensions at 50000 are correct: `FindBoolOption` matches
  by *number* out of the `UnknownFieldSet`.
- **`NastyValue()` is self-checking.** The bytes are spelled twice — once in proto
  text-format escaping inside the raw-string fixture, once in C++ escaping — and T1 fails
  loudly with a readable diff if the two spellings ever drift. The adjacent-literal split
  around `"\x01"` correctly prevents the hex escape running into the following `0xC2`.
- **No default-argument duplication.** Every new defaulted parameter is declared with its
  default exactly once (`schema_builder.hpp`, `cpp_backend_schema_visitor.hpp`, and the
  three file-local `generator.cpp` definitions), and the out-of-line definitions omit it.
- `#include <memory>` correctly added to `generator.cpp` for the new `unique_ptr`.
- Unknown `--fletcher_opt` tokens are ignored by the option loop, so
  `metadata_from_option=...` is not rejected before `ParseMetadataRules` sees it.
