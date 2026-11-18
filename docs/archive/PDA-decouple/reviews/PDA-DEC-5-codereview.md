# PDA-DEC-5 — code review (step 4b, independent)

**Diff base:** `9d8921b..29fac8b` · branch `feature/protocol-driver-abi` · 8 files, **+440/−64**
**Reviewer built:** isolated worktree at `/c/tmp/pda5rev` (removed); `conan create` core + pubsub,
`pubsub-conformance` with `FLETCHER_CONFORMANCE_XRCE=OFF`, and the `gateway-end-to-end` harness
binaries rebuilt from this commit before `npm test`.

**Counts: 0 blocking · 2 should-fix · 7 nits.**

## Measured, not taken on trust

| Run | Result |
|---|---|
| `ctest -R 'Registry\.'` (baseline) | **17/17 pass** |
| **M4** — `ParseSchemaCarriage` parses the key but maps `carried` → `kAsDeclared` | **`InProcessCarriageComesFromTheDocument` RED, `InProcessResolvesAsABuiltIn` GREEN**, 16/17. The orthogonality claim is **re-derived**: neither test alone proves the mode comes from the document; the pair does. |
| Gateway refusals, post-change binary | `bogus` → exit **2**, `no built-in provider named "bogus"; available: fastdds, inprocess`. `./nope.so` → exit **2**, `…was read as a driver path… This build cannot load drivers…`. `""` → exit **2**. |
| `npm test` (gateway harness rebuilt from this commit) | **24/24 pass**, 17 in `end-to-end.test.ts` |

The two TS refusal cases are genuine staleness detectors. The pre-change `main.cpp` has exactly one
refusal string, `unknown provider: %s (expected inprocess|fastdds)`; it contains neither
`no built-in provider named`, nor `available:`, nor `cannot load drivers`. No pre-PDA-DEC-5
`gateway.exe` can satisfy either case. `TEST_PORT+8` (19099) collides with nothing — `TEST_PORT`
(19091) and `TEST_PORT+3` (19094) are the only other bound ports, `+6`/`+7` are never bound, and
the new `describe` is declared before both `describe.each` contexts, whose gateways are started by
their own `beforeAll` and therefore never coexist with it.

Lifetime composes correctly with PDA-DEC-4. `RegisterInProcessProvider` installs a capture-free
lambda; `Create` anchors `Anchor{shared_ptr<Factory>, provider}` and hands back an aliasing handle.
No cycle (the factory does not reach the provider), no double ownership, and the gateway's
`ProviderRegistry registry` being a block-scoped local destroyed before `provider` is used is
precisely the case the anchor exists for. The built-in path is byte-identical to any other factory
path — `MakeProvider` in `registry.cpp` is base-typed throughout and no test downcasts.

Nothing in-tree still names the retired forms (`git grep SchemaCarriage` outside `plans/` hits only
the anonymous-namespace enum in `in_process_provider.cpp` and the harness's unrelated
`conformance::SchemaMode`). `inprocess_carrying_main.cpp` is converted, not weakened: same subject
name, same `SchemaMode::kCarried`, same instantiation — only the construction argument changed.

---

## should-fix

### 1. The CRLF strip is asserted by nothing — mutation verified silent (confidence: high)

`pubsub/src/in_process_provider.cpp:73-75`

    if (!entry.empty() && entry.back() == '\r') {
        entry.pop_back();
    }

The comment justifies this by a named hazard: *"H2: a document written on Windows is CRLF, and the
same text must mean the same thing on every platform."* I deleted those three lines, re-ran
`conan create pubsub` and the full harness: **17/17 Registry entries still pass.** The full
non-XRCE conformance run cannot see it either — the only in-tree carried document is the LF-free
`"schema_carriage=carried"` in `inprocess_carrying_main.cpp`.

So the one piece of tolerance the reader went out of its way to add is the one piece with no guard.
Every document in the test set is a single LF-free line: no test covers CRLF, a blank line, a
trailing newline, an empty value (`schema_carriage=`), leading/trailing whitespace, or a
NUL-bearing document. The consequence of the regression is a loud refusal rather than a wrong
answer, which is why this is not blocking — but "documents authored on Windows work" is currently
a claim, not a measurement. One extra `refuse`-style row and one accepting row
(`"schema_carriage=carried\r\n"` → carried) closes it.

Related, and the same three lines: the tolerance is asymmetric. `key=value\r` is accepted,
`key\r=value` is refused with a message that renders the CR raw. Nothing wrong follows from it, but
a partial normalisation that is untested and one-sided is machinery earning less than it costs.

### 2. `QuoteEntry` is weaker than the tree's own `Quoted`, and truncates on a byte §4.2 sanctions (confidence: high)

`pubsub/src/in_process_provider.cpp:45-49`

    std::string QuoteEntry(const std::string& entry) {
        std::ostringstream out;
        out << '"' << entry << '"';
        return out.str();
    }

`ProviderConfig::document` is documented in `provider_registry.hpp` as *"a pointer and a length …
the **length authoritative** — the bytes may contain NUL."* A document holding a NUL therefore
reaches this reader legitimately. Matching is safe (`std::string` compares by length), so no wrong
answer — but the refusal message is built by concatenation into a `std::string`, handed to
`PubSubError`'s `std::runtime_error` base, and read back through `what()` → `c_str()`. The operator
sees `InProcessPubSubProvider: unknown document key: "` and nothing else. Same for a stray CR
*inside* an entry, which renders raw and hides the exact character that must be deleted — the
hazard `provider_registry.cpp`'s `Quoted` was written to prevent, with a measured argument, three
files away.

**In the forbidding direction, which is the better fix:** mirror `ProviderSelector::Parse` and
refuse a document containing a NUL at the door, once, in one line. Then the quoting question
disappears, `QuoteEntry` collapses to plain concatenation, and the new `<sstream>` include comes
back out of the provider TU — which makes the "dependency-free, nothing beyond `<string>`" claim in
the function's own comment strictly true instead of nearly true. As it stands the helper carries a
five-line comment defending its existence while doing less than a `+` would.

---

## Nits (one line each)

- `while (start <= document.size())` is behaviourally identical to `<` on every input (the extra iteration always yields an empty entry and `continue`s); the `<=` invites a reader to hunt for a case that is not there.
- `refuse("schema_carriage", "schema_carriage", "an entry with no '='")` — the `Mentions` half is near-vacuous: `schema_carriage` appears verbatim in two other refusal templates, so only the `kInvalidArgument` half of that row discriminates.
- `MakeSchema` (`registry.cpp`) checks the return of `ArrowSchemaSetTypeStruct` but not of `ArrowSchemaSetName` / `ArrowSchemaSetType`.
- `spawnGatewayExpectingExit` resolves on `'exit'`, not `'close'`; Node only guarantees stdio is drained by `'close'`. I could not reproduce a miss (0/500 across two shapes, including a deliberately busy parent), so this is latent rather than observed — `'close'` costs nothing.
- `spawnGatewayExpectingExit` installs no `'error'` handler; a spawn failure would hang to the vitest timeout rather than name itself (`findGatewayBinary` makes this near-unreachable).
- `std::bad_alloc` out of `Register` / `Create` escapes `main` untyped (the `catch` is `const PubSubError&` only) — pre-existing and OOM-only, disclosed in `provider_registry.hpp`.
- `in_process_provider.hpp` now includes `provider_registry.hpp`, so every consumer of the loopback header transitively gains the registry's surface; harmless within one library, worth knowing.

---

## Numbers — counted, not taken from the account

`git diff --numstat` gives **+440/−64** against a declared **+270/−60** — 63% over on adds.
Classifying every added line:

| file | code | comment | blank |
|---|---|---|---|
| `in_process_provider.cpp` (+106) | 66 | 30 | 10 |
| `registry.cpp` (+155) | 91 | 47 | 17 |
| `in_process_provider.hpp` (+24) | 3 | 20 | 1 |
| `gateway/src/main.cpp` (+33) | 16 | 16 | 1 |
| `end-to-end.test.ts` (+73) | 45 | 24 | 4 |
| docs + README (+48) | — | 48 | — |

**222 code lines, 185 comment lines, 33 blank** (plus the 1-line subject conversion). The account
holds: the overrun is prose and test bulk, not extra machinery. The document reader itself is ~40
code lines against "~25 by design" — proportionate, and five of those lines are the five distinct
typed refusals, which is where lines should be. No file simplification is called for on volume
grounds; the one place volume is not carrying weight is `QuoteEntry` (finding 2).

**Public surface, strictest count: 0, not −1.** Retired: the name
`InProcessPubSubProvider::SchemaCarriage` and the overload `InProcessPubSubProvider(SchemaCarriage)`.
Added: the name `RegisterInProcessProvider` and the overload
`InProcessPubSubProvider(const ProviderConfig&)`. Counting names → −1/+1 = **0**. Counting
declarations → −2/+2 = **0**. The plan reached −1 by charging the retired constructor as a
retirement while not crediting the replacement constructor as an addition. Either way the item does
not grow the surface, which is the substance of the claim.

---

## RECORD:

- `in_process_provider.hpp:62` still says "In `kCarried` a declaration with no schema is refused" — `kCarried` is no longer a public name.
- The task brief describes three TS cases "impossible to pass against a stale `gateway.exe`"; the file's own comment correctly claims two (the READY case is a positive control, not a detector).
- The account gives docs/README as +46; `--numstat` gives +48 (23 spec + 25 README).

---

# Re-check — fix cycle 1 (2026-09-02)

**Range:** `29fac8b..0f6dbe3` (`a995f41` the fix, `243cea1` + `0f6dbe3` record-only).
**Reviewer built:** isolated worktree at `/c/tmp/pda5rc2` (removed, tree left clean); `conan create`
core + pubsub, conformance harness with `FLETCHER_CONFORMANCE_XRCE=OFF`, gateway harness rebuilt
from HEAD. Shared Conan cache restored to pristine `fletcher-pubsub` and re-verified green after the
last mutation.

**Both of my should-fix findings are CLOSED.** One new should-fix (a survivor mutation), two nits,
one RECORD line.

## Confirmations asked for

| Claim | Measured |
|---|---|
| `Registry.` 19/19 | **19/19 pass** |
| Non-XRCE conformance 80/80 | **80/80 pass**, 28.9 s, no intermittents |
| Gateway 24/24, binary postdating the cycle | **24/24**; my own `gateway.exe` built **2026-09-02T11:16:24** |
| No new link dependency | **Confirmed** — `git diff --name-only 29fac8b..HEAD` touches no `CMakeLists.txt` and no `conanfile.py`; the TU's include set *shrank* (`<sstream>` gone) |
| `Registry.InProcessResolvesAsABuiltIn` green and unchanged in meaning | **Confirmed** — `registry.cpp` is `98 / 0` in `--numstat`, purely additive; the forcing test's body is byte-identical |
| Record fixes | All four correct, verified in the diff. The brief now reads "**0 at its strictest** (+2/−2)"; the design's false `carried`-mode mutation claim is retracted *and* explains itself; the README scoping is narrowed to "the mutations in the list above"; `in_process_provider.hpp` no longer names `kCarried` |

## Finding 1 (CRLF / tolerance rules unguarded) — CLOSED

Re-derived by building. Deleted the three-line CR-strip block, `conan create pubsub`, rebuilt the
harness:

    === MUT-A: delete the \r strip
    Registry.InProcessDocumentToleratesCrlfAndBlankLines ... ***Failed
    95% tests passed, 1 tests failed out of 19

Exactly one entry red, and it is the new one. The guard is faithful: `is_carried` witnesses the mode
through a refusal (`kTopicNotDeclared`) that is unreachable from `as_declared`, needs no accessor,
and each of its four calls constructs a **fresh** provider, so the composed case is not riding on a
previous one. The two new `refuse` rows close the empty-value and no-trimming gaps I listed. Design
§2's tolerance rules are now measurements rather than claims.

## Finding 2 (NUL) — CLOSED, and the truncation reproduced

Taken in the forbidding direction, which is the right call. Deleting the check reproduces the
failure mode **exactly** as described — I captured the message myself:

    the refusal does not say why: InProcessPubSubProvider: unknown value for
    'schema_carriage': "schema_carriage=carried

Stops dead at the NUL: no `tail`, no closing quote. Note the first `EXPECT_EQ` still passed under
that mutation (`kInvalidArgument` either way), which confirms the original grading — a diagnostic
defect, not a wrong answer — and is exactly why the `Mentions(message, "NUL")` /
`Mentions(message, "offset 23")` pair, not the status assertion, is what earns its place here.

Answering the three specific questions:

- **Is the refusal complete?** Yes. `ParseSchemaCarriage` has exactly one caller —
  `InProcessPubSubProvider(const ProviderConfig&)`'s member-init list — and `config.document` is the
  only route by which a NUL can reach any of the concatenated messages. There is no second entry
  point, no lazy re-read, and the reader keeps no copy of the document. Every path that can see a
  NUL passes the check first, because the check is the function's first statement.
- **Is the offset correct?** Yes. `document.find('\0')` is a byte index into the same buffer the
  message describes, with no adjustment applied — no off-by-one available. `schema_carriage=carried`
  is 23 bytes and the test pins `offset 23`; reporting `nul + 1` would redden it.
- **Did losing `QuoteEntry`'s stream lose escaping something relied on?** No. `QuoteEntry` never
  escaped anything — the `ostringstream` was pure ceremony around two quote characters, and the
  replacement concatenation is byte-identical in output. `provider_registry.cpp`'s `Quoted`, which
  *does* escape and whose injectivity argument is load-bearing, is untouched and still the only
  escaping helper. The comment now correctly says the NUL check is what makes plain concatenation
  safe here.

**Residual, deliberately not raised again:** a non-NUL control byte inside an entry (e.g. a CR in
the middle of a key) still renders raw in the refusal, so on a terminal it can overwrite the start
of the message. It cannot truncate and it cannot be reached except from a document the operator
wrote. Below the bar.

## NEW — should-fix: a survivor the 19 entries still miss (confidence: high, verified by building)

**MUT-C — the `inprocess` factory memoises one instance per registry:**

    void RegisterInProcessProvider(ProviderRegistry& registry) {
        auto memo = std::make_shared<std::shared_ptr<InProcessPubSubProvider>>();
        registry.Register("inprocess", [memo](const ProviderConfig& config) {
            if (!*memo) *memo = std::make_shared<InProcessPubSubProvider>(config);
            return std::static_pointer_cast<PubSubProvider>(*memo);
        });
    }

**Result: `Registry.` 19/19 green, and the full non-XRCE conformance run 80/80 green.** Nothing in
the tree sees it.

Under that mutation the second and every later `Create` on one registry returns the **first**
caller's instance, **in the first caller's mode**, silently ignoring its own `config.document`. That
is a wrong answer with no signal: a host that builds one registry and makes an `as_declared`
provider for one subsystem and a `carried` one for another gets one provider in one mode and no
error. It is the precise hazard shape `provider_registry.hpp` already warns about in prose
("`Create` never caches … several instances of one provider with different configurations are
ordinary") — the registry enforces it on *its* side, and
`Registry.EachCreateReturnsAnIndependentInstance` pins it, but both only for the *probe* factories.
The one built-in factory the tree ships is unpinned, and it is the seat PDA-ABI is about to reach
through by name. A mutable cache hosted at a factory seat is also the exact construct that produced
two blocking bugs earlier in this round.

The gap is structural and one sentence long: **no test creates two providers from one registry with
two different documents.** All three original PDA-DEC-5 tests use one registry and one `Create`
each; the new tolerance test does make four, but all four ask for `carried`, so a memo answers them
all correctly.

I verified the fix as well as the hole. Adding this to the end of
`Registry.InProcessCarriageComesFromTheDocument` (~8 lines) is **red under MUT-C and green on
pristine source** — both runs executed:

    std::shared_ptr<PubSubProvider> plain = MakeProvider(registry, "inprocess", ProviderConfig{});
    EXPECT_NE(plain.get(), provider.get())
        << "two Creates on one registry returned the same instance";
    EXPECT_NO_THROW(PublishRowTo(*plain, {"registry", "second-create"}, 0x1c))
        << "the second Create inherited the first's carried mode";

It costs nothing — the registry and the carrying provider are already in scope — and it converts
"the mode comes from *this* document" from a one-shot into a per-call property, which is what the
test's own name claims.

## The `--help` change — judged

**The conclusion is right; the reason recorded for it is not the binding one.** Dropping the
enumeration is the correct call and I would not change the code.

The cited constraint is wrong, though. The `static_assert` at the foot of `provider_registry.hpp`
pins `decltype(&ProviderRegistry::Create)` and nothing else; adding a `Names()` accessor would
compile straight through it. What actually forbids the accessor is the *prose* freeze two doc
comments above — "So is the rest of this class's public surface: `Create`, `Register`,
`SetPathResolver`, **and nothing else**" — which is stronger than the assert and does cover it.
Worth correcting so nobody later discovers the assert does not fire and concludes the surface is
open.

And there **is** a way to derive the list without touching the frozen surface, which the reasoning
should acknowledge before dismissing: the gateway is itself the code that registers both names, five
lines below. A `std::vector<std::string> RegisterBuiltIns(ProviderRegistry&)` returning the names it
just registered would give `--help` an enumeration that cannot drift, with `ProviderRegistry`
untouched. Its real cost is not the freeze but ordering — `--help` is printed inside `ParseArgs`,
which `std::exit(0)`s before any registry exists, so taking that route means building the registry
before parsing arguments. For a two-name list that is a poor trade, and "`--provider NAME`, the
refusal is the authority" is the better answer. Sound decision, wrong premise stated for it.

## Nits (one line each)

- Second survivor, lower value: allowing a duplicate `schema_carriage` when the two values are *identical* passes all 19 — the duplicate row uses `as_declared` then `carried`, so only the disagreeing half of "a duplicate key is refused" is pinned.
- Nothing in either TS harness asserts `--help` or `--version` output, so the new usage line's promise ("exits 2 naming what this build supports") is backed only indirectly, by the `available:` assertion in the refusal case.

## RECORD:

- `integration-tests/pubsub-conformance/README.md:362` still says the registry binary has "17 entries" — it now has 19, and neither `InProcessDocumentToleratesCrlfAndBlankLines` nor `InProcessRefusesADocumentContainingANul` appears in that section's per-entry table or its mutation list.
