# PDA-DEC-1 — architecture-conformance review (step 4a)

Reviewed cold at `adc4f1f` (diff `2de2469..adc4f1f`, 35 files, +2927 / −183). Working
tree clean at that commit. Oracles in precedence order: `docs/pubsub-interface-spec.md`
§7/§7.1/§7.2 → the PDA-DEC rulings ledger → `plans/PDA-DEC-1-conformance-suite.md`
(APPROVE-WITH-DEBT(6)) → `plans/reviews/PDA-DEC-1-design-review.md` +
`plans/reviews/design-debt.md` → `plans/PDA-decouple-locked-decisions.md` (11/12/13) →
`plans/PDA-DEC-1-brief.md`.

**Verdict: ISSUES — 5 blocking conformance findings, 3 low, 6 RECORD.**

The shape shipped is the shape approved: one clause TU, subject registration, publisher in
a child process, all-or-nothing backlog, traits keyed by provider, no `GTEST_SKIP`, no
provider reachable from a clause body. Every ordered deletion executed, every DEBT-C2 item
discharged, no scope breach found, no wire bytes moved, no ABI surface anywhere. The
findings are concentrated in one place: **clause 6 as landed cannot do the job locked
decision 12 exists for, and the reason is a deviation from the approved clause body — not
an inherent limit of cross-process subjects.** That matters more than everything else here,
because two later rounds inherit the answer.

---

## The central question, ruled

### Is locked decision 12 satisfied?

**In mechanism, yes; in purpose, not yet — and the shortfall is inside this item's reach.**

The cross-process subject is genuine, not cross-process in name only. Verified against the
tree, not the report:

- `src/child_process.cpp` really spawns a separate OS process (`CreateProcessA` / `fork` +
  `execv`) with real pipes; `subjects/fastdds_peer_main.cpp` is a distinct executable
  wired in by `$<TARGET_FILE:conformance_fastdds_peer>`.
- The peer publishes through a **separate `FastDDSPubSubProvider` on the shipped defaults**
  (`subjects/fastdds_main.cpp:29-33` sets only `domain_id`), on its own domain 152, so
  intra-process delivery cannot serve it and data-sharing is eligible on both ends. The
  falsified reader QoS would have engaged.
- No local-publish path exists to fall back to: `ProviderSubject` exposes no
  `PubSubProvider&`, no `Publish`, no `CreateTopic`, and `conformance_support`'s link line
  is `fletcher-pubsub` and nothing else — a provider header cannot resolve from the clause
  library. Rung-1 item 1 holds as a machine check.
- Retention comes from `RetentionForProvider` (`src/fixtures.cpp:25-45`), one row per
  provider, thrown-on-unknown. A subject cannot declare its way to green. Rung-1 item 3
  holds.

So §7.2's requirement is met. What is **not** met is the acceptance condition the design
attached to it, in its own words:

> confirm `FastDdsCrossProcess/ProviderConformance.LateJoinerBacklogIsAllOrNothing` goes
> red while `FastDdsLocal/...` stays green, restore. **If it does not go red, the
> cross-process subject is wrong and the item is not done.**

It did not go red (12/12 green against the falsified provider, per the harness README),
while `gateway-fastdds-ts` failed with the documented signature against that same
provider. By the design's own gate the item is not done. The implementer recorded the gap
honestly instead of stopping — that is the right disclosure but not a discharge of the
gate, and locked decision 12's justification is currently unsupported by anything this
item ships.

### Diagnosis: it is the sentinel, not the shape

`src/clauses.cpp:154-176` — clause 6 as landed does something the design's clause 6 does
not:

```cpp
    // One live row after subscribing. Per-writer order (clause 4) puts the whole
    // retained backlog ahead of it, so its arrival is a bounded, deterministic
    // end to the wait — for the retaining and the dropping case alike.
    CONF_MUST_PUBLISH(topic, kSentinel);
    ASSERT_TRUE(collector.WaitForSeq(kSentinel, Deadline()))
```

The approved design specifies clause 6 as "publish N rows, subscribe, count arrivals from
before the subscribe". There is no live sample after `Subscribe` in the design, and the
design review verified the claim against *that* sequence. The sentinel is an
implementer-added convenience — it gives the `kDrops` case a deterministic end instead of
paying a settle budget — and it is, I believe, precisely what masks the defect.

Mechanism: the writer is **RELIABLE + KEEP_ALL + TRANSIENT_LOCAL** (`qos_defaults.cpp:24-33`)
and the sentinel is published by the **same writer on the same topic**. A Fast DDS
reliable reader does not release a non-contiguous change to the application: seq 6 is held
in the writer proxy until 1..5 are either received or GAP'd. So publishing a live sample
after `Subscribe` and then waiting for *it* converts the question

- from "did the match-time TRANSIENT_LOCAL replay deliver everything?" — which is the
  defect —
- into "was the gap resolved before the next sample could be released?" — which reliable
  NACK/repair resolves, with all five payloads still in the 100-slot KEEP_ALL history.

The clause then reads `Seqs()` and finds 1..5 present, because the transport was *forced*
to repair the very gap the clause exists to observe. `gateway-fastdds-ts` publishes three
rows and then **nothing** (`integration-tests/gateway-fastdds-ts/src/fastdds_peer.cpp:72-80`
— the loop, then `READY`, then idle), so there is no in-order barrier and no later sample
to trigger the repair; the loss stays visible. The 2 s late-joiner gap experiment does not
discriminate, because the sentinel was present in that run too.

**The README's stated cause is therefore contradicted by the README's own evidence.** It
says "this harness's single-writer / single-reader cross-process shape does **not**
reproduce it" — but `gateway-fastdds-ts` is *also* single-writer, single-reader,
cross-process, and it does reproduce. The shape is not the differentiator.

**The shape that would reproduce it** (naming, not designing): publish N before any
reader exists; subscribe; publish **nothing further**; bound the wait by the clause
deadline — a positive `WaitForCount(N)` when `kRetains`, a negative `kSettleBudget` wait
when `kDrops`. That is the pattern clauses 9 and 11 already use, so it needs no new
machinery and costs one settle budget on the loopback subject. Two weaker remaining
candidates, both worth naming to the owner: the gateway-side reader sits under a
`Subscriber` fan-out layer rather than directly on the provider, and
`gateway-fastdds-ts`'s peer participant also holds a *reader* (TsToCpp) beside its writer.
I could not re-run the falsification here (Docker unavailable, and I will not mutate
`qos_defaults.cpp` in a review), so the sentinel diagnosis is analytic — but it is
testable in one edit and one re-run of the procedure the design already mandates.

### Is the README disclosure honest and sufficient?

**Honest about the fact, wrong about the cause, and therefore not sufficient.** The
"What these clauses do NOT prove" section is exactly the right construct and states the
measured outcome without hedging (12/12, 8/8, and the contrasting
`gateway-fastdds-ts` result). It does not imply coverage it lacks — "do not read it as
covering that defect class" is unambiguous. But its one causal sentence points a future
round at the wrong conclusion ("cross-process subjects cannot see this class") when the
supported conclusion is "clause 6's wait termination masks it". PDA-ABI-7 (zero-copy
receive) and any future data-sharing round will read that sentence as evidence that a
cross-process conformance subject is not worth building. That is the expensive kind of
wrong.

### Does the brief still claim more than shipped?

Yes. `plans/PDA-DEC-1-brief.md` opens with "two of them **across a process boundary**,
where transport behaviour is visible at last." Transport behaviour *is* now observable —
that half is true. But the specific transport defect the round cited as the whole reason
for §7.2 and decision 12 is still not observable, and the brief's "Decisions — all three
answered" §2 ("Partial late-joiner delivery is never acceptable") reads as delivered
coverage. The owner must not close this stage believing the shipped defect class is now
pinned. PM-owned correction at close, but it is a substantive claim, not a wording nit.

---

## Blocking findings

### C1 — Clause 6 publishes a live sentinel after `Subscribe`, which is not in the approved design and defeats the falsification gate

`integration-tests/pubsub-conformance/src/clauses.cpp:165-171`. Design clause 6 is
"publish N rows, subscribe, count arrivals from before the subscribe"; the design review
verified decision 12's justification against that sequence. The added post-subscribe
publish turns a match-time-replay assertion into an eventual-delivery assertion, which
reliable in-order delivery + NACK repair satisfies. Consequence: the design's binding
verification ("if it does not go red, the cross-process subject is wrong and the item is
not done") fails, and locked decision 12 ships unjustified.

*Acceptable fix:* drop the sentinel and bound clause 6 by the clause deadline —
`WaitForCount(kBacklog, Deadline())` under `kRetains`, a negative `kSettleBudget` wait
under `kDrops` (the clause 9/11 pattern) — then re-run the design's falsification
procedure and record the result.

### C2 — The README attributes the gap to a cause its own evidence rules out

`integration-tests/pubsub-conformance/README.md`, "Clause 6 is not proven able to catch the
shipped data-sharing defect": *"this harness's single-writer / single-reader cross-process
shape does not reproduce it"*. `gateway-fastdds-ts` has that same shape and does reproduce
(`src/fastdds_peer.cpp` — one writer, one reader, two processes). Two later rounds inherit
this sentence as the reason not to trust cross-process conformance subjects.

*Acceptable fix:* replace the causal sentence with the observed difference (clause 6
publishes a live row after `Subscribe`; `gateway-fastdds-ts` publishes none) and label it
the open hypothesis, so the next round tests it instead of inheriting it.

### C3 — The design's pipe-helper stop-and-ask threshold was crossed without the stop-and-ask

Design premise 4: *"**STOP-AND-ASK** … if the pipe helper exceeds ~250 lines or is
unstable on MSVC — then propose splitting the cross-process subject into its own item
rather than shipping it degraded."* Landed: `src/child_process.cpp` 344 lines (301
non-comment) + `src/child_process.hpp` 71 = **415**, 66% over the threshold. No
stop-and-ask was raised; the item shipped through the gate.

The substance is not degraded — the helper is bounded-wait, no-retry, no-reconnect, and
the two platform branches are clean. So the remedy the design names (split the subject out)
is almost certainly the wrong call now. But the gate exists so the owner sizes this, not
the implementer.

*Acceptable fix:* PM records the crossed premise explicitly and either accepts the
overrun in the round record or takes the design's named remedy; no code change if
accepted.

### C4 — On Linux a dead peer child kills the parent by `SIGPIPE`, so rung-2 item 9's typed refusal becomes a process death

`src/child_process.cpp` (POSIX branch, `WriteLine`) writes to the child's stdin and handles
`n <= 0`, but nothing ignores `SIGPIPE` — `grep -rn SIGPIPE` finds no handler anywhere in
the repo. If the peer has exited (crash, `execv` failure after READY, `_exit(127)`), the
next `write()` raises `SIGPIPE` and the default action **terminates the gtest process**.

The design's rung 2 item 9 is explicit: *"peer child crash, hang or EOF ⇒ the request's
deadline expires and returns a typed failure that fails the clause; no retry, no reconnect,
no partial mode."* A signal-killed test binary is neither a typed failure nor a clause
failure — it is the refusal ladder collapsing into a crash. This sits in the branch that
has **never been compiled or run** (Windows box, no Docker), on the lane whose first
execution is the new Linux CI job, and it presents as a hard-to-diagnose lane failure
rather than a named clause failure.

*Acceptable fix:* ignore `SIGPIPE` in the parent (`signal(SIGPIPE, SIG_IGN)` at
`ChildProcess` construction, or `send`-with-`MSG_NOSIGNAL` equivalent), so a dead peer
yields the deadline `nullopt` the ladder requires.

### C5 — `provider.hpp` still promises a non-null schema future, while clause 10 now asserts it resolves null on the schema-less subject

`pubsub/include/fletcher/pubsub/provider.hpp:20-28`: *"the future resolves with a
**non-null** SharedSchema once the schema is known"*. `src/clauses.cpp:262-269` (clause 10,
`kAbsent` branch) asserts `sub.schema.wait_until(...) == ready` **and**
`sub.schema.get() == nullptr`, and `pubsub/include/fletcher/pubsub/in_process_provider.hpp`
documents the loopback as null throughout. The contradiction was latent before (the gateway
loopback already did `MakeReadySchemaFuture(null)`); this PR makes it load-bearing by
pinning the null resolution in a shipped conformance suite and by promoting the class to a
documented `pubsub/` built-in.

This is the identical failure mode DEBT-C2-1 existed to prevent — an unamended seam header
contradicting the executable contract landing in the same commit — and locked decision 5
puts the normative rule in the header. §7 clause 1 wins, so the header is the defect.

*Acceptable fix:* one sentence at `provider.hpp:20-28`, saying the future resolves with a
non-null schema on a schema-carrying transport and with null throughout on a schema-less
one (§7 clause 1), never mixing the two.

---

## Low findings

### L1 — The harness conanfile requires the XRCE provider unconditionally, so `FLETCHER_CONFORMANCE_XRCE=OFF` does not give the inner loop the graph the design promised

`integration-tests/pubsub-conformance/conanfile.py:44-46` requires
`fletcher-xrcedds-pubsub-provider` regardless of the CMake option, while the runbook's
`inner_loop_cmd` builds only `core pubsub fastdds-pubsub-provider` ("Add
xrcedds-pubsub-provider when FLETCHER_CONFORMANCE_XRCE is ON"). On a cold cache the inner
loop's `conan install` fails before CMake is ever reached. The Agent superbuild *is*
correctly gated — only the package requirement is not.

*Fix:* add `xrcedds-pubsub-provider` to the inner-loop component list unconditionally, or
make the Conan requirement conditional on the option.

### L2 — On POSIX, a missing Agent binary fails after a 20 s timeout without naming the path

`subjects/xrce_main.cpp`: the Windows branch `FAIL()`s naming
`MICRO_XRCE_AGENT_PATH` when `CreateProcessA` fails, but the POSIX branch always forks
successfully and `execv` failure is only visible as the child's `_Exit(127)`.
`WaitUntilReachable()` then fails naming `127.0.0.1:2019` and the last probe error — never
the path. Rung 2 item 8 says *"the subject **fails** naming the path"*. Loud, but not loud
about the right thing, and Linux is the platform where this branch is untested.

*Fix:* `waitpid(WNOHANG)` the agent child before probing, and name
`MICRO_XRCE_AGENT_PATH` in the failure message on both platforms.

### L3 — `DeclaredSchema::Encode` widens `Publisher`'s pre-existing semantics slightly

`pubsub/include/fletcher/pubsub/internal/schema_conflict.hpp:44-46` returns empty/encodable
for `schema->release == nullptr`; the code it replaced
(`publisher.cpp`, old `if (schema) { SerializeSchemaIpc(...) }`) would have attempted the
encode. A released-but-non-null `ArrowSchema` is now "no schema" rather than
"unencodable". Almost certainly the better behaviour, and clause 8's A/B pair never reaches
it — but it is a silent semantic change at a pre-existing call site, made while discharging
a refactor debt. Worth one line in the header comment so it is a decision rather than a
side effect.

---

## What I verified and found clean

**Nothing survived that should not.**

- `gateway/src/main.cpp`: the `InProcessProvider` class *and* its 10-line rationale block
  are gone (−74 lines), the includes it needed are gone, and the construction site is
  `fletcher::InProcessPubSubProvider`. `grep -rn InProcessProvider` outside
  `docs/archive/**` returns only docs/plans prose. Not merely unused — deleted.
- `FastDDSPubSubProviderTest.DefaultQosReplaysEveryRetainedRowToALateJoiner`: deleted with
  its rationale block and its now-unused `<algorithm>` include. Not skipped, not renamed;
  `grep` finds no test of that name anywhere.
- The word "may" is gone from **both** places: `docs/pubsub-interface-spec.md` §7 clause 3
  and `pubsub/include/fletcher/pubsub/provider.hpp:68`. The remaining hits repo-wide are
  quotations in the rulings ledger, the design review, and two code comments that quote the
  amendment.
- **The spec amendment is exactly the ordered one line and nothing more** — the diff of
  `docs/pubsub-interface-spec.md` is `-may be rejected` / `+**must** be rejected`, single
  hunk.
- **`qos_defaults.cpp` is untouched.** It is not in the diff at all;
  `data_sharing().off()` is still at line 68 with its full measurement comment. The
  temporary falsification was fully restored.

**CI coverage is not net-reduced (design BLOCKER 3 closed in the tree).**

- `.github/workflows/ci.integration-test.pubsub-conformance.yml` (161 lines) mirrors the
  interop lane structurally: two jobs, `workflow_call` with the devcontainer image, the
  Windows Conan-home cache, the **split Agent restore/save** (`if: success() &&
  cache-hit != 'true'`), and the trim step. The Linux job has no Agent cache — neither
  does the interop lane's Linux job, so the mirror is faithful.
- `ci.pr.yml`: output plumbed, path filter added, job added with the same
  `needs`/`if`/`uses`/`secrets` shape, **and** added to both the `pr-gate` `needs` list and
  its results expression. Nothing half-wired.
- **Sparse-checkout, checked on every job that builds a consumer.** The new lane's own list
  names `core pubsub fastdds-pubsub-provider xrcedds-pubsub-provider
  integration-tests/pubsub-conformance .conan-profiles .github`. The loopback move needs no
  edit elsewhere: `ci.gateway.yml`, `ci.pubsub.yml`, `ci.fastdds-pubsub-provider.yml` and
  `ci.xrcedds-pubsub-provider.yml` all already check out `pubsub`; the `cd.*` lanes check out
  only their own dir and resolve deps from the remote, which is the pre-existing pattern.
  The new `xrcedds → pubsub/internal/schema_conflict.hpp` include adds no new directory
  (that provider already included `pubsub/internal/segments.hpp`).
- **Path filters do not lose the loopback.** `integration-gateway-e2e` and
  `integration-gateway-fastdds-ts` both already list `pubsub/**`, so moving the loopback out
  of `gateway/**` does not stop the gateway suites firing on a loopback change.
- `pubsub/conanfile.py` needs no edit: `exports_sources` uses `src/*`/`include/*` globs and
  `package()` copies `*.hpp` recursively, so both new headers ship.
- Whole-tree gates: every new tracked file carries SPDX + Copyright in its first ten lines
  (`README.md` is `.md`, denylisted by `ci.license-headers.yml` — correctly no header), and
  **`clang-format --dry-run --Werror` at exactly 18.1.3 is clean on all 13 changed C++
  files.**

**The three divergence fixes are fixes, not suppressions; no wire bytes moved.**

- Loopback (`pubsub/src/in_process_provider.cpp:38-69`): the silent overwrite is gone;
  `slot.declared` is an `std::optional<internal::DeclaredSchema>` so "declared with no
  schema" and "never declared" are distinct states, and only the first can conflict.
  Subscriber-first and publish-first lazy creation do **not** set `declared`, so
  subscriber-first still works.
- XRCE (`xrce_dds_pubsub_provider.cpp:449-580`): `if (impl_->topics.count(name)) throw` is
  replaced by `if (ts.is_publisher) { conflict? throw : return; }`. **Not over-broad** — a
  genuinely conflicting re-declaration still throws
  (`"XRCE: topic already declared with a conflicting schema"`). Entity reuse is guarded by
  `participant_id.type == UXR_INVALID_ID` / `schema_topic_id.type == UXR_INVALID_ID`, and
  `UXR_INVALID_ID == 0x00` (verified in the vendored `object_id.h`), so the
  default-constructed `uxrObjectId{}` reads as absent — the same idiom `Subscribe` already
  used at `:639`. `declared`/`is_publisher` are set only **after** the announcement, so a
  failed declaration leaves nothing to short-circuit on.
- No test anywhere asserted the old XRCE "topic already exists" throw
  (`grep` over `xrcedds-pubsub-provider/tests/` and `fastdds-xrce-interop/tests/`), so
  nothing was deleted to accommodate the fix.
- Decision 13: the suite writes 8 opaque bytes via `WriteBuffer::AppendFixed` with no
  codec, no Arrow C++ and no generated type, so it cannot express a payload-layout
  assertion; no encode/decode path is touched by any of the three fixes.

**DEBT-C2-1..C2-5 discharged as specified.**

| Item | Result |
|---|---|
| C2-1 | `provider.hpp:65-71` now says a provider **must** reject a conflicting re-declaration, **by throwing**, and cites the ruling date. |
| C2-2 | Retention keyed by provider (`RetentionForProvider`), `schema_mode` per subject, both documented as such in `subject.hpp:36-45`. The PDA-DEC-3 handoff no longer collides with rung-1 item 3. |
| C2-3 | Clause 12's honesty note is in the clause comment **and** the README's "do NOT prove" section; the local subjects genuinely publish from two threads with a busy-wait hold window and an atomic in-flight counter bumped before the record mutex. |
| C2-4 | **One** comparison (`internal::DeclaredSchema`), **three** call sites (`Publisher`, loopback, XRCE), zero duplicated logic. Gateway cannot regress, verified: `Publisher::CreateTopic` throws on conflict and `return`s early on identical, so the provider sees neither, and `ws_session.cpp:173` is the only gateway entry point — it goes through `Publisher`. |
| C2-5 | `plans/PDA-decouple-interface.md` records the as-landed harness name, the five subjects, the CI lane and the PDA-DEC-3 sixth-subject handoff. The line-count re-declaration was not done — RECORD below. |

**The five flagged deviations, judged.**

| | Judgement |
|---|---|
| (a) clause 2 in `src/clauses_carried.cpp`, linked only into carrying binaries | **Acceptable, and in fact the only way to get the design's property.** A `TEST_P` registers per *suite*, not per instantiation, so an in-TU gate cannot make one clause absent for one instantiation without `GTEST_SKIP`. The mechanism differs from "applied at instantiation"; the property the design demanded is exactly preserved — verified empirically: `conformance_inprocess --gtest_list_tests` shows 11 clauses with no `CallbackNeverSeesNullSchema`, the other binaries show 12. `GTEST_SKIP` appears nowhere but in comments. |
| (b) OBJECT rather than STATIC libraries | **Acceptable.** The stated reason is real (static initialisers with no referenced symbol are dropped from an archive) and the outcome is verified by the registration lists above. |
| (c) `fixtures.hpp` split into `fixtures.hpp` + `suite.hpp` + `peer.hpp` | **Acceptable.** `conformance_fastdds_peer` / `conformance_xrce_peer` link no gtest, which was the point; two extra headers beyond `Files-to-touch` is proportionate. |
| (d) ctest suffix `/<Subject>` not `/0` | **This deviation does not exist.** I ran all three binaries: names are `<Subject>/ProviderConformance.<Clause>/**0**`, exactly as designed. `PrintTo` affects the value printer, not the name generator, and no name generator is passed to any `INSTANTIATE_TEST_SUITE_P`. The README documents a format the tree does not produce — RECORD below. |
| (e) `pubsub/include/fletcher/pubsub/internal/schema_conflict.hpp` not in `Files-to-touch` | **Acceptable, and genuinely internal — not new public surface by another name.** The `segments.hpp` precedent is exact: same `internal/` directory, same `fletcher::internal` namespace, same cross-package consumption by both providers, same packaging path. Header-only and inline, so no cross-package layout constraint. "New public surface: 1" survives, but only under the convention that `include/**/internal/` is not public surface — which the design's stated rule ("product-visible") does not quite say. RECORD below. |

**Scope discipline: clean.** Grepping the added lines for `extern "C"`, `dlopen`,
`LoadLibrary`/`GetProcAddress`, version negotiation, driver vtable, host-callback struct,
`nlohmann`/yaml/rapidjson/JSON config: **no hits.** `PubSubProvider`'s method set is
byte-identical (the only `provider.hpp` change is the doc comment); `Publish` is still
inverted; no copy is introduced on the row path (`InProcessPubSubProvider::Publish` still
moves out of `VectorWriteBuffer::Finish()`, the harness's `EncodeRow` writes straight into
the provider-supplied buffer) or the attachment path (const-ref throughout). Nothing above
the seam branches on built-in vs loaded — the gateway's `if (args.provider == "fastdds")`
is untouched, which is correct: that is PDA-DEC-4/5's work.

**Isolation / flake controls** as designed: domains 151/152/153 and Agent port 2019, none
colliding with `fastdds-xrce-interop` (145/2018); one `RESOURCE_LOCK` per binary; XRCE gets
a single ctest entry; own `AGENT_PREFIX` (`C:/fl-uxa-conf`) with shared
`AGENT_INSTALL_DIR` (`C:/fl-uxa-install`); Agent-complete check requires binary **and** a
non-empty lib dir; option OFF emits `message(STATUS)` and the subjects cease to exist; every
wait is a bounded predicate wait on `kClauseBudget`/`kSettleBudget` with no sleeps or
retries in the clause bodies. The runbook config is wired (inner loop with the harness and
the provider components, `pubsub-conformance` first in `full_suite_cmd`'s `for H in`,
baseline rebaselined `70` → `69` with the reason, and the 36-entry/59-case new set
recorded). Counts verified against the binaries: 11 + 24 = 35 per-clause entries + 1 XRCE
entry = 36; 11 + 24 + 24 = 59 gtest cases.

**On the two disclosed environment risks.** The unexercised POSIX branch is the larger of
the two and C4 is a concrete instance of exactly why — a never-compiled branch shipped a
signal-disposition gap that the Windows branch cannot have. Beyond C4 the POSIX code reads
correct: `fork` is followed only by async-signal-safe calls before `execv`, `execv` takes
the absolute `$<TARGET_FILE:>` path, `EINTR` is retried on both `read` and `write`, and the
reap is bounded with a `SIGKILL` fallback. The `ws2_32`/`iphlpapi` addition is a strict
improvement and cannot regress anything (system libs are additive on Windows and gated on
`settings.os`), and the "one provider per binary" property that exposed it is the same
property that will expose the next such omission — that is the harness earning its keep.

**Budget.** Declared `+1750 / −115`, reviewer-estimated `~+1900`, actual `+2927 / −183` —
**+67% on adds.** Attribution, measured: the harness itself is 2406 of the 2927. The
overrun is concentrated in four ordered-but-under-costed items —
`subjects/xrce_main.cpp` at 219 lines against the design's "~40-line registration TU"
premise (the Agent `::testing::Environment` with two platform branches and a Windows env
block, ~130 lines of it lifted from the interop fixture); `child_process.{hpp,cpp}` at 415
against its own ~250 threshold (C3); `README.md` at 151 and `CMakeLists.txt` at 178,
neither costed; plus `schema_conflict.hpp` at 64 for the C2-4 refactor, which post-dates
the estimate. **I found no unordered work in the overrun** — no product feature, no ABI
groundwork, no speculative abstraction. It is honest under-estimation of ordered scope, not
scope creep, and the only part of it that crossed a declared gate is the pipe helper.

---

## RECORD — PM corrects in place, no fix cycle

- `integration-tests/pubsub-conformance/README.md` "Shape": says a full test name reads
  `<Subject>/ProviderConformance.<Clause>/<Subject>`; the binaries produce
  `.../<Clause>/0`. (The design was right; the README is not.)
- `plans/PDA-DEC-1-conformance-suite.md` "Numbers" still declares `+1750 / −115`;
  DEBT-C2-5 asked for "nearer +1900", and the actual is `+2927 / −183`.
- `plans/PDA-DEC-1-brief.md` "Numbers" carries the same `+1750 / −115`, and its
  `*As landed (<date>…)*` line is still the placeholder.
- `docs/pubsub-interface-spec.md` §0 (l.21) still says an `InProcessProvider` "still lives
  inside `gateway/src/main.cpp`" and §10 (l.440) still phrases the move as pending; both
  are now false. Correctly **not** part of the ordered one-line amendment, so this is a
  record correction rather than an implementer omission.
- `.claude/runbook.PDA-DEC.config.md:36-37` still instructs "PDA-DEC-1 creates the
  conformance harness AND its cross-process subject. WIRE BOTH IN HERE once it lands" —
  stale now that both are wired below it.
- The "new public surface: 1" accounting rule as written ("only *product*-visible additions
  count") does not by itself exclude `internal::DeclaredSchema`, which ships in the
  `fletcher-pubsub` package. One clause — "`include/**/internal/` is not public surface"
  — makes the count of 1 self-evidently right instead of requiring the `segments.hpp`
  precedent to be recalled.

---

# Fix cycle 1 verification

Verified at `34172e8` (`git diff adc4f1f..34172e8`, 22 files, +1423 / −207; 424 of the
adds are this review file and 301 the step-4b review, so ~700 lines of real change).
Working tree clean. This is a verification pass over my five blocking findings plus a check
that the restructuring did not introduce new departures — not a re-review of what cycle 0
cleared.

**Verdict: all five blocking findings discharged. Conformant enough to close under the
2026-09-01 "Ship the guard, hunt elsewhere" ruling.** Three new low findings, all in the
never-run POSIX branch or in disclosure reach; none blocking. Two new record items.

## The five findings

### C1 — DISCHARGED, and I was wrong about the cause

The sentinel is gone. `src/clauses.cpp` clause 6 is now the approved sequence verbatim —
declare, publish N, subscribe, **no publish of any kind past that line** — and it is
*stronger* than what the design specified, not weaker:

- `kRetains`: `EXPECT_TRUE(WaitForCount(kBacklog, Deadline()))`; `kDrops`:
  `EXPECT_FALSE(WaitForCount(1, SettleDeadline()))` — the clause 9/11 pattern, so the
  dropping case pays its budget rather than being handed a deterministic end.
- The all-or-nothing equality is then asserted **independently of which wait ran**
  (`EXPECT_EQ(replayed, Retains() ? kBacklog : 0u)`), so no count between 0 and N passes
  under either trait value. `Foreign() == 0` was added too.
- Retention is still keyed by provider, so the escape hatch stays closed.

My NACK/repair mechanism was mechanically sound and is refuted as *the* cause by
measurement (still green 12/12 without the sentinel, against the same falsified package
that turned `gateway-fastdds-ts` red 2/4 twice in four runs). I accept that. The removal
stands on its own footing — it was a deviation from the approved clause body — and the
clause comment records the refutation at the point of the code, which is the right place
for it. **The design's "if it does not go red, the item is not done" gate is relieved by
the owner's ruling and I do not carry it forward.**

### C2 — DISCHARGED

The causal claim I flagged is gone. What replaced it is better than a correction: the
paragraph now **leads** with the guard against the wrong inheritance ("This is an OPEN
question, not a closed one. Do not read the paragraph below as a reason to distrust
cross-process conformance subjects in general — that reading has been measured and
refuted"), gives a three-row measurement table, names both hypotheses as refuted with the
evidence that refutes each, and names three remaining candidates in priority order. See
the sufficiency assessment below.

### C3 — accepted and recorded by the PM. Closed; not re-raised.

### C4 — DISCHARGED

`src/child_process.cpp`, POSIX constructor: `std::signal(SIGPIPE, SIG_IGN)` (process-wide,
idempotent, documented as such), and `WriteLine` now returns on `EPIPE` and every other
write error with a comment tying it to the bounded `ReadLine` that turns it into `nullopt`
→ `Outcome::kHarnessFailure` → clause failure. The refusal ladder's rung 2 item 9 is
restored on the platform where it was broken.

Two unrequested hardenings in the same file, both correct: the forked child now closes
every descriptor above stderr before `execv` (the parent's provider is constructed *before*
the spawn, so the peer was inheriting the parent's DDS sockets and shared-memory handles —
a real cross-process hygiene defect, and `close()` is async-signal-safe so it is legal
between `fork` and `execv`), and the parent's pipe ends get `FD_CLOEXEC`. See N2 for the
one risk that came with it.

### C5 — DISCHARGED, and the widening does **not** overreach

`provider.hpp` now states both schema modes on `SubscriptionResult`, that it resolves
either way so a waiter never hangs, and why `MakeReadySchemaFuture(null)` is legitimate.
That is §7 clause 1 restated, nothing more.

I checked the two *new* obligations against all three shipped providers rather than taking
the prose at face value:

- *"Unsubscribing a topic with no subscription is a no-op, not an error"* — true in all
  three: `if (it == topics.end()) return;` in `InProcessPubSubProvider`,
  `FastDDSPubSubProvider` and `XrceDDSPubSubProvider`. This is load-bearing now, because
  `ScopedSubscription`'s destructor calls it unconditionally.
- *"a provider that delivers from its own thread must not let a delivery already in flight
  outlive the call"* — also true in all three, by three different mechanisms: the loopback
  holds `mu` across the callback; FastDDS deletes both readers outside the lock and
  documents that "deleting the schema reader first waits for any in-flight schema delivery
  to finish"; XRCE dispatches under `impl_->mu` ("single recursive-mutex pump model"), so
  `Unsubscribe`'s `lock_guard` cannot be taken while a delivery is running. It is a
  sharpening of §7 clause 6 rather than a new promise, and it is the only reading under
  which clause 11 means anything. Not overreach.

The `Subscribe` re-anchor also deletes a sentence that was simply false ("Returns the
schema that the publisher provided") — it returns a future. `schema_conflict.hpp`'s
released-`ArrowSchema` widening is now recorded as a decision, which closes my L3.

## Is the README's disclosure honest and sufficient? — Yes, with three one-line gaps

Judged as the artifact the ruling rests on, so: hard.

**What it gets right, and these are the load-bearing ones.** Both hypotheses are named
*and* refuted with the evidence that refutes each, so neither can be re-offered as an
explanation. The measurement table separates the sentinel and sentinel-free runs, so a
reader can see that removing the sentinel changed nothing. The control sits in the same
table, which is what makes "the defect is live and reproducible" checkable rather than
asserted. The reproduction procedure includes the trap that would produce a false green
("check the source in the resolved Conan package folder, not just that a build ran") — that
one sentence is what makes the next round's re-run trustworthy. The remaining candidates
are ordered, and one carries new evidence I had not seen: the *schema*-propagation test
fails in the same runs, so a retained `KEEP_LAST(1)` sample is lost too, which points at
the number and mix of data-sharing endpoints per participant rather than at the row
channel. And it states plainly that the first candidate **cannot be tested from this
harness as designed**, because the peer protocol has no `subscribe` verb, and that changing
that is a design decision — handing the choice up instead of quietly widening the peer
protocol. That last paragraph is the difference between a disclosure and an excuse.

**No implied coverage remains in the README itself.** "Do not read it as covering that
defect class" survives, and clause 6's residual value is stated exactly: it "fails on any
partial replay it *does* observe, under either trait value".

**The three gaps, one line each (N3 below):** the evidence table names no platform or
Fast DDS version, so a later round cannot tell the measurement is Windows / Fast DDS 3.4.0
and that Linux is untested for this defect; it names no owning stage, so the table has no
addressee now that the ruling has given it one (PDA-ABI-7 / PDA-ABI decision 10); and the
disclosure lives **only** in the README, while `conanfile.py`'s docstring and the CI lane's
header comment still juxtapose "the provider's own suite stayed green through a shipped
receive-side data-sharing defect" with "the cross-process subjects are the point" and carry
no pointer to the blind spot — a reader of either file takes away coverage the suite does
not have.

## Did the restructuring break anything? — No conformance departure found

**Clause bodies, checked against what the design says each asserts.**

- **Clause 8** improved most. `EXPECT_TRUE(reply.refused())`, not "not ok": a dead peer, an
  expired deadline or a garbled reply are `Outcome::kHarnessFailure` and can no longer
  satisfy the one clause the loopback and XRCE conflict fixes exist to satisfy. That is
  rung 2 item 9 applied to a *negative* clause, which cycle 0 did not have. It still
  asserts only *that* the provider refused — `detail` is a printed string, never switched
  on, and `Outcome` distinguishes provider-refusal from harness-failure, not one exception
  type from another, so rung 2 item 10 is intact and no exception taxonomy has been started
  ahead of PDA-DEC-3/9. The three-valued `Reply` is harness-local, in
  `integration-tests/**`, and ships in no package.
- **Clause 9** still asserts cardinality only: `EXPECT_FALSE(WaitForCount(2, Settle))` plus
  `Count() == 1`, with the second `Subscribe` allowed to throw or to replace and no winner
  asserted. DEBT-8 holds. The `std::optional<ScopedSubscription> second` keeps the second
  registration alive to the end of the clause; its destructor unsubscribes the shared topic
  and `first`'s call then no-ops, which is safe given the idempotence verified above.
- **Clause 11** correctly keeps an **explicit** `Subject().Unsubscribe(topic)` — the
  behaviour under test — with the scope exit as an idempotent second call, and says so in a
  comment. The assertion is unchanged. Had the RAII conversion swallowed that call into
  scope exit, §7 clause 6 would have lost its only forcing test; it did not.
- Clauses 1, 3, 4, 5, 7, 10, 12 and 2: assertions byte-identical apart from `sub.schema` →
  `sub.Schema()`. Clause 12's error capture works because `Reply` default-constructs to
  `kOk`.
- All 12 clause names unchanged; 11 in `clauses.cpp`, 1 in `clauses_carried.cpp`. The
  generated ctest files still list 11 entries for `conformance_inprocess` and 24 for
  `conformance_fastdds`, so the axis gate and the 36-entry / 59-case shape are intact. No
  `GTEST_SKIP` in any source.

**`ScopedSubscription` — the ordering invariant holds everywhere.** Mechanically checked
all 12 clauses: the `Collector` is declared before the subscription in every one, so reverse
destruction order tears the subscription down first. The destructor swallows exceptions
(required — `Unsubscribe` may throw, and a throwing destructor during a failed clause would
terminate), and `Subscribe`'s `[[nodiscard]]` is satisfied by the stored `result_`.

**Other changes, none of them a departure.** `Deadline()` is now anchored at the first wait
rather than at `SetUp`; the design's stated property is "one shared deadline" and that is
preserved (memoised per fixture instance, fresh per test), no negative wait uses it, and no
assertion is weakened — only harness round-trip time stops eating the budget. `hold_us_`
became atomic and is set before `Subscribe`. Traits are now composed inside the factory
lambda, so `RetentionForProvider`'s throw-on-unknown-provider surfaces as a readable
failure instead of `std::terminate` before `main` — a rung-2 improvement, with the
provider-keyed table untouched. `RejectUnsendableTopic` closes a real hole (a segment
containing `/`, space or tab would have reached the peer as a different topic) and reports
it as a harness failure, not a refusal. The peer protocol is tagged end to end with `quit`
included, and there is still **no `subscribe` verb**, so rung-1 item 5 holds.
`peer_main.cpp` moved out of `conformance_support` into `conformance_peer_loop`, linked only
into the peer binaries — tighter than cycle 0 and closer to "link narrowly". XRCE session
keys are now unique per client per run with non-overlapping bases (parent 0x51511000 /
0x51513000 + n, child 0x51512000 + pid&0xFFF, max 0x51512FFF), and the Agent environment
gained a `SpawnedAgentAlive()` guard so the suite refuses to certify against a leftover
foreign Agent on port 2019 — a strengthening of rung 2 item 8, which also now names the
path on POSIX, closing my L2. CI/CMake changes are caching and `TIMEOUT` properties only;
the sparse-checkout lists, path filters and `pr-gate` wiring I cleared are untouched.
`clang-format` 18.1.3 is clean on all changed C++ files, and scope greps for `extern "C"`,
`dlopen`, `LoadLibrary`/`GetProcAddress`, version negotiation and any JSON/YAML dependency
return nothing but my own review prose.

## New findings (low — none blocking)

### N1 — On a local subject a harness schema-construction failure still reports as a provider refusal

`src/local_subject.cpp`: the comment says `Outcome::kHarnessFailure` is "unreachable here by
construction", but `MakeConformanceSchema(schema)` is evaluated **inside** the `try` and
throws `std::runtime_error("conformance: ArrowSchemaSetTypeStruct failed")` on nanoarrow
failure. That is a harness failure mapped to `kRefusedByProvider` — the one reachable
violation of the invariant this cycle introduced, on the exact path clause 8 asserts.
Vanishingly unlikely, but the point of the three-valued `Reply` is that likelihood is not
the test. *Fix: build the schema on the line before the `try`.*

### N2 — The forked child's descriptor-close loop is bounded by `sysconf(_SC_OPEN_MAX)`

`src/child_process.cpp`, POSIX: the loop runs to the soft `RLIMIT_NOFILE`. In a container
that is commonly 1048576 and can be far larger, giving ~10^6 `close()` syscalls per peer
spawn (~26 spawns a run) between `fork` and `execv` — Linux only, in the branch whose first
execution is this lane. It cannot deadlock, but on an unlucky limit it can eat the 45 s peer
startup budget. *Fix: cap the loop (e.g. `min(max_fd, 4096)`) or use `close_range(2)` where
available.*

### N3 — Three one-line gaps in the artifact the ruling rests on

(a) The README's evidence table states no platform or Fast DDS version, so a later round
cannot see that the measurement is Windows / 3.4.0 and Linux is untested. (b) It names no
owning stage; per the ruling the table's addressee is PDA-ABI-7 / PDA-ABI decision 10.
(c) The disclosure exists only in the README:
`integration-tests/pubsub-conformance/conanfile.py` (docstring) and
`.github/workflows/ci.integration-test.pubsub-conformance.yml` (header comment) both still
pair the shipped defect with "the cross-process subjects are the point" and carry no
pointer to the blind spot, so a reader of either file infers coverage the suite does not
have. *Fix: one clause in each of the three places.*

## New RECORD items (PM's to fix in place)

- **Retraction of my own cycle-0 record item.** I reported the README's ctest name format
  as wrong. It was right; I had checked only the *gtest* name. Verified in the generated
  `build/conformance_inprocess[1]_tests-Release.cmake`: the ctest name really is
  `<Subject>/ProviderConformance.<Clause>/<Subject>` while the `--gtest_filter` it invokes
  is `.../<Clause>/0`, because `gtest_discover_tests` substitutes the printed parameter for
  the index. The fix cycle now documents **both** names and why they differ, which is more
  accurate than what either of us had. Nothing owed; strike the item.
- `docs/pubsub-interface-spec.md` §7.2 cites the receive-side data-sharing defect as the
  motivation for requiring a cross-process subject. The requirement is satisfied literally
  and the residual gap is accepted by ruling, but the oracle now reads as a coverage claim
  to anyone who does not also open the harness README. One cross-reference to the
  blind-spot table keeps the oracle honest.
