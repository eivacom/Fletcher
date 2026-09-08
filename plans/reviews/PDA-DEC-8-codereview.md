# PDA-DEC-8 — independent code review

Scope: `git diff e99eaeb..b7b33f3`, 7 files, +705/−25. Zero product code changed.
Reviewed as code: `integration-tests/pubsub-conformance/subjects/fastdds_main.cpp`
(+473) and `integration-tests/pubsub-conformance/CMakeLists.txt`. Spec/README/plan
text read for intent only.

**Verdict: no blocking defects.** The specific risks the item was flagged for —
journal races on listener threads, short-circuited absence assertions, threads
outliving providers, teardown on failing paths — are all handled correctly. Three
`should-fix` items are about the proof staying interpretable and non-vacuous over
time, not about it being wrong today.

## Counts

- blocking: 0
- should-fix: 3
- nit: 5

---

## What I checked and found correct (stated once, no evidence blocks)

**The journal is sound.** `Journal::Record` builds the `Entry` outside the lock and
takes `mu_` only for the `push_back`; `Snapshot()` returns `entries_` **by value**
under the lock; `Count()` reads `size()` under the lock. `mu_` is `mutable`, so the
`const` accessors genuinely lock. No reference, iterator or pointer into `entries_`
escapes: `Markers()`/`Shapes()` take a `const&` to the *returned temporary*, which
lives to the end of the full expression. No DDS call is made under the lock, so
there is no lock-order edge against Fast DDS's own listener locks. Nothing reads
`entries_` outside the lock, including in the streamed failure messages
(`a.SharedJournal().Count()` is the locked accessor).

**Absence assertions genuinely pay the window.** Every negative claim is preceded by
an unconditional `std::this_thread::sleep_for(kSettle)` — isolation case line 416,
concurrent case line 517, bounds case line 580 — never by a sampling loop.
`WaitForCount` is used only for positive halves, as its comment claims. The absence
window is in fact *strictly wider* than the control's: the sleep starts after the
positive `WaitForCount`s return, which is already after the foreign publish.

**Thread lifetime is right, on every path.** In
`TwoInstancesStayIsolatedUnderConcurrentTraffic` the two `std::thread`s are created
and joined inside a bare `{ … }` scope with **no `ASSERT_*`/`EXPECT_*` between
creation and join**, and the per-thread failure is carried out as data (`a_threw`,
`b_threw`) and only *then* asserted, after both joins. This is the correct shape and
it defeats the gtest-`ASSERT_*`-returns-early hazard rather than tripping over it.
The `a_threw`/`b_threw` writes are each done by exactly one thread and read after
`join()`, so they are ordered.

**Teardown is right, including on failure.** `Instance` is a stack object in every
case, so an `ASSERT_*` early return still destroys it and deletes its participant —
no leaked shm segment on a failing path. Member order is load-bearing and correct:
`provider_` is declared last so destroyed first, stopping the DataReaders (and thus
the listeners that reach a journal) before the journals and `SubscriptionResult`s go
away. If the constructor throws part-way (e.g. out of `Subscribe`), the already-
assigned `provider_` member is still destroyed, so that path leaks nothing either.

**QoS makes the control robust.** `MakeFletcherDefaultWriterQos/ReaderQos` are
RELIABLE + KEEP_ALL + TRANSIENT_LOCAL with `max_samples = 100`, and receive-side
data-sharing is `off()`. So (a) a row published before cross-participant matching is
still delivered from the writer's TRANSIENT_LOCAL history once matching completes —
the control does not depend on discovery having finished before `PublishShared`;
(b) 32 concurrent rows are well inside the 100-sample limit, so no write blocks or
drops; (c) the in-order whole-journal comparison in the concurrent case is a real
contract (one writer per reader, RELIABLE), not a hope.

**ctest wiring is right.** `conformance_fastdds` is discovered per-case with
`RESOURCE_LOCK conformance_fastdds`, so the four new cases serialise against each
other and against every existing fastdds clause — they cannot collide on a domain
or on the peer child. Worst-case runtime for each new case is bounded at roughly
`2 × kClauseBudget` (schema waits) `+ 2 × kClauseBudget` (`WaitForCount`) `+ kSettle`
≈ 122 s, inside the 180 s per-entry `TIMEOUT`. This item correctly does **not**
repeat `pubsub-arrow-fastdds`'s mistake: it shares topic names deliberately but
separates by domain *and* relies on a per-case ctest lock.

**Simplification: the duplication that should stay.** Each case constructs its own
`ProviderRegistry` and calls `RegisterFastDDSProvider`. That is four repeated lines
and it should **not** be lifted into a fixture: a per-case registry is what makes
mutation M4 (a registry that memoises one provider per name) visible. Likewise the
markers-as-strings choice is right — the recorded reds (`{ "B:0", "A:0" }` vs
`{ "A:0" }`) are readable, and the failure messages name the instance, the topic
role and the claim, so a future engineer can act on them without reading this file.

---

## should-fix

### S1. The control conflates "no crossing at all" with "crossing slower than `kSettle`", and never surfaces its margin (confidence: high)

`TwoInstancesOneDomainDoInterfere` (lines 456–464):

```cpp
const bool crossed = WaitForCount(a.SharedJournal(), 2, kSettle);
const std::vector<std::string> markers = Markers(a.SharedJournal().Snapshot());
EXPECT_TRUE(crossed) << "the control measured NO crossing on one domain within kSettle, so "
                        "TwoInstancesTwoDomainsStayIsolated proves nothing: …";
```

Two problems, and they compound:

1. **The message can be wrong.** `crossed` and `markers` are two separate
   observations. If the second row lands between `WaitForCount`'s timeout and the
   `Snapshot()`, the case reports "measured NO crossing" while the two `EXPECT_NE`s
   below both pass and `markers` visibly contains `B:0`. That is precisely the
   marginal-timing failure this control exists to report, and it reports it in the
   least interpretable possible way.
2. **The margin is invisible.** The control asserts only `crossing ≤ 1500 ms`. The
   ~260 ms measurement that justifies 1500 exists in the plan and log, not in the
   suite. If the real crossing time creeps to 1400 ms — a slower runner, a Fast DDS
   version whose initial PDP announcement burst is spaced differently — the control
   stays green and nobody learns the margin evaporated. The next environment change
   then flips it red as a mystery flake, and the flagged failure mode follows: it
   gets disabled, and the isolation claim silently becomes vacuous.

**Fix (both, cheaply):**

```cpp
const auto started = std::chrono::steady_clock::now();
const bool crossed = WaitForCount(a.SharedJournal(), 2, kClauseBudget);
const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
const std::vector<std::string> markers = Markers(a.SharedJournal().Snapshot());
RecordProperty("crossing_ms", static_cast<int>(elapsed.count()));
ASSERT_TRUE(crossed) << "the control measured NO crossing on one domain at all …";
EXPECT_LE(elapsed, kSettle) << "a real crossing took " << elapsed.count()
    << " ms, outside the " << kSettle.count() << " ms window "
    << "TwoInstancesTwoDomainsStayIsolated pays for its absence claim — widen kSettle, "
       "do not narrow this control";
```

This keeps the single-`kSettle` invariant (the window is still one constant and
widening it still strengthens both cases) while splitting the two reds that need
different responses: *"the arrangement has no teeth"* (`ASSERT`, and the isolation
claim is void) versus *"the window needs widening"* (`EXPECT`, tuning). The recorded
`crossing_ms` also turns the margin into something a future run reports instead of
something a plan document remembers.

### S2. `AwaitSubscriptionsLive` returns `kOk` when the wait succeeded but the schema is null — the guard is written and inert (confidence: high on the logic, low on reachability)

Lines 324–331:

```cpp
const PubSubStatus shared = shared_result_.schema.Wait(kClauseBudget, &schema);
if (shared != PubSubStatus::kOk || schema == nullptr) return shared;
```

If `shared == kOk` **and** `schema == nullptr`, this returns `shared`, i.e. `kOk`.
The `schema == nullptr` disjunct therefore cannot ever change the returned value: on
that path the caller's `ASSERT_EQ(a.AwaitSubscriptionsLive(), PubSubStatus::kOk)`
passes with no schema resolved — exactly the condition the helper's own comment says
it rules out ("Both subscriptions have their schema, so neither publish below can be
absent merely because a subscription was not live yet — the control's named failure
mode"). Same shape on the `priv` line.

Consequence is bounded — a genuinely dead subscription is still caught downstream by
the positive `WaitForCount` halves, and a null-schema delivery would still redden
`Shapes(...) == {1}` via the `-1` sentinel — so this is not a false green today. But
it is a guard that reads as live and is not, in the one helper every case routes its
liveness check through.

**Fix:** make the bad state unrepresentable rather than mapped onto `kOk`:

```cpp
if (shared != PubSubStatus::kOk) return shared;
if (schema == nullptr) return PubSubStatus::kInternal;  // §7 forbids a null resolution
```

(and the same two lines for `priv`). `kInternal` exists for exactly this
(`core/include/fletcher/core/status.hpp:54`). Better still, return
`std::optional<std::string>` — the caller only ever compares against `kOk` and then
prints `SchemaWaitMessage()`, so the status value carries no information the message
does not.

### S3. Each case takes several independent `Snapshot()`s of the same journal, so its assertions can be derived from divergent observations (confidence: high; simplification, forbidding direction)

`TwoInstancesTwoDomainsStayIsolated` snapshots `a.SharedJournal()` twice (line 418
for `Markers`, line 430 for `Shapes`); the concurrent case snapshots each shared
journal twice as well (527/535, 529/537); the control's `crossed`/`markers` pair
(S1) is the same defect with a worse symptom. Nothing forces the two snapshots to
agree, so a case can in principle report "the markers are exactly A's own rows" and
"the shapes are wrong" about two different histories — and the reader of that red
has no way to tell.

It is also more code than the alternative. **Fix:** one observation per journal per
case, both claims derived from it:

```cpp
const std::vector<Journal::Entry> a_shared = a.SharedJournal().Snapshot();
EXPECT_EQ(Markers(a_shared), (std::vector<std::string>{a.Mark(0)})) << …;
EXPECT_EQ(Shapes(a_shared), (std::vector<int64_t>{1})) << …;
```

This makes "two assertions in one case disagreeing about what arrived"
unrepresentable rather than merely improbable, and it is the same edit in all three
cases.

---

## Nits

- **Domain 154 is not owned outright.** `integration-tests/gateway-end-to-end/test/end-to-end.test.ts:350` already uses `domainId: '154'`, so line 177's "no other test in the tree names any of these" is false. Practical risk is negligible (different topic names; that suite is `npm test`, not a ctest entry; 151/152 already overlap the same file pre-existing), but the claim is what a future engineer will allocate against. Fix: shift the block to 161–167, or state the overlap.
- **`Publish`'s `row_bytes - 2` underflows for `row_bytes < 2`** (line 354). Unreachable today (only 2 and 8192 are passed). Forbidding fix: take `padding_bytes` and `AppendZeros(padding_bytes)`, so the two marker bytes are not something a caller can subtract away.
- **`Instance`'s constructor throws `std::runtime_error`** on a null provider (line 304), which reaches gtest as an untyped "C++ exception with description … thrown in the test body" rather than a named assertion. Loud and typed enough, but a `ProviderSelector` that stopped resolving would read better as a case-level `ASSERT_NE`.
- **The `schema_children == -1` sentinel is only ever asserted against on *shared* journals in two of the four cases.** Private journals' shapes are never checked, and `TwoInstancesKeepTheirOwnPayloadBounds` checks no shapes at all — so the `Entry` comment's "asserted against rather than tolerated" holds for less than it says.
- **If `std::thread publisher_b`'s constructor throws** (line 495), `publisher_a` is left joinable and its destructor calls `std::terminate`. Practically unreachable; a `jthread`-style RAII joiner would close it.

---

## RECORD (paperwork for the PM — never blocking)

- README §"`Registry.*` cases that live in a PROVIDER binary": "plus **five** more `Registry.` entries that live in the two PROVIDER binaries" and "(**4** more entries, and 1 in `conformance_xrce`)" — `conformance_fastdds` holds **5** `Registry.` cases (`FastDdsResolvesAsABuiltIn` plus this item's four), so the total outside `conformance_registry` is **6**, not 5, and the fastdds figure is 5, not 4.
- `integration-tests/pubsub-conformance/CMakeLists.txt:294`: "conformance_fastdds carries two subjects (domains 151 and 152)" — the binary now also joins 153 and 154–160. The `RESOURCE_LOCK` conclusion is unaffected; only the parenthetical is stale.
- `plans/PDA-DEC-8-multi-instance.md` "provider suites unchanged (fastdds 85/84…)" versus the reported fastdds 85/85.
- `fastdds_main.cpp:9-10`'s file header still says "Fixed, distinct domains so the harness cannot collide with … (domain 145) or with itself" — true of 145, not of 154 (see the first nit).
