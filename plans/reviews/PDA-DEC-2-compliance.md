# PDA-DEC-2 — compliance review (step 4a)

Diff: `1f5d229..666ced8` (`666ced8 feat(PDA-DEC-2): copy-accounting oracle makes
zero-copy falsifiable`), branch `feature/protocol-driver-abi`.
Oracles: `docs/pubsub-interface-spec.md` §3.1/§3.2/§8/§8.1;
`plans/PDA-DEC-2-copy-accounting-oracle.md`; `plans/PDA-DEC-2-brief.md`;
`plans/reviews/PDA-DEC-2-design-review.md` + `plans/reviews/design-debt.md`;
`plans/PDA-decouple-locked-decisions.md` 6, 7, 11, 13, 14; the PDA-DEC rulings
ledger (three 2026-09-01 rulings).

**Verdict: ISSUES (4).** The instrument is real — I re-derived its falsification
by mutating the tree and rebuilding, not by trusting the report, and it went red
in every direction it is supposed to. The four findings are one substantive
(leg 3 does not measure what it is documented to measure, which softens the
owner's "pin at one" tripwire), one brittleness, and two size breaches. Tree left
clean and 7/7 green.

---

## Findings

### 1 — BLOCKING · Leg 3 does not implement the design's shape, and the "pin at one" tripwire is weaker than the ruling requires

The design's leg 3:

> **Receive-side borrowed memory** (own test, not the forcing test): `SeamProbe`
> **delivers** an attachment whose bytes live in its arena — a stand-in for a
> transport-loaned sample. `Blob = shared_ptr<const vector<uint8_t>>` cannot alias
> foreign memory (§3.2), so **the provider must copy**.

What landed (`integration-tests/pubsub-conformance/src/copy_accounting.cpp`,
`RunBorrowedAttachmentRoundTrip`) inverts this. The **harness** stages bytes into
the provider's arena, then itself constructs the `Blob`

```cpp
attachments.emplace("loaned", std::make_shared<const std::vector<uint8_t>>(
                                  loaned_base, loaned_base + loaned.size()));
```

*before* calling `Publish`, and the provider forwards that map verbatim. The
provider copies nothing. The "1" the test measures is a property of the test's own
`make_shared` call.

**Measured, not inferred.** I swapped `DirectRunner runner(provider)` for
`DirectRunner runner(std::make_shared<InProcessPubSubProvider>())` — leaving the
arena staging in place but taking `SeamProbeProvider` off the publish path
entirely — rebuilt, and `BorrowedAttachmentCostsExactlyOneCopy` still passed. The
leg is provider-independent: no provider can make it fail and none can make it
pass differently.

Why this is a conformance defect and not a style choice:

- The owner's 2026-09-01 ruling is *"Pin at one — **Removing the copy turns this
  test red**, forcing the next stage to come back and update the guard. Silence is
  how such a fix gets forgotten or half-landed."* As landed, removing the copy
  does **not** turn this test red. The only thing that can turn it red is
  PDA-DEC-3 changing the `Blob` **type** so that
  `shared_ptr<const vector<uint8_t>>` stops converting — a compile break, not a
  test red. If PDA-DEC-3 delivers aliasing *additively* while keeping the vector
  form constructible (a plausible, backward-compatible shape for locked decision
  6), this test stays green at 1 and the fix lands silently. That is precisely the
  failure mode the ruling exists to prevent.
- It is documented as receive-side in three places the code does not support:
  `README.md` ("the one §3.2 **receive-side** copy, pinned at exactly one"), the
  test's own comment, and — newly normative — `docs/pubsub-interface-spec.md`
  §8.1 ("The one copy §3.2 forces on borrowed **receive** memory is pinned at
  exactly one"). §8 grounds that claim in "**Receive:** *not* there". Nothing
  receive-side is measured.

*Owed:* move the `Blob` construction into `SeamProbeProvider::Publish` so the
provider is the one forced to copy arena-resident bytes at delivery, i.e. the
design's actual shape (~15 lines). Then the leg is provider-dependent, the label
"receive-side" is true, and a provider that gains the ability to hand its arena
bytes over without copying makes the test red by itself.

### 2 — Brittleness · the refill counter's liveness check is pinned to a production provider's allocation strategy

`CopyAccounting.RefillMovementIsCountedNotFailed`
(`integration-tests/pubsub-conformance/src/copy_clauses.cpp`) asserts
`EXPECT_GT(grown_verdict.refill_moves, 0)` against **`InProcessLoopback`**, i.e.
against `VectorWriteBuffer`'s current reallocation points (512 / 1536 / 3584 — I
re-derived them by hand and they produce exactly the 3 moves / 5632 B the design
and the README claim; confirmed at runtime).

The consequence: pre-sizing the loopback's send buffer so it stops relocating —
an *improvement*, and one the brief's Decision 1 discussion explicitly
contemplates as the cost of option (a) — turns this test red, and the failure
message ("either the append granularity stopped forcing it or the refill counter
is inert") sends the reader hunting for a broken instrument. A guard on the
*instrument* should not fail when a *provider* gets better.

*Owed (small):* check the counter's liveness against a harness-owned growable
buffer (or a purpose-built growable probe subject), not against
`InProcessPubSubProvider`. Keep the `SeamProbe`-must-be-zero direction as is.

I do **not** read this test as violating rung-1 item 2 ("a per-subject expected-
copies trait"): the numbers it pins are refill counts, which the ruling makes a
*published number* rather than a copy count, and `CopyVerdict` still carries no
subject-keyed expectation. The 7th entry is warranted — see "Declared deviations".

### 3 — Spec amendment size · §8.1 is 26 added lines against the design's own `≤12`

`plans/PDA-DEC-2-copy-accounting-oracle.md` Files-to-touch:
"`docs/pubsub-interface-spec.md` §8.1 — record the mechanism and its scope
(**≤12 lines**); §3.1 gains **a sentence** for `Data()`." Landed: §8.1 +26/−3,
§3.1 clause 5 +7 (a numbered normative clause, not a sentence). Spec file total
+33/−3.

On substance I clear both amendments — they do **not** bend the oracle to fit the
code:

- §8.1 previously prescribed "an instrumented blob and buffer that **count**
  copies". The design rejected counting with reasons (pooled-buffer blindness; no
  interposition across a Windows provider DLL carrying its own CRT — blind exactly
  where a loaded driver lives) and the architecture review endorsed the rejection
  ("Alternatives rejected for correct reasons"). Recording the approved mechanism
  in the oracle is what §8.1 is for.
- The rewrite carries all three 2026-09-01 rulings into the spec: refill permitted
  and **reported as a number**; a **Scope** paragraph limiting green to the seam;
  the receive copy **pinned at exactly one** (though see finding 1 for what the
  code behind that sentence actually measures).
- §3.1 clause 5 adds no obligation a C view did not already have — §3.1's preamble
  already declares the window to be `{data, capacity, pos}`. Decision 1 makes
  PDA-DEC the only round that may define the seam, so this is in charter.

What is not clear is the size: both are roughly double their declared budget, and
§8.1 now restates the README nearly verbatim.

*Owed:* trim to the declared budget, or have the PM record the amended budget
deliberately.

### 4 — Budget · +1097/−11 against declared +560/−5 (~2×)

The overrun is **not** scope creep in code. Measured across the three new TUs:
493 code lines, 299 comment lines, 118 blank. 493 code lines buying 7 ctest
entries, 3 subjects, an arena provider, a live negative control, a two-runner
abstraction and a pure `Judge()` is proportionate for a guard item, and the third
subject (`InProcessViaPubSub`) was **owed** by DEBT-1, so it cannot be cut.

The overrun is prose. Four arguments — what counts as a copy; why provenance and
not counting; premises P2/P5; the scope limit — are each written out up to four
times: design doc, `copy_accounting.hpp` (149 of its 253 lines are comment),
`README.md` (+78), spec §8.1 (+26).

*My judgment:* **the remedy is prose consolidation, not scope reduction.** Cutting
scope here would cut the guard. Give each argument one home — the README for the
mechanism argument and the premises, the spec for the contract — and have the TU
headers cite rather than restate. That recovers on the order of 200 lines without
touching a single test. The PM still has a ~2× declared-budget breach to rule on.

---

## Verified clean — no finding

### Falsification, re-derived by mutation (not taken on report)

Four mutations, each rebuilt and run; tree restored afterwards, `git status`
clean, 7/7 green.

| Mutation | Result | Reading |
|---|---|---|
| M1 `Judge()` always-zero | **3 red**: `StagingIsCaught`, `BorrowedAttachmentCostsExactlyOneCopy`, `JudgeArithmeticIsSound` | matches the report, plus a third the report omitted |
| M2 inert instrument (`ledger.encode_base = nullptr`) | **5 red / 2 green** — 3 forcing subjects + Borrowed + Refill red; Staging + JudgeArithmetic green | exactly as reported |
| M3 containment instead of strict equality (DEBT-2) | `JudgeArithmeticIsSound` red | DEBT-2 is genuinely guarded, not merely written down |
| M4 a `std::vector` materialised above the seam, in the `Publisher`/`Subscriber` path | **only** `…/InProcessViaPubSub` red | DEBT-1's third subject earns its keep; nothing else covers that ground |

**Is there a third break that leaves everything green?** I could not construct
one. `Judge()` is pure and directly unit-tested; there is exactly one
delivery-capture path and one encode-sampling path, so there is no per-subject
branch for a missed copy to hide in; and a false green requires a copy landing
*at* the live encode window address, which P5's liveness precondition forbids
without a use-after-free. The one residual is a subject silently dropped from
`CopyAccountingSubjects()` — nothing fails, only `ctest -N` shrinks. That is the
machine check the design nominated, and it works (below).

### Rulings

- **Refill permitted, published as a number.** `PublishRefillCost` writes both
  numbers to stdout and to the JUnit XML via `RecordProperty`, on every leg
  (observed: `SeamProbe 0/0`, `InProcessLoopback 3/5632`, `InProcessViaPubSub
  3/5632`). Guarded in **both** directions by the 7th entry, so neither an
  always-zero nor an always-nonzero counter survives. *Residual, named not
  charged:* the publication itself is unguarded — delete `PublishRefillCost` and
  nothing fails. Guarding a print is not worth a test; the measurement is
  guarded, which is what matters.
- **Scoped to the interface, limit in the README.** `README.md` §"What green does
  NOT prove" states it, and names the DDS/XRCE **publish-side** loan path as
  unmeasured (DEBT-1's second half) so the existing `Loaned*`/`DataSharing*` tests
  cannot be misread as covering it. No DDS subject registered — P4 honoured.
- **Receive-side copy pinned at exactly one.** Asserted at 1. See finding 1 for
  what the assertion is actually a property of.

### Debt (all 7 landed)

DEBT-1 both halves (third subject **and** narrowed copy definition **and** the DDS
publish-side line in the README) · DEBT-2 strict equality, guarded, proved by M3 ·
DEBT-3 P5 in the design premises, the header, the README and spec §8.1 · DEBT-4
P3's consequence restated · DEBT-5 `kAppendChunk = 64`, and the predicted 3 moves
/ 5632 B is exactly what `VectorWriteBuffer` produces · DEBT-6 `Finish()` and the
`[Data(), Data()+Position())` bound in both the header and §3.1 clause 5 · DEBT-7
brief citation swapped to §3.1 clause 1.

### Scope, deletions, and the converse

- **`Files-to-delete: none`** — and the converse holds. Deletions in the whole
  diff are 3 spec lines and 8 plan lines; **no code is deleted anywhere**. Nothing
  the design ordered retired survives, because nothing was ordered retired.
- `integration-tests/gateway-fastdds-ts` intact and untouched (2026-09-01
  blind-spot ruling: "do not weaken or delete it").
- The design's non-supersession claim still holds: the Fast DDS `Loaned*` /
  `DataSharing*` tests assert delivery, never absence of copies — and the README
  now says so explicitly.
- No `retired:` / `re-anchored:` citations to check; none were claimed.

### Locked decisions

- **14 (nothing ABI):** grepped the diff for `extern "C"`, `dlopen`,
  `LoadLibrary`, `GetProcAddress`, vtable, version negotiation, host-callback
  struct, `.so`/`.dll` — zero hits.
- **3 (built-in vs loaded invisible):** the oracle sees only `PubSubProvider`;
  nothing branches on the provenance of a provider.
- **13 (wire format):** rows are an opaque deterministic byte pattern; no layout
  assertion.
- **11 (guard-first):** this is the second guard, landing before the vocabulary
  work, as ordered.

### Corner-case ladder

Rung-1 items 1–7 all hold: no peer verb and an unsynchronised ledger (1); no
subject-keyed expectation in `CopyVerdict` (2); no `GTEST_SKIP` anywhere in the
suite, and the subject list is fixed at build time (3); content `memcmp` asserted
before the address verdict (4); addresses compared, never allocation counts (5);
no ABI surface (6); no wire-format assertion (7). Rung-2 8–10 are refused at the
door by `COPY_MUST_DELIVER_CLEANLY`, which aborts on a throw, on
`deliveries != 1`, on a garbled row and on a garbled attachment **before** any
verdict is computed — no fallback, no partial mode, no recovery path.

### Public surface

**1**, as declared: `WriteBuffer::Data()`. The harness is a standalone,
non-installed, non-packaged CMake project
(`project(pubsub-conformance-integration)`, "Not published as a Conan package"),
with no `install()` rules for its headers, so `CopyRunner` / `CopySubject` /
`CopyLedger` / `Judge` are not public surface. Only two `WriteBuffer` subclasses
exist; `Data()` collides with nothing.

### Gates

clang-format 18.1.3 (the CI version) clean on all four changed C++ files. SPDX +
copyright headers present on the three new files. `core/**` and
`integration-tests/pubsub-conformance/**` are both in the
`integration-pubsub-conformance` path filter (`.github/workflows/ci.pr.yml`
:250-255), and the lane's `conan build .` calls `cmake.test()` with no `-R`, so
the 7 new entries run on both platforms.

### Declared deviations — both assessed benign

**(a) ctest names.** Verified against the generated ctest file and `ctest -N`:
`gtest_discover_tests` parses gtest's `# GetParam() = SeamProbe` comment, so the
registered names really are
`CopySubjects/CopyAccounting.PublishAndReceivePerformNoPayloadCopies/SeamProbe`
(and `/InProcessLoopback`, `/InProcessViaPubSub`). The design's nominated machine
check — "`ctest -N` (a subject that stopped being registered)" — therefore works.
Only the instantiation prefix differs from the design's guess. Cosmetic.

**(b) the 7th entry, `RefillMovementIsCountedNotFailed`.** Warranted and, on the
evidence of M2, load-bearing: without it an inert `Data()` would leave the refill
number silently zero with nothing failing on that account. It is what stops the
owner's "published as a number" becoming "published as a constant zero". Its one
weakness is finding 2.

---

## RECORD

- `plans/PDA-decouple-interface.md:136-144` still describes PDA-DEC-2 as "an
  instrumented blob and write buffer that **count copies**" / "**Counts** the
  encode path" — contradicted by the landed mechanism and by spec §8.1 as amended
  in this same diff, where counting is rejected outright.
- `plans/PDA-decouple-interface.md:91` — tracker row for PDA-DEC-2 still ⚪
  not-started.
- `plans/PDA-DEC-2-copy-accounting-oracle.md` §Subjects still declares
  `CopySubject { std::string label; std::function<std::shared_ptr<PubSubProvider>()> make; }`;
  the landed shape is `std::function<std::unique_ptr<CopyRunner>()>` over a
  `CopyRunner` abstraction the design never names. The design was amended in this
  commit for DEBT-1..6 but this line was left stale.
- The design doc is now **297** lines (267 at review; the tracker/review noted a
  stale 245). Still ≤300, no breach.
- The design's `Numbers` section still reads `+560 / −5`.
- `plans/PDA-DEC-2-brief.md` was edited (DEBT-7) though the brief is not in the
  design's `Files-to-touch`.
