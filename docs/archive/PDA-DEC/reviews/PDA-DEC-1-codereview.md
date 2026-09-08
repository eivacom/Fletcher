# PDA-DEC-1 — independent code review (step 4b)

Diff reviewed: `git diff 2de2469..adc4f1f` (35 files, +2927/-183), branch
`feature/protocol-driver-abi`.

Method note: the POSIX branch of `child_process.cpp` had never been compiled or
run. I compiled it in a local WSL Ubuntu (g++ 13.3, `-std=c++20 -Wall -Wextra`)
and additionally **ran** it against a stub peer covering: normal request/reply,
child crashes mid-conversation, child hangs and never replies, two live
children, and a failed `execv`. The POSIX spawn extract of `xrce_main.cpp` was
compiled the same way. clang-format 18.1.3 `--dry-run --Werror` is clean on
every changed C++ file; every new tracked file carries the SPDX + Copyright
header (`README.md` is `.md`, which the gate excludes).

Counts: **2 blocking**, **10 should-fix**, **12 nits**.

---

## Blocking

### B1. POSIX: writing to a dead child raises SIGPIPE and kills the whole test binary

`ChildProcess::WriteLine` (POSIX branch) writes to `plat_->to_child` with no
SIGPIPE protection, and nothing in the repository ignores SIGPIPE
(`grep -r 'SIGPIPE|MSG_NOSIGNAL'` -> no matches). If the peer child has died, the
`write()` in the *next* `Request` raises SIGPIPE with its default disposition and
terminates the parent.

This directly contradicts the contract stated in `child_process.hpp`:

> a child that crashes, hangs or closes its stdout makes the pending request
> return nullopt, which the subject turns into a typed failure and the clause
> fails on.

**Reproduced**, not inferred. Driver + stub peer in WSL, stub `_Exit(3)`s on the
first request:

```
==== die
ready=READY
r1=<none>                      <- correct: EOF -> nullopt
about to write to a dead child...
exit=141                       <- 128 + 13 = SIGPIPE; the *parent* was killed
```

Consequence on the new Linux lane: a peer-child crash (a provider assert, a DDS
abort, an OOM kill on a loaded runner) does not produce a readable clause
failure. The gtest binary dies on signal 13, so ctest reports the binary as
crashed, the failing clause is not named, and every clause after it in that
binary never runs. Windows is unaffected (`WriteFile` returns an error instead).

Acceptable fix: `signal(SIGPIPE, SIG_IGN)` once in the POSIX `ChildProcess`
constructor (or process-wide in the subject `main`), and check `write()`'s
`EPIPE` return so `Request` still yields `nullopt`.

### B2. The clause-local `Collector` is destroyed while the provider still holds a callback bound to it

`Collector::Callback()` returns `[this](...){ Record(...); }`, and every clause
declares `Collector collector;` on the **test-body stack**. `SubscriptionResult`
carries only a `std::shared_future` — it does not unsubscribe on destruction.
The provider is owned by the fixture and is destroyed later, in `TearDown()`.

So the destruction order is always: `collector` (end of test body) -> provider
(`TearDown`). The success paths get away with it only because they end with an
explicit `Subject().Unsubscribe(topic)`. Every `ASSERT_*` failure path returns
from the test body **without** reaching that line — e.g.
`ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))` in
`SchemaBeforeDataAcrossHandoff`, `PerWriterOrderIsMonotonic`,
`BacklogNeverInterleavesWithLiveSamples`, `CallbackNeverSeesNullSchema`, and both
`ASSERT_FALSE(err_a/err_b)` in `DeliveryIsSerializedPerSubscription`. The
`CONF_MUST_*` macros are `ASSERT_`-based and have the same effect.

Between those two points the subscription is still live and the delivery threads
are still running. `FastDdsLocal` (TRANSIENT_LOCAL replay fires on the DDS
receive thread at match time), `FastDdsCrossProcess` and both XRCE subjects
(dedicated `run_thread`) all deliver from a background thread, so a late sample
calls `Collector::Record` on freed stack memory — locking a destroyed mutex and
`push_back`-ing into a destroyed vector.

It fires exactly when a clause fails, i.e. when a readable diagnosis matters
most, and it turns that into UB / a crash that also loses the remaining clauses
in the binary.

Acceptable fix: make the leak unrepresentable rather than remembered — add a
small RAII `ScopedSubscription{Subject(), topic}` that unsubscribes in its
destructor, declared *after* the `Collector` so it tears down first; or give
`Collector` `shared_ptr` state that the callback captures.

---

## Should-fix

### S1. Clause 8 counts a harness/transport failure as proof of rejection (false pass)

`PeerSubject::Exchange` returns a plain `std::string` for three different things:
a genuine `err <...>` reply from the peer, "peer: no reply within budget", and
"peer: unparseable reply". `ConflictingRedeclarationIsRejected` then asserts only
`EXPECT_TRUE(err.has_value())`. A peer that timed out, died, or answered
garbage makes the clause **pass** — and this is the one clause the whole XRCE
`CreateTopic` fix exists to satisfy. The same shape applies to the local
subjects, where any unrelated exception out of `CreateTopic` also reads as
"rejected".

Fix in the forbidding direction: have `Exchange` return a small
`enum {kOk, kRefusedByPeer, kHarnessFailure}` + detail, and let a negative clause
assert only on `kRefusedByPeer`, so a transport failure cannot satisfy it.

### S2. The request/reply stream has no tag, so one unsolicited child line desyncs it permanently

`Request` writes a line and returns *the next line the pump produced*, whatever
it is. Any line the child writes to **stdout** that is not a reply — a Fast DDS
`Log` line, a Micro-XRCE verbose print, a library `printf` — shifts the stream by
one forever. From then on every `Request` silently returns the *previous*
request's reply, so a `create` that actually failed reports the stale `ok`. That
is a silent false pass, not a loud failure. (The same shift results from a
`Request` that times out and whose reply arrives afterwards; that one is bounded
because the child is per-clause and the timeout already fails the clause.)

Today's Fast DDS default consumer sends Warning/Error to stderr, so reachability
is low — but it is one build flag or one library version away.

Fix: prefix each request with a monotonic id and require the reply to echo it
(mismatch => harness failure); or move replies to a dedicated pipe and leave the
child's stdout free for logs.

### S3. XRCE `Unsubscribe` deletes a `__schema` topic the publisher side now shares

This change deliberately made `ts.schema_topic_id` shared between the two sides
of one instance — `CreateTopic` reuses a subscriber-created schema topic
("Reuse it rather than replacing the id that reader is attached to"). But
`Unsubscribe` was not taught about that sharing and still deletes it
unconditionally:

```cpp
if (ts.schema_topic_id.type != UXR_INVALID_ID) {
    uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.schema_topic_id);
    ts.schema_topic_id.type = UXR_INVALID_ID;
}
```

After `Subscribe` -> `CreateTopic` -> `Unsubscribe` on one instance and topic, the
schema **DataWriter** (`ts.schema_writer_id`) survives with its topic deleted on
the Agent, and `ts.schema_topic_id` is now invalid, so nothing repairs it: a
later `CreateTopic` short-circuits on `is_publisher`. The retained
TRANSIENT_LOCAL schema sample is then unavailable to any later subscriber,
in-process or remote — silently, since `Publish` reports nothing. The
publisher-first ordering (`CreateTopic` -> `Subscribe` -> `Unsubscribe`) hits the
same line and predates this change; the new subscriber-first ordering adds a
second route and makes the sharing intentional.

No conformance clause reaches it (fresh instance + fresh topic per clause), which
is precisely why it needs to be fixed rather than trusted.

Fix: delete the schema topic only when the publisher side does not own it —
guard the block with `if (!ts.is_publisher)`, alongside the existing
`schema_subscriber_id` cleanup.

### S4. XRCE `Publish` does not require the topic to have been declared for publishing

`Publish` only checks that the map entry exists, then calls
`uxr_buffer_topic(..., ts.writer_id, ...)`. `ts.writer_id` is
default-constructed (invalid) for a topic whose entry came from `Subscribe`, or
from a `CreateTopic` that threw. `uxr_buffer_topic` has no failure path a caller
sees, so the row goes to an object id that does not exist on the Agent and
`Publish` returns success: **silent data loss**.

This change widens the reachable set: `CreateTopic` now does
`auto& ts = impl_->topics[name];` *before* anything that can throw, so a failed
declaration leaves an entry with `is_publisher == false` behind, and a caller
that retries with `Publish` instead of `CreateTopic` loses rows quietly.

Fix in the forbidding direction:
`if (!ts.is_publisher) throw std::runtime_error("XRCE: topic not declared for publishing: " + name);`
(and/or insert into `impl_->topics` only once the declaration has succeeded).

### S5. The one clause budget is consumed by synchronous cross-process work

`ProviderConformance::SetUp` sets `deadline_ = now() + kClauseBudget` (20 s), and
every `WaitFor*` in the clause shares that absolute point. On a cross-process
subject the pipe round-trips *between* SetUp and the wait also spend it —
`PerWriterOrderIsMonotonic` does 21 round-trips (each individually allowed 15 s
by `kPeerRequestBudget`) before its only `WaitForCount`. On a loaded CI runner
the delivery wait can therefore start with almost no budget left and fail on
timing rather than on behaviour. A flaky conformance suite gets re-run forever.

Fix: give the asynchronous waits their own budget taken at the moment of the
wait (or re-arm `deadline_` after the last synchronous setup call), so the
ceiling measures delivery latency and not harness throughput.

### S6. POSIX `Shutdown` busy-spins a core for up to five seconds

```cpp
for (;;) {
    const pid_t r = waitpid(plat_->pid, &status, WNOHANG);
    ...
    std::this_thread::yield();
}
```

A tight `waitpid(WNOHANG)` + `yield()` loop. Measured in WSL with a hanging
child: teardown took **7040 ms**, spinning throughout. There are ~26
cross-process teardowns per run; any peer that is slow to exit burns a core the
loaded runner needs.

Fix: `std::this_thread::sleep_for(std::chrono::milliseconds(5))` in the loop
(or a blocking `waitpid` on a helper thread with the kill as the timeout).

### S7. The Linux lane pays the MicroXRCEAgent superbuild on every run

`ci.integration-test.pubsub-conformance.yml` has Agent restore/save steps for
the **windows** job only. On Linux `AGENT_INSTALL_DIR` is
`${CMAKE_BINARY_DIR}/microxrcedds_agent-install`, i.e. inside the Conan build
folder, so nothing persists and the ~10-15 min superbuild runs every time the
lane triggers. The CMakeLists comment ("that install is what the CI cache buys,
so the ~10-15 min superbuild happens once per machine, not once per harness") is
true only for Windows.

Fix: give the Linux job the same `actions/cache/restore` + conditional
`actions/cache/save` pair against a fixed install dir outside the build tree.

### S8. XRCE session keys are fixed per subject and reused by all 13 clauses

`kLocalSessionKey`, `kPeerSubscriberSessionKey` and `kPeerPublisherSessionKey`
are compile-time constants baked into the `INSTANTIATE_TEST_SUITE_P` arguments,
so every clause builds a *new* client with the *same* client key against one
long-lived Agent — 13 sequential sessions per subject. The next clause's
`create_session` races the previous session's teardown on the Agent (and the
teardown travels over UDP). It works here; it is the kind of thing that starts
failing on a loaded runner.

Fix: derive the key per subject instance (a base plus an atomic counter,
computed inside the factory lambda rather than at `INSTANTIATE` time) so no two
clients ever share a key.

### S9. POSIX child fds are not `CLOEXEC`

`pipe()` (not `pipe2(O_CLOEXEC)`) and no `FD_CLOEXEC`, and the forked child
`dup2`s stdio then closes only the four pipe fds before `execv`. The peer child
therefore inherits every other open descriptor in the parent — including the
parent provider's XRCE UDP socket and Fast DDS sockets / `/dev/shm` handles,
because `make()` is evaluated as a constructor argument and so runs *before* the
spawn. Bounded today (member order destroys `child_` before `provider_`, and only
one child is ever alive), but it also means a second concurrent `ChildProcess`
would hold the first child's stdin write end, so closing it would not produce EOF
and teardown would pay the full 5 s kill path.

Fix: `pipe2(..., O_CLOEXEC)` on the parent ends, and close descriptors above 2 in
the child before `execv`.

### S10. `WaitUntilReachable` never checks that the Agent it spawned is the one answering

The probe loop only tries to connect to `127.0.0.1:2019`. If our Agent failed to
bind (port already held by a leftover Agent from an interrupted run, a common
state on a self-hosted box) the probe succeeds against the *foreign* Agent,
possibly on a different DDS domain, and the suite runs against it. Related:
`FAIL()` inside `SpawnAgent` only returns from `SpawnAgent`, so `SetUp` proceeds
to spend the full 20 s in `WaitUntilReachable` before failing again.

Fix: after a successful probe, verify the spawned process is still alive
(`WaitForSingleObject(..., 0)` / `waitpid(WNOHANG)`) and fail naming the port if
it is not; and `return` from `SetUp` after `SpawnAgent` reports a failure.

---

## Nits

- Windows `ChildProcess` ctor: `if (!CreatePipe(A) || !CreatePipe(B)) throw` leaks A's two handles when B fails.
- POSIX `PumpStdout` does not strip a trailing `\r` (the Windows branch does); harmless asymmetry.
- `RetentionForProvider` throws from `INSTANTIATE_TEST_SUITE_P`, i.e. during static init -> `std::terminate` rather than the readable message it composes.
- The peer line protocol splits on whitespace and joins with `/`, so a topic segment containing a space or a `/` misparses silently; `FreshTopic` never produces one, but nothing forbids it.
- `execv` (not `execvp`): a relative peer path would fail with nothing but a missing `READY`. The CMake `$<TARGET_FILE:...>` genex gives an absolute path, so fine today.
- `src/peer_main.cpp` is compiled into `conformance_support` and therefore into every subject binary, not just the peers; harmless over-linking.
- `Collector::hold_` is a non-atomic member written by the test thread and read by a provider thread; ordered by the `Subscribe` call in practice.
- `xrce_main.cpp`'s POSIX Agent fork allocates (`std::string`, `setenv`) between `fork` and `execv`; safe only because that particular fork happens while the process is still single-threaded. The `ChildProcess` fork, which does run multi-threaded, correctly uses only async-signal-safe calls — verified by reading and by compiling.
- Neither ctest entry sets a `TIMEOUT`; the default 1500 s is probably enough for `conformance_xrce`'s ~26 peer spawns but is not stated anywhere.
- Windows `Shutdown` joins the pump after reaping, so a hypothetical grandchild holding the stdout write end would hang the join forever. Peers spawn nothing.
- `conformance_xrce` is a single ctest entry, so a crash in one clause loses the whole subject pair's results — which is what makes B1/B2 costly rather than merely untidy.
- `xrce_main.cpp` defines `kLocalSessionKey`, `...0003`, `...0004` with no `...0002`; presumably a leftover gap.

---

## Reviewed and found sound

- `child_process.cpp` POSIX branch **compiles clean** under g++ 13.3 `-Wall -Wextra`, and behaves correctly for: normal exchange, EOF->`nullopt`, deadline->`nullopt`, `execv` failure (constructor succeeds, first `ReadLine` returns `nullopt`, no zombie), two simultaneous children, and hung-child kill. No zombies left behind in any scenario (`ps -ef | grep peer_stub` -> none).
- Line framing across a 512-byte buffer, partial reads, short writes and `EINTR` are all handled on both platforms; no both-ends-write deadlock is possible because the pump drains continuously and `Request` serialises on `request_mu_`.
- Windows pipe inheritance is correct: both `CreatePipe`s run before the two `SetHandleInformation` clears, so only the child's ends are inheritable at `CreateProcess` time. `QuoteArg` implements the `CommandLineToArgvW` backslash rules correctly.
- `ReadLine`'s `cv_.wait_until(pred)` return-value handling is right (`false` only on a timeout with the predicate unsatisfied).
- The lifted `InProcessPubSubProvider` is a faithful move: callback copied to a local before dispatch, dispatch under a non-recursive mutex so `Unsubscribe` from another thread blocks until the in-flight delivery completes (clause 11 holds), re-entrancy deadlocks rather than corrupts. The only semantic change — a conflicting re-declaration now throws instead of silently overwriting — is invisible to the gateway because `Publisher::CreateTopic` throws on the same comparison one layer up.
- The XRCE fix does still refuse a genuine conflict: `is_publisher` (not "topic state exists") is the key, so an identical re-declaration is a no-op, a subscriber-first declaration proceeds, and A->B throws. The state it keys on is per-topic and cannot leak across topics; `AllocId` hands out fresh bases so the reused participant/topic ids never collide with the new publisher/writer ids.
- `DeclaredSchema::Encode(schema.get())`'s `schema->release == nullptr` test is equivalent to the old `if (schema)` on `OwnedSchema`, so `Publisher::CreateTopic`'s absent-vs-present-vs-conflicting semantics are preserved exactly (empty bytes still conflict with non-empty). All three call sites agree; the in-process provider's extra `std::optional` wrapper correctly keeps "never declared" distinct from "declared with no schema", which the other two sites do not need.
- CI wiring is complete and matches the house pattern: filter output, filter paths, job, `needs` list and the aggregate `result` check are all present; both jobs' sparse-checkouts cover every component they build, and no *existing* workflow needs a new path (the new headers ship inside the already-checked-out `pubsub` package, whose `package()` uses a recursive `*.hpp` copy and picks them up).
- CMake target graph is sound: OBJECT libraries so `TEST_P` registrations survive the link, `conformance_support` linking `fletcher-pubsub` and nothing else as the machine check that the clauses are provider-agnostic, clause-2 gated by the link line, `add_dependencies` on both peers, `RESOURCE_LOCK` per binary. The `conanfile.py` `build()` does configure+build+test, which works on both the single- and multi-config generators.
- `ws2_32`/`iphlpapi` `system_libs` addition is correct and correctly guarded by `self.settings.os == "Windows"`.
- Domain ids (151/152/153) and Agent port (2019) do not collide with `fastdds-xrce-interop` (145/2018); `FreshTopic` (pid + atomic counter, generated parent-side and passed to the peer) gives every clause and both subjects in one binary a private topic.
- The retired `DefaultQosReplaysEveryRetainedRowToALateJoiner` is genuinely superseded: it exercised two provider instances in one process with data-sharing AUTO; `FastDdsCrossProcess` clause 6 exercises the same QoS across a real process boundary, which is the stronger case for a shared-memory mechanism. (Coverage judgement flagged for the compliance reviewer, not asserted here.)

---

## RECORD (paperwork for the PM, non-blocking)

- `integration-tests/pubsub-conformance/CMakeLists.txt`: the comment "that install is what the CI cache buys, so the ~10-15 min superbuild happens once per machine, not once per harness" is true of the Windows job only — the Linux job has no Agent cache and installs under `CMAKE_BINARY_DIR` (see S7).

---

# Fix cycle 1 verification

Verified against `git diff adc4f1f..34172e8`. This is a verification pass over my
own findings plus the restructuring risk the coordinator flagged; it is not a
fresh review of anything I already cleared.

Re-ran the same WSL Ubuntu (g++ 13.3) harness as the first pass, rebuilt against
the fixed `child_process.cpp`, with scenarios added for the new tag protocol and
the new fd-closing loop.

**Verdict: both blocking findings discharged. Nothing the fixes broke.**
No new blocking or should-fix items. Three nits, one measurement that downgrades
a worry of mine, and one precise answer to the "structurally impossible?"
question.

## B1 — SIGPIPE: discharged, and the fix is complete rather than symptomatic

Re-measured, same driver and stub as before:

```
==== die
ready=READY
r1=<none>                 <- EOF -> nullopt
writing to a dead child...
r2=<none>                 <- was: process killed by signal 13
SURVIVED
exit=0                    <- was: exit=141
```

The three sub-questions asked:

- **Is a global `SIG_IGN` in a constructor the right scope?** It is correct and
  idempotent, and the process-wide side effect is benign-to-desirable (it makes
  Fast DDS / asio socket writes return `EPIPE` rather than kill the process, which
  is what those libraries want anyway). Two small points, both nits: `std::signal`
  in a multi-threaded program is formally unspecified in C++ (POSIX defines it, and
  glibc is fine), and a constructor is a surprising home for a process-global
  mutation — `std::call_once` in the subject `main`, or `sigaction`, would say the
  same thing in the right place. Also worth noting as *correct*: `exec` preserves
  ignored dispositions, so the peer child inherits `SIG_IGN` too; that does not
  orphan it, because the peer still exits on stdin EOF.
- **Does every write path check `EPIPE`?** `WriteLine` is the only write path on
  either platform, and its loop now returns on any non-`EINTR` error. `Request`
  then calls `ReadLine`, and because the pump has already latched `eof_`, that
  returns `nullopt` immediately rather than burning the deadline. Confirmed by the
  run above (`r2` came back at once, not after 3 s).
- **Does the read side have the analogous hazard?** No. `read()`/`ReadFile` on a
  pipe whose write end is gone yields EOF (`n == 0`), never a signal. The pump's
  `n <= 0` branch already handles it.

## B2 — Collector lifetime: discharged, and the ordering is structurally enforced

`ScopedSubscription` is correct: it holds `ProviderSubject&` (owned by the fixture,
destroyed later in `TearDown`), unsubscribes in a `try/catch(...)` destructor, and
exposes the future via `Schema()`. `shared_future::get()`/`wait_until()` are both
const and repeatable, so clause 10's use is fine.

**On "structurally impossible, not merely fixed at each site" — a precise answer,
because the two halves differ:**

- **Declaration order: yes, structurally impossible to get wrong.** The
  constructor takes the callback, so `ScopedSubscription` cannot be named before
  the `Collector` that feeds it — C++ name lookup enforces it, not the comment.
  A clause author physically cannot write the subscription first. That is a real
  guarantee, not a convention.
- **Bypassing `ScopedSubscription` entirely: still possible.** `ProviderSubject::Subscribe`
  remains public, so a future clause could call it directly and reintroduce the
  UAF. Verified that no clause does today: `grep 'Subject().Subscribe\|SubscriptionResult '`
  over `clauses.cpp` and `clauses_carried.cpp` returns nothing, all 12 subscribing
  clauses use `ScopedSubscription`, and the only surviving explicit
  `Subject().Unsubscribe` is clause 11's, which is the behaviour under test.
  Closing the last gap would mean making `Subscribe`/`Unsubscribe` non-public on
  the subject and granting `ScopedSubscription` friendship — a nit, given the
  clause set is small and the header comment is loud.

**Double `Unsubscribe` (clause 11's explicit call plus the scope exit) — checked
in all three providers, all idempotent:**

- `InProcessPubSubProvider`: `find`, then `callback = nullptr`. No-op on the second call.
- `FastDDSPubSubProvider`: readers are nulled and listeners `std::move`d out under
  the lock on the first call, so the second detaches nothing; the one place that
  could have thrown — `SchemaChannel::Break` calling `set_exception` twice — is
  guarded by `if (resolved) return`, so it is a silent no-op, not a
  `promise_already_satisfied`. (Had it thrown, the destructor's `catch(...)` would
  have swallowed it anyway.)
- `XrceDDSPubSubProvider`: every block is gated on an id or flag the first call
  invalidated (`schema_resolved`, `schema_reader_id`, `schema_subscriber_id`,
  `schema_topic_id`, `has_reader`). No-op on the second call.

Clause 9's `std::optional<ScopedSubscription>` handles both legal provider
behaviours: on refusal the `emplace` throws and leaves it disengaged (only `first`
unsubscribes); on replacement both engage and the second teardown is the verified
no-op.

## Restructuring risk — the three reshaped surfaces

**Three-valued `Reply` (S1).** Correct and complete. `LocalSubject` maps every
exception to `Refused` and cannot produce `kHarnessFailure` — right, since there is
no pipe between the clause and `CreateTopic`. `PeerSubject` maps `err ` to
`Refused` and deadline/EOF/unparseable to `HarnessFailure`. Clause 8 now asserts
`reply.refused()`, so a dead peer can no longer satisfy it, and its failure
message distinguishes "accepted" from "a harness failure". `CONF_MUST_*` assert
`ok()`, so a harness failure still aborts the clause loudly. The new
`RejectUnsendableTopic` closes my whitespace/slash nit in the forbidding
direction.

**Tagged request/reply (S2).** Verified working, including the two silent-false-pass
paths I described:

```
==== noise      (child emits a log line before AND after every reply)
r0=ok  r1=ok  r2=ok  r3=ok        <- no shift; untagged lines discarded

==== slowfirst  (first request replies 3 s late, after its 500 ms deadline)
r0(expect none)=<none>
r1(must NOT be r0's stale ok)=ok  <- stale "#1 ok" discarded, not mis-read
r2=ok
```

Prefix matching is unambiguous because the tag is compared with its trailing
space, so `#1 ` does not match `#10 ok`. The discard loop is bounded by the same
deadline, and an EOF exits it immediately, so it cannot spin. `next_tag_` is
atomic and each `Request` matches only its own tag under `request_mu_`, so
clause 12's two publishing threads can take tags out of order without confusion.

**Concurrency of the collector path.** Asked for `-fsanitize=thread`-style
reasoning: I tried to run TSan and it will not start under this WSL2 kernel
(`FATAL: ThreadSanitizer: unexpected memory mapping`), so I am **not** claiming
sanitizer coverage — the following is analytic, by enumeration of the shared
state.

Every `Collector` member is now either atomic or guarded by `mu_`:
`in_flight_`, `max_in_flight_`, `foreign_` atomic; `deliveries_` and `cv_` under
`mu_` on both sides (`Record` writes, `Snapshot`/`Count`/`Seqs`/`WaitFor*` read);
and `hold_us_` was the last non-atomic member, now `std::atomic<int64_t>`. So the
race I filed as a nit is genuinely closed rather than closed by observation. Two
residuals, neither a race: `MaxInFlight()` is sampled after `WaitForCount` returns,
so a later delivery could bump it unobserved — that direction only risks a false
*negative* for clause 12, never a false positive, so the clause stays sound where
it matters; and `Count()` inside a failure message is a second, later sample,
which is cosmetic. Clause 12's `reply_a`/`reply_b` are each touched by one thread
only and read after `join()`, and both threads are joined before the
`ScopedSubscription` destructor runs.

## Should-fix spot-checks — all as described

S3 `Unsubscribe` now guards the `__schema` topic with `!ts.is_publisher`, so only
the side that owns it deletes it; the publisher's schema writer keeps a live topic
and publisher entities go with the session, as before. S4 `Publish` throws unless
`ts.is_publisher`, and this does not break clause 11, because `Unsubscribe` never
clears that flag, so the post-unsubscribe publishes still succeed. S5 `Deadline()`
is lazily anchored at the first call, preserving "one deadline per clause" while
no longer charging pipe round-trips against the delivery wait. S6 `sleep_for(5ms)`
replaces the spin. S7 the Linux lane now restores/saves the Agent install through
`FLETCHER_AGENT_INSTALL_DIR`, with the same split restore/save and no prefix
fallback as Windows. S8/S10 as reviewed below. S9 verified by measurement: a child
spawned after the parent opened two extra descriptors reported
`inherited_fds=0`. Nits: `CreatePipe` leak fixed by splitting the two calls, POSIX
`\r` strip added, `MakeTraits` moved inside both factory lambdas (so
`RetentionForProvider` no longer throws during static init), `hold_` atomic,
explicit ctest `TIMEOUT`s (120/180/900 s). `peer_main.cpp` also moved out of
`conformance_support` into its own `conformance_peer_loop` object library linked
only by the two peer binaries — correct, and it does not double-link
`conformance_support`'s objects, since object-library objects are contributed only
by a direct link dependency.

## Left deliberately — both accepted

- **Windows `Shutdown` pump-join order.** Accepted. The join can only hang if a
  grandchild inherits the child's stdout write end, and the peers spawn nothing.
- **`execv`, not `execvp`.** Accepted. The path always comes from
  `$<TARGET_FILE:...>`, so it is absolute by construction, and a bad path already
  surfaces as a named constructor/READY failure.

## New nits (fix cycle only)

- **The fd-closing loop's cost — measured, and smaller than I expected.** `sysconf(_SC_OPEN_MAX)`
  is the soft `RLIMIT_NOFILE`, which containers often set very high, so the loop
  can be up to a million `close()` calls per spawn. Measured, 5 spawn+teardown
  cycles: **32 ms at nofile 4096, 60 ms at 65536, 504 ms at 1048576** — i.e. ~95 ms
  per spawn at the worst limit Docker commonly hands out, or ~2.5 s across a
  full run. Real but immaterial against the 180 s/900 s ctest timeouts, so this
  is a nit and not the should-fix I had been prepared to file. `close_range(3, ~0U, 0)`
  / `closefrom(3)` would make it O(1) and would also remove the
  `static_cast<int>(max_fd)` narrowing, which is UB if a limit ever exceeds
  `INT_MAX`. Also, `sysconf` is not on the async-signal-safe list, though glibc's
  implementation is just a `getrlimit`.
- **`std::signal` in the constructor** — see B1 above: prefer `std::call_once` in
  `main`, or `sigaction`.
- **Session-key derivation (S8) is much better but not airtight.** The peer uses
  `base + (pid & 0x0FFF)`, so two children of one run collide only if their pids
  differ by an exact multiple of 4096 — unlikely, not impossible on a busy runner,
  and the consequence is exactly the key reuse S8 removed. The stated reason ("not
  expressible through `MakePeerSubjectFactory`'s by-value args") is not quite so:
  the factory lambda captures `peer_args` by value and could rewrite a
  `%INSTANCE%` placeholder per construction. Also minor: `NextSessionKey`'s counter
  is one static shared by both bases, so keys are sparse within each 0x1000 band —
  harmless at 26 subjects, worth a word in the comment. And `SpawnedAgentAlive()`
  reaps the Agent when it finds it dead without clearing `pid_`, so `KillAgent`
  later signals a reaped pid (harmless today; `pid_ = -1` there is a one-liner).

## Notes, not findings

- Clause 6 was restructured beyond my findings (the sentinel publish removed). I
  checked only its code, not the measurement question the coordinator says is
  settled: the all-or-nothing assertion is now made independently of which wait
  ran, the counts cannot pass at any value between 0 and N, and the added
  `Foreign()` check is a strengthening. One consequence worth stating: it is now
  the only clause whose pass depends on a positive wait with no forcing publish
  behind it, which makes it the suite's most timing-sensitive clause and the
  likeliest first CI flake. The 20 s budget looks ample for local DDS/XRCE.
- The XRCE `Unsubscribe` → re-`Subscribe` gap I noted in the first pass
  (`subscriber_id` is not invalidated while the `reader_id` entity is deleted, so a
  re-subscribe on the same instance requests data on a deleted reader) is
  untouched, pre-existing, and reached by no clause. Correctly left alone.
- `integration-tests/pubsub-arrow-fastdds`'s 1-2-of-4 intermittent failure is
  **pre-existing and unrelated to this diff**: nothing in `adc4f1f..34172e8` or
  `2de2469..adc4f1f` touches that harness, and the cause the author gives (28-way
  ctest parallelism over four tests sharing DDS domain 137 *and* topic names) is
  independent of everything reviewed here. Agreed: known flake, not a regression.
- Gates re-checked after the fix cycle: clang-format 18.1.3 `--dry-run --Werror`
  clean on all changed C++ files; the only files added are two `.md` reviews, which
  the licence gate excludes, so it cannot regress.
