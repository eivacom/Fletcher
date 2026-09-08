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

---

# Re-check — 2026-09-02, fix cycle 1 (`29fac8b..0f6dbe3`)

Scope: my three findings, the coordinator's two questions (the `--help` ruling; NUL versus
the seam sanction), and the still-true list. Everything below that says "executed" was run in
an isolated scratch project that links the installed `fletcher-pubsub` package plus a private
copy of `pubsub/src/in_process_provider.cpp`; the shared checkout and the shared build trees
were not mutated.

**Verdict: all three findings CLOSED. Nothing blocking.** One seam-contract question answered
in the implementation's favour, with evidence. RECORD list at the end — including four stale
README numbers the fix cycle created.

## Finding 1 (BLOCKING) — CLOSED, re-derived by mutation

Control at HEAD: **19/19 green**. Both halves of the finding re-derived, each executed:

| Mutation | Result at HEAD |
|---|---|
| Delete the `if (!entry.empty() && entry.back() == '\r') entry.pop_back();` block | `InProcessDocumentToleratesCrlfAndBlankLines` **RED**, other 18 green — throws `unknown value for 'schema_carriage'` on `"carried\r"` |
| Neuter the empty-entry skip (`&& document.empty()`) | same test **RED**, other 18 green — throws `document entry with no '=': ""` on the blank line |

Both were 17/17 GREEN before this cycle, so the guard is genuinely new signal, not a
restatement. The test's own comment predicted the exact failure mode (the exception escapes
`MakeProvider` rather than a clean `EXPECT` returning false) and that is what happens. The
composed case (`"\r\nschema_carriage=carried\r\n\r\n"`) covers an interior blank line, not
only edges — stronger than the finding asked for. The three added `refuse` rows (empty value,
leading whitespace, trailing whitespace) pin "nothing else is trimmed", which was previously
prose in design §2 only. §4.1 of the spec now carries both tolerance rules, so the oracle no
longer omits them either.

## Finding 2 — CLOSED, and the ruling the coordinator asked for

Verified against the post-fix binary (11:02:54): `--help` prints `[--provider NAME]` plus one
sentence, with no enumeration; `--provider bogus` still prints
`no built-in provider named "bogus"; available: fastdds, inprocess`. Both header comment
blocks now state what this file registers *and disclaim being the authority for it*. Rung-1
forbidden case 4 is satisfied: the gateway holds no list an operator or a third provider would
have to keep in sync.

**Ruling on "would a name-listing accessor breach the freeze?" — the implementer reached the
right answer, and the header (not the `static_assert`) is the authority for it.**

1. The `static_assert` at `provider_registry.hpp:287-295` pins **`Create`'s signature only**.
   A `Names()` accessor would not trip it. So the reason as stated — "adding one would breach
   the freeze PDA-DEC-4 pinned with a `static_assert`" — is slightly overbroad, and a later
   implementer citing the `static_assert` for something it does not cover is how a machine
   check gets believed past its range.
2. The real authority is the prose freeze immediately above it, which **is** normative and
   **is** breached: *"So is the rest of this class's public surface: `Create`, `Register`,
   `SetPathResolver`, **and nothing else**."* (`provider_registry.hpp:250-253`). Adding a
   fourth method is a stop-and-ask against the spec, not an edit — and this item's own premise
   **P2** independently makes "this item adds a registry method" a STOP-AND-ASK. Doing it
   inside a fix cycle, unasked, would have been a worse deviation than the finding it closed.
3. **And the accessor is the wrong shape on the merits, which is the answer I would give even
   if it were asked properly.** Once PDA-ABI installs a resolver, the set of *available*
   providers is not enumerable: a path selector names a driver that is not in `factories_`.
   A `Names()` could therefore only ever return the **built-ins**, handing code above the seam
   a way to distinguish built-in from loaded — precisely what locked **decision 3** forbids
   ("built-in versus loaded is invisible above the seam"). A registry whose refusal message
   lists what it has, while exposing no enumeration API, is the shape that keeps that
   invisible. So: not merely out of scope — it should be refused if proposed.

Net: no change owed. If the PM wants the `main.cpp` comment to survive scrutiny, the one word
worth changing is the authority it cites (the header's "and nothing else", not the
`static_assert`). Record-level, listed below.

## Finding 3 — CLOSED

DEBT-4's disclosure landed at `gateway/src/main.cpp:123-130`, immediately above the `try` it
describes, and says the sharp thing rather than the soft one: *"it is NOT the 'observably
unchanged' this item's other half claims to be; said here because it is not said anywhere else
durable."* That is adequate and better placed than `plans/` — it sits where a reader of the
changed code meets it. Grepped the tree: the only surviving occurrences of the phrase are this
disclosure, the debt register and the design review (both records of the debt itself), and
design §H1, where it is a *citation* of the claim in the course of justifying a different
residue, not a fresh assertion. No overclaim survives in the brief or the design.

## New — the NUL refusal versus `provider_registry.hpp`'s explicit sanction

**No contradiction. The refusal ratifies the sanction, and I measured that it does.**

The sanction (`provider_registry.hpp:129-131`, spec §4.1) is about **representation**: the C
form is pointer-plus-length, *the length is authoritative*, and the bytes may contain NUL. It
binds the **seam** — Fletcher must carry those bytes to the provider without truncating. It
does not oblige a provider to *accept* them: §4.2 assigns the format to the provider, §4.1
calls the document "bytes Fletcher transports and does not read", and a provider refusing
content its format cannot represent is rung-2 "refused typed at the door", which is the ladder
this item is required to hold.

The stronger point is that `Registry.InProcessRefusesADocumentContainingANul` is a **live
machine check for the sanction**. Executed: modelling a NUL-truncating boundary between the
seam and the factory (`ParseSchemaCarriage(std::string(config.document.c_str()))`) turns that
test **RED at all three assertions** (`:709` status, `:712` "NUL", `:714` "offset 23") while
the other 18 stay green — because the truncated document is simply `schema_carriage=carried`
and constructs cleanly. Nothing else in the suite catches that. So if any future boundary ever
treats `document` as a C string, this test is what fails. It defends §4.1's clause rather than
eroding it, and PDA-ABI inherits that guard.

One wording risk, not a defect: the new spec sentence *"A document containing an embedded NUL
is refused, mirroring `ProviderSelector::Parse`"* has no explicit subject and sits fifteen
lines below the seam-wide *"the bytes may contain NUL"*. It is inside the "As landed
(PDA-DEC-5)" **loopback** paragraph, so it is correctly scoped in context — but this is the
one document PDA-ABI, BIND and PDA-DEC-6/7 all read, and a reader arriving at it cold could
take it as a seam rule and reproduce the refusal in the Fast DDS or XRCE reader, or worse, in
a C boundary. Naming the subject ("The loopback refuses…") removes the ambiguity. RECORD.

Adjacent observation, non-blocking and 4b's ground rather than mine: the comment justifies
refusing NUL because the diagnostic channel cannot carry it, but `QuoteEntry` still emits every
*other* control byte raw and unescaped — a `\r` in the middle of an entry, for instance, quotes
to something visually identical to a valid entry (I saw exactly that in the mutation run:
`unknown value for 'schema_carriage': "schema_carriage=carried"`). The argument for refusing
NUL is sound on its own terms (truncation, not confusion) and the design never required
escaping — it deliberately declined to share the registry's escaping `Quoted` under decision 8
— so nothing is owed here. Noted only so the reasoning is not later generalised.

## Still true — re-verified at HEAD

`SchemaCarriage` exists in exactly one file, `pubsub/src/in_process_provider.cpp`, in an
anonymous namespace; no header, test, subject, gateway path or back door reaches it, and the
document remains the only route · `provider_registry.hpp` has a **zero-line diff** this cycle,
so `Create`'s signature, the `static_assert` and the three-method freeze are untouched · no
`extern "C"`, no C header, no loader (14) · the reader is still one static function in the
provider TU, unshared and not promoted to a header — and `<sstream>` was **removed**, so its
dependency set shrank to `<string>` (decision 8 held tighter than before) · no CMake or
conanfile change anywhere in the cycle, so no dependency was added · the gateway still makes
one `Create` call, holds one `shared_ptr<PubSubProvider>`, and names no concrete provider type
after it (3) · `PubSubProvider`'s method set untouched (4) · no provider outside `pubsub/`
touched; `FastDDSProviderOptions` / `XrceConfig` whole for PDA-DEC-6/7 · `gateway-fastdds-ts`
untouched.

## RECORD (this cycle) — PM fixes in place

- **The README's Registry-suite header is stale in four ways, all created by this cycle**
  (`integration-tests/pubsub-conformance/README.md:362-364`, `389-391`, `418-436`):
  it still says **"17 entries"** (the suite is **19**); "for all but the **three** PDA-DEC-5
  entries" (now **five**); the entry table has no rows for
  `InProcessDocumentToleratesCrlfAndBlankLines` or `InProcessRefusesADocumentContainingANul`;
  and `InProcessRefusesAnUnrecognisedDocumentEntry`'s row still lists four refusals where it
  now asserts seven. The mutation paragraph likewise does not name the new tolerance, NUL or
  no-trim mutations — which is a pity, because the NUL one (a truncating boundary) is the
  best guard the suite gained. *Note this is the second consecutive item to leave a stale
  count in this same paragraph; PDA-DEC-4's close corrected the same sentence.*
- **The design still says "net −1" for public surface in two places** —
  `plans/PDA-DEC-5-inprocess-builtin.md:252-253` (Risks bullet) and `:300-301` (## Numbers).
  The brief was corrected at `243cea1`; the design's two copies were not. The strictest count
  is **0** (+2/−2).
- `gateway/src/main.cpp:88-91` cites the wrong authority for why the registry has no listing
  accessor. The `static_assert` pins `Create`'s signature only; the binding text is
  `provider_registry.hpp:250-253` ("`Create`, `Register`, `SetPathResolver`, and nothing
  else"). Same conclusion, correct citation.
- `docs/pubsub-interface-spec.md:395-396` — give the NUL sentence an explicit subject so it
  cannot be read as a seam-wide rule fifteen lines under "the bytes may contain NUL".

The four record items the PM reports as already fixed are confirmed fixed: the design's copy of
the false `carried`-mode mutation claim is now a corrected parenthetical naming the review that
disproved it; the brief's surface count reads "0 at its strictest"; the README's "Five of these
mutations" is scoped to the list above it; and the public header no longer names `kCarried`.
