# BIND-5 — code review, all slices

**Subject:** the code BIND-5 landed.
- **5a:** `SubscriberArrow` (`dotnet/src/Fletcher/SubscriberArrow.cs`) with its supporting changes in
  `Subscriber.cs` and `AttachmentsBuilder.cs`, and the shim's schema check on publish (D-BIND-60:
  `c-abi/src/binding.cpp`, `handles.hpp`, `binding.h`).
- **5b:** the copy oracle from C#. That is the test-only package `integration-tests/copy-probe/`, the
  probe shim (`c-abi/probe/`, `with_probe_shim`), `dotnet/tests/Fletcher.CopyOracle.Tests`, and the
  migration of `integration-tests/pubsub-conformance` onto the package.
- **D-BIND-63:** the window ceiling.
- The tests and CI lanes that go with all three.

Commits `4522614..0670ad2`: 7 commits.

**Reviewed at:** `0670ad2`. CI: `ci.pr` was green at `38d6d78` (run 36443379137) and at `255ad43`
(run 36447834274), 50/50 on the first attempt both times.

**Outcome: 9 BLOCKERs, 5 questions for a ruling, 23 DEBT, 17 NITs.** One blocker is in the native
shim, and it is the likely cause of D-BIND-60's open finding. Eight are in the managed Arrow
subscriber. Of those eight, three are behaviours a C++ port was supposed to mirror and does not;
the port's own tests hid them by changing their parameters. The copy oracle itself held up: no
reviewer found a way to make it report a false zero-copy.

## How this review was done, and its limit

BIND-4's method, repeated. Three reviewers with **fresh context** each took one area:
1. native: the shim, the probe shim and copy-probe;
2. managed: `SubscriberArrow` and what it relies on;
3. tests, CI and acceptance.

None had seen the session that wrote the code. The contract was the ABI header, the seam
sources, the C++ `pubsub-arrow` originals and the locked decisions. They built and ran nothing.

**Every BLOCKER below was then checked by the author.** Six were reproduced by running something:
five with a throwaway xUnit file over `inprocess` (deleted afterwards) and one with standalone
MSVC builds. Three were confirmed by reading end to end. DEBT and NIT items are marked
*verified* where that was done and *reported* where they rest on the reviewer's reading.

**The limit that remains** is the one BIND-4 named. The reviewers are the same model as the author,
briefed by the author. Fresh context removes "I remember why this is right"; it does not remove
shared blind spots. Two reviewers found the same deadlock independently. Nobody found a way to fool
the oracle, which is evidence, not proof.

---

## What was verified mechanically (claim → result)

| Claim | Result |
|---|---|
| The tracing window can report `encode_copies == 0` only for a payload appended from the caller's buffer | ✅ Both the native and the tests reviewer traced every path. Blind spots read as sourceless, which scores as a copy; unseen bytes fault the trace, and the fault sticks |
| The conformance migration preserves the three old probe modes | ✅ `Judge` is byte-identical. The changed delivery condition is equivalent for `kZeroCopy`/`kStaging`/`kGrowable`. Slot size, scribbling and staging are unchanged |
| The shipped shim is unchanged in what it compiles, registers and packages | ✅ Same sources; the probe only builds under `FLETCHER_BUILD_PROBE_SHIM`. `with_probe_shim` stays in the package ID. No copy or find pattern can match `-probe` |
| `test_pubsub_arrow`'s 33 cases are mapped by name | ✅ 33 `TEST` lines, each with a named counterpart, and the one exclusion justified. **Property** gaps are in DEBT, and three are BLOCKERs |
| `pr_gate` reads every lane it waits for | ✅ 26 needs, 26 results (the BIND-4 B3 fix held) |
| The new suites ran on both platforms in CI | ✅ copy-probe 13, c-abi 86 (7 probe rows), CopyOracle 5×4, managed 313 then 316 ×4, transport 33×2 |
| The shim builds with `/EHsc` | ✅ `<ExceptionHandling>Sync` in the generated project, the CMake default for MSVC |

---

## BLOCKERs

### B1 — the D-BIND-60 helper is inside the `extern "C"` block, so its throw is undefined under `/EHsc`

*Native reviewer; confirmed by reading and by reproduction.*

- **Where it is.** `c-abi/src/binding.cpp` opens `extern "C" {` at line 181 and closes it at
  line 947. `RequireRowsMatchTopic` is declared in an unnamed namespace at line 637, **inside** that
  block, so its function type has C language linkage. It is the only C++ helper in the block; every
  other helper is defined above line 181.
- **Why it matters.** MSVC's `/EHc`, part of `/EHsc`, assumes a C-linkage function never throws.
  The compiler may therefore omit the unwind actions around it.
- **Reproduced.** The standalone program has the shim's exact shape: an `extern "C"` entry point,
  the `try` inside a C++ `Contain` template, and a lambda calling a C-linkage helper that throws
  while holding a `lock_guard`.
  - Under `/EHsc` it **fails fast** (`0xC0000409`).
  - Under `/EHs` both destructors run, the entry returns 1, and the mutex relocks.
- **This is the best-founded explanation of D-BIND-60's open finding.** The first version threw
  while holding the guard, and the next call then saw "resource deadlock would occur". The earlier
  standalone repros did not reproduce it, most likely because their throwing helper had C++
  linkage.
- **It is still live.** The current helper releases the lock before throwing, but it still throws
  from C-linkage code. So the behaviour of every refused publish on Windows is undefined:
  - it fails fast in the repro;
  - the c-abi tests show it caught with destructors skipped, which leaks `key`, the message
    temporaries and the caller's `segments`.

  The same path is taken by every fused publish with an **invalid topic**, since `JoinSegments`
  now throws from inside the helper.
- **Fix.** Move the helper above `extern "C" {`, and write the rule down: nothing but `fl_*`
  definitions inside that block.
- **To close the finding.** Restore the lock-held throw with the helper outside the block, and
  check that the deadlock is gone.

### B2 — a timer-thread handler that unsubscribes deadlocks against a concurrent delivery

*Managed and tests reviewers, independently; reproduced.*

`Flush` holds `_deliver` while the handler runs (`SubscriberArrow.cs:500-560`). The sequence:

1. The timer thread runs `Flush(Timeout)` and is inside the handler.
2. A delivery thread fills the window, or hits the D-BIND-63 split, and blocks on `_deliver`
   inside its native delivery.
3. The handler calls `Unsubscribe`, so `Stop()` runs, then a re-entrant closing flush, then
   `_subscriber.Unsubscribe`. That call is made from a non-delivery thread, so native waits for the
   delivery thread's delivery to drain.

Neither thread can move. **Reproduced:** `MaxRows = 2`, `Timeout = 20 ms`; the handler waits for a
second thread to publish rows 2 and 3, then unsubscribes and never returns.

The same class of deadlock follows from `SubscriberArrow.Dispose()` in a timer handler, and from
a `HandlerFaulted` listener that cancels.

C++ does not deadlock here, because its `Flush` drops `mu_` around the callback.

The author's own record ("a row-limit flush can wait behind a timer-thread handler") described the
wait and missed the deadlock. `UnsubscribingFromInsideATimeoutFlushIsSafe` passes only because no
delivery is in flight.

→ **Q2** asks how to fix it: drop the serialisation, or defer the native cancel until the handler
returns.

### B3 — corrupt rows count toward `MaxRows`; C++ counts only decoded rows

*Tests reviewer; reproduced.*

- **C++** flushes at `decoder_->num_rows() >= max_rows_`
  (`pubsub-arrow/src/subscriber_arrow.cpp:69,114`), so a corrupt row never counts.
- **C#** flushes at `_ends.Count >= _maxRows` over **raw** rows.
- **Reproduced:** `MaxRows = 2` and `[A, corrupt, B]`. C# delivers `RowLimit` with 1 row and 1
  dropped, then holds B; C++ delivers 2 rows and 1 dropped.

The port's `ACorruptRowIsDroppedAndTheBatchStaysAligned` changed the parameters (`MaxRows = 100000`,
closing flush), so it cannot see this. Its note does not say so, and there is no ruling. The header
claims "rule for rule".

### B4 — rows split across messages decode as good rows with the wrong attachments

*Tests reviewer; reproduced.*

`DecodeBatch(rows, ends.Length)` checks only the total bytes and the row count, never where each
message ends (`FletcherCodec.cs:240-243`); the per-row pass runs only if the whole call throws.
Two messages that are each invalid but valid together therefore decode "successfully", pairing
rows with the wrong attachments.

**Reproduced:** two valid encoded rows split at a non-boundary across two `PublishRaw` calls give
2 rows and 0 dropped. C++ decodes per message on arrival and drops both. This silently breaks the
"attachments align" claim.

Fix: check that the decode consumed exactly each row's recorded end, which needs per-row ends
from the native decode, or decode per row.

### B5 — only handler exceptions are contained; anything else in `Flush` kills the process on the timer thread

*Managed reviewer; verified by reading. The trigger has not been reproduced.*

**What can throw outside the handler's `try`:**
- `_rows.WrittenSpan.ToArray()` (`:520`), a second full copy of a window that can reach ~2 GiB;
- `Decode`'s fallback allocation (`:574`);
- `CArrowArrayImporter.ImportRecordBatch`, which the `:540` filter does not catch (it catches only
  `FletcherException or ArgumentException`).

**What happens then:**
- **On the timer thread,** an unhandled exception terminates the process. The file header's "none
  of the three threads may be taken down" does not hold.
- **On the delivery thread,** the window has already been reset (`:526-529`), so its rows are lost
  uncounted.
- **In `Stop`,** there is no `try/finally`, so the timer and codec leak. `Unsubscribe` skips the
  native cancel, so the subscription stays live and every later row is discarded. `Dispose` has
  already set `_disposed`, so it never finishes and cannot be retried.

This is also D-BIND-63 rule 4 half-kept: only the row copy in `OnRow` treats an OOM as a drop (tests
reviewer, A9).

### B6 — rows lost where neither `RowsDropped` nor `SubscriberArrow`'s own fault counter sees them

*Managed reviewer; verified by reading.*

Any exception out of `OnRow` is absorbed by the **inner** `Subscriber`. Its `HandlerFaulted` and
`AbsorbedCallbackFailures` are not reachable from `SubscriberArrow`. Only `Diagnostics.AbsorbedTotal`
moves, so D-BIND-19 rule 5 is not met for this tier. Two concrete causes:

- **Empty attachment keys.**
  - `OnRow` copies attachments through `AttachmentsBuilder.Set`, which refuses an empty key
    (`AttachmentsBuilder.cs:101-104`).
  - C++ accepts empty keys, both locally and on the wire: `Attachments::Set` refuses only a zero
    byte (`core/include/fletcher/core/types.hpp:204-212`). `PDA-DEC-AG2` explicitly declined an
    empty-key refusal.
  - So **every row a C++ publisher sends with an empty-key attachment is lost silently in C#**,
    where the C++ subscriber delivers it.
- **`Resolve` catches only `FletcherException`.** A schema Apache.Arrow's importer refuses with
  its own exception type leaves `_undecodable` false. The row is then lost, and the next row retries
  and is lost too. *Reported:* the concrete schema that triggers it was not constructed.

→ **Q3:** should `SubscriberArrow` surface the inner subscriber's absorbed failures as its own?

### B7 — a decodable topic can deliver a NULL batch, which the contract reserves for "schema could not be opened"

*Managed reviewer; verified by reading.*

- **What the contract says:** the delegate's doc, D-BIND-59 item 3 and the surface note all say
  NULL means "the schema could not be opened, every row dropped for the life of the subscription".
- **What the code does:** `SubscriberArrow.cs:540-548` also delivers NULL when a whole window fails
  to decode. That is exactly D-BIND-63's element-count case, "dropped whole and reported".
- **Why it matters:** a handler that trusts the doc and stops on the first NULL stops on a live
  topic.
- C++ has the same code-versus-doc gap, but it is not a ruling here.
- **Fix:** deliver a zero-row batch, as the window-of-only-drops case already does.

### B8 — accepted `Timeout` values misbehave

*Managed reviewer; `TimeSpan.MaxValue` reproduced.*

Only negative timeouts are refused (`SubscriberArrow.cs:194`).

- **`TimeSpan.MaxValue`, a natural "never":** the tick conversion at `:439` overflows and the
  deadline wraps to now + 1 ms. **Reproduced:** a window flushed within 500 ms.
- **Above `Timer`'s ~49.7-day maximum:** `Timer.Change` throws inside `ArmDeadline` on the delivery
  thread. The deadline flag is already set, so the window never gets a deadline, and the fault is
  absorbed invisibly (B6).

Fix: refuse anything above the `Timer` maximum except `Timeout.InfiniteTimeSpan`, or clamp such
values to "never".

### B9 — another subscriber's `Subscription` cancels this subscriber's subscription with the same id

*Managed reviewer; reproduced.*

`SubscriberArrow.Unsubscribe` keys only on `subscription.Id`, and `Subscriber.Unsubscribe` never
checks the subscription's owner. Ids are per-subscriber counters from 1, so collisions are the
norm.

**Reproduced:** `a.Unsubscribe(subscriptionFromB)` stopped `a`'s own subscription #1.

The root is BIND-4's `Subscriber`; its doc says a typed handle makes this mistake
"unrepresentable", and it does not. 5a adds a second public route to it. Fix: refuse a
subscription whose owner is not this subscriber, in both tiers.

---

## For a ruling, not a finding

- **Q1 — the D-BIND-60 check compares strict IPC bytes.** *Native reviewer.* **RULED 2026-09-29,
  D-BIND-65:** compare wire layout (the codecs' field plans), cached per (rows, topic).
  - Metadata or a nullability difference between the codec's schema and the topic's is now
    `FL_INVALID_ARGUMENT`. That is stricter than "decodes into the wrong fields", and consistent
    with "the seam's comparison".
  - The comparison is also silently skipped when either schema carries a dictionary, because
    `DeclaredSchema` is not encodable then (D3).
  - Is that the intended contract? The whole of it should be stated on both publish functions.
- **Q2 — how to fix B2.** **RULED 2026-09-29, D-BIND-64:** drop the serialisation to match C++,
  and have `Stop` wait for an in-flight timer flush.
  - Drop `_deliver` around the handler, as C++ does: this removes the deadlock and gives up "never
    called twice at once".
  - Or keep the serialisation and defer the native cancel of a cancel made from inside a flush
    until the handler returns.
- **Q3 — should `SubscriberArrow` wire the inner subscriber's `HandlerFaulted` into its own event
  and counter?** **RULED 2026-09-29, D-BIND-66:** no; the intake and the flush contain their own
  failures and count them as dropped rows, as C++'s `AddRow` does. That would make every B6-class loss visible even after its specific causes are
  fixed.
- **Q4 — D-BIND-60's reach.** **RULED 2026-09-29, D-BIND-67:** refuse a topic this publisher did
  not declare, `FL_TOPIC_NOT_DECLARED`, on the bound-rows paths, as C++ does.
  - Rows published through a second `fl_publisher`, when another instance declared the topic, are
    not checked.
  - Neither are rows published to a topic nobody declared.
  - This is by ruling ("left to the seam"). Is it acceptable for C#'s one-publisher-per-topic
    patterns?
- **Q5 — the architecture diagrams.** *Tests reviewer.* **RULED 2026-09-29:** update them. Section 2 marks `SubscriberArrow` `<<built>>` and gains one
  note for the probe shim and copy-probe as test-only artifacts; sections 6 and 7 are redrawn with the
  publish checks and `SubscriberArrow`'s path; section 8 is unchanged.
  - `BIND-architecture-diagrams.md:315` still draws the C# `SubscriberArrow` as `<<planned>>`.
  - D-BIND-62's second shim artifact and test package, and D-BIND-60's check, are not drawn.
  - Do they count as "a shape, name, count or direction" under the four-places rule?

---

## DEBT

**Native**
- **N-D1 — a stale trace can be scored as the current publish.** `last_trace_` is cleared only
  when a publish reaches the provider, and `fl_test_probe_score` scores "the last publish". So a
  publish refused earlier scores the previous one. The C# tests are safe today (a fresh provider,
  one publish each); the exported API is not. Fix: consume the trace on score, or carry a sequence
  number. *Verified.*
- **N-D2 — nothing stops `fl_test_*` leaking into the shipped shim's export table.** `ci.c-abi.yml`
  allows any `^fl_`. Add `! grep -q '^fl_test_'` on Linux and a dumpbin check on Windows. *Verified.*
- **N-D3 — the D-BIND-60 check is skipped for any schema with a dictionary.** Not documented, and
  the comment at `binding.cpp:588-590` says otherwise. See Q1. *Verified.*
- **N-D4 — the check sits on the per-row hot path:** a `JoinSegments`, a lock, a hash lookup and a
  byte compare of the IPC bytes on every `publish_row`. Compare shared pointers first, or cache per
  `(rows, topic)`. The comment at `binding.cpp:476-478` ("allocates nothing") is stale. *Verified.*
- **N-D5 — the probe arena reuses the loaned attachment's slot.** It is four slots, and the fourth
  publish after a loan encodes over the loaned bytes. That breaks "never share an address", and a
  row/attachment pointer swap would score clean then. It is pre-existing, and now reachable through
  an exported function. Fix: reserve a dedicated loan slot. *Verified.*
- **N-D6 — `fl_test_probe_loan` accepts `len == 0`,** which then means "no loan", so the attachment
  leg passes with nothing checked. Refuse it. *Verified.*

**Managed**
- **M-D1 — every flush copies the window twice.**
  - `ToArray()` runs under `_gate`, blocking deliveries for up to a ~2 GiB copy.
  - A fresh writer is created per flush and re-grows every time.
  - The per-row form pays two copies and about five allocations per row.
  - Fix: swap the writer out under the gate and decode from its span. *Verified.*
- **M-D2 — D-BIND-63's "a column's bytes are a subset of the rows'" is false for null slots.** The
  wire writes nothing for a null, so a null `FixedSizeList<float, 65536>` decodes to far more than
  its raw bytes. The raw ceiling does not bound decoded buffers there. *Reported.* The ruling's
  wording should be corrected with the fix for B7.
- **M-D3 — the first delivery's schema is latched forever.** A NULL schema latches `_undecodable`,
  where C++ retries on the next sample and reports nothing until it has a schema. This divergence is
  not ruled. *Reported.*
- **M-D4 — the `Unsubscribe` doc is wrong.** It says that from inside the handler "the closing flush
  has nothing left". That is true only on the delivery thread; from the timer or closing thread the
  nested closing flush calls the handler again, nested. C++ nests the same way, so this is a doc
  fix. *Verified.*
- **M-D5 — `SubscribeBatched` racing `Dispose` leaves a batcher that is never stopped.** Its codec
  and timer leak, and a timeout flush can call the handler after `Dispose` returned. `_disposed` is
  an unlocked bool. *Verified by reading.*

**Tests and CI**
- **T-D1 — `WindowsFollowOneAnother` checks row counts and reasons, not values in order.** A
  batcher that re-delivered window 1 would pass. C++'s `ReuseAcrossWindows` checks the values.
- **T-D2 — `BatchesAreWellFormed` uses int32 and utf8 only.** C++'s case has a dictionary and a
  `list<utf8>`. The note mentions only the missing `ValidateFull`.
- **T-D3 — the options cases assert only `NotSupported` from `inprocess`.** C++ asserts the options
  that reached the provider. A `SubscriberArrow` that forwarded the wrong non-empty profile would
  pass, and no transport-lane case covers `SubscriberArrow`'s options. The header does not mark this
  as weaker.
- **T-D4 — `ASubscriberArrowSchemaWatch…` does not check "without a data subscription", nor that
  `UnsubscribeSchema` did anything.**
- **T-D5 — the corrupt-row port uses `[0x00]` where C++ uses a truncated valid row, and has no list
  column.** A truncated row is also the probe for B4.
- **T-D6 — untested behaviours:**
  - the negative-`Timeout` refusal and `InfiniteTimeSpan`;
  - the `MaxRows < 1` clamp;
  - `Dispose` refused inside a delivery;
  - `Dispose` flushing every window as `Closing`;
  - the "anchored to the previous flush" rule;
  - D-BIND-63 rule 4 (the OOM catch never runs in a test).
- **T-D7 — the multi-row publish is not oracle-measured** *(author's own finding, confirmed)*.
- **T-D8 — `fl_publisher_publish_rows` does not check `first + count` before publishing**
  *(author's own finding, confirmed; unreachable from C#)*.
- **T-D9 — the c-abi and dotnet lanes no longer build the release configuration of c-abi.** They
  build `with_probe_shim` only, a different package ID. So the shim size and glibc checks measure a
  package that is never released. "Same build" is plausible from CMake, and nothing checks it. Fix:
  build both, or measure the default package.
- **T-D10 — the `dotnet` path filter lacks `core/**`, `pubsub/**`, the providers and
  `.conan-profiles/**`,** although the lane builds all of them. That predates BIND-5, and copy-probe
  now widens it.
- **T-D11 — D-BIND-63 never reached the development plan.** That breaks the four-places rule.
- **T-D12 — the public-surface note has drifted from the code:**
  - the class diagram lacks `SubscriberArrow.HandlerFaulted` and `BatchReason`;
  - `AttachmentsBuilder`'s entries lack `KeyAt`/`ValueAt`/`TryFind` and the empty-key refusal.

---

## NITs

- The copy-probe README says unseen bytes "fault the trace"; the leftover lent-room case is
  sourceless instead. It fails safe, and the header has it right.
- `publish_rows` now refuses `count == 0` for an invalid topic or mismatched rows; no test pins it.
- A publish racing its own `CreateTopic` is unchecked in the gap between the seam recording the
  topic and the shim recording it. Negligible.
- `builtins_probe.cpp` duplicates `builtins.cpp`, so a fourth builtin could drift between them.
- `Subscriber.cs:271-278`: the new property sits between an existing doc block and its method,
  giving one member two `<summary>` elements and the other none.
- `HandlerFaulted` for the very first deliveries can carry `SubscriptionId = 0`, because the id is
  set after `Subscribe` returns.
- `MaxRows` above `int.MaxValue` is clamped silently.
- `SubscriberArrow.Dispose` checks `_disposed` before the in-delivery refusal, which is the reverse
  of `Subscriber.Dispose`. Its message also points at a method a `SubscriberArrow` user cannot reach.
- `OnRow` copies attachments before the stopped, undecodable and oversize checks, so a dropped row
  still pays for the copy.
- `AttachmentsBuilder.KeyAt`'s lifetime note is stricter than true. Harmless.
- `CopyOracleTests.PayloadOf`'s pointer leaves its `fixed` block. That is safe because Arrow
  allocates native memory and the export pins it while bound; the comment gives only the second
  reason.
- `AnAttachmentCopiedOutOfTheDeliveryIsCaught` reads the loan pointer after the delivery, and does
  not assert that the loan was delivered at all.
- `TheDefaultCeilingIsTheMostOneManagedBufferHolds` restates the constant's own formula.
- The c-abi refusal test does not assert that nothing was delivered.
- The 5a record's "27" counts the `[Fact]`s, including one with no C++ mirror. The file maps 28
  C++ cases onto 26 facts.
- The shipped `Fletcher` and `Fletcher.Interop` assemblies now name `Fletcher.CopyOracle.Tests` as
  an unsigned friend, as they already did for `Fletcher.Tests`.
- `ci.dotnet.yml` cites a `cd.dotnet.yml` caller that does not exist. When it arrives, it will
  inherit T-D9.

## What was not found

- No way to make the tracing window, the probe shim or `Judge()` report a false zero-copy.
- No lock-order inversion between `_gate` and `_deliver`.
- No handler call after `Stop` returns (B5 aside).
- The codec is never disposed while a flush uses it.
- No endless split loop.
- No leak of the shipped shim into a release package.

## Resolution log

- **B1 — fixed.** `RequireRowsMatchTopic` moved above `extern "C" {`, and the block carries the
  rule "nothing but `fl_*` definitions". Confirmed as the cause of D-BIND-60's open finding by
  rebuilding the lock-held throw twice: inside the block it reproduced "resource deadlock would
  occur", and moved above it the same code passed. c-abi 86/86. D-BIND-65 and D-BIND-67 rewrite
  this helper next; it stays above the block.
  **And, by the maintainer's decision the same day, c-abi builds with `/EHs` on MSVC** (in
  `c-abi/CMakeLists.txt`, directory-wide, replacing CMake's `/EHsc`). The original defect, rebuilt
  under `/EHs`, passes: a C-linkage helper that throws is now harmless, not merely forbidden.
  Measured: `<ExceptionHandling>SyncCThrow` in the generated project, no D9025 warnings, c-abi
  86/86, managed 316/316 and the oracle 5/5 against the rebuilt shim.
- **B2 — fixed, as D-BIND-64 ruled.**
  - **The lock is gone.** `_deliver`, held across the handler, is removed. Windows are still cut in
    order under the state lock, and handlers may run concurrently, as in C++.
  - **`Stop` waits** for a timer flush running on another thread, unless it runs on that flush.
  - **The codec** is disposed by whichever of `Stop` and the last in-flight flush comes last. So a
    flush on another thread never decodes with a disposed codec, and nobody waits for one.
  - **Three tests:**
    - the review's reproduction, kept as a regression test;
    - a timer flush and a row-limit flush overlapping;
    - `Unsubscribe` waiting for a timer handler still running elsewhere.
  - **Results.** Managed 319/319. Putting the serialisation back hangs the run: the deadlock
    returns. Removing the wait fails `UnsubscribeWaitsForATimerHandlerStillRunning`. The deferred
    codec disposal has no deterministic test and rests on reading.
- **B5, B6 — fixed, as D-BIND-66 ruled. B7 — fixed with them** (the same catch block).
  - **The intake contains its own failures.** A row whose attachments cannot be copied, or whose
    bytes cannot be, is a counted drop; nothing reaches the inner `Subscriber`.
  - **Attachments cross as delivered,** through an internal `AttachmentsBuilder.AppendDelivered`: no
    refusal and no search, since the seam already vouched for the set. So an EMPTY key, which C++
    allows, arrives instead of losing the row.
  - **`Resolve` treats any import failure as undecodable,** not only the codec's own.
  - **The flush swaps the window's buffer out** instead of copying it with `ToArray()`. That removes
    the cut's OOM point and a second copy of up to ~2 GiB under the gate, which is review M-D1,
    fixed with it.
  - **Any failure in the decode, its fallback or the import** drops the window as counted rows.
  - **Such a window arrives as a zero-row batch, not NULL.** NULL stays reserved for a schema that
    cannot be opened, so the delegate's doc is true again (B7).
  - **Results.** One test, `ARowWithAnEmptyAttachmentKeyArrives`; putting the refusing `Set` back
    fails it. Managed 320/320.
  - **Not tested:** the OOM and importer-failure paths. They cannot be triggered deterministically
    without a test hook, and rest on reading.
- **B3, B4 — fixed, on D-BIND-68's ABI 0.7.**
  - **The native call.** `fl_decode_rows_framed` decodes a window whose row boundaries are known,
    in one call. A row the reader refuses, or one that does not end at its boundary, is skipped and
    reported per row. It skips by restart, so the decoder stays the one judge of a row.
  - **`SubscriberArrow` uses it,** so the per-row fallback pass is gone.
  - **B3:** a `RowLimit` window that came up short puts its good rows back at the front and waits
    for more, as C++ counts only decoded rows. It delivers short instead if stopped meanwhile or if
    the put-back would pass the byte ceiling.
  - **B4:** rows split across messages are dropped, not decoded into the wrong rows.
  - **Tests:**
    - c-abi: 4 cases (good window, corrupt row, split rows, a frame that does not describe the
      bytes), 90/90;
    - managed: `ACorruptRowDoesNotCountTowardMaxRows` (C++'s own `max_rows = 2` case) and
      `RowsSplitAcrossMessagesAreDroppedNotMisaligned`, 322/322, with the oracle 5/5 at 0.7.
  - **Mutations:** removing the native boundary check fails the split case in both suites, and
    never putting rows back fails the MaxRows case.
  - **The ABI** is 0.7 (47 entry points), and the handshake and READMEs are moved with it.
- **B8 — fixed.**
  - **The bounds.** A finite `Timeout` must lie between zero and the new `BatchOptions.MaxTimeout`
    (the timer's maximum, about 49.7 days); anything else is refused at `SubscribeBatched`, as a
    negative one already was. `TimeSpan.MaxValue` can no longer overflow the deadline into "now",
    and `Timer.Change` can no longer throw inside a delivery.
  - **Tests:** the bounds, accepted and refused; and a window at the maximum that waits and closes
    on `Unsubscribe`.
  - **Mutation:** dropping the upper bound fails the bounds test.
- **B9 — fixed, at both tiers.**
  - **The check.** `Subscription` now answers `BelongsTo(subscriber)`. `Subscriber.Unsubscribe` and
    `SubscriberArrow.Unsubscribe` refuse a subscription another subscriber issued, with an
    `ArgumentException`, before touching anything. The Arrow tier checks before looking up a batcher,
    so a foreign #1 cannot stop this subscriber's #1.
  - **The doc.** `Subscription`'s remark no longer claims the mistake is "unrepresentable".
  - **Tests:** one per tier, each asserting the refusal and that the owner's same-id subscription
    keeps delivering.
  - **Mutation:** removing either check fails its test.
- **Managed 326/326 on net8 and net10, and the copy oracle 5/5.**
- **D-BIND-65 and D-BIND-67 — in the shim (Q1, Q4; this also settles N-D3 and N-D4).**
  - **The recorded plan.** `fl_publisher_create_topic` records each topic's field plan, the type
    tree the wire is built from, by opening a codec over the declared schema. A schema the codec
    cannot plan is recorded as declared with no plan.
  - **The check.** The fused publishes refuse `FL_TOPIC_NOT_DECLARED` for a topic THIS publisher did
    not declare. Otherwise they compare the rows' plan with the topic's. Names, metadata and
    nullability no longer refuse, and a dictionary is compared as its value type instead of being
    skipped.
  - **The verdict cache.** The rows hold a strong reference to the last plan they matched, so a
    repeat publish compares one pointer, and a freed plan cannot come back at the same address as a
    false match.
  - **What is gone.** The per-row IPC byte comparison, and `DeclaredSchema` from the shim.
  - **`binding.h`** states both rules on both functions.
  - **Tests:**
    - c-abi: 3 new (names and nullability served, a dictionary checked by value type, another
      publisher's declaration refused), and 2 updated. `TheFusedPublishRunsOverTheWholeChain` had
      asserted that an undeclared topic is served, which is exactly what D-BIND-67 changes.
      c-abi 93/93.
    - managed: one message assertion updated, 326/326. No managed test had published bound rows
      without declaring.
    - transport: 25 of 33, unchanged; the 8 `xrce` cases need an Agent this machine does not have.
  - **Mutations:** serving undeclared topics, always matching plans, and a cache that ignores which
    topic it verified are each caught.
