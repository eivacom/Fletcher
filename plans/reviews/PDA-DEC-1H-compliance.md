# PDA-DEC-1H — compliance review (step 4a)

Independent, adversarial. Scope `git diff 39512da..ecc7b2c` (plus the docs commit `de5d2cb`),
tree clean at `de5d2cb`. Judged against `plans/PDA-DEC-1H-harness-ownership.md`,
`plans/PDA-DEC-1H-brief.md`, `plans/PDA-DEC-rulings.md` and `plans/reviews/design-debt.md`
§ROUND-1 — not against the code as found.

**Verdict: PASS-WITH-FINDINGS(4). No blocking items.** The guard is the right guard, the deleted
state is genuinely deleted rather than hidden, the scope held, and both harnesses refuse a
foreign Agent when measured live. Four non-blocking findings, two of which are wrong statements
now shipped in the tree.

Everything below was reproduced, not read. Evidence is inline.

---

## Findings

### F1 — should-fix. The two "byte-identical" copies answer two different questions, and the code claims parity

`integration-tests/pubsub-conformance/subjects/xrce_main.cpp:164-165` (Windows; interop copy `test_interop.cpp:217-218`) says, correctly:

> *IPv4 only: the Agent is started as `udp4`, so an IPv6 row on this port could not be the
> endpoint the suite certifies against.*

The Linux branch does not honour that. `UdpPortInodes` reads **both** `/proc/net/udp` **and**
`/proc/net/udp6` (`:229`, interop `:282`), and `UdpPortOwnership`'s comment (`:318-319`, interop `:371-372`) claims it applies *"the
same 'foreign beats ours' rule the Windows branch applies, for the same reason"*. It applies it
over a strictly wider row set.

Measured. I extracted lines 156–335 verbatim into a standalone translation unit and ran it under
WSL Ubuntu (g++ 13.3, `-Wall -Wextra`, clean):

```
child holds 127.0.0.1:24031 (v4); an unrelated pid holds [::1]:24031 (v6-only)
Linux verdict asked about the child: kSomeoneElses   (Windows AF_INET-only query says kOurs)
```

Same machine state, opposite verdict. In that state the Windows answer is the *correct* one — our
child does hold the IPv4 port, and the probe goes to `127.0.0.1` — so Linux produces a **false
refusal**, Linux-only, inside a block the design and both file comments present as one rule
duplicated. It can never be a false pass (extra rows can only push toward `kSomeoneElses`), so it
is loud rather than silent; hence should-fix, not blocking.

**Acceptable fix:** drop `/proc/net/udp6` from `UdpPortInodes` in both copies (matching the
`udp4` spawn and the Windows rule), or state in the comment that Linux is deliberately stricter
and Windows deliberately is not.

### F2 — should-fix. The design's headline measurement is a wrapper artifact, and it has already been copied into two shipped READMEs

The design (§*The mechanism*) and both READMEs state the doomed Agent *"logs `bind error` and
**exits in ~876 ms**"* / *"takes ~0.9 s to do it"*. The **kind** of claim is right; the **degree**
is off by roughly 25×.

Measured on this box, incumbent Agent holding the port:

| measurement | result |
|---|---|
| raw OS lifetime (`Process.ExitTime − StartTime`) | **28 ms** |
| four polled trials, spawn to exit | **33.6, 36.7, 40.4, 44.8 ms** |
| child still alive at ~16 ms after spawn | **true in 2 of 4** |
| `Measure-Command { & agent … }` (direct invoke) | 35 ms |
| `Measure-Command { Start-Process … -Wait }` | **1,027 ms** |

The last row is almost certainly where 876 ms came from: PowerShell `Start-Process -Wait`
overhead, not the Agent. The Agent's own log confirms the real sequence — `bind error` then
`server stopped`, 2.7 ms apart.

What survives of the account, all confirmed: the PM hypothesis **is** refuted (the child exits, it
does not linger); the leftover **does** hold the port and the OS records it (`0.0.0.0:2019`, which
is why the design's deliberate address-agnostic match is right); liveness **is** true-but-stale at
16 ms; and the interop fixture **had** no liveness check at all
(`git show 39512da:…/test_interop.cpp` — `SetUp` is `SpawnAgent(); WaitUntilReachable();`, no
check, so the design's "worse" is exact).

What does not survive is the *margin*. The race the old guard lost is ~10–30 ms wide, not ~900 ms.
That is a materially different risk statement — the pre-existing greens were a coin-flip, not a
near-certainty — and it is the number a reader will quote.

**Acceptable fix:** correct the figure in the design doc and in both READMEs (tens of
milliseconds), and say how it was measured.

### F3 — should-fix. The item's central deletion is guarded by nothing that runs

The design's *Forcing tests* table lists a third row:

> `the refusal-on-query-failure path | both | stubbing a runtime query failure — proved: ctest
> entry Failed where pre-fix it passed`

That is a one-off hand mutation, not a test. Grepped: no test in either harness reaches
`kQueryFailed` or `kNobody`. The forcing tests cover `kSomeoneElses` only. Re-introducing a
fall-back-and-pass on the `kQueryFailed` arm — the exact regression this item exists to make
impossible — **reddens nothing**. The `#error` arm is structurally safe because a compiler
enforces it; the runtime arm is safe only by inspection.

**Acceptable fix:** either give the query one indirection a test can swap (a file-scope function
pointer defaulting to `UdpPortOwnership`), or stop listing it as a forcing test and record it as
accepted debt.

### F4 — record-plus. The Linux half shipped with no build evidence anywhere, and the drift guard's authority is prospective

`gh run list --branch feature/protocol-driver-abi` returns **nothing** — the whole PDA-DEC round
has no CI run; both integration lanes are `workflow_call` from PR-triggered `ci.pr.yml`. So at the
moment of this review:

- the ~110 new Linux-only `/proc` lines, duplicated twice, had never been compiled or run;
- the design's drift guard — *"each harness carries its own equally-named forcing test **in its
  own CI lane**, so breaking either copy reddens that copy's test"* — has never fired;
- the code review's *"both lanes do run Linux, so the /proc path is genuinely exercised in CI"* is
  true of the workflow files and false of the world.

I supplied the missing evidence: the Linux branch compiles clean under g++ 13.3 `-Wall -Wextra`
and returns the correct verdict on six states, including the dead-pid one —

```
port held by our child, asked about the child   kOurs          OK
port held by our child, asked about ourselves   kSomeoneElses  OK
port we hold ourselves, asked about ourselves   kOurs          OK
port we hold ourselves, asked about the child   kSomeoneElses  OK
nobody on the port                              kNobody        OK
dead/absent pid on a held port                  kSomeoneElses  OK
```

so residual risk is low. The finding is that two documents state as *checked* what was only
*arranged*.

**Acceptable fix:** run both integration lanes on this branch before the close gate signs, or
reword both claims to future tense.

### Nits (one line each)

- `xrce_main.cpp:143-148` — the `kNobody`/`kSomeoneElses` paragraph is duplicated verbatim;
  copy-paste artifact of `ecc7b2c` (`git log -S` confirms).
- Windows `DeadChildRefusal` on a *dead child plus failed query* still ends with "configure with
  `-DFLETCHER_CONFORMANCE_XRCE=OFF`" — the run fails, so it is loud, but the printed remedy is
  "switch the subject off". Pre-existing wording, now reachable from one more path.
- Pre-existing, named only because the brief forbids the shape: `FLETCHER_CONFORMANCE_XRCE=OFF`,
  or a build dir cached at OFF, removes this guard **and** the whole XRCE subject with nothing in
  the tree reddening. Outside the item's stated scope; it is nevertheless "a guard you can forget
  to arm", one level up from the one this item fixed.

---

## Conformance confirmed (short, because there is nothing to report)

**The dangerous state is deleted, not hidden.** No `kUnprovable`, no `ownership_unprovable_`, no
`[   INFO   ]` branch, no `GTEST_SKIP` in either harness — the only surviving mention of
`kUnprovable` is the `#else` comment recording its removal. `#error` present in both copies. Every
`switch` arm returns a refusal string and the post-`switch` line refuses too, so a new enumerator
cannot pass. Live: with a foreign Agent on 2019, `conformance_xrce` exits **1** carrying the
operator-actionable sentence; with one on 2018, `integration_tests` exits **1** likewise.

**Ownership, not liveness.** `ProveOwnership()` runs *after* `WaitUntilReachable()` in both
copies; `Alive()` is demoted to story-picking. `foreign` and `ours` are accumulated over the whole
table before the decision on both platforms, so iteration order cannot invert it. The live Windows
refusal reads *"the OS records the port as held by another process, not by our child (pid …)"* —
i.e. it came from `ProveOwnership`, not from a liveness check.

**The block really is byte-identical.** `diff` of `xrce_main.cpp:156-335` against
`test_interop.cpp:209-388`: clean. The surrounding narrative comments differ by design — the
interop copy holds the long note, the conformance copy points at it.

**Bring-up-only scope is stated where the design says.** Both READMEs, both file headers, both
`proven()` doc comments — 6 of 6 — and the wording matches what the code guarantees: a one-shot
snapshot taken in the constructor and never re-taken.

**The corrected duplication reason is true; the false one survives only where labelled false.**
Both harnesses `self.requires("fletcher-xrcedds-pubsub-provider/…")`, and grepping for `../..` in
either `CMakeLists.txt` or `conanfile.py` returns nothing, so "consume it as a package and reach
outside their own directory for nothing" is exact. Both conformance-lane jobs already
sparse-checkout `xrcedds-pubsub-provider`, so the retracted CI reason is indeed false — and the
retraction is correctly *scoped* to that location (a header under `integration-tests/` would
genuinely need the four blocks; no shared-support precedent exists there). The false reason
appears only in the design doc and the code review, both marking it false, and in `5af2bfb`'s
message, which is history and deliberately left.

**Counts — entries and cases kept apart.**

| claim | measured |
|---|---|
| `pubsub-conformance` 82 ctest entries | `ctest -N -C Release` → **Total Tests: 82**, `conformance_xrce` is #82 |
| `conformance_xrce` 26 gtest cases | `--gtest_list_tests` → **26**; run → 26 passed, **0 skipped** |
| `fastdds-xrce-interop` 1 entry / 4 cases | `ctest -N` → **1**; `--gtest_list_tests` → **4**; run → 4 passed, **0 skipped** |

**Session keys, verified independently.** Conformance: five bases `0x50`–`0x54`000000 over one
shared counter masked `0x00FFFFFF` — five disjoint 16 M ranges, collision-free by construction,
and the old fixed `kProbeSessionKey = 0x50FFFFFF` is gone with its replacement staying inside the
probe's own range. Interop: `0xF0FF0000 + (counter & 0xFFFF)` spans `0xF0FF0000..0xF0FFFFFF`,
disjoint from the three fixed keys `0xF0F00001/2/3`. Both correct; the code review's should-fix 2
is properly closed.

**Scope held; nothing survived that should not.** Six files, all inside the two harness
directories. No provider, no `docs/pubsub-interface-spec.md`, no `src/clauses*.cpp`, no
`ProviderConformance` clause, nothing under `integration-tests/gateway-fastdds-ts` — the
2026-09-01 ruling honoured. No ABI, no config parser, no dependency added to Fletcher, no
link-size check. `MICRO_XRCE_AGENT_PATH` exists in exactly these two harnesses, so no third
Agent-spawning harness is left unguarded. `iphlpapi` sits inside `if(WIN32)` in both, and
additionally inside `if(FLETCHER_CONFORMANCE_XRCE)` in the conformance one; Linux links nothing
new; there is no macOS lane, so the `#error` cannot break one. No environment variable gates the
guard — the only `getenv` calls are the child's loader path, as before. Nothing the design ordered
deleted survives: the pre-fix `SpawnedAgentAlive()` guard, its two divergent platform messages and
the "build it" advice on a lost bind are all gone, and no existing clause, subject or wait budget
was weakened (conformance probe still 20 s / 2000 ms; interop still 15 s / 2000 ms).

**No Agent leaked.** 0 `MicroXRCEAgent` processes before and after each of seven runs — two clean
suite runs, two deliberate foreign-Agent refusals (which exercise the
`ASSERT_TRUE(agent_->proven())` failure path in `Environment::SetUp`, where `TearDown` still runs
and the destructor kills both the fixture's Agent and the forcing test's two), and three mechanism
experiments.

**Format.** `clang-format --dry-run -Werror`, clang-format 18.1.3, both files: clean.

---

## Was the compression defensible?

**Yes, narrowly — and the single review it left standing was enough to catch the substantive
drift, which is the honest test of the call.** A ~20-line guard with a crisp forcing test did not
need a 300-line design doc.

But it cost two things, and both are visible in the tree rather than hypothetical:

1. **An unverified number became shipped documentation.** Nobody asked "how was 876 ms measured?"
   It is a PowerShell wrapper artifact (F2), and by the time the design doc was written
   retrospectively the figure had already been copied into two READMEs. A cycle whose only job is
   to attack the design's premises is exactly what catches a load-bearing measurement taken with
   the wrong instrument.
2. **A false premise survived from dispatch into the PM's own prose.** The CI sparse-checkout
   justification for duplicating 165 lines was wrong, was repeated to the owner, and was caught
   only because the code reviewer went and read the workflow files instead of taking it on trust.
   Checking a design's premises against the tree is an architecture review's core job; here it
   happened by luck of instruction.

Neither changed the fix, and the fix is right and strictly stronger than what it replaced. What
the gap actually left behind is thinner than a missing design step: an untested refusal arm (F3),
a platform asymmetry nobody compared side by side (F1), and a Linux half that shipped with no
build evidence (F4). All three are the kind of thing a second pair of eyes on the *design* asks
about — "what proves the arm you just created?", "are the two mechanisms really the same rule?",
"where has the other platform been built?" — and none is the kind of thing more prose would have
fixed.

**So: the call was defensible. What the item needed was not a design doc but one reviewer told to
re-derive the premises — which is what the code review turned out to be, and it found two of the
three.** I would not repeat the compression on an item that **forks by platform**, because a
platform fork silently doubles the surface nobody is looking at.

---

## RECORD (paperwork — never blocking)

- `plans/reviews/design-debt.md` §ROUND-1 still sits under *"owned by nobody yet"* and still says
  *"the **mechanism is unconfirmed**"*, describing the now-refuted hypothesis ("does not exit") as
  the gap. The entry needs a close annotation pointing at PDA-DEC-1H.
- Design doc *"Actual +1263 / −246"* is the **sum of per-commit churn** (449+474+340 /
  57+24+165 — each verified exactly). The **net** `39512da..ecc7b2c` diff over
  `integration-tests/` is **+1111 / −94**. Both are defensible; only one is labelled.
- Brief says *"no stray Agents across six checks"*; add seven more from this review, all 0.
- Design doc and both READMEs carry the ~876 ms / ~0.9 s figure (F2) — a document contradicted by
  measurement, noted here as well so it is not lost if F2 is deferred.
