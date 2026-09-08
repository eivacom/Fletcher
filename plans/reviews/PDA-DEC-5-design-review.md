# PDA-DEC-5 — architecture review (cycle 1 of 2)

Design: `plans/PDA-DEC-5-inprocess-builtin.md` (`0068d5a`). Brief:
`plans/PDA-DEC-5-brief.md`.

**Verdict: APPROVE-WITH-DEBT(5).** No BLOCKERs. Two rulings the implementer may
rely on **without stop-and-asking** — premise **P1 does not fire** (§A) and
premise **P3's stop-and-ask does not fire; the provider stays in
`fletcher-pubsub`** (§B). One substantive evidence finding (§C, DEBT-1): a named
mutation on the gateway battery does **not** redden, verified against the
harness.

---

## §A — Ruling on P1: it does **not** fire

`TranslateSeamFailure` (`core/include/fletcher/core/status.hpp:133-150`) opens
with `catch (const PubSubError&) { throw; // already typed — do not re-wrap and
lose the cause }`. A `PubSubError` thrown by a factory therefore reaches
`ProviderRegistry::Create`'s caller with its status intact, and this is not an
inference: `Registry.AFactoryThatFailsIsReportedAsATypedSeamFailure`
(`integration-tests/pubsub-conformance/src/registry.cpp:521-552`) already
registers a `typed-thrower` factory throwing `PubSubError(kTransportFailure)`
and asserts the refusal arrives as `kTransportFailure`, distinct from the
`kInternal` the untyped thrower gets.

So the loopback's bad-document refusal will arrive as `kInvalidArgument`, and
`Registry.InProcessRefusesAnUnrecognisedDocumentEntry` will pass for the right
reason. **Do not raise P1.** The one obligation this leaves on the implementer
is the trivial converse: the document reader must throw `PubSubError`
(`kInvalidArgument`) itself — a `std::invalid_argument` would be mapped to
`kInternal` by that same function, which is the state P1 was worried about. The
design already specifies `PubSubError(kInvalidArgument)`; nothing to change.

## §B — Ruling on P3's fallback: **wrong remedy, and the stop-and-ask does not fire**

The design escalated Brief decision 1 rather than resolving it, and asked review
to rule. Ruling: **a provider reading its own document is honoured in substance
here; a parser does not land "in Fletcher" by the accident of where the loopback
lives; do not move the provider to its own component.**

Grounds, in the order they bind:

1. **§4.2's own words contemplate it.** The clause reads "no provider carrying a
   parser *it cannot afford* on a `<75 KB` Flash target" — a per-provider reader
   is the anticipated shape, and the constraint is on **cost and dependency**,
   not on existence. A ~25-line `std::string` slice over one key is the
   affordable end of that.
2. **The 2026-08-31 configuration ruling says whose job it is**: "an opaque
   JSON/YAML/XML blob **only the driver parses**". The loopback is the driver
   here. It parses. That is compliance, not evasion.
3. **Decision 8's subject is the seam, and the seam still reads nothing.**
   `provider_registry.cpp` copies and forwards (`Create` → factory, verified);
   `provider.hpp`, `publisher`, `subscriber` never touch `document`. The only
   reader is `in_process_provider.cpp`, a provider TU.
4. **PDA-DEC-4 already drew this exact line and it was reviewed.** Its forbidden
   case 5 states the machine check as "no **transport SDK** reachable", and
   explicitly says "Not 'no provider header': `in_process_provider.hpp` is
   *inside* `fletcher-pubsub` and PDA-DEC-5 links this very binary against it"
   (DEBT-7). Reversing that now would be a review re-litigating a landed item.
5. **The fallback would buy nothing.** Moving the loopback to its own component
   relocates the identical 25 lines into a different CMake target. If a parser
   inside `fletcher-pubsub` were the violation, the criterion would be "which
   target the .cpp compiles into", which is not what decision 8 says. It would
   also cost `fletcher-pubsub` its only built-in and force the gateway's default
   path into a new component dependency — strictly worse.

**Scope of this ruling** (the boundary the implementer must not cross): the
reader lives in `in_process_provider.cpp`, is reachable only from the loopback's
constructor, adds no dependency, and is **not** promoted to a header, to
`pubsub/src/internal/`, or to anything a second provider could call. The design
already commits to all four ("no library, no shared parser, no dependency"), so
no change is required. Forward note, not a finding: PDA-DEC-7 gives XRCE the
same `key=value` idiom from its own component — factoring the two into a shared
helper inside `fletcher-pubsub` **would** be Fletcher gaining a config parser,
and is a fresh stop-and-ask if it is ever proposed.

Brief decision 1 option (b) — two registry names, one per mode — is correctly
rejected: it turns a setting into permanent vocabulary and makes one provider's
two modes look like two protocols, against §4's uniformity.

## §C — The gateway half, attacked

**The staleness detector is real, and it is the right instrument.** Verified:
the pre-change binary refuses *both* shapes through one branch
(`gateway/src/main.cpp:89-94`, message `unknown provider: %s (expected
inprocess|fastdds)`, exit 2), so the `./nope.so` case — which must say this
build cannot load drivers (`provider_registry.cpp:198-207`) — cannot pass
against any stale `gateway.exe`. There is no C++ unit test over `ParseArgs`
(`gateway/tests/` holds only the two codec tests), so this TS block really is
the only instrument that exists for the gateway's selection path. Case 3
(`--provider inprocess` → `READY`) is a positive control, not a detector, and
the design does not claim otherwise.

**But one of the three cases is weaker than the design's claim** — DEBT-2. "stderr
naming the registered providers" is satisfied by the *old* text, which also
contains `inprocess` and `fastdds`. Only the new wording (`no built-in provider
named …; available: …`) distinguishes them. The design's protocol — run it red
first — would catch a weak assertion, but the assertion should be pinned so it
does not depend on that.

**And one named mutation is false** — DEBT-1, the substantive finding.
B.1 claims: "register `inprocess` with the `carried` mode → the schema/publish
cases go red, which proves the gateway's provider genuinely comes from the
registry **and in the right mode**." It would not go red. Verified against
`integration-tests/gateway-end-to-end/test/end-to-end.test.ts`:

- Every publish in the battery is preceded by `createTopic(topic, SCHEMA)` —
  lines 225/237, 335/353, 408 — with a real schema, so `kCarried`'s two refusals
  (`CreateTopic` with a null schema, `Publish` to an undeclared topic,
  `in_process_provider.cpp:61-66` and `143-147`) are never reached.
- The one case that subscribes without a declaration ("subscribed response —
  routing only", lines 170-189) asserts the *absence* of `schema`/`schemaIpc`.
  In `kCarried` that subscription gets a **pending** arrival
  (`in_process_provider.cpp:176-186`), so the gateway's `Wait(0ms)` returns
  `kPending`, no `schemaIpc` is attached, and the assertion still passes.

The *other* named mutation — drop the `inprocess` registration → `Create`
refuses `kInvalidArgument` → exit 2 → every in-process context fails in
`beforeAll` — is sound, and it is the one that carries the load-bearing claim
("the gateway's provider genuinely comes from the registry"). So the stage's
behaviour remains provable and this is not a BLOCKER; the mode half of the claim
is simply not something this battery can see, and the mode is proved instead by
`Registry.InProcessCarriageComesFromTheDocument`. Note the gateway's mode is not
pinned by anything today either (it constructs with the defaulted enum), so the
item loses no coverage — but the design should not claim coverage it does not
create. This round's log records six PDA-DEC-4 guards that "asserted nothing …
coverage was thinnest exactly where the code already worked"; a mutation claim
that does not hold is the same defect class one layer up, which is why it is
DEBT-1 rather than a footnote.

**Registration-before-selection (question 4) is sound.** No ordering hazard:
`Register` is total on a fresh registry, the `fastdds` closure body does not run
until `Create` selects it, and nothing reads the selector before both
registrations. It cannot make an unavailable provider look available in this
tree, because `gateway/CMakeLists.txt:70` links the Fast DDS provider
unconditionally — availability really is the link-time fact the design says it
is. Were that link ever made conditional, the registration would have to move
inside the same condition, which is the correct shape (a build-configuration
branch, not a selector branch), and decision 3 is untouched by it.

## §D — Verified claims and scope

| Claim | Verdict |
|---|---|
| Exactly one in-tree construction with an explicit `SchemaCarriage` | **TRUE** — `subjects/inprocess_carrying_main.cpp:36-37`, and it is in Files-to-touch. The other five sites (`inprocess_main.cpp:31`, `seam_vocabulary.cpp:273,329`, `copy_accounting.cpp:540,545`) default-construct and keep compiling against `explicit InProcessPubSubProvider(const ProviderConfig& = {})` — correctly absent from Files-to-touch |
| No harness or script parses the unknown-provider stderr text (P4) | **TRUE** — the only in-tree occurrence is `gateway/src/main.cpp:91` itself |
| `MakeProvider(registry, string, config)` exists, names no concrete type, one `Create` | **TRUE** — `registry.cpp:107-110` |
| `gateway-end-to-end` runs the whole battery per provider incl. `inprocess` | **TRUE** — `PROVIDERS`, lines 47-55, `describe.each` line 153 |
| Registration = availability is enforceable without a CMake change; `conformance_registry` already links `fletcher-pubsub` | **TRUE** — no CMakeLists entry is owed, consistent with Files-to-touch |
| §7's `SchemaCarriage` reference is editorial | **TRUE** — spec line 514-519 is a parenthetical about *how* the mode is chosen; the normative "never mix … per subscription" is untouched. Not an oracle amendment, no stop-and-ask |

**Document-key requirement is structural (question 2).** With the enum out of
the header there is no second route: `ProviderConfig` carries no typed carriage
field and is frozen at two fields by §4.1 plus the 2026-09-02 ruling; the
registered factory constructs only from `c`; the sole remaining constructor
takes `ProviderConfig`. Nothing above the seam can set the mode other than
through the document. C2-9 / PDA-DEC-4 §5's "a document key, NOT a second
construction API" is discharged structurally, not by convention.

**Scope discipline (question 5) holds.** No `extern "C"`, no C header, no
loader (14). Nothing above the seam branches on built-in vs loaded — the
gateway's `if`-chain and its name list both go, and one
`shared_ptr<PubSubProvider>` remains (3). No `PubSubProvider` method change (4).
`Create`'s signature is untouched and the `static_assert` at
`provider_registry.hpp:290-295` still pins it. No provider outside `pubsub/` is
touched; `FastDDSProviderOptions`/`XrceConfig` are left to PDA-DEC-6/7. The
`fastdds` closure is **not** a coexistence bridge — it is the only path to that
provider, so there is no window to close, and merging PDA-DEC-5 into PDA-DEC-6
would drag in decision 9's blast radius (4 external files / 19 occurrences plus
a provider-internal test rewrite) to save rewriting three lines. The split is
the cheaper shape; nothing to raise with the PM.

**Files-to-delete is present and real** (three constructs, each with a named
replacement); no whole-file retirement is expected for an item of this shape.
No hand-composed post-change ledger anywhere in the design.

## §E — Budget

- Design measures **301 lines** against the 300-line budget — a one-line
  overrun, not the 250 the launch brief assumed. Disposed of editorially: two
  duplicate-content lines in §2's document rules were merged, no claim removed.
  It is now under budget and needs no rework.
- Brief 60/60. Declared **+270/−60** is plausible against the work named
  (~120 lines of conformance tests, ~45 TS, ~80 provider, ~20 gateway, ~20 docs).
- Public surface: +1 / −2, net **−1** on the design's count; on the strictest
  count (the replacement constructor counted as an addition) it is +2/−2 = 0.
  Either way inside the ≤3 cap.
- **Is the design longer than the item needs?** Marginally. §4 ("what
  PDA-DEC-6/7/8 do") and the Risks bullet on coexistence windows restate §3, and
  the RTTI parenthetical in §1 re-argues a point PDA-DEC-4's header already
  disposes of. But the item genuinely has two halves and the gateway's
  false-green problem is worth its 25 lines, so this is at the top of the range
  rather than padded. Not a finding.

## §F — DEBT (5) — appended to `design-debt.md`, owed by the implementer

1. **DEBT-1** — B.1's `carried`-mode mutation does not redden the gateway
   battery (evidence in §C). Correct the claim: the battery proves *the provider
   comes from the registry* (via the drop-the-registration mutation), not *and in
   the right mode*; the mode proof is
   `Registry.InProcessCarriageComesFromTheDocument`. If the gateway's mode is to
   be pinned at all, the only client-visible difference the battery could see is
   a publish to a never-declared topic — one ~10-line case asserting it produces
   no error frame. Optional; say which, do not leave the false claim standing.
2. **DEBT-2** — pin the `--provider bogus` assertion to wording the pre-change
   binary cannot emit (`available:` or `no built-in provider named`). "Names the
   registered providers" is satisfied by the old `expected inprocess|fastdds`.
   While in that block, give the `READY` case its own port — `TEST_PORT` and
   `TEST_PORT+3` are held by the two `describe.each` contexts.
3. **DEBT-3** — rung-2 case 6 lists four refusals (unknown key, unknown value,
   no `=`, **duplicate key**) but names three test inputs. Add
   `schema_carriage=as_declared\nschema_carriage=carried` to
   `InProcessRefusesAnUnrecognisedDocumentEntry`, or the duplicate-key rule is
   the one refusal asserted by nothing.
4. **DEBT-4** — undisclosed gateway behaviour change: today a Fast DDS
   construction failure escapes `main` uncaught (there is no try around
   `main.cpp:109-116`) and aborts; after this item it is a `PubSubError` caught
   at the same `catch` and exits 2 with a message. That is an improvement and
   §5.1-consistent, but "observably unchanged" should say it. Put `Parse`, both
   `Register` calls and `Create` inside the one `try` so the whole registry block
   has one exit path.
5. **DEBT-5** — record that the gateway has **no route for a document**: it
   passes `""`, so requirement (b) ("configure the driver with protocol-specific
   setup at runtime") is unreachable from `gateway.exe` after this item. Correct
   for PDA-DEC-5 — the loopback's default *is* today's behaviour — but PDA-DEC-6
   cannot move Fast DDS QoS into the document without a gateway route for it, and
   no item currently names that surface. One line so it is not discovered late.
