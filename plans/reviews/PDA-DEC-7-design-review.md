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
