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
