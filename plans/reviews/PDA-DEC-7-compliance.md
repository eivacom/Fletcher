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
