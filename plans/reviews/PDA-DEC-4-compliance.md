# PDA-DEC-4 — compliance review (step 4a, independent)

Diff: `94ae4bc..aea67f0`, 9 files, **+1069 / −20**. Branch `feature/protocol-driver-abi`.
Oracles: design `plans/PDA-DEC-4-provider-registry.md`, brief, design review + `design-debt.md`
(10 items), spec §1/§4/§4.1/§4.2/§9, locked decisions 2/3/4/8/14, the 28-entry rulings ledger.

**Verdict: ISSUES — 2 blocking (both small), 1 minor. The defining claim HOLDS.**

Everything below the `Findings` heading is a finding. What is not listed was checked and
conforms. All measurements were made from `git show aea67f0:<path>` blobs, not the working
tree — see RECORD-6.

---

## The signature claim — attacked empirically, and it holds

This is the item the round exists for, so it was attacked with a build rather than a
reading. I compiled the committed `provider_registry.cpp` + `registry.cpp` against the
packaged `fletcher-pubsub` and pushed 28 selector strings through `Parse` + `Create`:

| Class | Inputs | Result |
|---|---|---|
| name | `fastdds` `FastDDS` `in-process` `xrce_dds` `a` `myDriver` `libzenoh` | name; unknown ones → `kInvalidArgument` listing what is registered |
| path | `fastdds.v2` `libzenoh.so` `zenoh.dll` `z.dylib` `./x.so` `../x.so` `/opt/fletcher/libzenoh.so` `/opt/fletcher` `C:driver.dll` UNC and backslash spellings `~/x.so` `fastdds ` `fastdds<CR><LF>` ` fastdds` `fast dds` UTF-8 `<01>` | path; no resolver → `kNotSupported` naming the offending offset and hex byte |
| refused at `Parse` | the empty string, `fastdds<NUL>/../evil.so` | `kInvalidArgument`; the NUL message carries offset 7 and `Quoted` renders the byte as an escape |

Total, disjoint, registry-independent — verified against a registry with `fastdds`
registered and against one holding only a resolver. The single misclassification
(`myDriver` / `libzenoh`: a bare relative filename with no dot and no separator reads as a
name) **is ruling 27 as written** — "a plain word like `fastdds` is a name; anything else,
like `/opt/x.so`, is a path" — it is loud, and the operator's repair is `./myDriver`, a
config edit. No input misclassifies silently, and none is repaired by an overload, a flag or
a disambiguator.

Three further widening routes were checked and are closed:

- **A mutable module cache inside the resolver against a `const Create`.**
  `std::function::operator()` is const-qualified and type-erases the target's constness, so
  a mutable closure holding a module cache compiles and runs against `Create() const`. No
  `mutable` member, no non-const overload, no widening. (Verified by compiling one.)
- **A loaded driver reached by *name*, and MCU static registration.** Both go through
  `Register(name, Factory)`, which already exists; all four (built-in|loaded) × (name|path)
  combinations are expressible through the frozen surface. (This is also the root of
  finding F2.)
- **Distinct error reporting for an unloadable path.** `TranslateSeamFailure` rethrows a
  `PubSubError` unchanged, so a resolver picks its own status without touching the seam.
  Verified: a factory throwing `PubSubError(kTransportFailure)` surfaces as
  `kTransportFailure` through `Create`.

`static_assert` pins the member-pointer type, not the return type (DEBT-3 discharged), and
it lives in the header the suite includes, so the suite cannot build against a widened call.

## Falsifiability — M2 and M6 re-derived, plus M4/M7/M8

Built from the committed blobs in an isolated tree; baseline 12/12 green. Each mutation
applied singly to production code.

| Mutation | Result | Note |
|---|---|---|
| **M2** `Create` uses `factories_.begin()` | forcing test RED **at `registry.cpp:196` only** — `rows[1].tag` is `alpha`, expected `beta` | Confirms the design review's warning exactly: the *first* direction stays green under M2. Asserting `alpha`→alpha alone would have been vacuous. |
| **M6a** path-without-resolver returns the first factory's provider | negative control RED; **`PathSelectorResolvesThroughTheSameCall` GREEN** (11 passed) | The control is genuinely independent, not a restatement. |
| **M6b** path-without-resolver returns a default-constructed provider | negative control RED + `SelectorShapeDecides…` RED; positive path GREEN | Same conclusion by a second route. |
| M4 `Parse` classifies everything as a name | 4 RED | as claimed |
| M7 `Register` last-wins | `DuplicateRegistrationIsRefused` RED | as claimed |
| M8 `Create` memoizes | 5 RED | as claimed |

The cached Conan package header is byte-identical to the tree's, so the harness's 12/12
(`ctest -R 'Registry\.'`, entries #38–#49) is not the design's named stale-cache false green.

---

# Findings

## F1 (BLOCKING, ~10 lines) — the resolver seat's failure path is normative and unfalsified

`provider_registry.hpp` (the `Create` doc block) and spec §5.1 promise:

> - a factory **or resolver** that returns null → `kInternal`, named;
> - anything a factory **or resolver** throws → translated by §5.1's rule.

The code implements both for the resolver (`TranslateSeamFailure` around
`path_resolver_(...)`, then the null check). **Nothing tests either half.**
`AFactoryThatFailsIsReportedAsATypedSeamFailure` covers four factory shapes and no resolver
shape. Two single mutations, each applied alone, leave **all 12 entries green**:

- **E13a** — delete `TranslateSeamFailure` around the resolver call, so a
  `std::runtime_error` out of a loader escapes the seam untranslated: **12/12 PASSED**.
- **E13b** — delete the resolver's null check, so `Create` hands a caller `nullptr`:
  **12/12 PASSED**.

This is the branch PDA-ABI fills, and its failure mode is the one ruling 26 is about ("an
unloadable driver path… fails distinctly"). The round has four prior instances of this
defect class and the config's rule is that green tests do not compensate. The item bought
six entries beyond the design's six and 67% of budget; the one entry that would guard the
seat is not among them.

**Remedy:** two `RefusalOf` lines inside the existing test — a resolver that throws
`std::runtime_error` (expect `kInternal`, original message preserved) and one that returns
`std::shared_ptr<PubSubProvider>{}` (expect `kInternal`).

## F2 (BLOCKING, one word in two places) — DEBT-1's lifetime rule binds only half the route PDA-ABI takes

The rule as landed (beside `SetPathResolver`, and spec §4 clause 2) obliges **a resolver**:

> A **resolver** must keep everything the provider it returns depends on — including a
> loaded module … — alive for **at least as long as that provider**, independently of this
> registry's lifetime and of the resolver's own.

Read as PDA-ABI must read it, unable to ask a question: the obligation does not reach a
`Factory`. But the design review's §C — which the design's §3 and premise P2 rely on — says
PDA-ABI reaches a loaded driver through `Register` as well, twice:

> "PDA-ABI wraps the C entry point in a `Factory` lambda in its own component and calls
> `Register(name, …)`." · "**A loaded driver reachable by *name*** … `Register("zenoh",
> factory_that_dlopens)`."

The registry owns `factories_` exactly as it owns `path_resolver_`. A `Factory` closure
holding a module handle, plus the sanctioned "populate, then share" discard of the registry
(tested by `ProvidersOutliveTheRegistryThatMadeThem`), is the identical use-after-unload
DEBT-1 was raised to close — reached by the route the linkage ruling's static half and the
"one config on MCU and desktop" idiom both use. As landed, PDA-ABI can satisfy the spec's
letter and still unload a `.so` under a running provider.

The rule is otherwise unambiguous: module ⊒ provider, independent of both the registry's and
the resolver's lifetime, with the `shared_ptr`-deleter idiom named and the
registry-owned-cache anti-pattern named. The code does not contradict it (the registry holds
no reference to what it made).

**Remedy:** "A **resolver or a factory** must keep …" in `provider_registry.hpp` and in
spec §4 clause 2; the header sentence belongs beside `Register` too, or in the class comment.

## F3 (minor) — DEBT-5's C form for the *selector* landed in the header, not in the oracle

Locked decision 1: PDA-ABI and BIND-C#/BIND-Rust "meet **only** at
`docs/pubsub-interface-spec.md`". §9: the bindings "implement … §4 selection". DEBT-5 asked
for the selector's C form precisely because "§4 selection is binding-visible surface (§9), so
a binding must not have to invent it."

The landed §4.1 states the **document's** C form (pointer + length, borrowed, length
authoritative). The **selector's** C form is stated only in `provider_registry.hpp`. The
operative half — the embedded-NUL refusal — did reach spec §4 clause 1, so the hazard is
covered; what is missing is the one sentence telling a parallel round the selector is
bytes-plus-length rather than a NUL-terminated string. One sentence in §4 clause 1.

## Non-blocking observations (book, do not fix here)

- **Concurrency obligation on caller-supplied callables.** The header promises "`Create` is
  `const` and safe to call concurrently" while `Create` invokes a caller's `Factory` /
  `PathResolver`. Nothing obliges either to be reentrant, and PDA-ABI's natural module cache
  lives inside exactly those closures. Same family as DEBT-1; it does not require widening
  (a resolver can lock internally), so it is forward debt, not a fix.
- **The §4 clause 2 freeze is written unqualified.** "The registry's **whole public surface**
  is fixed here — `Create`, `Register`, `SetPathResolver`, and nothing else." DEBT-2 asked
  only that *PDA-ABI* add no method; as written this also binds PDA-DEC-5/8 — e.g. a
  `Names()` accessor for a gateway `--provider` help listing is now a stop-and-ask. Likely
  intended and safe (the unknown-name refusal already lists what is registered), but the PM
  should know the freeze is broader than the debt item asked for.

---

# What was checked and conforms

- **Rulings 26 / 27 / 28.** 26: a path is a valid selection, refused `kNotSupported`,
  distinct from an unknown name's `kInvalidArgument`, both asserted as *statuses*; the
  stand-in resolver proves the identical call end-to-end. 27: shape decides, no prefix, one
  setting, the rule never consults the registry (asserted in an empty registry). 28: the
  typed core is exactly `{max_payload_bytes, domain_id}`; spec §4.1 and the header both say
  "exactly", and widening is a stop-and-ask.
- **Decision 8.** Nothing in `provider_registry.cpp` reads, inspects, validates or
  schema-checks `document`; `config` is forwarded by `const&` and never touched. No parser,
  no format, no new dependency in `pubsub/CMakeLists.txt` (one source line) or in
  `conanfile.py`. `ConfigurationReachesTheProviderAndIsNeverRead` pushes non-UTF-8 bytes with
  an embedded NUL through byte-for-byte.
- **Decision 14 / no ABI.** No `extern "C"`, no C header, no vtable, no version negotiation,
  no host-callback struct. `dlopen` / `dlfcn` / `LoadLibrary` appear nowhere in `pubsub/`
  except in prose explaining their absence. `PathResolver` is a seat — a `std::function` over
  C++ types that says nothing about what a loader looks like — nothing calls it, no build
  depends on one, and the name branch never touches it.
- **Decisions 3 and 4 above the seam.** `provider.hpp` changed by comment only; no
  `PubSubProvider` method added, removed or reordered; `Publish` still inverted. No caller
  branches on built-in vs loaded; `MakeProvider` is byte-for-byte the same call for both.
  `gateway-fastdds-ts` untouched.
- **The corner-case ladder.** Rung-1 items 2–6 and rung-2 items 7–11 all hold in code and are
  each covered by a status assertion or a tagged-row assertion. No refusal became a recovery
  path; no forbidden state is tolerated with a fallback. Rung-1 item 1 is correctly
  downgraded per DEBT-6, and the replacement wording is *true* (the rule is public and pure,
  RTTI is available, and what decision 3 guarantees — config edit, never caller edit — is
  what is claimed instead).
- **All 10 DEBT items discharged**: 1 (partially — F2), 2, 3, 4, 5 (partially — F3), 6, 7, 8,
  9, 10a, 10b (10b overshot — RECORD-1).
- **Scope, both directions.** `Files-to-delete: none`, and nothing was deleted from product
  code. The converse — a construct the design ordered deleted that survived — has no
  candidates, because the design ordered none. No shim, alias, re-export or coexistence
  window is created; nothing here is scheduled for later deletion. Nothing pre-empts
  PDA-DEC-5: `InProcessProvider` is not registered, `SchemaCarriage` is untouched, the
  gateway is untouched. No `retired:` / `re-anchored:` citations were made and none was owed.
- **Spec amendments are warranted and in charter.** §4 clause 1 records the landed rule and
  the two statuses (design `Files-to-touch` + rulings 26/27); clause 2 freezes the whole
  surface and carries the lifetime rule (DEBT-2, DEBT-1); §4.1 fixes the two fields and the
  document's C form (ruling 28, DEBT-5). Nothing normative was removed or weakened — every
  edit tightens the constraint on PDA-ABI rather than bending the oracle toward the code. The
  only stretch beyond the `Files-to-touch` note is §4.1's `IsPayloadBound(0)` sentence, which
  is the design's own residue 13 and benign.
- **Public surface is exactly 5** — `ProviderSelector`, `ProviderConfig`, `ProviderRegistry`,
  `ProviderRegistry::Factory`, `ProviderRegistry::PathResolver`. Nothing else leaked into a
  public header; only `provider.hpp` and the new header changed under `include/`.
- **The link line is the machine check it claims to be**: `conformance_registry` names
  `fletcher-pubsub::fletcher-pubsub` + GTest, no transport SDK, no `conformance_support`, no
  `RESOURCE_LOCK`.

---

# RECORD (fix in place; not a fix cycle)

1. **`pubsub/include/fletcher/pubsub/provider.hpp` (the block replacing lines 68-70)
   overclaims in the present tense.** "Provider-specific configuration (e.g. QoS) **is
   supplied** when the provider is created, through `ProviderConfig`" — no provider in this
   tree takes a `ProviderConfig`; Fast DDS still takes `FastDDSProviderOptions` and XRCE
   `XrceConfig` until PDA-DEC-6/7, as the same paragraph's last sentence admits. DEBT-10(b)
   asked for an update; the update states the end state as current. One-line fix (future
   tense, or "at the registry seam").
2. **Budget +1069/−20 against declared +640/−25 = +67%,** above the 50% threshold. Counted:
   header 251 (182 comment / 48 code), impl 176 (25 / 138), suite 503 (91 / 345), spec +41,
   README +47, harness CMake +27, design doc +15, `pubsub/CMakeLists.txt` +2, `provider.hpp`
   +7. The account ("the header's normative documentation plus a 12-entry suite where the
   design named 6") names the two largest contributors correctly — the six extra entries are
   ~170 lines and the header's comments are 182 — but omits a third: ~115 lines of
   DEBT-driven documentation ripple (spec, README, CMake comments) the design budgeted at a
   fraction. **Remedy: nothing.** Executable product code is 186 lines; four of the six extra
   entries discharge DEBT-4, DEBT-5 and the design's own rung-2 items, so consolidation would
   cost a guard. Record it as the round's third under-costing.
3. `Files-to-touch` lists `.claude/runbook.PDA-DEC.config.md`; that file is untracked and
   already carried `-R 'Registry\.'` before `94ae4bc`. The item touched 9 files, not 10.
4. `plans/PDA-DEC-4-provider-registry.md` was edited inside the implementation commit
   (forbidden case 5, P2, Risks, `Files-to-touch`). The edits match DEBT-6/7/10a exactly, so
   this is the sanctioned fold-in rather than a design bent to fit code — noted because a
   design doc moving inside a `feat(` commit normally is not.
5. Design §5 says "`document` is **copied** and handed to the factory"; the implementation
   forwards `const ProviderConfig&` without copying, which is what the header and §4.2's C
   form actually specify. The design prose is the loose one, not the code.
6. **The working tree was not stable during this review.** An untracked
   `plans/reviews/PDA-DEC-4-codereview.md` appeared mid-session and
   `pubsub/src/provider_registry.cpp` briefly contained a `// MUTATION M2` edit — a
   concurrent step-4b agent mutating in place. All findings above were derived from
   `git show aea67f0:` blobs in an isolated build tree. Process note: if a mutating reviewer
   exits uncleanly it leaves the tree mutated.
