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
