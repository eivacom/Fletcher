# PDA-DEC-7 — architecture-conformance review (step 4a, independent)

**Verdict: PASS-WITH-FINDINGS(4).** No blocking conformance defect. No ruling contradiction,
no locked-decision deviation, no weakened safety contract, nothing surviving that the design
ordered deleted. Subject: `4baac0f` (base `5756e67`, 20 files, +1471/-228, tree clean).
Oracles read: `docs/pubsub-interface-spec.md` §4/§4.1/§4.2/§5.1,
`plans/PDA-decouple-locked-decisions.md` (decision 8 above all),
`plans/PDA-DEC-rulings.md`, `plans/PDA-DEC-7-xrce-by-document.md`,
`plans/reviews/PDA-DEC-7-design-review.md` (both cycles),
`plans/reviews/design-debt.md` §PDA-DEC-7 + cycle 2, `plans/PDA-DEC-7-brief.md`.

The four findings are all statements-in-the-tree, each one line to fix; none changes behaviour
and none opens a design question. They are listed most-blocking-first anyway.

---

## Findings

### F1 — the spec §4 clause 4 amendment overreaches, and the tree contradicts it

The amendment turns a scoped true sentence into an unscoped false one:

> `RegisterXrceProvider`, PDA-DEC-7) — each in the provider's own component, so **no caller
> names a concrete provider type**.

Before the amendment this read "so **the gateway** names no concrete provider type", which was
true and is still true. As amended it is contradicted by the tree in five places, four of them
landed by *this* commit: `integration-tests/pubsub-conformance/subjects/xrce_main.cpp:91`,
`subjects/xrce_peer_main.cpp` (the `make_shared<XrceDDSPubSubProvider>` at the end of the
factory), `integration-tests/fastdds-xrce-interop/tests/test_interop.cpp:299,364,430`, the
provider README's own example (`XrceDDSPubSubProvider direct(config);`) and
`xrcedds-pubsub-provider/test_package/src/example.cpp`. Direct construction is *deliberately*
still legal — design §1 keeps exactly one public constructor rather than removing it — so the
spec should not claim nobody uses it. The spec is the authoritative oracle here, which is why
this is finding 1 rather than a nit.

*Acceptable fix (one line):* restore the scope — "so the gateway names no concrete provider
type", or "so a caller that selects by name names none".

### F2 — §4.1 is *named* as the single tolerance oracle for both readers, but does not state the rules for this one

Design §3 is explicit: "**Spec §4.1 is the single tolerance oracle for both readers**", and
cycle-1 DEBT-4 asked for exactly that naming. The XRCE paragraph names it
("the two in-tree `key=value` readers share this section as their single tolerance oracle and
nothing else") and then states **none** of the rules the XRCE reader actually implements: the
trailing-`\r` strip, the blank-entry skip, "nothing else trimmed", no case folding, no
comments, and the up-front NUL refusal. The only statement of those rules inside §4.1 lives in
the PDA-DEC-5 paragraph *below* it, written as the loopback's own — "**its** document ..." — and
the NUL rule there is explicitly narrowed to the loopback: "**This provider** refuses a
document containing an embedded NUL ... a provider-level rule about what its own format can
represent, not a seam-level one."

So the oracle that `xrce_document.hpp` cites ("Tolerance, verbatim from the loopback (spec
§4.1, as landed by PDA-DEC-5)") and that `XrceConfig.ToleranceRulesMatchTheLoopback` claims to
pin does not, as amended, say those rules apply to this reader. The code is right; the oracle is
thin, and it is the artefact a third `key=value` reader would be judged against.

*Acceptable fix (one sentence, in the XRCE paragraph):* "Tolerance is exactly the loopback's,
below — trailing `\r` stripped, blank entries skipped, nothing else trimmed, no case folding,
no comments — and an embedded NUL is refused up front for the same reason."

### F3 — `connect_timeout_ms` in 1...1000 is accepted, can never work, and the README describes the band wrongly

Verified in the client source now on disk (`microxrcedds_client` 3.0.1, Conan build cache
`.../fletc2077cb5338099/b/build/_deps/microxrcedds_client-src`):

- the provider computes `retries = max(0, (budget_ms - 1) / 1000)`
  (`src/xrce_dds_pubsub_provider.cpp`), so **every** budget from 0 to 1000 inclusive gives
  `retries == 0`;
- `wait_session_status` with `attempts == 0` sends once and returns `true` **without ever
  listening**, leaving `last_requested_status == UXR_STATUS_NONE` (`session.c:742-746`;
  `UXR_STATUS_NONE` is `0xFF`, `session_info.h:37`);
- `uxr_create_session_retries` then computes `created = received && UXR_STATUS_OK == ...`
  (`session.c:246-247`), which is **false unconditionally**.

So any `connect_timeout_ms` <= 1000 makes construction throw `kTransportFailure` *no matter what
is on the other end*. The README says the opposite of that:

> **every value below 1000 ms means one attempt** and `0` means one attempt that does not wait
> for an answer at all.

There is no "one attempt" for 1...999 that differs from 0 — none of them waits, and none of them
can succeed. The installed header repeats the same shape ("`kTransportFailure`: ... an Agent that
does not answer within `connect_timeout_ms`"). The design shares the imprecision (§6: "Row 1's
`connect_timeout_ms=0` does buy a single attempt"), so this is **design-inherited, not drift** —
but it is a sentence in the tree contradicted by the tree, in the round's most-ruled-on class
(a range that admits a value which cannot work, documented rather than said plainly). C2-2 asked
for the granularity note and got one; it is the *content* of the note that is wrong, not its
absence.

*Acceptable fix (one line, README + header):* say it plainly — "any budget of 1000 ms or less
means the handshake is not awaited at all, so construction always fails; the first budget that
can connect is 1001 ms." (Refusing 1...1000 in the reader is the stronger option but would make
`0` a special case, and design §6 requires `0` to stay legal.)

### F4 — `AgentUnreachableIsATransportFailure` cannot witness unreachability, and says it can

The row uses `agent=127.0.0.1:19999` + `connect_timeout_ms=0`, which is exactly what review
C2-5 prescribed — **so this is conformance, not deviation.** But by F3's mechanism the
constructor fails at 0 ms whether or not anything answers on 19999, which makes the row's own
comment and its guarded branch untrue:

```cpp
// Only reachable if something answers XRCE on 19999, which would be a broken machine
// rather than a broken build - say so instead of failing on the status.
ASSERT_EQ(provider, nullptr) << "something is answering XRCE on 127.0.0.1:19999";
```

`registry.Create` can never return non-null here, so that assertion is dead. The row still
earns its place — it is the witness for M8 (typed `kTransportFailure` rather than `kInternal`
through `TranslateSeamFailure`) and for M14 (registration under the name `xrce`, in the
provider's own Agent-free CI), both of which I confirmed are real. Only its *name and comment*
claim a third thing it does not have.

*Acceptable fix (one comment line):* state that at a 0 ms budget the handshake is not awaited,
so the row witnesses the typed status and the registry route, not reachability. (Leaving the
document alone is correct — C2-5 chose it deliberately, and a budget that *could* succeed would
make the row Agent-sensitive.)

---

## Claims pressure-tested — reproduced, not accepted

1. **P1 — CONFIRMED as fact, on both platforms.** `uxr_init_tcp_transport` calls
   `uxr_init_tcp_platform` and returns true only if it did (`tcp_transport.c:311-333`).
   `tcp_transport_windows.c` and `tcp_transport_posix.c` both do `socket()` -> `getaddrinfo()`
   -> a loop of **blocking `connect()`**, setting `rv = true` on the first success and returning
   `false` when every candidate fails. There is no lazy-connect path in this client. The
   forcing row's oracle is sound and the stop condition does not fire.
2. **Red-for-the-right-reason — reproduced.** I applied M1 (`impl_->settings =
   internal::XrceSettings{}` after the parse) in the cache build and rebuilt:
   `DocumentConfiguresTransport` failed on **both** stated mechanisms — "nothing ever connected
   to 127.0.0.1:64523, which the document named" and "elapsed 2006 ms vs 1000". Both are real:
   the accept latch is a kernel-chosen port no build can reach, and the 2006 ms is deterministic
   (`UXR_CONFIG_MIN_SESSION_CONNECTION_INTERVAL` is fixed at 1000 and the default budget buys 2
   attempts), against a correct-build cost of a few milliseconds. Not timing-flaky: ~200x
   headroom below the bound, 2x above it. Every other `XrceConfig.*` row stayed green under M1,
   which is the correct polarity — row 1 carries the falsifiability alone, as designed.
3. **The drift guard reads the README at run time, in the packaged build — reproduced.** In the
   `conan create` cache build (`.../fletce2d815f99790d`) I edited the exported README's
   published `agent=` port and re-ran the binary **with no rebuild**:
   `PublishedDefaultsAreExact` went red naming the drift. Removing the file made it fail hard
   with the path, not `GTEST_SKIP`. `README.md` is in `exports_sources`, so the cache build
   reads the file the repository holds and the recipe revision moves with the README's content.
   PDA-DEC-6's held-copy defect is not reproduced here.
4. **`EveryKeySetNonDefaultLandsWholeStruct` — reproduced, and it is the only row that catches
   the class.** I applied M11 (range-check `session_key`, then assign the old constant
   `0xAABBCCDD`): the whole-struct row went red, while `DocumentRefusalsAreTypedAndQuoted` and
   `PublishedDefaultsAreExact` **both stayed green** — precisely the accept-and-discard hole
   cycle 1's B1 named. All four surviving keys are set away from their defaults in one document,
   the comparison is a defaulted `operator==` over the aggregate, and no Agent or socket is
   involved. A hard-coded constant in the reader cannot survive it.
5. **Deviation (a) — `XrceTransportKind` without `kSerial`: strictly stronger, and the guard set
   is not weakened.** Design "Rung 2" item 12 already required serial to be refused **in the
   reader, before any transport exists**; dropping the enumerator makes the settings value
   unrepresentable as well, which is design rung-1 case 4's own ambition. M7 ("route serial into
   the transport switch") now needs an enumerator added first, i.e. it is a two-edit mutation —
   but the guard it validates is intact and asserts `kNotSupported` twice, from the pure function
   *and* through the constructor, so any routing that produced `kTransportFailure` still reddens.
   Loss of a one-edit mutation is not loss of reachability.
6. **Deviation (b) — value-side whitespace: honest engineering, and it matches the higher
   oracle.** Spec §4.1's rule is "nothing else is trimmed"; refusing `agent= 127.0.0.1:2018`
   would require the reader to *judge the host*, which design H1 explicitly reserves to the
   client's resolver ("the host is handed to the client unchanged ... Fletcher does not know what
   that resolver accepts"). The implementation keeps the byte verbatim, **asserts** it verbatim
   (`EXPECT_EQ(ParseXrceDocument(...).agent_host, " 127.0.0.1")`), and the value then fails typed
   at the transport. Nothing is silently repaired, nothing is accepted-and-discarded, and the
   README's refusal list was corrected rather than left claiming a refusal that does not happen.
   This is **not** documenting a silence: the silence this item exists to forbid — a
   half-specified address — is forbidden by the one-key rule, and is tested. Conforming.
   *Record for the design file:* cycle 1's line "Value-side whitespace (`agent=x:2018 `) dies in
   the port parse ... so 'nothing is tolerated silently' is total" is true only for the port half.
7. **Counts — measured, all three correct.** Provider suite: **14 ctest entries** = 13 gtest
   cases (6 surviving `XrceProviderTest.*` + 7 new `XrceConfig.*`) + the MSVC nodiscard probe;
   ran 14/14 in the packaged build. `conformance_xrce`: **one** ctest entry (`add_test`, not
   `gtest_discover_tests`) and **25** gtest cases by `--gtest_list_tests` (1 `Registry` + 12 x 2
   `ProviderConformance` instantiations); ran 25/25. `fastdds-xrce-interop`: one entry, three
   cases; ran 1/1. No off-by-one this time.
8. **Retirements — each replaced, none weakened.** `DefaultValues`/`CustomValues` (struct field
   echo, including the inert `max_payload`) -> `PublishedDefaultsAreExact` (whole-struct, read
   off disk) + `EveryKeySetNonDefaultLandsWholeStruct` (whole-struct, all four keys non-default):
   strictly stronger, and the three fields whose defaults are no longer asserted
   (`max_payload`, `stream_history`, `run_loop_ms`) are the deletions cycle 2 approved.
   `ConstructorThrowsWithoutAgent` (`EXPECT_THROW(std::runtime_error)`) ->
   `AgentUnreachableIsATransportFailure` (asserts `kTransportFailure` *and* routes through
   `RegisterXrceProvider` + `Create`): stronger on both axes, see F4 for the one thing it does
   not witness. `SerialTransportNotImplemented` -> `SerialIsRefusedAsUnsupported` (asserts
   `kNotSupported`, distinct from a typo, and re-checks through the constructor): stronger. The
   retirement comment block left in `test_xrce_provider.cpp` cites real design sections and the
   named tests are genuinely gone. No coverage was removed without replacement.

## Converse check — what survived that should not have

Nothing. Grepped, not taken on trust:

- **`XrceConfig` / `XrceTransport`:** no declaration, no definition, no shim, no coexistence
  window, no deprecation marker anywhere in the tree. Every surviving occurrence is (a) prose
  about the retirement (spec, README, header banner), (b) the *test suite name* `XrceConfig`,
  which the design mandates (`ctest -R '^XrceConfig\.'` is the runbook's inner loop), or (c)
  the internal `XrceTransportKind` in `src/internal/`, which is not public surface.
  `XrceConfigFor(...)` in the two harnesses is a local factory returning `ProviderConfig`.
- **A path configuring XRCE without a document:** none. One public constructor, `explicit`,
  **no default argument**, over `ProviderConfig`; the only other entry is
  `RegisterXrceProvider`. An empty document is H2 (all published defaults) and is asserted as
  such, not as a refusal.
- **A parser or dependency in Fletcher (decision 8):** none. `pubsub/` changes in this diff are
  comment-only (`provider.hpp` 5/5, `provider_registry.hpp` 6/4). The reader is
  `xrcedds-pubsub-provider/src/internal/xrce_document.{hpp,cpp}`, `<string>` + `<chrono>` +
  `PubSubError`, unshared and not installed. The document holds content, never a filename, and
  nothing opens a file on a provider's behalf (the only file read in the diff is a *test*
  reading the README).
- **JSON/YAML linked into the edge provider, or a link-size check:** neither. No new dependency
  in `xrcedds-pubsub-provider/conanfile.py` (the only change is exporting `README.md`), and no
  size/footprint assertion anywhere — correct, that proxy is PDA-ABI's
  (`plans/PDA-decouple-interface.md:216-218`).
- **ABI work:** no `extern "C"`, no C header, no `dlopen`/`LoadLibrary`, no version negotiation,
  no vtable. Nothing above the seam branches on built-in vs loaded; the registry's signature is
  untouched (P4 holds).
- **`ProviderConfig`'s typed core:** still exactly `{max_payload_bytes, domain_id}` plus
  `document`. No field added, no field typed for XRCE's benefit.
- **Copies on the row or attachment path:** none introduced. The diff touches construction and
  config only; the one data-path edit is `impl_->config.domain_id` -> `impl_->domain_id`, an
  already-narrowed `uint16_t` read at two `uxr_buffer_create_participant_bin` call sites. The
  provider does **not** retain the document (spec §4.1's "a provider that keeps the document
  copies it" — this one does not keep it).
- **The two `key=value` readers:** I diffed them line by line
  (`xrce_document.cpp` vs `pubsub/src/in_process_provider.cpp:75-131`). The tolerance rules are
  *genuinely* identical, not nominally: same `while (start <= document.size())` entry split,
  same single trailing-`\r` pop, same blank-entry `continue`, same up-front `find('\0')`
  refusal, same no-`=` / unknown-key / duplicate-key refusals quoting the entry, same
  `kInvalidArgument`. No drift yet. **Nothing makes the claim mechanically testable**: the pin
  is two hand-written row sets that must be kept in step by hand
  (`XrceConfig.ToleranceRulesMatchTheLoopback` here; `Registry.InProcessRefusesAnUnrecognised-`
  `DocumentEntry` / `...DocumentToleratesCrlfAndBlankLines` / `...RefusesADocumentContainingANul`
  in `integration-tests/pubsub-conformance/src/registry.cpp`). That is decision 8's accepted
  cost and design §3 says "do not re-propose sharing it" — so it is not a finding, but it
  remains a drift generator, and F2 is why it now matters more: the shared oracle those two row
  sets are supposed to encode is not fully written down. One structural asymmetry, not drift:
  XRCE has a free-form value (`agent`), the loopback has none, so the value-side rows exist only
  on the XRCE side.

## Corner-case ladder — survived

Rung 1: (1) no document-free construction (no default argument). (2) half-specified address
unrepresentable (one key; refusals tested). (3) no silent narrowing — `ParseDecimal` into
`uint64_t` with overflow rejection, then per-key range check; `agent=127.0.0.1:67554` (2018 +
65536) is refused, which is the row that proves it. (4) serial *stronger* than designed (claim
5). (5) `max_payload` deleted. (6) validate-then-touch-the-world: the constructor's order is
`ParseXrceDocument` -> `domain_id` -> bound -> buffers -> transport -> session, so every
document refusal precedes all I/O, and the accept-and-discard half is closed by the whole-struct
row. (7) swapped duration is a compile error (`std::chrono::milliseconds`, and its only integer
partner was deleted). (8) IPv6 refused and disclosed.

Rung 2 (9-13): every refusal has a row asserting **the status and the quoted entry**; no refusal
became a recovery path; the two deleted key names (`stream_history`, `run_loop_ms`) and three
deleted field names (`max_payload`, `serial_device`, `agent_ip`) are all tested as unknown keys.
`domain_id > 65535` and a bad `max_payload_bytes` are refused by the constructor as
`PubSubError(kInvalidArgument)`, not `std::invalid_argument` — P2's `kInternal` trap is closed.

## Budget

Verified from `--numstat`: total **+1471/-228**, under the declared +1900/-400. Production
**+517/-111** counting everything outside `tests/`, docs and READMEs — i.e. **+506/-102**
excluding the two comment-only `pubsub/` header edits, which is the figure reported, so the
breakdown is honest and the new `src/internal/` header is inside it. Nothing owed was skipped
to get there: all seven designed provider rows landed, plus the `conformance_xrce` registry
row, and I found no designed guard missing. The leanness is real and comes from form — the
refusal table is ~65 lines of one-line `ExpectRefused` calls where ~350 were budgeted, and the
cross-platform listener is ~110 against ~250 — not from dropped coverage.

## RECORD (PM corrects in place; not blocking, no fix cycle)

- `integration-tests/pubsub-conformance/CMakeLists.txt`'s comment on the `conformance_xrce`
  entry still says "24 clauses, ~26 peer spawns"; the binary now holds 25 gtest cases
  (24 clauses + `Registry.XrceResolvesAsABuiltIn`).
- `Files-to-touch` lists `xrcedds-pubsub-provider/tests/discard_probe.cpp` and
  `integration-tests/pubsub-conformance/src/registry.cpp`; neither was touched. Neither needed
  it — the probe holds an `XrceDDSPubSubProvider&` and names no retired type (it still compiles
  and its ctest entry still passes), and registry.cpp's entry was cross-reference prose only.
- `plans/PDA-decouple-interface.md:214` says "Four of `XrceConfig`'s POD fields become document
  keys"; it is five fields behind four keys (design §2: 2 typed core + 5 behind 4 keys + 5
  deleted = 12).
- Design cycle-1 review's "value-side whitespace ... 'nothing is tolerated silently' is total"
  is true for the port half only; see claim 6.

## Evidence run on this box (2026-09-02, at `4baac0f`)

- `conan create xrcedds-pubsub-provider --build=fletcher-xrcedds-pubsub-provider/* -o
  run_tests=True` — genuinely compiled (microcdr + microxrcedds_client from source, not
  "Already installed!"), **14/14** ctest, 4.63 s; `test_package` built and ran against the
  installed header, `kPayloadBytes<64 * 1024> == 65536` checked there.
- `conformance_xrce.exe` (the binary the implementer built at 18:49 from this commit) —
  **25/25**, Agent spawned per test.
- `fastdds-xrce-interop` `ctest` — **1/1** (3 cases), after clearing
  `C:\ProgramData\eprosima\fastdds_interprocess`.
- Mutations M1 and M11 applied in the cache build, rebuilt and run (results above), then
  reverted; the packaged `.lib` predates both mutations (packaged 19:08:03, mutations after
  19:09) and the final restored build is green 7/7. Repository working tree untouched.

---

# Cycle 2 re-review (2026-09-02)

Independent, adversarial. Subject: the fix pass **`33d7514`** over `4baac0f`. Cycle 1
(`PASS-WITH-FINDINGS(4)`) and `PDA-DEC-7-codereview.md` (1 blocking, 3 should-fix, 6 nits) read,
not repeated. Every claim below was reproduced on this box — build, mutate, run, restore — not
read. Working tree verified clean at start and at finish.

**Verdict: PASS-WITH-FINDINGS(3).** The blocking B1 fix is real and I proved it live against an
Agent. The socket leak is structurally closed and I measured zero leaked handles on both throw
paths. S2, F4 and F2 all landed and do what they claim. **No blocking conformance defect. No
ruling contradiction — including the one the owner asked about, on which the direction stands.**

---

## THE DIVERGENCE QUESTION — verdict: the ruling does NOT govern document tolerance. The direction stands. No stop-and-ask is owed.

The ruling in question, verbatim:

> "Fix in-round, before the ABI — Any divergence found is fixed as part of PDA-1/PDA-2 so all
> three providers agree before the ABI wraps them. Means the ABI is defined over consistent
> behaviour rather than freezing an inconsistency."
> **Context:** what happens to divergences the conformance suite finds. **Applies to:**
> PDA-DEC-1. [...] Pinning a divergence as known-divergent instead of fixing it is a violation.

### The case that it DOES govern document tolerance (put as strongly as it deserves)

1. The operative words are unqualified — "**Any** divergence", "**all three** providers agree" —
   and the closing sentence is a flat prohibition with no carve-out.
2. This round *itself* treated the two `key=value` readers as owing each other agreement. Design
   §3: "The tolerance rules are PDA-DEC-5's, adopted **verbatim** ... **Spec §4.1 is the single
   tolerance oracle for both readers**." Design §8 item 3: "Document tolerance is strict.
   *Authority:* spec §4.1 as landed by PDA-DEC-5." A test is literally named
   `ToleranceRulesMatchTheLoopback`. Against that framing, making one reader stricter and writing
   the difference into the spec is textbook "pinning a divergence as known-divergent".
3. The one ruling that *did* carve itself out of the divergence ruling drew the line the other
   way: 2026-09-01 "Pin at one" says "the 2026-08-31 divergence ruling ... governs
   **cross-provider** divergences; this is a uniform `Blob` limitation". XRCE-versus-loopback
   tolerance is cross-provider on its face, so that carve-out does not shelter it.
4. In-round precedent points at fixing: 2026-09-01 "Conflicting topic re-declaration is refused"
   took a genuine three-way behavioural divergence and *fixed all three* rather than record it.

### The case that it does NOT — and why it wins

**(a) Scope, from the ruling's own recorded context.** The Context line is "what happens to
divergences **the conformance suite finds**"; the Applies-to line is "**PDA-DEC-1**"; the
rationale is "so the ABI is defined over consistent behaviour rather than freezing an
inconsistency" — and what the ABI is defined over is the delivery contract. I checked
mechanically whether the suite can even *find* a document divergence: it cannot, and cannot be
made to without changing what it is. There is **no** `ProviderConformance` clause about documents.
Every document row in the tree is provider-specific and lives outside the parameterised suite —
`Registry.InProcessRefusesAnUnrecognisedDocumentEntry`,
`Registry.InProcessDocumentToleratesCrlfAndBlankLines`,
`Registry.InProcessRefusesADocumentContainingANul` in
`integration-tests/pubsub-conformance/src/registry.cpp`, and `XrceConfig.*` in the provider's own
suite. The suite's 24 clauses are the delivery contract; document syntax is not in them.

**(b) The owner's own configuration rulings mandate per-protocol document syntax, so the broad
reading makes two of the owner's rulings contradict each other.** 2026-08-31, configuration shape:
"Everything else is an **opaque** JSON/YAML/XML blob **only the driver parses** — for Fast DDS that
can be its native XML QoS profile". 2026-09-02: "**The XML text** — The setting carries the profile
content." And spec §4.2, normative: "The document's format is the *provider's* — Fast DDS's native
XML QoS profile, `key=value` for XRCE, JSON5 for a future Zenoh. One mechanism, **no uniform
format**." XML tolerates whitespace inside elements as a matter of the format's definition;
`key=value` refuses it. If the divergence ruling governed document tolerance, PDA-DEC would have
been in violation from the moment Fast DDS's document became XML — by the owner's own instruction.
A reading that puts two of the owner's rulings in conflict is the wrong reading of one of them.

**(c) Decisively: nothing was pinned, because there is no divergent behaviour to pin.** I read
every branch of the loopback reader (`pubsub/src/in_process_provider.cpp:75-131`) and enumerated
the input space. The loopback has exactly **one** key (`schema_carriage`) matched by exact string
equality, and **one** closed value set (`as_declared` | `carried`), also exact. Therefore, for any
document entry containing a byte below `0x21`:

- the byte is left of the `=` → `key != "schema_carriage"` → `PubSubError(kInvalidArgument)`,
  quoting the entry;
- the byte is right of the `=` → value in neither branch → `kInvalidArgument`, quoting the entry;
- the entry has no `=` (including an all-whitespace entry) → `kInvalidArgument`, quoting the entry;
- the byte is a trailing `\r`, or the entry is blank → stripped/skipped by **both** readers alike.

And in the other direction: the *only* documents the loopback accepts are blank entries and
exactly `schema_carriage=as_declared` / `schema_carriage=carried`. **None contains a byte below
`0x21` inside an entry.** So adding the `0x21` rule to PDA-DEC-5's reader would change **no
outcome for any input** — it would change only which sentence the message carries. The two readers
accept the same set of documents and refuse the same set with the same status. XRCE's rule has
independent force *only* where XRCE has a **free-form value** (`agent`'s host) and the loopback has
**none**.

There is consequently nothing "known-divergent" to fix: the remedy the owner feared owing — tighten
PDA-DEC-5's reader to match — would buy exactly zero behavioural change while re-opening a closed
item; and reverting S2 would restore a real defect (`agent= 127.0.0.1:2018` accepted with a space
kept in the host, refused a layer down by the resolver H1 says Fletcher must know nothing about).
Both remedies are worse than the landed state, which is the tell that the ruling was never pointed
here.

**Verdict.** The 2026-08-31 divergence ruling governs the **delivery contract** the conformance
suite tests, not per-protocol document syntax; per-protocol document syntax is legitimately
per-protocol by spec §4.2 and by two of the owner's own configuration rulings; and no observable
divergence was created or recorded. **S2 is in scope, the direction stands, no stop-and-ask.**

**But the sentence that landed overstates it, and that is finding F5.** "XRCE is **stricter on one
point**" invites precisely the reading the owner was worried about, in the artefact PDA-DEC-8/9 and
both ABI rounds inherit. See F5.

---

## Findings

### F5 — spec §4.1 records a divergence that does not exist, in the sentence two ABI rounds inherit

`docs/pubsub-interface-spec.md` §4.1 now reads:

> XRCE is **stricter on one point**, because "nothing is trimmed" turned out to be weaker than it
> sounds: any byte below `0x21` *inside* an entry — a space, a tab, a mid-entry CR — is refused
> [...] A third `key=value` reader is judged against this paragraph, not against either
> implementation.

The rule statement matches the code exactly (verified below). What is wrong is the *framing*: by
the enumeration in (c) above, the two readers refuse the same set of documents with the same
status. XRCE is stricter in **rule**, not in **outcome**, and the difference is unobservable
because the loopback has no free-form value for the rule to bite on. As written, the paragraph
reads as a recorded cross-provider divergence — the thing the 2026-08-31 ruling forbids — and it is
the paragraph a third reader and both ABI rounds will be judged against.

The honest sentence is also the stronger one, and it is one sentence:

> Both in-tree `key=value` readers refuse every entry containing a byte below `0x21`; XRCE states
> that as one rule up front because it is the only one of the two with a free-form value (`agent`'s
> host), where the loopback's closed key and value sets already refuse the same inputs. The rule
> governs bytes inside an entry and never the separators between them.

Same normative content for a third reader, no recorded divergence, and it stops a future reader of
the spec from re-litigating the question. *Not blocking* — the code is right and the rule is stated
correctly; the defect is that the oracle describes the rule's *status* wrongly.

### F6 — the socket-leak fix landed with no witness, and its absence is not disclosed

S1 is genuinely fixed and I measured it (below). But the thing that measured it is **gone from the
tree**. The test binary the fixer built at 19:38 is still on disk and contains a case that no
longer exists in any source file:

```
$ xrce_provider_tests.exe --gtest_list_tests   # binary of 19:38, pre-commit
LeakProbe.
  FailingConstructionDoesNotLeakTheSocket
```

and `xrcedds-pubsub-provider/build/Testing/Temporary/CTestCostData.txt` still lists it. `grep -rn
LeakProbe` over the source tree finds it in no `.cpp`, `.hpp`, `.txt` or `.md`. So the "200 failing
constructions → 0 handles leaked, versus 200 with the close suppressed" measurement was made with a
throwaway probe that was deleted before commit.

Compare B1, whose fix **did** land its table (`ConnectTimeoutBudgetBuysWholeAttempts`) — and whose
own root cause the code review diagnosed as "the only arithmetic in this item that lived outside
the reader, and it was the only arithmetic that was wrong ... it survived because only `=0` was
tested". S1 is now the piece of this item with a correct implementation and no red-on-regress: an
early `return`, a second close site, or a `catch` re-added above the destructor would restore the
leak silently. The structural argument in the code (one close site, unwind-driven,
`open_transport` read by both ends) is strong and I confirmed there is exactly one `uxr_close_*`
call site in `src/`, so this is **DEBT-shaped, not blocking**. What is not acceptable as it stands
is the *silence*: the design's fix-cycle-1 note says "S1. `Impl` gains a destructor, so the
transport is closed on every constructor-throw path" and does not say that nothing watches it.
Either land the probe (it is ~20 lines and needs no Agent — mine is reproduced below) or disclose
the gap in the design's fix-cycle note as DEBT.

### F7 — `ToleranceRulesMatchTheLoopback` and design §3's "adopted **verbatim**" now name something the code does not do

Same class as cycle 1's F4 (a row whose *name* claims a third thing): the test named
`ToleranceRulesMatchTheLoopback` now contains four rows that no loopback rule produces —

```cpp
ExpectRefused("agent= 127.0.0.1:2018",  ...);
ExpectRefused("agent=127.0.0.1 :2018",  ...);
ExpectRefused("session_key= 7",         ...);
ExpectRefused("agent=127.0.0.1\r:2018", ...);
```

— and its own body comment says so ("ONE whitespace rule, and it refuses (fix cycle 1, review 4b
S2)"), so the case simultaneously claims to match the loopback and documents its departure.
`plans/PDA-DEC-7-xrce-by-document.md:76` still says "The tolerance rules are PDA-DEC-5's, adopted
**verbatim**", contradicted by the tree; only the design's fix-cycle-1 appendix records S2, so the
design body and its own appendix disagree. Under F5's framing the name is *recoverable* — the two
readers do refuse the same documents — so the fix is small: rename to something like
`ToleranceRulesMatchSpec41` (or keep the name and add "and where it is stricter, the loopback
refuses the same inputs by its closed key/value sets"), and correct design §3's "verbatim" to
"PDA-DEC-5's, with the whitespace rule stated once instead of arrived at four ways".

---

## Claims reproduced — build, mutate, run, restore

Local build `xrcedds-pubsub-provider/build` (VS 17 2022, Release), rebuilt from `33d7514`.
Baseline: **15/15 ctest green, 6.66 s.**

1. **B1's mapping — proved live, in both directions.** I restored the exact old expression
   (`max(0, (ms-1)/1000)`, spelled equivalently), rebuilt, and ran against a real Agent
   (`C:/fl-uxa-install/bin/MicroXRCEAgent.exe udp4 -p 2018`) through a temporary probe:

   | budget | ceiling (landed) | floor (mutation) |
   |---|---|---|
   | 250 ms | attempts=1, **connected** (7 ms) | attempts=0, **NO** — *"failed to create a session ... within 250 ms (is the Agent running?)"* while it was |
   | 500 ms | attempts=1, **connected** (2 ms) | attempts=0, **NO** — same misattributed diagnostic |
   | 1500 ms | attempts=2, **connected** (2 ms) | attempts=1, connected |
   | 3000 ms | attempts=3, **connected** (2 ms) | attempts=2, connected |

   The defect and the fix are both exactly as described. `ConnectTimeoutBudgetBuysWholeAttempts`
   goes red under the old expression — **12** assertion failures, not the 11 reported (11
   `EXPECT_EQ` rows plus the range-loop `ASSERT_GE` firing at `ms=1`, which then aborts the loop).
   Restored and re-verified green.
2. **Mapping edges.** `60000 → 60` is right (each attempt costs up to
   `UXR_CONFIG_MIN_SESSION_CONNECTION_INTERVAL` = 1000 ms, so 60 attempts ≈ the 60 s budget).
   Above it is unreachable: the reader refuses `connect_timeout_ms=60001` with `kInvalidArgument`,
   and that refusal is asserted **inside the same row** as the table, which is the right place for
   it. Negative is impossible from the reader (parsed unsigned, then range-checked 0...60000) and
   is handled anyway (`ms <= 0 → 0`). Overflow of `ms + 999` needs `ms > 2^63 - 1000`, unreachable
   through the only caller and stated as a precondition in the header. `SessionAttempts` lives in
   `src/internal/`, which `conanfile.py::package()` does not copy (only `include/`), so the
   public-surface count (+2/-3, net -1) is unchanged.
3. **The socket leak — measured, zero, on both post-init paths.** 200 failing constructions each,
   `GetProcessHandleCount` before/after, after a 5-iteration warm-up:
   - UDP, throw **after** transport init (session creation fails): `before=77 after=77 delta=0`.
   - TCP, throw **after** transport init (connected to a test-owned listener that does not speak
     XRCE, 205 accepts observed): `before=86 after=85 delta=-1`.

   Path coverage checked by reading, not assumed: after transport init the only remaining throw
   sites are the `comm == nullptr` `kInternal` belt and the `uxr_create_session_retries` failure —
   both covered by `open_transport`, neither by `session_created`; and after `session_created =
   true` the only remaining failure is `std::thread` construction, where `run_thread` is not
   joinable and both conditionals still do the right thing. Exactly **one** `uxr_close_*` /
   `uxr_delete_session` call site exists in `src/` (`xrce_dds_pubsub_provider.cpp:569,577,580`, all
   inside `Impl::~Impl`), `~XrceDDSPubSubProvider` is `= default`, and `impl_` is a `unique_ptr`,
   so no double-close is reachable. Four successful constructions plus destructions against a live
   Agent completed clean, so the full-teardown path is not broken by the change.
4. **S2 breaks no legitimate structure, and fires before the `=` split.** Confirmed by
   construction, not by reading the code position: `"foo bar"` — an entry that is *both*
   whitespace-bearing *and* has no `=` — is refused with the **whitespace** message, so the rule
   is genuinely the first gate and nothing can slip through on the malformed-entry path.
   `"transport=tcp\r\n"` still parses to `kTcp`; `""` and `"\n\r\n\n"` still parse to the defaults;
   `"transport=tcp\rextra=1"` (mid-entry CR) is refused. Separators intact.
5. **F4 — the assertion is live, and I made it fire.** With an Agent started on `udp4 -p 19999`,
   `XrceConfig.AgentUnreachableIsATransportFailure` fails on exactly the intended line:
   `test_xrce_document.cpp(662) ... something is answering XRCE on 127.0.0.1:19999`. With no Agent
   it takes **1.04 s** (one whole awaited attempt) locally and 1.04 s in the packaged build, versus
   ~0 at the old `=0`. The row now witnesses unreachability as its name claims, and it still
   witnesses M8 (typed `kTransportFailure`) and M14 (registration under `xrce`).
6. **F2 — spec §4.1 matches the code rule-for-rule.** `\n` split ok; single trailing-`\r` pop ok;
   blank entry skipped ok; nothing else trimmed ok; no case folding ok; no comments ok; NUL refused
   up front ok; byte `< 0x21` inside an entry refused, before the `=` split ok; separators
   untouched ok. The framing of that last rule is F5.
7. **Counts — 14 gtest / 15 ctest, measured three ways.** `--gtest_list_tests` = **14**;
   `ctest -N` = **15** (`gtest_discover_tests` mints one entry per case, plus the MSVC nodiscard
   probe); `ctest -C Release` ran **15/15**. The `conan create ... -o run_tests=True` cache build
   that carries the fix (`fletc496600dfbe1af`, sources byte-identical to the repo's) shows
   **15/15** in its `LastTest.log`. No off-by-one. `conformance_xrce`: one ctest entry, 25 gtest
   cases, ran **25/25**.
8. **Mandated full run, at HEAD.** `conformance_xrce` via ctest **1/1 (25 cases), 21.1 s**; the
   binary by hand a further **3 x 25/25**; `fastdds-xrce-interop` **1/1 (3 cases)** after clearing
   `C:\ProgramData\eprosima\fastdds_interprocess`. The conformance and interop binaries link the
   packaged provider `fletc0adc67d4de9bb`, whose exported `src/` I diffed byte-for-byte against the
   repo's — identical, ceiling mapping and `Impl::~Impl` both present. Not an "Already installed!".

## Two things the fixer left — both reasons hold

- **4b nit 3 (`transport=udp\ntransport=serial` reports duplicate-key, not `kNotSupported`).**
  Reason holds, though not for the reason given. Reproduced both orders:
  `transport=udp\ntransport=serial` → `kInvalidArgument` "duplicate document key";
  `transport=serial\ntransport=udp` → `kNotSupported`. It is order-dependent. But the owner's
  2026-09-02 "Accept it, fail distinctly" ruling protects a **valid selection** that this build
  cannot honour; a document with a duplicated key is not a valid selection at all, and design
  rung-2 item 9 independently requires a duplicate key to be `kInvalidArgument`. Reporting the
  structural defect first is the operator-correct answer — they must fix the duplicate whatever the
  value is. *Correction to the stated reason:* it is not "the first refusal in document order";
  both candidate refusals are on the **same** entry, and the reader checks duplicate before value.
  Say "structure before value" and the reason is exact.
- **4b nit 6 (the drift guard reads the source-tree README at run time).** Reason holds, and it is
  the design's explicit mandate, not a preference: the forcing-test mapping names M13 ("bake the
  block in at configure time via `file(READ)`/`configure_file` → re-creates PDA-DEC-6's held-copy
  defect") as **forbidden, not tested**, and `tests/CMakeLists.txt` restates it. Cycle 1
  reproduced the guard reddening in the packaged build with no rebuild. Unreadable → hard failure
  naming the path, never a skip. Conforming; the consequence (the test binary is not runnable out
  of a package whose source tree is gone) is disclosed in the code review and is the correct trade
  for the defect it kills.

## The disclosed `conformance_xrce` anomaly — sound as to this item; the mechanism named is half right, and chasing it turned up a live false-pass hole that PDA-DEC-1 owns

I could not reproduce it: **4 runs** (one by hand immediately after a green ctest run, then three
back-to-back), all **25/25**. Judgement on the explanation:

- The "port overlap" half is **not** what the guard would allow to *fail* — it is what the guard
  fails to *catch*. `WaitUntilReachable`'s `SpawnedAgentAlive()` checks that the process this
  binary spawned is still alive, **not that it owns UDP 2019**. I tested it: with a foreign Agent
  of mine already holding `udp4 2019`, `conformance_xrce.exe` reported **25/25 PASSED** and the
  foreign Agent's own log shows it served the entire run — `client_key: 0x50FFFFFF` (the probe),
  `0x54000000` (`Registry.XrceResolvesAsABuiltIn`), 76 client create/destroy pairs. The guard's own
  comment promises the opposite ("the suite will not run against a foreign one"). So port overlap
  does not produce two spurious failures; it produces a **silent false pass**.
- The "domain overlap" half is the credible mechanism for the two failures actually seen. Both are
  delivery-shaped on DDS domain 153 (`Registry.XrceResolvesAsABuiltIn` publishes and receives a
  row; `...SchemaModeIsUniformNeverMixed` asserts a subscription never sees two shapes). The Agent
  is torn down with `TerminateProcess`/`SIGTERM`, so its Fast DDS participants never shut down
  cleanly — which is exactly this box's known-accepted leaked-`fastdds_interprocess` hazard, and
  stale endpoints on the same domain and topic names are precisely what would make a "never mixed"
  clause see a foreign sample.

**Conclusion for this item:** it does not hide an ordering or state defect introduced by `33d7514`,
which touches nothing in the harness's Agent lifecycle; both failures are cross-run environment
residue and the suite is deterministic across four consecutive runs.
**Out of this item's scope but proved live, for the PM to route:** `SpawnedAgentAlive()`
(`integration-tests/pubsub-conformance/subjects/xrce_main.cpp`, introduced by PDA-DEC-1 `a963211`,
untouched here) does not detect a foreign Agent on the suite's port, so the XRCE conformance suite
can certify against one and report green. Fix is one line — probe the spawned Agent's *ownership*
of the port, or make failure-to-bind fatal.

## Converse check — what survived that should not have

Nothing from the fix pass. `kMsPerAttempt` exists only in the reader (the constructor's copy is
gone, and the constructor now holds no arithmetic, per S3). The old
`EXPECT_EQ(ParseXrceDocument(...).agent_host, " 127.0.0.1")` assertion — which pinned the behaviour
S2 abolished — is gone, as is the README paragraph that explained it ("not trimmed either, and not
second-guessed": no occurrence in the tree). The duplicated readiness-probe document in
`xrce_main.cpp` is gone (nit 5). `~XrceDDSPubSubProvider`'s old teardown body is gone, not
duplicated. Every file the fix touched is inside the design's `Files-to-touch`. The one thing that
survived and should not have is covered by F7 (design §3's "verbatim"), and the one thing that did
*not* survive and arguably should have is F6 (the leak probe).

## RECORD (PM corrects in place; not blocking, no fix cycle)

- The fix pass's reddening evidence says `ConnectTimeoutBudgetBuysWholeAttempts` fails with **11**
  assertion failures under the old mapping; measured **12** (the range-loop `ASSERT_GE` is the
  twelfth, and it aborts the loop).
- `xrcedds-pubsub-provider/tests/test_xrce_document.cpp:6` — "**Six** of these eight cases touch
  nothing but a pure function; the forcing case ... and the **two** constructor-refusal cases" is
  6+1+2 = 9 of 8. The true split is 4 pure (`EveryKeySet...`, `ConnectTimeout...`,
  `DocumentRefusals...`, `Tolerance...`) + 1 pure-plus-a-disk-read (`PublishedDefaultsAreExact`) +
  1 forcing + 2 constructor. The pre-fix "five of these seven" was already wrong; the fix bumped
  both numbers mechanically.
- `integration-tests/pubsub-conformance/CMakeLists.txt:297` now reads "25 clauses (PDA-DEC-7 added
  one)". It is **24 clauses plus one `Registry` case** — PDA-DEC-7 added a registry test, not a
  clause. Cycle 1's RECORD said exactly that; the correction re-introduced the conflation.
- `xrce_document.cpp`'s S2 comment says the rule catches "the space, the tab, and **every other
  control byte**". It catches bytes below `0x21`; `0x7F` (DEL) is a control byte and is not caught
  (measured: `agent=127.0.0.1:2018\x7f` falls through to the port parse — still refused, by a
  different rule). Spec §4.1 and the README say "below `0x21`" and are accurate.
- Measured, recorded here so nobody re-discovers it as a hang: `transport=tcp` towards a routable
  but closed port costs **~2.0 s** per attempt inside `uxr_init_tcp_transport` and is **not**
  bounded by `connect_timeout_ms` (the budget governs the session handshake, which is what the
  header and README say, so nothing is contradicted). Pre-existing client behaviour; a loop of such
  constructions is slow, not leaky (verified: no handle growth).
- `agent=<non-ASCII>:2018` remains representable and is refused a layer down by the resolver
  (measured: `agent=é:2018` parses, host kept verbatim). Consistent with H1 and with the `0x21`
  rule as written; the README's "a host with whitespace in it is not representable" is precise and
  should not be widened.
- The owner's three record corrections are landed and correct: spec §4 clause 4 is re-scoped to the
  gateway with the legitimate direct-construction sites named; the field arithmetic in
  `plans/PDA-decouple-interface.md` now reads five-behind-four-keys / five deleted / two to the
  typed core (= 12); the harness clause count was corrected (see the conflation above).
