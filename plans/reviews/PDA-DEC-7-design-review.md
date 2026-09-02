# PDA-DEC-7 — architecture review, cycle 1 of 2

**Verdict: NEEDS-REWORK — 1 BLOCKER, 9 DEBT.**
Subject: `plans/PDA-DEC-7-xrce-by-document.md` (288 lines) + `plans/PDA-DEC-7-brief.md` (60).
Oracles read: spec §4 clause 4, §4.1, §4.2, §5.1; locked decision 8; the rulings ledger;
`plans/PDA-decouple-interface.md:211`.

No spec contradiction, no ruling deviation, no locked-decision deviation. **No STOP-AND-ASK.**
Budgets pass: design 288 ≤ 300; brief 60 at cap; new public surface +2/−3 (net −1), counted and
confirmed — `XrceSettings`/`ParseXrceDocument` in `src/internal/` are not surface (`segments.hpp`
precedent, and the tree already treats `fastdds-pubsub-provider/src/internal/` this way).

---

## BLOCKER

### B1 — Four of the six document keys have no guard that an accepted value lands anywhere

The mapping table's coverage of *accepted* values is: `transport` and `agent` (row 1 of the
forcing test, end-to-end through a real socket — genuinely strong), and **the defaults** of all
six (`PublishedDefaultsAreExact`). `DocumentRefusalsAreTypedAndQuoted` covers only the refusal
side. There is **no row anywhere that parses a non-default value and asserts where it went.**

Consequence, concretely. A build that reads the document, range-checks every key, and then hands
the XRCE client hard-coded `4` / `10` / `0xAABBCCDD` / `3000` passes:

- every `XrceConfig.*` row in the provider suite (nothing observes a non-default value);
- `PublishedDefaultsAreExact` (it compares defaults, which such a build gets right);
- `DocumentRefusalsAreTypedAndQuoted` (the refusals still fire — the design calls this "the guard
  that stops a reader which parses and shrugs", but it only stops a reader that fails to *refuse*;
  it does nothing about a reader that *accepts and discards*).

Of the four, `session_key` has an Agent-gated witness (the 24 `conformance_xrce` cases assign
unique keys precisely because collisions break, so ignoring it reddens them) and
`connect_timeout_ms` has a weak one (5000 silently becoming 3000 would probably still pass).
**`stream_history` and `run_loop_ms` have no witness anywhere in the tree** — nothing in-tree sets
them, so a build that validates and discards them is green everywhere, forever. An operator who
raises `stream_history` to carry bigger rows gets the same 2 KiB buffers
(`MTU × 4`, `xrce_dds_pubsub_provider.cpp:352-355`) and no signal; one who raises `run_loop_ms`
gets the same 10 ms quantum and no signal. That is a wrong answer with no typed signal.

This is the defect class this round has already paid for once, and the ledger now carries a
standing note about it — the 2026-09-02 "whole quality-of-service" ruling, review note C2-1:
*"as designed, nothing watched the policies a supplied profile omits, so a build implementing
either answer would have passed every row. The implementation must mandate the form … and assert
it, or this ruling is unfalsifiable."* Same shape, four keys.

A second, free hazard in the same place: `run_loop_ms` and `connect_timeout_ms` are both plain
integers of the same type in `XrceSettings`, so swapping them at the assignment compiles silently
and no listed row would catch it either.

**Acceptable fix — forbidding is cheaper than handling for two of the four, and I would approve
either:**
1. Add **one** row to `test_xrce_document.cpp` that parses a document setting **every** key to a
   non-default value and compares the result **whole-struct** against the expected `XrceSettings`
   (the mirror of `PublishedDefaultsAreExact`, ~15 lines, no Agent). This closes the
   accepted-and-discarded and wrong-field classes for all six keys at the reader boundary; and
2. for `stream_history` and `run_loop_ms`, which have no witness that the *constructor* then uses
   the parsed value — **cheapest: forbid them.** Drop both keys from the document. Nothing in-tree
   sets either, both defaults are the only values ever used, and the design's own rung-1 reasoning
   about `max_payload` ("a payload cap that caps nothing") applies almost verbatim. If they are
   kept, assert application in-process through the test hook the suite already has
   (`xrcedds-pubsub-provider/src/internal/xrce_test_hook.hpp`, already reachable via
   `tests/CMakeLists.txt:13-14`): one row asserting the output buffer is `MTU × stream_history`
   for a non-default history; and
3. give the two millisecond fields distinct types (`std::chrono::milliseconds`) so a swap does not
   compile — rung 1, ~0 lines.

---

## What I attacked and found sound (recorded so cycle 2 does not re-litigate it)

**P1 and the forcing guard.** The claim that matters — *the listener distinguishes "the document
configured the transport" from "something connected"* — holds, and holds for a structural reason
the design gets right where PDA-DEC-6 got it wrong: the listener's port is **ephemeral, chosen at
run time**, so it is not any default of Fletcher's, of the client's, or of the Agent's. A build
that ignores the document (M1) or reads the host but not the port (M3) cannot reach it. That is a
falsifiable in-process guard on a transport-observed setting, which is exactly what DEC-6's
`history`/`resource_limits` blind spot lacked.

P1 itself I could not machine-verify from this tree: the Micro XRCE-DDS Client is FetchContent'd
(`xrcedds-pubsub-provider/CMakeLists.txt:60-64`, v3.0.1) and no copy of its source is on disk
here, and I have no shell. The evidence I could reach is consistent with P1 and nothing
contradicts it: the vendored header contracts `uxr_init_tcp_transport` as returning "`true` in
case of successful initialization" over a `uxrTCPPlatform` that is a single `WSAPOLLFD`
(`C:/fl-uxa-install/include/uxr/client/profile/transport/ip/tcp/tcp_transport{,_windows}.h`), the
provider already treats a `false` return as "failed to init TCP transport"
(`xrce_dds_pubsub_provider.cpp:368-372`), and upstream's TCP platform init performs a blocking
`connect()` — there is no lazy-connect machinery in this client. The premise is **stated, scoped,
and carries a stop condition that does not weaken the row**, which is the correct treatment; I am
not converting an unverifiable-here substrate fact into a finding. Step 3 should confirm it in its
first ten minutes, before the listener is written, because the stop condition changes the item's
shape.

**§4.1's disclosure clause: nothing owed.** Checked all three of the ways a refusal could defer.
(a) A key whose validity depends on the Agent: only `session_key` uniqueness, disclosed as H3;
`domain_id` is typed core, not a document key, so its Agent-side outcome is outside the clause's
scope, and its own refusal (>65535) is up front. (b) A transport failure surfacing on first
publish: no — the transport is opened and the session created inside the constructor
(`:357-389`), and `uxr_create_participant_bin` is reached from `CreateTopic`/`Subscribe`
(`:524`, `:704`) but takes only the already-validated `uint16_t domain_id`, never a document byte.
(c) Anything the factory reaches after construction returns: the run-loop thread reads
`run_loop_ms` and the session, both already validated. No XRCE key is topic-scoped, so the "first
`Publish`" moment DEC-6 has does not exist here. The design's structural argument is correct.

**Duplication of the reader is forced, not convenient.** Decision 8 verbatim
(`PDA-decouple-locked-decisions.md:68-74`) makes a shared reader in `pubsub/` a stop-and-ask, and
the tree has already made this exact call once at code-review level for a *one-line* helper —
`in_process_provider.cpp:44-47`: *"Deliberately not the shared `Quoted` helper in
provider_registry.cpp (decision 8: no shared parser, no dependency between a provider TU and the
registry TU)."* Against that precedent, declining to share ~20 lines of entry-splitting is the
consistent answer, not a second mechanism. The 2026-09-01 "one mechanism only" ruling is about one
*format* / one *waiting mechanism*, and there is one format here, specified once in spec §4.1. The
residual drift risk is real but bounded and disclosed; see D-7.4.

**`agent=host:port` survives the parse attack.** Bare host (no colon) → refused; `:2018` (empty
host) → refused; `agent=` → refused; two-or-more colons → refused; port 0 → refused; >65535 →
refused (parse wide, range-check); non-numeric port → refused; hostname-with-colon is not a legal
hostname and is refused by the same rule. Value-side whitespace (`agent=x:2018 `) dies in the port
parse rather than being trimmed, so "nothing is tolerated silently" is total. The one thing the
one-colon rule forecloses is IPv6 — see D-7.4.

**Wire identity, verified.** `kPayloadBytes<64*1024>` is `65536` (`payload_bound.hpp:56-58`), the
current `XrceConfig::payload_bound` default is exactly that (`xrce_dds_pubsub_provider.hpp:37`),
and **no in-tree caller sets `payload_bound`** — `xrce_main.cpp:71-79`, `test_interop.cpp:107-117`
and `xrce_peer_main.cpp` all leave it default. So `max_payload_bytes == 0 → 65536` reproduces
`FletcherTypeName(65536)` bit-for-bit. No wire change, no stop-and-ask. `domain_id` is `uint16_t`
on the client call (`uxr_buffer_create_participant_bin`, `create_entities_bin.h:80-84`), so
"refuse above 65535, never narrow" is right and matches the forward note it cites
(`provider_registry.hpp:113-116`, verified).

**Deletions are real retirements, and the premise under them is true.** Grepped the whole tree:
`max_payload` appears only in the header (declaration + comment), `README.md:21,53,66` and
`test_xrce_provider.cpp:52,63,71` — **no read in `src/`**. Same for `serial_device` /
`serial_baudrate`: header + `README.md:64-65` only. P5 and rung-1 item 4 are sound. Nothing is
deprecated, no shim, no coexistence bridge, nothing scheduled for later deletion — so the
"merge the stages and delete first" finding does not apply to this item.

**Other tree claims spot-checked and true:** `in_process_provider.cpp:56-73` states the tolerance
rules; `status.hpp:133` is `TranslateSeamFailure` and it does turn `std::invalid_argument` /
`std::runtime_error` into `kInternal` via its `std::exception` arm (`:145-146`);
`PubSubError` derives from `std::runtime_error` (`:97`) so the two surviving
`EXPECT_THROW(..., std::runtime_error)` rows do stay green; `kNotSupported` / `kTransportFailure`
exist and are frozen (`:48-50`); `provider.hpp:73-77` and `provider_registry.hpp:113-116` both
carry the forward note; `xrce_dds_pubsub_provider.cpp:398-406` documents the run-loop mutex hazard
the `run_loop_ms` range cites; `tests/CMakeLists.txt:13-14` already puts `../src` on the include
path; `xrcedds-pubsub-provider/test_package/src/example.cpp` exists; `conformance_xrce` is on
2019/domain 153 (`xrce_main.cpp:42-43`) and interop on 2018/domain 145
(`test_interop.cpp:69-71`); the provider suite is 11 ctest entries (10 gtest cases + the MSVC
nodiscard probe). No hand-composed post-change ledger anywhere in the design — the counts it
carries are pre-change premises with stop conditions, which is the right form.

**No over-scope.** `plans/PDA-decouple-interface.md:216-218` says a link-size check is not
required; the design adds none. Correct.

---

## DEBT (9) — appended to `plans/reviews/design-debt.md`, handed to the implementer

D-7.1 README-drift guard cannot run in the packaged build as specified (`conanfile.py` missing
from Files-to-touch); must hard-fail, never skip, and must read at run time.
D-7.2 §6 row 2 is a harness control, not a build guard; its stated mutation M2 is unachievable.
D-7.3 Header include set in §1 is wrong (`ProviderConfig`/`ProviderRegistry` live in
`provider_registry.hpp`); keep `payload_bound.hpp` if the header keeps the `kPayloadBytes<N>`
advice (DEC-6 review 4a F7).
D-7.4 The one-colon rule makes IPv6 unrepresentable — matches today's hard-coded `UXR_IPv4`, so no
regression, but currently an undisclosed loss. Also: name spec §4.1 as the single tolerance oracle
and cross-reference both readers' tests.
D-7.5 "M1/M3 also redden all 27" over-claims — the 3 interop tests run their Agent on the *default*
port 2018 and differ only in `domain_id` (typed core, not the document).
D-7.6 Give built-in registration an Agent-free witness by routing
`AgentUnreachableIsATransportFailure` through `RegisterXrceProvider` + `Create("xrce", …)`.
D-7.7 Brief hygiene: all three owner decisions are answerable from the ledger/spec.
D-7.8 Net-lines realism: expect ~1900–2100, not 1400.
D-7.9 Forcing-test wall clock: budget ~4–5 s; an empty document cannot shorten its own retry
budget.

Full text of each is in `design-debt.md` under `## PDA-DEC-7`.

## NITs — fixed in place, silently

Four factual/wording corrections applied to the design doc: the §1 include sentence, the §5
"every refusal is kInvalidArgument … serial is kNotSupported" self-contradiction, P5's inaccurate
grep count, and the interop half of the M1/M3 claim.

---

## The brief's three owner decisions — judgement

All three are answerable without the owner, and one of them invites an answer that would
contradict a landed oracle. Recommend the PM strike or reframe before asking.

1. *Inert payload cap → delete or make real.* Bookkeeping. Option (b) — "keep it as a document key
   that genuinely caps sends" — is **new behaviour**, which the 2026-09-01 split ruling puts
   outside this round ("It should not do any development on any of the ABIs, only prepare for
   them"; DEC "will update the interface and also the existing protocol code"). An option the
   owner cannot take without widening the round is not a real option. The design should just
   delete and disclose, which it already does in Files-to-delete.
2. *Serial nameable but distinctly refused.* The brief itself cites the ruling that answers it
   ("mirroring your ruling that an unloadable driver fails distinctly from a typo" — 2026-09-02
   PDA-DEC-4, *"Accept it, fail distinctly … separate from an unknown name"*). A decision the
   ledger already answers must be struck, not asked. Strike.
3. *Document tolerance strict vs forgiving.* Already decided and **already in the spec**: §4.1's
   PDA-DEC-5 paragraph records "a trailing `\r` … is stripped … nothing else is trimmed, no case
   folding, no comments" as landed. Option (b) would force a spec amendment and split one format
   into two dialects. Asking re-opens an oracle. Strike, or reframe as a confirmation with no
   alternative offered.

Form is otherwise good — each is behaviour-visible, with options, a recommendation and a default,
which is more than most briefs in this corpus manage.

## Plan-shape note for the PM

The item is a single-stage retirement with no bridge, so there is no stage-merge finding. The one
plan-shape observation: B1's cheapest fix reduces the key table from six keys to four, which also
removes two range-check rows from the refusal table and ~30 lines of the declared cost. If the PM
takes the forbid branch, the item gets *smaller*, not larger — worth saying out loud so B1 is not
read as a demand for more test mass.

---

# Cycle 2 review (2026-09-02) — final cycle

**Verdict: APPROVE-WITH-DEBT(7). B1 is closed. No BLOCKERs, no STOP-AND-ASK.**
Subject: revision 1 of `plans/PDA-DEC-7-xrce-by-document.md` (300 lines, exactly at cap) +
`plans/PDA-DEC-7-brief.md` (60). No shell in this session, so I could not run
`git diff c682be1..c50f7e5`; I read the revised files as they stand and checked every claim the
revision makes against the tree and against cycle 1's record of the prior state.

Nothing cycle 1 cleared has been disturbed: the ephemeral-port listener still carries the
falsifiability alone (§6), §4.1's disclosure clause is still answered structurally (§4), the
unshared reader is still argued from decision 8 + `in_process_provider.cpp:44-47` (§3, with
"do not re-propose it" added), `agent=HOST:PORT` is unchanged, `0 → 65536` is unchanged (P3),
and the `max_payload` / serial deletions are unchanged. All nine cycle-1 DEBT items are folded.

## B1 — closed, and here is the one mutation that still passes

The three legs of the revision hold up:

**1. The two witness-less keys are genuinely deleted, and the loss is real.** Verified: today
`stream_history` is read at `xrce_dds_pubsub_provider.cpp:352` (buffer sizing) and `:393`/`:396`
(the `history` argument to `uxr_create_{output,input}_reliable_stream`), and `run_loop_ms` at
`:412`. The *only* places either is **set** are the two tests this item retires
(`test_xrce_provider.cpp:64-65`, which merely echo the field back). So "no production caller sets
them, nothing observes them, fixed constants of the same value" is accurate, the narrowing is
real (a C++ caller can set them today and cannot after), and it is disclosed in the design (§2,
Risks, Files-to-delete), the Brief's *Deleted* bullet and the README plan. Adequate disclosure —
the owner meets it in the Brief without being asked to decide it, which is right for a narrowing
that cycle 1 itself named as the cheapest fix. Values match today's defaults exactly
(`…hpp:45,48`), so decision 13 is untouched. The constants must stay out of the installed header
or the surface count changes; §1's "declares exactly two things" already says so.

**2. `connect_timeout_ms` as `std::chrono::milliseconds` really does make the swap class a
compile error** — and with `run_loop_ms` gone there is no swap partner left anyway. Belt and
braces, ~0 lines, correct.

**3. The whole-struct row closes the reader boundary, but only the reader boundary.** A defaulted
`operator==` over `XrceSettings` is total over the fields that exist, and the row's document sets
all four keys to values distinct from their defaults (`0x12345678` vs `0xAABBCCDD` — both
decimal spellings in the table are exact), so any field the parser accepts and then fails to
assign differs from the expectation. M11 reddens it **for a hard-code inside
`ParseXrceDocument`**. It does not redden a hard-code in the *constructor*, because the row calls
the pure function and nothing else.

So, answering the question directly — **one mutation still passes**: *`ParseXrceDocument` is
correct; the constructor ignores `settings.connect_timeout_ms` and passes the old constant 3000
to `uxr_create_session_retries` (`:388`)*. The whole-struct row is green (it never reaches the
constructor); §6 row 1 is green (the listener still accepts, and the constructor still fails
`kTransportFailure`, just ~3 s later); the 24 `conformance_xrce` cases are green (they set 5000
purely as headroom, so 3000 only flakes on a slow Agent). The same mutation on the other three
keys **is** caught: `transport`/`agent` by row 1 through a real socket, and `session_key` by the
24 conformance cases — verified as a genuine witness, not a hopeful one:
`xrce_main.cpp:45-69` hands out a fresh key per clause from a counter *because* "13 sequential
sessions reusing one key" races the previous session's teardown, and `xrce_peer_main.cpp:38-46`
derives its own from the pid.

That residual is **DEBT, not a BLOCKER**: the consequence of a wrong connect budget is a typed
`kTransportFailure` at the wrong deadline, not a wrong answer with no signal, and the fix is ~3
lines rather than a redesign (C2-2 below). B1's own hazard — two keys with *no witness anywhere,
green forever* — is gone, and the acceptable-fix line cycle 1 wrote (whole-struct row + forbid
the two + distinct duration type) has been taken in full.

## The demotion of the empty-document row — honest, but it took H2's only witness with it

The demotion is honest about falsifiability: the port is ephemeral, so no build can hard-code
its way to an accept, M2 was indeed unachievable, and §6 plus the mapping table now both say the
row is a harness control that no build can redden. Nothing else in the design leans on it as a
guard.

But after the demotion, that row is the **only** place in the item where an *empty* document is
constructed at all, and it asserts only "no connection". §4.1 fixes "empty document = every
published default" (H2), and `PublishedDefaultsAreExact` proves that for a *non-empty* README
block, not for emptiness. A build that refused an empty document with `kInvalidArgument` passes
every row as written. Loud, not silent — so DEBT (C2-3), with a one-line fix that costs nothing
because the row already runs.

## Mutations vs mechanisms — checked row by row

Each stated mutation is distinguishable through the mechanism its row uses; this is the class
that cost PDA-DEC-6 a cycle and it is clean here. M1/M3/M10 all fail to reach an accept on an
ephemeral port ✓. M4 (accept-and-default one key) reddens that key's refusal row ✓. M5 ✓.
M6 reddens `PublishedDefaultsAreExact` because the README parse and a default-constructed
`XrceSettings` diverge ✓. M7: the serial arm throws at the transport switch (`:375-376`) before
any transport object exists, so `kTransportFailure` ≠ `kNotSupported` ✓. M8/M14 both work only
because DEBT-6 routed that row through `Register` + `Create` — verified: `TranslateSeamFailure`
is the translating factory (`core/.../status.hpp:133`, rethrow at `:136`) ✓. M9 ✓. M11/M12 as
analysed above ✓. M13 is a form mandate rather than a mutation, which is the right treatment for
a build-system defect. Row 1's `connect_timeout_ms=0` does buy a single attempt: `retries =
max(0, (0-1)/1000) = 0` (`:385-387`) ✓, so the 4–5 s budget is right (the control row pays the
default 3000 → 2 retries).

One overclaim inside the new row: "a field added later and left unassigned is caught without
editing the row" is false — a fifth field the parser forgets compares default-against-default in
the expectation and stays green. C2-4.

## The three recorded decisions — authority citations checked

1. *Inert `max_payload` cap deleted.* **Citation does not name an oracle.** "the round's
   delete-first lean default" (design §2, §8.1; Brief decision 1) appears nowhere in
   `docs/pubsub-interface-spec.md`, `plans/PDA-decouple-locked-decisions.md` or the rulings
   ledger — grepped; the only occurrences in the tree are review prose about coexistence bridges.
   The **decision is nonetheless authorized**, by two things it should cite instead: cycle 1's
   verified P5 (nothing in `src/` reads `max_payload`, so nothing observable changes) and the
   2026-09-01 split ruling ("It should not do any development on any of the ABIs, only prepare
   for them"), which puts "make the cap real" outside the round. Not a misattribution to a
   ruling that says something else — an attribution to a norm that is not written down. C2-6.
2. *`transport=serial` nameable but distinctly refused.* **Accurate.** The 2026-09-02 ruling's
   own words are "The path is a valid selection and fails with a distinct … message, separate
   from an unknown name", and the design uses the very status that ruling assigned
   (`kNotSupported` vs `kInvalidArgument`). Applied as a pattern beyond the ruling's `Create`
   scope, and both documents say so in those terms. Cycle 1 read it the same way.
3. *Document tolerance strict.* **Substance accurate, item wrong.** The tolerance paragraph is
   landed by **PDA-DEC-5** (`docs/pubsub-interface-spec.md:420-436`), not PDA-DEC-6 (which
   landed §4.1's Fast DDS paragraph and the disclosure clause). Fixed in place. The Brief's
   wording ("spec §4.1 as landed") was already correct.

Zero owner asks remain in the Brief ✓.

## Budgets, counted

- Design **300/300** — at cap, not over. Every required section survives: rungs 1/2, handled
  residue with a *why not forbidden* line each, P1–P6 with stop conditions, mutations on every
  row, real Files-to-delete, Numbers. It did not grow by restating the spec (it cites §4.1/§4.2
  rather than quoting them). Consequence for the PM: the doc has **no room left**, so every
  cycle-2 DEBT item is written to land in code/README/tests, not in the design.
- Brief **60/60** ✓.
- Public surface **+2/−3 = net −1**, recounted: `RegisterXrceProvider` and
  `XrceDDSPubSubProvider(const ProviderConfig&)` in; `XrceConfig`, `XrceTransport` and
  `XrceDDSPubSubProvider(const XrceConfig&)` out. `XrceSettings`/`ParseXrceDocument` are
  `src/internal/` ✓. Field arithmetic checks out against the header: 12 fields = 2 typed core
  (`payload_bound`, `domain_id`) + 5 behind 4 keys + 5 deleted.
- Test-count arithmetic 10 − 4 + 7 = 13 (+ MSVC probe = 14) matches the seven provider-suite
  rows listed, with `Registry.XrceResolvesAsABuiltIn` correctly outside them — and correctly
  outside `conformance_registry`, whose narrow link line spec §4 clause 4 protects
  (`Registry.FastDdsResolvesAsABuiltIn` lives in `subjects/fastdds_main.cpp:68`, the precedent
  the design mirrors; `src/registry.cpp` stays untouched except for cross-reference prose).
- **+1900/−400 is honest, not arbitrary.** It matches cycle 1's independent estimate
  (1900–2100), the basis is itemised and sums to ~1670 with ~230 of slack, and it moves in the
  direction the last three items' outturns demand. If it is wrong it is wrong on the additive
  side (the cross-platform TCP listener at ~250 is the item most likely to run over); the −400
  deletion half is generous — actual removals look nearer 200 — but a generous deletion figure
  is not a hazard.

## P1 — framing intact, still not machine-verifiable here

§6 states it as a premise with a pointer to P1, and P1 says in terms: "**Not machine-verified**
(the client is FetchContent'd, no source on disk) … confirm before writing the listener.
**STOP-AND-ASK if it defers the connect** … Do not weaken the row to 'the constructor threw'."
No promotion to fact anywhere. I tried again to obtain the client source cheaply — no
`tcp_transport*.c` anywhere in the tree, no `_deps` sources, and `c:/fl-uxa-install` carries
headers only. So it stays a premise; step 3 confirms it in its first ten minutes, as written.

## No oracle-wins tripwire

No design sentence contradicts spec §4.1/§4.2, the rulings ledger, or a locked decision.
Decision 8 is honoured (the reader is the provider's, unshared, dependency-free — §4.2's own
requirement), decision 13 is untouched (both deleted fields become their current values),
decision 14 is untouched, and no link-size check was added (interface line 216-218 forbids
requiring one). One plan-doc drift for the PM, not the architect: `PDA-decouple-interface.md:214`
still reads "`XrceConfig`'s POD fields become document keys", which is now four keys plus five
deletions — the PM updates that doc at close (PDA-DEC-1 precedent), and it is oracle 4, below
the spec and the rulings, so it does not bind the design.

## NITs — fixed in place, silently

Four one-line corrections applied to the design: the `xrce_test_hook.hpp` claim (the header
exposes `RunReentrantUnsubscribeSchemaFlushScenario` only — no buffer accessor, so a future
`stream_history` witness needs *a hook like* it, not that hook as it stands); §8.3's
PDA-DEC-6 → PDA-DEC-5; M11's parenthetical ("B1's build, reader half", per the analysis above);
and the two "~1 s" claims attached to `SerialTransportNotImplemented`, which cannot cost that
(it throws before any session attempt) — the ~1 s belongs to `ConstructorThrowsWithoutAgent`.

## DEBT (7) — appended to `design-debt.md` under §PDA-DEC-7, cycle 2

C2-1 P5's sentence is false for the two newly-deleted fields and its stop condition misfires
(must land in this PR).
C2-2 `connect_timeout_ms` is the one key whose landing is asserted only at the reader boundary;
bound row 1's wall clock, and carry the 1000 ms granularity note into the README.
C2-3 The demoted control row is now H2's only witness; have it assert `kTransportFailure`, not
merely "no connection".
C2-4 "a field added later … caught without editing the row" is false; §2's key-with-its-witness
rule is what polices growth.
C2-5 `AgentUnreachableIsATransportFailure` must name an unused port (`19999`) and
`connect_timeout_ms=0`, or an Agent on 2018 turns it red for the wrong reason.
C2-6 Recite decision 1's authority as verified-P5 + the 2026-09-01 split ruling; "delete-first
default" is not in any oracle.
C2-7 `xrcedds-pubsub-provider/README.md:101` names both retired tests and mis-attributes ~1 s to
the serial one; the rewrite replaces it with the 4–5 s forcing-case story.

Full text in `design-debt.md`.
