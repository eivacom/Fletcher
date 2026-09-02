# PDA-DEC-4 (provider registry) — independent code review

Diff base `94ae4bc..aea67f0`, 9 files, +1069/−20, branch `feature/protocol-driver-abi`.
Reviewer built and ran the suite; all empirical claims below were produced by
`conan create pubsub` + rebuild + `ctest -R 'Registry\.'`, not by reading.

**Counts: 0 blocking · 3 should-fix · 6 nits.**

## Verdict on the production code

I found no correctness defect in `provider_registry.{hpp,cpp}`.

`ProviderSelector::Parse` is genuinely pure, total and disjoint. `IsNameChar` uses
explicit ranges rather than `<cctype>`, so it is locale-free and — checked — gives the
same answer whether `char` is signed or unsigned (`0xc3` fails every range on both).
`Parse` takes `std::string` by value and stores it, so there is no `string_view`
lifetime to outlive; the NUL check is `text.find('\0')` on the `std::string`, i.e.
length-authoritative, so `"fastdds\0/../evil.so"` cannot be smuggled past by a
C-string-length reading. The empty string is refused before the NUL scan, the
`is_name_` flag is set from the same predicate `Register` validates against, and the
path branch's `FirstNonNameChar(selector.text_)` cannot return `npos` because `Parse`
is the only constructor and only sets `is_name_ = false` when a non-name char exists.
`kNotSupported` (path, no resolver) and `kInvalidArgument` (unknown name) are distinct
on every path, `kInternal` is used only where no named status exists (null factory /
null resolver return), and every caller-supplied callable is wrapped in
`TranslateSeamFailure`, so `std::overflow_error` still lands as `kPayloadTooLarge`
rather than degrading to `kInternal`.

Threading is stated, not assumed: the header says `Create` is `const` and concurrent,
`Register`/`SetPathResolver` are not, and the code matches — `Create` only reads
`factories_` and `path_resolver_`, and a `std::function` is never invoked after being
replaced because replacement is refused outright. `fletcher-pubsub` gained one TU and
no link dependency; `conformance_registry` links `fletcher-pubsub` + gtest_main and
nothing else. Nothing accidental rode along in `provider.hpp`.

**M2 and M6 verified by build, as asked:**

- **M2** (`const auto entry = factories_.begin();`): 4 tests red, including the forcing
  test — and it fails **only at the second direction**, confirmed from the output
  (`journal->rows[1].tag` "alpha" vs "beta"; the `rows[0]` assertions passed). A
  one-direction forcing test would indeed have been vacuous.
- **M6** (`if (!path_resolver_) { if (!factories_.empty()) return factories_.begin()->second(config); }`):
  exactly **one** test red — `PathSelectorWithoutResolverIsRefusedAsUnsupported` — while
  `PathSelectorResolvesThroughTheSameCall` stayed green. The negative control is
  independent, not a restatement.

Fresh-instance and duplicate semantics hold up. `EachCreateReturnsAnIndependentInstance`
asserts `first.get() != second.get()` **and** `live_probes == 2`, so it is two distinct
objects, not two handles on one. `DuplicateRegistrationIsRefused` re-creates after the
refusal and asserts the tag is still `alpha`, not `impostor` — it cannot pass on "it
threw". Duplicate refusal cannot be bypassed by whitespace (a space is not a name char,
so `"alpha "` is refused at `Register` as a path-shaped name); `"Alpha"` is a different
name by the documented case-sensitive rule, not a bypass.

---

## Findings

### S1 — should-fix (high confidence, build-verified). The path branch's config forwarding is asserted by nothing, and the failure is silent.

Mutation `path_resolver_(selector.text_, ProviderConfig{})` — the resolver receives a
default-constructed config instead of the caller's — leaves the suite at **12/12 green**.
Verified by building.

Every path-branch test constructs `const ProviderConfig config;` (default), so
`domain_id == 0` and `max_payload_bytes == 0` are indistinguishable from "the config
never arrived". `ConfigurationReachesTheProviderAndIsNeverRead` covers only the *name*
branch. The consequence of the mutation is exactly the class this round calls a wrong
answer rather than a failure: a loaded driver joins **domain 0** with no payload bound
and no error anywhere.

This matters more than an ordinary coverage hole because the path branch is the seat
PDA-ABI fills **blind**, against this header. The guard that would catch a resolver
call mis-wired at that hand-off does not exist. `PathSelectorResolvesThroughTheSameCall`'s
own comment claims the path reaches the resolver "with the identical config — only the
string differs", and that clause is unasserted.

Fix is two lines inside the existing test: give the path selection a non-default config
(`max_payload_bytes`/`domain_id`/a document with an embedded NUL) and assert
`journal->last_config` after the path `Create`, the way the name branch already does.

### S2 — should-fix (high confidence, build-verified). Two thirds of the name alphabet is unasserted.

Both of these leave the suite at **12/12 green**, verified by building:

- deleting `(c >= '0' && c <= '9')` from `IsNameChar`
- deleting `(c >= 'A' && c <= 'Z')` from `IsNameChar`

No test registers or selects a name containing a digit or an uppercase letter. The names
in the file are `alpha`, `beta`, `gamma`, `in-process`, `xrce_dds`, `fastdds`,
`null-returner`, `thrower`, `typed-thrower`, `overflower` — all lowercase ASCII letters,
`-` and `_`. `fastdds.v2` contains a digit but is asserted to be *refused*, and it is
still refused for its `.` under both mutations, so it does not cover the digit range.

`[A-Za-z0-9_-]` is normative in the header and in the spec, and PDA-ABI reads it as the
contract. Under either mutation a build silently narrows the vocabulary and `FastDDS` or
`zenoh2` becomes a *path* refused with `kNotSupported` ("this build cannot load
drivers"). That refusal is loud and located, which is why this is should-fix rather than
blocking — but the header's own claim that "the two vocabularies cannot drift" is only as
strong as the alphabet the tests pin, and today they pin `[a-z_-]`.

Fix: one added registration+selection using a name like `Fast2DDS-v1_x` in
`RegistrationAndSelectionShareOneVocabulary`.

### S3 — should-fix (medium confidence). The DEBT-1 resolver lifetime rule is prose the code could enforce, in the forbidding direction.

The header spends ~15 lines making a normative rule the object does not hold anyone to:
a resolver "must keep everything the provider it returns depends on … alive for at least
as long as that provider, independently of this registry's lifetime and of the
resolver's own", with the failure mode named as "a use-after-unload and not reliably
loud". The code documents a hope. A resolver author who parks a module handle in
registry-owned state gets exactly the described crash, silently, and nothing in this seam
notices.

The registry can make that state unrepresentable for a few lines. Hold the resolver as
`std::shared_ptr<PathResolver>` and have `Create` return an aliasing `shared_ptr` that
owns a copy of that handle alongside the provider:

```cpp
auto keepalive = path_resolver_;                       // shared_ptr<PathResolver>
auto provider  = TranslateSeamFailure([&]{ return (*keepalive)(selector.text_, config); });
if (!provider) Refuse(...);
return std::shared_ptr<PubSubProvider>(
    std::shared_ptr<void>(std::move(keepalive)), provider.get());  // aliasing
```

The resolver — and therefore whatever its closure captured, including a module handle —
now outlives every provider it made, mechanically, whatever the resolver author did. The
hazard class the header describes stops being reachable, and most of the DEBT-1 paragraph
becomes a one-line note. Cost is one indirection on a path that already does a `dlopen`.
(Note the aliasing constructor keeps the *provider's* own control block alive too, so the
provider's deleter still runs first — the ordering is right.)

This is not a defect today, since no resolver exists; it is a design-economy call for the
owner, and the reason to make it now is that PDA-ABI is the component that would pay for
getting it wrong and it implements against this header without seeing this code.

---

## Volume

I counted. **The implementer's account holds** — unlike PDA-DEC-2.

| file | total | comment | blank | code |
|---|---|---|---|---|
| `provider_registry.hpp` | 251 | 182 | 21 | **48** |
| `provider_registry.cpp` | 176 | 25 | 13 | 138 |
| `registry.cpp` (tests) | 503 | 91 | 67 | 345 |

Delta over declared is 429 lines. Header prose is 182 of it, the README section is 47,
and 6 tests beyond the designed 6 are roughly 200 — the account covers the overrun with
room to spare, and there is no third, unexplained bucket. The 251-line header is 48 lines
of declarations: it is contract, not padding, and this item genuinely owes it because
PDA-ABI implements against it and nothing else.

Where volume is not carrying weight is the test file, mildly: **unknown-name →
`kInvalidArgument` is asserted three times** (in `UnknownNameIsRefusedWithTheAvailableNames`,
in `PathSelectorWithoutResolverIsRefusedAsUnsupported`, and in
`SelectorShapeDecidesAndIsRefusedWhenItCannotMeanAnything`) while the resolver's config
and two thirds of the name alphabet are asserted zero times. That is the proportionality
comment: the extra six tests restated a guard that existed rather than adding the two
that were missing. Not a reason to cut anything — a reason to spend S1 and S2.

## Nits (one line each)

1. `Quoted` escapes control bytes but not `\` or `"`, so `"a\x41b"` in a diagnostic is ambiguous between a literal backslash-x-4-1 and the escape of byte 0x41 — and every Windows path prints with raw backslashes.
2. Nothing asserts the hex byte in either refusal message; the CRLF test checks `"path"` and `"offset 7"` only, so `kHex[byte >> 4]`/`kHex[byte & 0x0f]` transposed would go unnoticed.
3. The offset in the path refusal goes through `std::ostringstream`, imbued with the global locale — a grouping `numpunct` would print `offset 1,234`; `Parse`'s `std::to_string` is not affected, so the two refusals can disagree in format.
4. No upper bound on selector length: a multi-megabyte selector is copied and then re-emitted into an error message at up to 4x expansion.
5. "`Create` is `const` and safe to call concurrently" is true of the registry but invokes caller-supplied factories concurrently too; the header does not say that obligation passes to the factory author.
6. A non-ASCII name (`cafe` with an accented e) is a second documented-rule misclassification beyond the `myDriver` case the header names — refused as `kNotSupported` "cannot load drivers" rather than "no such provider"; loud and located, so cosmetic only.
