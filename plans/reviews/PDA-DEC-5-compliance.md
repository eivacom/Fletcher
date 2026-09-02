# PDA-DEC-5 — compliance review (step 4a)

Diff `9d8921b..29fac8b`, 8 files, **+440/−64**. Oracles: `plans/PDA-DEC-5-inprocess-builtin.md`,
`plans/PDA-DEC-5-brief.md`, `plans/reviews/PDA-DEC-5-design-review.md` (+ 5 DEBT),
`plans/PDA-DEC-rulings.md` (28 entries), `plans/PDA-decouple-locked-decisions.md` 3/4/8/14,
`docs/pubsub-interface-spec.md` §4/§4.1/§4.2/§7.

**Verdict: ISSUES (3).** The structural half of the item is sound and I verified it by
execution, not by reading. Three conformance defects below.

---

## What I verified by running, not reading

All builds in an isolated scratch project that links the installed `fletcher-pubsub` package
plus a private copy of `pubsub/src/in_process_provider.cpp`, so the shared checkout and the
shared build trees were never mutated.

- **Control:** `conformance_registry.exe` (built 10:28:17, post-change) — **17/17 green**;
  the README's "17 entries" is correct. The scratch probe reproduces the same 17 green.
- **M4 re-derived, executed.** Mutation `return carriage;` → `return SchemaCarriage::kAsDeclared;`
  (parse the key, hard-code the mode). Result: `InProcessResolvesAsABuiltIn` **GREEN**,
  `InProcessRefusesAnUnrecognisedDocumentEntry` **GREEN**,
  `InProcessCarriageComesFromTheDocument` **RED** at `registry.cpp:560` (`kOk` where
  `kTopicNotDeclared` was required). The orthogonality claim holds exactly as the design and the
  README state it: neither test alone is sufficient.
- **M2 re-derived, executed.** Factory default `kCarried`: forcing test **RED at
  `registry.cpp:525`**, the schema-arrival assertion — precisely where the README's mutation
  paragraph says it reddens. The other two stay green. That wording is accurate, not merely
  plausible.
- **The gateway staleness detector is real.** Ran both binaries directly:
  - post-change (`integration-tests/gateway-end-to-end/build/gateway_build/Release/gateway.exe`,
    10:30:33): `--provider bogus` → exit 2, `no built-in provider named "bogus"; available:
    fastdds, inprocess`; `--provider ./nope.so` → exit 2, `… This build cannot load drivers …`
  - pre-change (`gateway/build/Release/gateway.exe`, 2026-09-01 20:51): both →
    `unknown provider: … (expected inprocess|fastdds)`.

  Neither pinned substring (`no built-in provider named`, `available:`, `cannot load drivers`)
  is emittable by the old binary. **DEBT-2 is discharged and measured.** Case 3
  (`--provider inprocess` → READY) is a positive control a stale binary *would* pass — that is
  the design's stated shape and it does not weaken the block, because the two detectors sit in
  the same jest file as the `describe.each` battery: a green `npm test` now *entails* a
  post-change binary, which is what makes the "unchanged behaviour" battery mean something
  again. The `fastdds` closure is covered by the same run, via the battery's `fastdds` context.
- **The document key is genuinely structural.** `SchemaCarriage` appears nowhere outside
  `pubsub/src/in_process_provider.cpp` (anonymous namespace) — not in any header, test, subject,
  the gateway, or a test-only back door. The only constructor is
  `explicit InProcessPubSubProvider(const ProviderConfig& = {})`; `ProviderConfig` is frozen at
  `{max_payload_bytes, domain_id, document}`; the registered factory constructs only from `c`.
  All other in-tree construction sites default-construct; the one explicit site
  (`inprocess_carrying_main.cpp`) is converted, not retired. C2-9 / PDA-DEC-4 §5 discharged
  structurally. Validation runs *before* `Impl` exists, so rung-1 case 2 holds.
- **Decision 8's boundary held.** The reader is a single static function in the provider TU,
  reachable only from that constructor, not promoted to a header or to `pubsub/src/internal/`,
  depending on nothing beyond `<string>`/`<sstream>`, and deliberately not sharing
  `provider_registry.cpp`'s `Quoted`. It throws `PubSubError(kInvalidArgument)` — never a `std::`
  type, never `kInternal` — and `InProcessRefusesAnUnrecognisedDocumentEntry` asserts the status,
  not "it threw". P1's converse obligation met. No library, no dependency, no CMake change.
- **Scope clean.** Exactly the 8 declared `Files-to-touch`; no new files (as declared); no
  `extern "C"` / C header / loader (14); `provider.hpp` untouched (4); `Create`'s signature
  untouched and the `static_assert` at `provider_registry.hpp:290-295` intact; no provider
  outside `pubsub/` touched; `FastDDSProviderOptions` / `XrceConfig` left whole for PDA-DEC-6/7;
  `gateway-fastdds-ts` untouched. After `Create` the gateway names no concrete provider type and
  holds one `shared_ptr<PubSubProvider>` (3). Nothing here must be undone by a later item except
  the `fastdds` closure body, as designed.
- **Files-to-delete converse — "what survived that should not?"** Public `SchemaCarriage` and its
  constructor: gone. The gateway `if/else` construction chain: gone. The gateway name validation
  and its message: gone — and no in-tree harness, script or CI file asserts the old text (P4
  holds; the only surviving occurrences are the new TS comments explaining why the old text
  cannot pass). One construct did survive: finding 2.

---

## Finding 1 — BLOCKING. Two of the design's own document rules are asserted by nothing, proved by mutation

Design §2 commits to two rules beyond the four refusals: *"A trailing `\r` on an entry is
stripped"* (handled residue **H2**, whose stated justification is the owner's 2026-09-02 ruling
"the same rule in every build") and *"An empty entry (`\n\n`, a trailing newline) is skipped"*.
Both are implemented. **Neither is guarded.** Executed against the full 17-entry suite:

| Mutation | Result |
|---|---|
| Delete the `if (!entry.empty() && entry.back() == '\r') entry.pop_back();` block | **17/17 GREEN** |
| Refuse an empty entry when the document is non-empty (so a trailing newline is fatal) | **17/17 GREEN** |

Under the first mutation a CRLF-authored document — the normal artefact on this project's
primary platform — is refused `kInvalidArgument` on Windows and accepted on Linux: exactly the
platform split H2 cites the ruling to prevent, landing silently. Under the second, a document
with a trailing newline stops working.

This is the same class the design review itself raised as **DEBT-3** ("or the duplicate-key rule
is the one refusal asserted by nothing"); applied consistently, `\r` and the empty entry are two
more rules asserted by nothing, and one of them is backed by an owner ruling rather than by
convenience. It is also the round's most-repeated defect (the PDA-DEC-4 log records ten guards
that asserted nothing). Cost to close: two positive cases — `"schema_carriage=carried\r\n"` and
`"schema_carriage=carried\n"` must both yield a carrying instance.

Related, same root: the §4.1 paragraph this diff adds to the spec describes the format as
"a sequence of `\n`-separated `key=value` entries" and omits both rules, so the oracle does not
carry them either.

## Finding 2 — the gateway's provider vocabulary survived in two places, and `Files-to-touch` said it would not

Rung-1 forbidden case 4: *"The gateway holds no list of valid names; the registry's table is the
only one."* `Files-to-touch` names `gateway/src/main.cpp` **"(registry + one `Create`; the
`if`-chain and the validation out; `--help`/comment text)"**. The `--help` / comment text was
**not** touched. Still present in `gateway/src/main.cpp`:

- line ~83: `"[--provider inprocess|fastdds] …"` in the `--help` usage string;
- lines 14-15 and 36-38: the file-header CLI block and the "Provider note", both enumerating
  `inprocess` / `fastdds` and asserting "Both are compiled in; the switch selects between them at
  runtime."

Neither can refuse anything, so this is weaker than the deleted validation — but it is a second
and third copy of the one vocabulary this item exists to centralise, it is the surface an
operator actually reads, and the design ordered it edited. The registry's own refusal already
prints the authoritative list (`available: fastdds, inprocess`), so the help text can simply stop
enumerating. Judged against the design rather than against the code as found, this is a
`Files-to-touch` line not honoured plus a rung-1 case tolerated rather than forbidden.

## Finding 3 — DEBT-4's disclosure half is undischarged while brief and design still claim "observably unchanged"

DEBT-4 has two halves: *(a)* put `Parse`, both `Register`s and `Create` inside one `try`, and
*(b)* disclose that a Fast DDS construction failure changes from **uncaught escape from `main` +
abort** to **`PubSubError` caught, message, exit 2**, because "'observably unchanged' should say
it". Half (a) landed exactly. Half (b) is **nowhere** — not in `main.cpp`'s comments (which record
DEBT-5 but not this), not in the README, not in the spec, not in the brief's risk list. The
brief's forcing test still reads "plus the gateway's battery over `--provider inprocess`,
unchanged", and the design still calls this item's other half "observably unchanged", with the
one behaviour change that is *not* observably unchanged left unstated. One line, anywhere
durable.

---

## RECORD (PM fixes in place; not a fix cycle)

- `plans/PDA-DEC-5-inprocess-builtin.md:222-223` still states DEBT-1's **false** mutation
  verbatim — "register `inprocess` with the `carried` mode → the schema/publish cases go red …
  and in the right mode". The review proved it does not redden; the debt said "do not leave the
  false claim standing". Nothing in the landed tree repeats it (the TS comments and the README's
  mutation paragraph both correctly attribute the mode proof to
  `Registry.InProcessCarriageComesFromTheDocument`), so this is the last copy.
- **Budget: +440/−64 vs declared +270/−60 — 63% over the declared adds.** Counted; the
  implementer's account is directionally right but mis-itemised. Added lines per file:
  `registry.cpp` 155 (91 code / 47 comment); provider `.cpp`+`.hpp` **130** (69 code /
  50 comment) against ~80 estimated — **the single largest overrun, and it is doc-comment, not
  mutation evidence**; TS 73 (45 / 24); docs + README 48 against ~20; gateway 33 (16 / 16).
  137 of the 440 added lines are comments inside code files and 48 are prose docs = **42%
  prose**. No new mechanism, no file outside `Files-to-touch`, no new dependency → **over budget,
  not scope creep**.
- **Public surface:** design and brief say net **−1** (+1 `RegisterInProcessProvider`, −2 the enum
  and the `SchemaCarriage` constructor). On the **strictest count the answer is 0**: the replacing
  `explicit InProcessPubSubProvider(const ProviderConfig&)` constructor is itself an addition, so
  +2/−2. Design review §E already said this; the brief's "net −1" carries no caveat. Either count
  is inside the ≤3 cap.
- `integration-tests/pubsub-conformance/README.md:438` — "Five of these mutations left the suite
  fully green when the entries were first written" is a PDA-DEC-4 sentence that now trails the
  newly-inserted PDA-DEC-5 mutation paragraph, so "these mutations" reads as covering the five
  PDA-DEC-5 ones too. Needs a PDA-DEC-4 scoping word.
- `pubsub/include/fletcher/pubsub/in_process_provider.hpp:63` — "In `kCarried` a declaration with
  no schema is refused" names an enumerator that is no longer public; the rest of the header
  spells the modes `as_declared` / `carried`.
- `integration-tests/gateway-fastdds-ts/build/gateway_build/Release/gateway.exe` is dated
  2026-09-01 22:05, i.e. **pre-change**. If that harness was reported green as part of this
  item's evidence, that green was measured against a pre-change gateway and says nothing about
  the change. Not a defect in the diff (the harness is correctly unmodified, and the `fastdds`
  registry path *is* covered by the rebuilt `gateway-end-to-end` `fastdds` context) — an evidence
  caveat only if it was cited.

## Not findings

- The spec amendments are in charter. §7's parenthetical swap is exactly what the design
  authorised and is editorial; §4 clause 4 and §4.1 use the "**As landed** (PDA-DEC-x)" idiom the
  document already established for PDA-DEC-4, and neither changes a normative clause. §4.1's
  paragraph is longer than the "document example" the design promised, but it is accurate and
  sits inside a `Files-to-touch` entry.
- DEBT-2, DEBT-3 and DEBT-5 are discharged as written (pinned wording plus its own port
  `TEST_PORT+8`; the duplicate-key input with an explicitly-passed expected quote; the
  no-document-route note in `main.cpp`).
- The loopback ignoring `max_payload_bytes` (H1) is the design's disclosed residue, unchanged.
