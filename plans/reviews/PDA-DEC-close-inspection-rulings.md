# PDA-DEC close — rulings, locked decisions, and the design-debt register

**Read-only inspection.** Nothing was edited outside this file.
Written 2026-09-08 at `4ad003e`. Scope by assignment: `plans/PDA-DEC-rulings.md`,
`plans/PDA-decouple-locked-decisions.md`, `plans/reviews/design-debt.md`. The rest of
the execution corpus is a sibling inspector's
(`plans/reviews/PDA-DEC-close-inspection-corpus.md`).

**Verified count: 59.** `grep -c '^## 2026' plans/PDA-DEC-rulings.md` → `59`. Rulings are
numbered below by their order in the file (ruling *n* = the *n*-th `## 2026` header); that
is the numbering the ledger's own cross-references use ("entry 48, immediately above 49",
"ruling 55", "ruling 59").

---

## 0. Headline — read this first

**FIVE contradictions between a ruling and the kept corpus are live today, and one of them
is in the oracle's own `frozen` text.** All five are against **ruling 46** — the same ruling,
in the same direction, as the contradiction an external reviewer caught on 2026-09-07. Ruling
59 fixed one site and scoped itself to *"`docs/pubsub-interface-spec.md` **and the sites that
inherit the claim**"*; the verification pass behind it enumerated **eight** sites by
phrase-matching *"pure C"* / *"C boundaries"*, and therefore **missed the one site that states
the same claim in different words** — `docs/pubsub-interface-spec.md:1123`, in frozen §9,
which still grants the Rust/C#-driver licence the owner ruled against verbatim. See **§4,
contradiction C0** — that is the most important thing in this report.

**The 59 rulings do not need 59 migrations.** `.claude/runbook.PDA-ABI.config.md:130-132`
already instructs that PDA-ABI's rulings ledger is created *at kickoff by copying the
then-current `plans/PDA-DEC-rulings.md`*. That instruction is the single highest-leverage
action in this report: it carries all 59 forward verbatim into the next round's working set,
and it is already sanctioned. **But it breaks the moment the ledger moves** — the config
names a path, not an archive. See §3, action A0.

**Coverage is otherwise unusually high.** The seam spec cites 20 owner rulings by date and
name; `core/README.md`, `integration-tests/pubsub-conformance/README.md`,
`docs/wire-format-specification.md` and `fastdds-pubsub-provider/README.md` carry the rest of
the published-residue class. Of the 59:

- **47 are stated normatively in the kept corpus** (fully, or in substance) — §1.1;
- **5 are EXECUTION-ONLY** with no forward force at all (11, 13, 17, 33, 47) — §1.2;
- **2 are superseded** and must not travel as live (48 by 52; 56 by 58) — §1.3, §5;
- **9 rulings generate 9 migration actions** — §1.4. Most of the nine are *partly* captured;
  what is missing in each case is the half that binds a **later round** rather than this one.

Counts overlap by design: a ruling can be captured as product behaviour and still have an
uncaptured forward obligation (ruling 52 is the type case).

---

## 1. The 59 rulings — classification

### 1.1 ALREADY CAPTURED (47 distinct rulings) — no action

Grouped by where the kept corpus states them normatively. Line references are to the file
named in the group header.

**`docs/pubsub-interface-spec.md` (frozen; the oracle) — 40 rulings**

| # | Ruling | Where |
|---|---|---|
| 3 | Configuration shape: small typed core + opaque per-driver document | §4.1 (typed core is *exactly* two fields, append-only, widening is a stop-and-ask); §4.2 "Fletcher parses nothing" |
| 5(1) | Zero-copy required for rows **and** attachments | §8 |
| 5(3) | Multi-instance must be ordinary; no bridge built; nothing forecloses one | §4 clause 3; §11 |
| 6 | No driver data-plane function is callable by a binding; selection is seam surface | §1.1 entire; §9's closing note |
| 8 | Conformance suite built in-round, and it pressure-tests the ABI | §7.1; §9; §12.1 ("contract text is not the test set") |
| 9 | XML profile only; `FastDDSProviderOptions` **retired, not deprecated** | §4.1:658; §10:1146 (cites both rulings) |
| 10 | Cross-provider divergences are FIXED in-round, never pinned | §7.1 |
| 12 | Zero-copy receive stays in scope, delivered by PDA-ABI | §8 receive bullet; §11's named deferral |
| 15 | Bindings interface the abstract seam, not the driver ABI; mix of built-in and loaded | §0.1(2); §1; §1.1 |
| 16 | The split; the two rounds meet only here; neither may change the seam | §1 entire; §12.1 |
| 18 | Conflicting topic re-declaration **must** be rejected, every protocol | §7 clause 3 |
| 19 | The suite ships with a documented blind spot | §7.2's closing paragraph; §12.3 (**widened to standing policy** by PDA-DEC-9) |
| 20 | Refill movement is permitted and published as a number | §3.1 clause 1; §8.1 |
| 21 | The copy guard claims the interface, not the transport | §8.1 "Scope" |
| 22 | The receive-side copy was pinned at exactly one (fired; now 0) | §8.1 (records the 1→0 transition as the tripwire firing) |
| 23 | A live subscription's schema mode is held, not switched | §7 clause 1 ("never mixed WITHIN ONE SUBSCRIPTION") |
| 24 | One error type, one stable numbered cause | §5.1 |
| 25 | One waiting mechanism, not two | §3.4; §10:1184; §12.2 condition 2a |
| 26 | An unloadable driver path fails distinctly (`kNotSupported` ≠ `kInvalidArgument`) | §4 clause 1:459; §4.1:604; §12.2 condition 4 |
| 27 | The selector's *shape* decides name versus path | §4 clause 1 (the landed rule, verbatim) |
| 28 | Protocol-specific settings live in the protocol's own document | §4.1:560 (quotes the ruling) |
| 29 | The setting holds the XML text, not a filename | §4.1:571 |
| 30 | A supplied profile is that endpoint's WHOLE QoS — no merge, no floor | §4.1; `fastdds-pubsub-provider/README.md:184` |
| 31 | Isolation is one application on one machine, with three exclusions published | §4 clause 3 ("Three exclusions, stated rather than implied") |
| 32 | The handoff states the platform evidence exactly | §12.4 entire, including the three forward instructions and the post-merge amendment |
| 34 | `kReentrantCall = 10` | §6 clause 6; §12.1's `append-only` class; `core/README.md` row |
| 36 | Cancel waits for a delivery in flight; the self-cancel carve-out is published | §7 clause 6 |
| 38 | A duplicate cancel waits too; exactly one exception survives | §7 clause 6 |
| 40 | A topic part containing `/` is refused | §3.5 refusal 3 |
| 41 | An empty topic part is refused | §3.5 refusal 4 |
| 42 | The `__` **prefix** is reserved (class, not instance) | §3.5 refusal 5, plus the driver injectivity + companion-namespace obligation |
| 43 | Joined topic name bounded at 246 bytes | §3.5 refusal 6 |
| 44 | The seam **permits** an uncopied row end to end | §8 rows bullet; §3.1 clause 6 |
| 45 | The guard claims what the interface permits, measured with a stand-in | §8.1 "Scope" |
| 49 | Destroying another subscription from inside a delivery is forbidden, and said so | §6 clause 5 |
| 51 | A subscriber's handler failure is not the publisher's business | §5.3 |
| 52 | Re-entry refused on every protocol (the *refusal* half) | §6 clause 6 entire |
| 53 | Attachment order is part of the value; the same message produces the same bytes | §3.2 clauses A1/A2; `docs/wire-format-specification.md:151,158` |
| 54 | A zero-byte side-data label is refused | §3.2 clause A3, **with its non-generalisation inline** |
| 57 | Refused on arrival as well as on attachment | §3.2 clause A3's arrival half; `wire-format:165` |
| 58 | §5.1 states the *general* escape rule | §5.1 ("in ANY message it publishes … an example of the rule and not a limit on it") |

**`docs/protocol-driver-abi-spec.md` + `plans/PDA-ABI-locked-decisions.md` — 4 further rulings**

| # | Ruling | Where |
|---|---|---|
| 2 | Linkage: dynamic **and** static, one C entry-point contract | ABI spec §2 (the table + `fletcher_driver_entry`); PDA-ABI decision 5 |
| 5(4) | Discovery: explicit path only | ABI spec §2.1; PDA-ABI decision 6 |
| 7 | Port scope: all three providers | PDA-ABI decision 11; plan items PDA-ABI-4/5/6 |
| 14 | The data-sharing regression is investigated, not patched around | PDA-ABI decision 10 verbatim; seam §12.3; plan PDA-ABI-7 |
| 46 | Every protocol driver is C++ (the *ruling text* itself) | ABI spec front matter + §0 + §0.2; seam spec §0 (via ruling 59) — **but see §4: four sites still contradict it** |

**Component READMEs — the published-residue class — 3 further rulings**

| # | Ruling | Where |
|---|---|---|
| 37 | The carve-out is one subscriber object; the cross-instance hang is published | `integration-tests/pubsub-conformance/README.md` §"What `CallerTier` does NOT prove" limit 3 — verbatim, citing the ruling and the owner's *loud hang over silent use-after-free* reasoning |
| 50 | A transport subscription outliving a handler-side cancel is left open, and said so | same README, limit 5 (quotes the ruling); seam §6 clause 6 |
| 35 | Unsubscribing something not live is a no-op at every tier | seam §7 clause 6; `BIND-csharp-inherited-constraints.md` item 2 |

### 1.2 EXECUTION-ONLY — no forward force

**Five are execution-only outright: 11, 13, 17, 33, 47.** Three more (22, 35, 48) are listed
here for their execution-only *half* and appear elsewhere for the rest.

- **11 — "The spec comes first."** Settled that a spec doc precedes any item. Both specs
  exist; PDA-ABI's plan already encodes it ("PDA-ABI-1 is reviewed as a specification, not
  code"). Discharged.
- **13 — Bring in the Fast DDS modernization first.** A prerequisite merge (`d77b9c4`). Its
  *product* consequence — `WriteBuffer`'s window-plus-refill shape — is seam §3.1.
- **17 — What PDA-DEC does to the existing protocol code.** A scope statement for this round
  ("the built-in protocol code will have been updated"), fully discharged.
- **22 — the pin at exactly one.** Listed above as captured *history*; as a live tripwire it
  fired and is spent (the pin is 0).
- **33 — The BIND-C# stop-and-ask absorbed into PR #126.** Settled *how the round ran*:
  eight amendments land before merge, denominator 10 → 18. Its substance became A1..A9 and is
  captured item by item. The mechanism it exercised (§1/§12.1's route-to-owner) is frozen text.
- **35 (bookkeeping half) — "authorised as a **ninth**, inside A4, so the denominator stays 18."**
  Item accounting. The *rule* is captured (see above).
- **47 — Ceremony is expensive; group into fewer, larger items.** Directed PDA-DEC's remaining
  item granularity. See §3 action A6 — there is a *thin* forward reading worth one line, but
  the ruling itself is spent.
- **48 — "Re-entry is refused only where it is unsafe."** **SUPERSEDED.** See §5.

### 1.3 Superseded — must not be carried forward as live (2)

See §5. Rulings **48** (superseded by 52, void as to direction) and **56** (widened by 58,
superseded as to scope).

### 1.4 MIGRATE (9 actions) — grouped by destination

#### → `plans/PDA-ABI-locked-decisions.md` — 6 actions

| Action | Ruling | What it constrains |
|---|---|---|
| **A1** | **52** — the forward obligation | Re-permitting re-entry once the loaned-sample receive path lands. **Not pre-authorised**; needs a fresh owner ruling; `EveryProviderMethodIsRefusedFromInsideADelivery` must be *re-aimed, not deleted*. Its only registered home today is `design-debt.md` AG1-DEBT-19 — the file whose fate is in question. PDA-ABI's own locked decisions and plan carry **nothing** for it. |
| **A2** | **55** — no typed name query | The owner was asked, against the architect's recommendation, and declined `ProviderRegistry::RegisteredNames()` "in any form, this round". Seam §4 clause 2 already freezes the registry surface, and §5.1's message rules are the substitute answer — but nothing in the kept corpus records that the question was *put and answered*, so PDA-ABI would re-put it as a fresh stop-and-ask. One line prevents that. (Sole surviving trace: a code comment at `integration-tests/pubsub-conformance/src/registry.cpp:1144`.) |
| **A3** | **39** — fix at the scope the guarantee is about | The ruling names itself *"Forward-relevant to the remaining amendments"* and sets a **standing stop condition**: when a guarantee keeps leaking, fix it at the scope the guarantee is actually about rather than publishing the leak — *provided the root cause has been NAMED* — and "a further case after the root-cause fix means the premise is broken and the item stops." Nowhere in the kept corpus. Binds any future item, not just A4. |
| **A4** | **37** — the narrow/widen asymmetry | Ruling 32 granted a licence to *infer* the owner's preference for a narrow claim without asking. Ruling 37 bounds it: the licence "permits **narrowing** without asking, never **widening**." Seam §12.4 records the four-consecutive-narrow-claims observation but **not the asymmetry** — which is the half that decides whether a later round must stop and ask. |
| **A5** | **1** + **4** — charter and ABI audience, reconciled | (a) The owner's charter is the family's acceptance standard, and requirement (a) — *"I set up Fletcher with the protocol driver at run-time"* — is **PDA-ABI's** to deliver (PDA-DEC shipped selection + configuration only). Nothing states this as PDA-ABI's DoD. (b) Decision 4's headline **"PURE C. No C++ types"** needs a reconciling clause against ruling 46: what survives is the C *forms* (ABI spec §3, which ruling 46 explicitly left standing) and the binary-stability constraints; what does not survive is any reading that licenses a non-C++ driver. See also §4 contradiction C3. |
| **A6** | **47** — ceremony | Thin but real: the owner ruled that per-item process cost is fixed regardless of item size and directed *fewer, larger items*, naming the traded cost (one compliance pass and one code review shared across grouped items). PDA-ABI's plan currently declares 8 items with per-item ceremony. One line under `naming`. |

#### → `plans/BIND-csharp-inherited-constraints.md` — 3 actions

| Action | Ruling | What it constrains |
|---|---|---|
| **A7** | **5(2)** — BIND are **full pub/sub clients**, not read-only accessors | The owner's own words, and the load-bearing premise under ruling 35 (a finaliser cancels unconditionally and cannot let an exception escape). Seam §9's table says BIND "implements the caller side of `Publisher`/`Subscriber` + §4 selection" — close, but that is a scope statement, not the ruling. The constraints file never states it, yet items 2, 3 and 8 all rest on it. |
| **A8** | **39** + **37** | Same two standing policies as A3/A4. BIND will hit the same question the first time a published promise leaks at a managed boundary, and the same narrow/widen gate the first time it wants a wider claim. |
| **A9** | **32** — instruction 3 | *"A Linux-only difference in seam behaviour is a question for the owner — a stop-and-ask against this spec, not a local fix."* Frozen in seam §12.4 and therefore reachable, but the constraints file — which is the note a BIND author actually opens — does not carry it, and PR #126 showed three of seven defects were exactly Linux-only differences. |

#### → the sites carrying a struck claim — 1 sweep + 1 stop-and-ask (see §4)

Sweep (ruling **59**'s own unfinished scope, not a new decision):
`plans/PDA-ABI-protocol-driver-abi.md:3,25-27`, `plans/PDA-ABI-locked-decisions.md:37`,
`docs/README.md:14`, `docs/technology-decisions.md:123`,
`.claude/runbook.PDA-ABI.config.md:95,136`.

Stop-and-ask: `docs/pubsub-interface-spec.md:1123-1126` (**frozen** §9) — **C0**, which ruling
59 does not cover.

---

## 2. `plans/PDA-decouple-locked-decisions.md` — the 14 locked decisions

The digest's own front matter subordinates it: *"Full rationale in
`docs/pubsub-interface-spec.md` (the oracle — it wins on any conflict with this digest or a
per-item design)."* Every one of the 14 has a frozen counterpart. **No migration is required
for any of them; 13 are ALREADY CAPTURED and 1 is EXECUTION-ONLY.**

| Decision | Where the kept corpus states it | Class |
|---|---|---|
| 1. The seam is the meeting point; only this round may define it | seam §1; §12.1; PDA-ABI decision 1 | CAPTURED |
| 2. Neither ABI mirrors the other; no shared C header | seam §1 (4th bullet); §3.2's closing paragraph; PDA-ABI decision 2 | CAPTURED |
| 3. Built-in versus loaded is invisible above the seam | seam §0.1(2); §4 clause 1; §12.2 **condition 5** (which records that *no machine* watches this, and that §12.1's frozen status is the only forward protection) | CAPTURED |
| 4. TYPES in scope, METHOD SET not; `Publish` stays inverted | seam §2 (both halves, verbatim) | CAPTURED |
| 5. Every crossing type gets a normative C-expressible ownership rule in the header | seam §3 preamble ("A type whose ownership is only implied by C++ semantics is not done"); §12.2 condition 1 | CAPTURED |
| 6. The seam must carry memory it does not own | seam §3.2 ("Consequence, **DELIVERED** by PDA-DEC-3") | CAPTURED (delivered) |
| 7. Zero-copy for rows AND attachments, falsifiable | seam §8, §8.1; PDA-ABI decision 9 | CAPTURED |
| 8. Typed core + opaque document; Fletcher gains no config parser | seam §4.1, §4.2 | CAPTURED |
| 9. `FastDDSProviderOptions` retired, not deprecated | seam §10:1146 | CAPTURED (discharged) |
| 10. The seam throws; each boundary translates; a callback must not throw | seam §5.1, §5.3 | CAPTURED |
| 11. Guard-first; divergences fixed in-round; a wire-byte fix is a stop-and-ask | seam §7.1; **PDA-ABI decision 14** carries the wire-byte guard forward | CAPTURED |
| 12. The suite needs a cross-process subject | seam §7.2 (quotes the same Fast DDS evidence); **PDA-ABI decision 12** | CAPTURED |
| 13. The wire format does not change | seam §11; **PDA-ABI decision 14** | CAPTURED |
| 14. Nothing ABI is built here | seam §11 (the itemised list, and §12.1 makes those *prohibitions* frozen rather than record) | EXECUTION-ONLY (scope fence for PDA-DEC; discharged) |

**One observation worth carrying, not a migration:** decisions 11 and 13 are the pair that
*reserve wire-byte movement to the owner*, and ruling 53 is the only authorisation ever given
under them. The forward guard is **PDA-ABI decision 14**, which is unqualified ("A wire-byte
change is a stop-and-ask") — so the reservation survives the archive intact. Do **not** add
"ruling 53 authorised attachment ordering" to decision 14 as an example; the ruling's own
words are *"given here for attachment ordering only, and does not generalise to any other
byte movement"*, and an example beside a prohibition reads as a precedent.

---

## 3. Actions, ordered by leverage

**C0 — raise the stop-and-ask (highest priority; it is the only item needing the owner).**
`docs/pubsub-interface-spec.md:1123-1126`, frozen §9, grants the Rust/C#-driver licence ruling
46 withdrew. Not covered by ruling 59. §12.1's *who may act* is "nobody alone." Raise it at
close, in the same non-generalising form as rulings 54/56/58/59, licensing that one paragraph
only. Full text and evidence in §4.

**A0 — the ledger-copy instruction (do this second, before anything moves).**
`.claude/runbook.PDA-ABI.config.md:17` sets `rulings_ledger_path: plans/PDA-ABI-rulings.md`
and `:130-132` instructs: *"Create the rulings ledger at kickoff by **COPYING** the
then-current `plans/PDA-DEC-rulings.md`, then appending this round's own … Every design-phase
ruling in that file governs this round too."* This is the mechanism that makes all 59 rulings
survive without 59 edits — **and it names a path that the archive will invalidate.** Either
perform the copy *as part of the archive step* (so `plans/PDA-ABI-rulings.md` exists before
`plans/PDA-DEC-rulings.md` moves), or update `:130-132` to name the archive location. Note
the same paragraph's own wording — *"the **pure-C** audience"* (`:136`) — is contradiction C4.

**C1..C4** — the contradiction sweep in §4, all within ruling 59's existing authorisation.
C1 and C2 are in the *next round's own plan and Goal statement*, which is where PDA-ABI's PM
and every PDA-ABI agent start reading. Search for the **claim**, not the phrase.

**A1..A9** — as tabled in §1.4. **A1 is the one that loses a real obligation if skipped**: it
is the only registered home for ruling 52's forward obligation, and seam §6 clause 6 points at
it by name (*"a **registered obligation** on PDA-ABI"*).

**§7's split** — do it in the same pass as A1, since four of the six migrated debt entries and
A1 land in the same destination.

---

## 4. CONTRADICTIONS with the kept corpus — LIVE TODAY

**All five are the same contradiction, against the same ruling, in the same direction as the
one an external reviewer found on 2026-09-07.** Ruling 46 (2026-09-05) is explicit that it
is *"a **deliberate supersession**, not an oversight"* and that *"until amended, the spec
contradicts this ruling and **this ruling wins**"*. Ruling 59 then authorised the seam-spec
correction and scoped itself to *"`docs/pubsub-interface-spec.md` **and the sites that inherit
the claim**"*.

**C0 is frozen text and is NOT covered by ruling 59's authorisation.** C1-C4 are plan-tier and
index-tier documents, are not frozen, and are ordinary corrections within ruling 59's scope.

### C0 — `docs/pubsub-interface-spec.md:1123-1126` — §9, FROZEN, and it grants the licence the owner withdrew

**This is the loudest finding in the report.** The oracle's frozen §9 says:

> A **driver written in Rust or C#** is entirely legitimate and is unrelated to a
> Rust or C# *application binding*: the former implements the driver ABI, the
> latter calls the seam. Same language, opposite directions — worth naming, because
> the two are constantly conflated.

The sibling oracle, `docs/protocol-driver-abi-spec.md:87-91`, says the opposite about the
*same sentence*:

> A driver is **C++**, by the owner's ruling of 2026-09-05. **The licence this paragraph used
> to give — that a driver written in Rust or C# implements role 1 and is entirely legitimate —
> is withdrawn, not relocated: it was put to the owner verbatim and ruled against.** A Rust or
> C# *application binding* is a different artifact entirely and is unaffected; it calls the
> seam from above.

And ruling 46 itself:

> **Supersedes:** `docs/protocol-driver-abi-spec.md` §0.2's **multi-language driver licence**
> and the `:21-22` claim.

So the seam spec's frozen §9 carries, word for word, the licence that ruling 46 withdrew and
that the driver ABI spec has already deleted from its own §0.2.

**Why ruling 59 did not catch it.** The verification pass that scoped ruling 59
(`plans/reviews/PDA-DEC-PR126-bind-pass2-verification.md:121-210`) enumerated **eight** sites,
found by matching the phrases *"pure C"* and *"C boundaries"*:

| # | Site | Disposition |
|---|---|---|
| 1 | `pubsub-interface-spec.md:29` | amended |
| 2 | `:65` "the two C boundaries" | **true as written — must NOT be edited** |
| 3 | `:130` "the shape both C boundaries want" | in list |
| 4 | `:690` "both C boundaries need a success value" | in list |
| 5 | `:750` "the two boundaries derive one rule" | **true as written — must NOT be edited** |
| 6 | `:764-768` §5.2 entire | in list |
| 7 | `core/README.md:31` | in list |
| 8 | `core/include/fletcher/core/status.hpp:33` | in list |

**`:1123` is not among them** — it states the claim semantically ("a driver written in Rust or
C#") rather than by either matched phrase, so a phrase-scoped sweep could not see it. Ruling 59
authorises editing *"the sites verified to contradict it"*, and this site was never verified,
so a good-faith reading is that **ruling 59 does not cover it.**

**Why it is worse than the site ruling 59 fixed.** `:29` was a stale *description* of the ABI's
language. `:1123` is an affirmative *licence* — "entirely legitimate" — to build the artifact the
owner ruled against, in the document PDA-ABI's spec review is told outranks its own oracle.
The verification pass's own severity note applies with more force here: *"A reader of frozen text
alone is misled; a reader of the ledger is not."*

**Why it is not ambiguous, only misleading.** Seam §0 (added by ruling 59) already states the
tie-break: *"**Where a ruling and this document disagree, the ruling wins** — as this document
wins over the plan and over any per-item design."* Ruling 46 therefore governs today. Nothing in
the tree is wrong; the oracle is.

**What it needs.** §12.1 puts §9 explicitly in the `frozen` list (*"Anything not named
`append-only` below is `frozen` — including §0, §1, §5.2, §8.1 and §9"*) and its *who may act* is
**"nobody alone."** §12's records carve-out is bounded to §10 and §11, so this is **not**
maintenance. **This is a stop-and-ask against the seam spec, and it is the correct one to raise at
round close** — a fifth frozen-text authorisation, in the same non-generalising form as 54, 56, 58
and 59, licensing exactly this one paragraph. Note the second sentence of §9's paragraph is
*correct and must survive*: a Rust or C# **application binding** is unaffected, and that
distinction is the reason the paragraph exists.

### C1 — `plans/PDA-ABI-protocol-driver-abi.md:3` — the next round's plan, first line

> Round plan + tracker for the **pure-C** protocol driver ABI **below** the pub/sub seam.

against ruling 46:

> "We decide that all protocol drivers will be C++, and so both sides of the protocol ABI
> will be pure C++."

This is the *identical phrase* ruling 46 struck from `docs/protocol-driver-abi-spec.md:3-6`
and ruling 59 struck from `docs/pubsub-interface-spec.md:29`. It survives in the document
PDA-ABI's PM and every PDA-ABI agent opens first.

### C2 — `plans/PDA-ABI-protocol-driver-abi.md:25-27` — the round's Goal

> Turn a protocol into a **driver**: a separate binary, implementable by anyone in
> **any language**, loaded and configured at runtime with no Fletcher rebuild

against ruling 46's own record of how it was given:

> "Given 2026-09-05 during a re-plan … in direct response to the PM quoting
> `docs/protocol-driver-abi-spec.md:21-22` (*"implementable by anyone in any language, in a
> separate binary"*) … **The owner was shown that text and ruled against it.**"

**This is the exact sentence the owner was shown and rejected**, still standing as the next
round's stated Goal. The ABI spec now says the licence is *"withdrawn, not relocated"*
(`§0.2`) — the plan says the opposite. Loudest of the four.

### C3 — `plans/PDA-ABI-locked-decisions.md:37` — decision 4, and its stop-and-ask

> 4. **Public, versioned, third-party-implementable PURE C.** No C++ types, no exceptions
> crossing … Downgrading to "internal decoupling with a C++ interface" would make this runtime
> *selection* … rather than a driver contract.

and `.claude/runbook.PDA-ABI.config.md:95`, which turns it into a gate:

> - Making the ABI a C++ interface, or letting a C++ type or an exception cross (decision 4).

against ruling 46's *"both sides of the protocol ABI will be pure C++"*.

**Subtler than C1/C2 and it must not be over-corrected.** Ruling 46 explicitly preserves two
things: the ABI spec's *"§3 The C types"* (`fletcher_blob`, `fletcher_write_buffer`,
`fletcher_status`), and — under *"Does NOT relax"* — the binary-stability constraints between
separately built C++ binaries (*"exceptions may not cross, layout may not be assumed"*). So
decision 4's C **forms** and its no-exceptions clause stand; what does not stand is
*third-party-implementable in any language*, and the config line at `:95` as written would
fire against a design that simply follows the amended ABI spec. Reconcile with a clause, not
a deletion.

### C4 — three inherited restatements in kept docs

- `docs/README.md:14` — `protocol-driver-abi-spec.md   # **Pure-C** protocol driver ABI below that seam (round PDA-ABI)`
- `docs/technology-decisions.md:123` — *"a **pure C** driver ABI *below* `PubSubProvider`"*
- `.claude/runbook.PDA-ABI.config.md:136` — *"the **pure-C** audience"*

`docs/README.md` is the index a new reader opens; `technology-decisions.md:123` is a TD
*Context* paragraph (a record of the round's premise, so the past tense is defensible — but
the phrase is the struck one, and ruling 46 was given *against* precisely this wording).

### The safeguard ruling 59 supplies, and which any sweep must honour

Ruling 59: *"**Two of the eight sites are true as written and must NOT be edited** — the
verification names them; a find-and-replace over the phrase would break them."* The seam spec
generalised that into a rule at §0 that a sweep should reuse verbatim:

> The ruling leaves that spec's §3 *"The C types"* standing … so *"a C boundary"* and *"both C
> boundaries"* below still hold as written: they name the C **forms** that appear at each of the
> two boundaries, not the language either ABI is implemented in.

**No `sed` over "pure C".** Each of C1..C4 is a hand edit, and `docs/protocol-driver-abi-spec.md`
§3's C types plus every "C form" / "C boundary" phrase in the seam spec are correct and must
survive untouched. **C0 is not in the sweep at all** — it is a stop-and-ask, and the same
discipline applies inside it: §9's *second* sentence (the application-binding distinction) is
correct and is the reason the paragraph exists.

**And note what C0 teaches about the sweep method.** Ruling 59's eight sites were found by
phrase-match; the ninth states the same claim in different words and survived. Any sweep for
C1..C4 should therefore search for the **claim**, not the phrase — grep for `Rust`, `C#`,
`any language`, `third-party`, and `implementable` as well as `pure.C`.

---

## 5. Superseded rulings — what a migration must not carry

### Ruling 48 — VOID as to direction

> **48** (2026-09-05) "Refuse only what's unsafe — Keep publish / declare-topic / subscribe
> working from inside a handler … Refuse only the genuinely unsafe case."

**49 is a distinct, still-live ruling; it is 48 that 52 killed.** Ruling 52's own text is
unambiguous:

> **SUPERSEDES the 2026-09-05 ruling "Re-entry is refused only where it is unsafe; the rest
> converges up"** (entry 48, immediately above 49). That ruling is **void as to direction**;
> its record stands.
>
> **Why it was superseded — the premise it rested on was false.** … Implementation tested it
> **by probe rather than by reading** — A: Declare+Publish only → hangs; B: Subscribe only →
> hangs; C: neither → completes — and a Fast DDS listener callback runs with the **RTPS reader
> mutex** held. The capability works on **XRCE only, one protocol of three.**

**Nothing in the kept corpus states ruling 48**, and seam §6 clause 6 states the refusal
uniformly — so the tree is correct today. The hazard is a *migration* that copies the ledger
without its supersession note, or that summarises the re-entry rulings into a digest line.
**Guard:** the ledger's supersession note sits inside entry 49's own body (bold, immediately
under its header), so a verbatim copy carries it. **Any digest line about re-entry must cite
52, never 48**, and A1's migrated text must say "supersedes ruling 48" explicitly.

### Ruling 56 — WIDENED by 58, superseded as to scope

> **56** (2026-09-06) "Amend §5.1, naming the one class." — authorised naming *one* message
> class in frozen §5.1.
>
> **58** (2026-09-06) "State the general rule." — *"This **widens ruling 56** and supersedes
> it as to scope … Ruling 56 stands as to the fact of the amendment and its non-generalisation
> to any *other* frozen section."*

**Already correctly recorded in the kept corpus.** Seam §5.1 states the general rule and names
the supersession in place:

> Amended by PDA-DEC-AG2 under the owner's rulings of 2026-09-06, **the later of which widened
> the amendment from that one named class to the general rule** stated here.

No action. A migration must not reintroduce 56's one-class sentence — which the AG2 landing
itself briefly shipped (*"That is the only class of message this seam renders differently"*,
`spec:719`, falsified by the item's own forcing test) and 58 removed.

---

## 6. The frozen-text authorisations (54, 56, 58, 59) — how the non-generalisation survives

Four rulings authorised writing into `frozen` spec text. Each was worded to be
**non-generalising**, and a later round reading them as a blanket licence is the failure mode
to prevent.

**They are already captured in the safest possible place: inline, in the section each one
amended, next to the amendment.**

| Ruling | Section amended | Where its limit is stated |
|---|---|---|
| 54 | §3.2 (zero-byte label) | §3.2's closing sentence: *"…the zero-byte-label ruling, which by its own terms authorises **that refusal alone** and is not a general licence to amend this section."* |
| 56 | §5.1 (one message class) | Superseded as to scope by 58; its surviving half (non-generalisation to any *other* frozen section) is §12.1's default rule |
| 58 | §5.1 (the general escape rule) | §5.1 names the rule and marks the in-tree class as *"an **example of the rule and not a limit on it**"* — the widening is bounded to §5.1 |
| 59 | §0 and the C-boundary sites | §0 records the ruling, its precedence, **and** the preserved-as-written carve-out (§3's C types, "a C boundary", "both C boundaries") |

**Recommendation — preserve the non-generalisation by NOT aggregating them.**

1. **Do not create a "frozen-text authorisations granted" list** in any migrated digest.
   Four authorisations in one table reads as a precedent — "the owner amends frozen text when
   asked" — which is exactly what each ruling's wording forbids. The ledger already holds them
   individually and in date order; that is the right shape.
2. **The general mechanism is §12.1, and it needs nothing added.** *"Anything not named
   `append-only` … is `frozen`. Who may act: **nobody alone**"*, plus the `append-only`
   class's *"making an append is itself a stop-and-ask, and the owner allocates the number or
   the field"*. Every one of the four went through that door. A later round reaching for
   precedent hits §12.1 first.
3. **If a digest line is needed at all**, phrase it as the gate, not the grants:
   *"§12.1 governs every change to the seam spec. Four prior authorisations exist (rulings 54,
   56, 58, 59); each names its own section and its own single change, each states its own
   limit inside the section it amended, and **none is precedent for any other**. Read the
   ledger entry, not this line."*
4. **The one place a limit is thin:** ruling 53's *"given here for attachment ordering only,
   and does not generalise to any other byte movement"* is **not** restated in §3.2 or in
   `docs/wire-format-specification.md`. The forward guard is instead PDA-ABI decision 14's
   unqualified *"A wire-byte change is a stop-and-ask"* — which is sufficient, and per §2's
   closing note should be left unqualified rather than annotated with 53 as an example.

---

## 7. `plans/reviews/design-debt.md` — recommendation: **SPLIT**

**Recommendation: SPLIT.** Archive the per-item debt tables with the round's review files;
migrate the six forward-addressed entries into the working sets that will read them.

### The facts the recommendation rests on

1. **The register defines itself as an index to files that are being archived.** Front matter:
   *"Debt accepted at architecture review, handed to the implementer. **Full text lives in the
   per-item review file; this register is the index, not a copy.**"* Every per-item review it
   indexes (`PDA-DEC-{1,1H,2..9,A1,A4,A5,AG1,AG2}-{design-review,compliance,codereview}.md`)
   is execution corpus. An index whose targets have moved is worse than no index.
2. **Nothing in the kept corpus links it.** A tree-wide grep for `design-debt` outside the
   file itself returns **only** PDA-DEC review files, PDA-DEC plan/progress documents, and the
   sibling inspector's report. No `docs/*.md`, no component README, no `plans/PDA-ABI-*`, no
   `plans/BIND-*`, no runbook config references it. Kept in place, it would be a live file that
   nothing live points at.
3. **The task brief's premise about older entries does not hold — there are none.**
   `grep -c '^## '` → **21** section headers, and every one is `PDA-DEC-*` or a `*-DEBT-*` /
   `ROUND-*` entry from this round. A grep for `RBA|GIR|RIR|HARD` returns a **single** hit
   (line 553), and it is an incidental *"a fourth instance of the HARD-4 pattern"* reference
   inside a PDA-DEC-AG1 entry — not an RBA/GIR-era entry. **This register is PDA-DEC-only.**
   That removes the strongest argument for keeping it in place as a cross-round artifact.
4. **Six entries are addressed forward, and they are the file's only live content.** They sit
   at lines 560-793 — the last 30% of a 793-line file:

   | Entry | Addressed to | Why it must not be archived |
   |---|---|---|
   | **AG1-DEBT-19** — re-permitting re-entry (`:560-593`) | PDA-ABI, at the loaned-sample receive item | **The registered obligation ruling 52 created.** Seam §6 clause 6 says *"Re-permitting re-entry is a **registered obligation** on PDA-ABI"* — and *this entry is the register*. It is also the only place the measured evidence is written down (the A/B/C probe; XRCE-only; same timeout at base `999b764`). |
   | **A1-DEBT-6** — overflow guard at one door (`:603-615`) | PDA-ABI, at the item giving the append path its C form | PDA-ABI will give `Append`/`AppendZeros` the same C form A1 gave the window; each needs its own copy of A1's door check, and *"the first one that forgets reproduces B1 at a new entry point."* |
   | **AG1-DEBT-20** — re-entrancy identity typed at one end (`:617-636`) | PDA-ABI, at the driver-side identity token | Thirteen ask sites hand-write `static_cast<const PubSubProvider*>(this)`; a door added later can omit the cast **silently**. Deferred *because* PDA-ABI is about to re-cut that tier. |
   | **AG2-DEBT-11** — unbounded attachment count on both decode paths (`:769-793`) | PDA-ABI, or any round with a measured deployment figure | *"The time defect is closed on its own terms; the memory question is **deferred, not answered**."* Carries an explicit method constraint — **measure first**, because a ceiling would be new normative text in frozen §3.2 and needs a fresh ruling. |
   | **AG1-DEBT-21** — a refused `Subscribe` still walks the rollback path (`:638-660`) | "the next item to touch `subscriber.cpp`'s subscription bookkeeping" | Not PDA-ABI-specific — the caller tier is what **BIND** wraps. Belongs in `BIND-csharp-inherited-constraints.md` as well as, or instead of, PDA-ABI's digest. |
   | **A5-DEBT-6** — gateway binary-`publish` path unwatched (`:601`) | nobody | Genuinely unowned, non-blocking (the seam refuses the name either way — the gap is in what the *gateway* case observes). Needs a home or a deliberate drop. |

   Plus **ROUND-1** (`:595`), which is **closed** — it became PDA-DEC-1H — and should be
   archived with a closure note rather than migrated.

### Why not the alternatives

- **KEEP IN PLACE** — leaves six live obligations at the tail of a 793-line index to archived
  reviews, with nothing live pointing at it. That is precisely *"a ruling that survives only in
  an archive is a ruling that will be re-litigated"*, one indirection removed.
- **ARCHIVE WHOLE** — loses AG1-DEBT-19, and with it the only registered home for ruling 52's
  forward obligation. Seam §6 clause 6 would then point at a register that has moved. Also
  loses AG2-DEBT-11's *measure-first* constraint, whose absence invites a *guessed* attachment
  ceiling — the thing its author explicitly declined.
- **MIGRATE WHOLE** — carries ~550 lines of discharged per-item debt (with `DISCHARGED (PM,
  2026-09-05)` annotations) into PDA-ABI's working set, where a reader cannot tell live from
  spent. Rejected for the same reason the ledger is kept verbatim but the *reviews* are not.

### The split, concretely

- **Migrate** AG1-DEBT-19, A1-DEBT-6, AG1-DEBT-20, AG2-DEBT-11 → `plans/PDA-ABI-locked-decisions.md`
  (or a new `plans/PDA-ABI-inherited-debt.md` referenced from it — four entries with full text
  is ~90 lines, which is a lot for a decisions digest; the digest should at minimum carry a
  decision-numbered pointer, because the digest is what every PDA-ABI agent reads).
  **Carry AG1-DEBT-19's full text, not a pointer** — the probe evidence exists nowhere else.
- **Migrate** AG1-DEBT-21 → `plans/BIND-csharp-inherited-constraints.md` (the tier it names is
  the one BIND wraps), and cross-reference it from PDA-ABI's set.
- **Decide** A5-DEBT-6: give it a home or drop it explicitly. Do not let it ride into the
  archive un-dispositioned.
- **Archive** everything else, with ROUND-1 marked closed-as-PDA-DEC-1H.
- **Leave a one-line stub** at `plans/reviews/design-debt.md` naming the archive path and the
  destinations, so seam §6 clause 6's *"registered obligation"* and every review cross-reference
  still resolve.

---

## 8. What this inspection did NOT find

- **No ruling contradicts `docs/protocol-driver-abi-spec.md`, `core/README.md`,
  `docs/wire-format-specification.md`, `integration-tests/pubsub-conformance/README.md`,
  `fastdds-pubsub-provider/README.md`, or `plans/BIND-csharp-inherited-constraints.md`.** Those
  six are clean. The five contradictions in §4 are: one in the seam spec's frozen §9 (**C0**,
  needs a ruling) and four in plan-tier / index-tier documents (**C1-C4**, within ruling 59's
  existing authorisation). All five are against ruling 46 and no other.
- **No locked decision needs migrating.** All 14 have frozen counterparts, and the digest's own
  front matter already subordinates it to the oracle. `plans/PDA-decouple-locked-decisions.md`
  can be archived as-is.
- **No RBA/GIR-era entry exists in `design-debt.md`** — see §7 fact 3. The register is
  PDA-DEC-only, which is what makes SPLIT the right answer rather than KEEP.
- **No ruling was found to be missing from the tree's *behaviour*.** Every ruling that governs
  code is implemented and, in 41 of 47 cases, guarded by a named test the conformance README
  records. The gaps this report names are all in **where a decision is written down**, not in
  what the tree does.
