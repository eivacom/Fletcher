# BIND-4 — code review, all slices

**Subject:** the code BIND-4 landed — the shim's value tier and subscriber half
(`c-abi/include/fletcher/abi/binding.h`, `c-abi/src/binding.cpp`, `handles.hpp`), the managed
pub/sub tier (`dotnet/src/Fletcher.Interop/`, `dotnet/src/Fletcher/`), the ported suites and the
CallerTier arm (`dotnet/tests/Fletcher.Tests/`, `integration-tests/pubsub-transport-conformance/`,
`integration-tests/pubsub-conformance/src/caller_tier.cpp`), the mapping checker and its CI lane,
and the per-row publish benchmark. Commits `029aed3` → `8cdb44b`, **excluding** what reached the
range through the merge of `main` (`957308f`): BIND-4's own footprint is 15 commits over 58 files.
**Reviewed at:** `8cdb44b`. CI on that commit: every fast lane green; `ci.pr` red on
`integration-test-gateway-fastdds-ts`, a single-publish Fast DDS race in a test from `main` (#89,
#128) that nothing in BIND-4 touches — flagged separately, rerun pending, not graded here.

**Outcome: 6 BLOCKERs, 1 question for a ruling, 17 DEBT, 8 NITs.** Two of the blockers are design
defects in the lifetime and refusal layer — the place this round treats as most dangerous, and the
place `Subscriber.cs` calls "the one file in this binding where a mistake is a use-after-free on a
transport thread rather than a failing test". The other four are checks that did not guard what
they claimed, which is why the first two survived.

## How this review was done, and its limit

BIND-3's review opened by admitting that the same author wrote and reviewed the code in one
session. **This one was split differently:** three reviewers with **fresh context** — none had seen
the session that wrote the code, its reasoning or its commit messages beyond what `git log` shows —
each took one area (native shim; managed tier; tests, CI and benchmark), with the ABI header, the
seam sources and the locked decisions as the contract. **Every BLOCKER below was then re-verified
by reading the code end to end, and three by reproduction or measurement.** DEBT and NIT items are
marked *verified* where that was done and *reported* where they rest on the reviewer's reading.

**The limit that remains:** the reviewers are the same model as the author, briefed by the author.
Fresh context removes the "I remember why this is right" failure; it does not remove shared blind
spots. #129 is a draft until BIND-10 (D-BIND-30), so this is still the only review the work gets
before the maintainer. **That it found two design defects the author's own passes missed is the
argument for doing every remaining review this way.**

---

## What I verified mechanically (claim → result)

| Claim | Result |
|---|---|
| The shim declares and defines every entry point | ✅ **44 declared, 44 defined** (`fl_binding_abi_version` lives in `binding_version.c`). **The record says 43** — true at ABI 0.3, stale since D-BIND-46 added `fl_schema_copy` |
| Managed code binds the surface | ✅ 42 of 44. The two unbound are the D-BIND-29 schema-watch stubs; the three `_raw` names are `EntryPoint` aliases; the three create/bind imports are private to their SafeHandles in `Handles.cs` |
| ABI version agrees across the boundary | ✅ header 0.4, `NativeLoader.HeaderVersionMinor = 4`, exact-minor check before 1.0 |
| `pr_gate` fails when any lane it waits for fails | ❌ **needs 26 jobs, reads 25 results** — `integration-test-pubsub-transport-conformance` is waited for and ignored (B3) |
| The CallerTier mapping check proves totality | ❌ **fooled on a scratch copy:** one mirror `[Fact(Skip = "x")]`, another commented out (40 lines) — still "total: 22 C++ cases, 22 C# mirrors", exit 0 (B4) |
| Every CallerTier mirror tests its case's property | ❌ `ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` mirrors a different case; `TheCallerTierDoorIsReachedBeforeItCanBlock` measured at **10 s** — its latch timeout — and asserts nothing about blocking (B5) |
| The benchmark's arms publish identical bytes | ✅ C1 == C2 and both harnesses against one pinned 40-byte payload, on both platforms |
| `c-abi/README.md` describes the component | ❌ still says "Only `fl_binding_abi_version()` is implemented" — false since BIND-2c |

---

## BLOCKERs

### B1 — cancelling a SIBLING from inside a delivery can free a `GCHandle` another thread is about to read

*Found independently by the native and managed reviewers; verified end to end.*

D-BIND-18's lifetime rule rests on one premise, stated in `binding.h:829-832` and in
`Subscriber.cs`'s header: **no invocation begins after unsubscribe returns, so an in-flight counter
is sufficient.** The seam honours that premise — but its "begin" is **passing the gate**
(`pubsub/src/subscriber.cpp:415-418`: lock the gate, check `retired`, call), and the managed counter
is incremented **later**, in `TryEnter` (`Subscriber.cs:576`), after the shim's thunk has built its
`SchemaScope` and `fl_attachments` view and after the reverse-P/Invoke transition.

On the blocking path that gap is harmless: native drains, so the counter is already moot. On the
**carve-out** — an unsubscribe issued from inside a delivery — native returns **without** draining
(`RetireAndDrain`, `subscriber.cpp:214-240`, defers its barrier to the canceller's outermost frame).
The self-cancel case is safe only because that thread has already incremented. A sibling is not:

1. Thread T2 passes subscription Y's gate and is inside the shim's thunk, before `TryEnter`.
2. On T1, subscription X's handler calls `Unsubscribe(Y)` (same Subscriber; ordinary over Fast DDS,
   one listener thread per reader; permitted by seam §6 clause 2). Native returns at once.
3. `Retire()` (`Subscriber.cs:590-597`) sees `_inFlight == 0` and frees Y's `GCHandle`.
4. T2 calls `GCHandle.FromIntPtr(ctx).Target` (`Subscriber.cs:492`) on the freed handle.

A freed handle's slot remains valid memory, so this is not native heap corruption. T2 reads `null`
(the row is dropped, uncounted), a stray object (the cast throws, the outer catch swallows it,
uncounted) — or, **if a new subscription reused the slot, another subscription's handler is invoked
with Y's row**: a silent cross-subscription delivery on a transport thread.

**No test exercises it.** `CancellingASiblingRunningOnAnotherThreadKeepsItPublished` covers a
sibling already inside its handler (and, per B5, not even that). `binding.h` repeats the premise, so
the header is wrong in the same place. The fix is a design choice — the free must wait for the same
event the seam defers its own barrier to — and is put to the maintainer, not made here.

### B2 — disposing a DIFFERENT Subscriber from inside a delivery terminates the process

*Managed reviewer; verified against the seam source.*

The managed refusal layer (`Subscriber.cs:371`, and `:250` for `Subscribe`) asks
`ReferenceEquals(_inDelivery, this)`. **The seam's rule is per provider and per thread, not per
Subscriber object**, and `subscriber.cpp`'s destructor says so in as many words: *"A handler on
subscriber X destroying subscriber Y over the same provider violates it"* — and rethrows
`kReentrantCall` out of a `noexcept` destructor, by owner ruling 2026-09-05.

X's handler calls `y.Dispose()` where Y (same provider) has a live subscription: the managed check
passes; Y's unsubscribe loop skips the provider teardown because the thread is inside a delivery on
that provider, leaving `provider_subscribed` set; `fl_subscriber_destroy` reaches
`provider->Unsubscribe`; **`std::terminate`**. A second route needs no sibling at all:
`_inDelivery` records only the innermost frame, so X's handler publishing on an `inprocess`
provider that delivers synchronously to Z, whose handler disposes X, also passes the check.

D-BIND-18's promise — refuse in managed code, while a managed exception is still possible — is
missed at exactly the case the seam's own comment singles out. **The same gap makes a `Subscribe`
from another Subscriber's handler on the same provider refused natively (as a `FletcherException`)
rather than by the managed refusal naming `DispatchAfterDelivery`.**

### B3 — `pr_gate` waits for the transport-conformance lane and ignores its result

*Tests reviewer; verified mechanically.* `ci.pr.yml:613` lists
`integration-test-pubsub-transport-conformance` in `needs:`; the `results:` string the gate action
reads (`:634-659`) does not contain it. **Bucket 4 — the cross-transport suite — can go red and the
PR stays green.** The lane runs; it gates nothing. The repo's own rule ("a lane that gains a
dependency needs every gate that selects it updated in the same commit") names the class; this
instance is the orchestrator's result list, one gate further than the rule lists.

### B4 — the CallerTier totality checker counts strings, not tests

*Tests reviewer; reproduced.* `CS_MIRROR` (`scripts/check_caller_tier_mapping.py:35`) matches the
`<summary>Mirrors CallerTier.X.</summary>` text wherever it appears, tied to no method and no
attribute. A skipped mirror and a commented-out mirror both still count (see the mechanical table).
The CI lane proves one failure mode — a marker removed — and not a stale marker or a C++ macro the
C++ regex does not know. **"Total" currently means "22 strings present"**, which the checker's own
header says it is not.

### B5 — the CallerTier arm is weaker than its record says

*Tests reviewer; two verified directly, the rest consistent with measured durations.* The checker
header and `c74ea1f` say **two** mirrors are weaker than their originals and say why. The reviewer
found nine that cannot fail for the property their C++ case exists for:

- **`ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` mirrors a different case** (verified). The
  C++ case re-subscribes to the **same** topic and expects success, no delivery in the current
  fan-out, and delivery on the next publish. The C# test subscribes to a **fresh** topic and expects
  a refusal — a duplicate of `SubscribingToANewTopic…RefusedByName` — and its comment's claim that
  "C++ reaches the provider and is refused with kReentrantCall" is false. **Served same-topic
  re-entry, which `Subscriber.cs:250-266` implements, has no mirror.**
- **`TheCallerTierDoorIsReachedBeforeItCanBlock` is vacuous and costs 10 s** (verified by
  measurement). `Declare` takes the `inprocess` mutex the holder's delivery holds, so the test
  thread blocks until the holder's latch times out; the stopwatch then times a refusal with nothing
  held.
- `CancellingOnAnotherSubscriberWaitsForItsDelivery` cancels from the test thread, not from inside a
  delivery on another Subscriber. `ASubscribeDuringADrainKeepsItsProviderSubscription` has no drain
  window. `ACancelRacingASelfCancelWaitsForThatHandler` self-cancels after parking, so the carve-out
  is not reached. `CancellingASiblingRunningOnAnotherThreadKeepsItPublished` runs sibling and
  canceller on one thread. `ACancelOfAFullyRetiredIdReturnsWithoutWaiting`,
  `UnsubscribeOfAnUnknownIdIsANoOp` and `AReleasedIdIsNeverReused` stop at managed short-circuits
  (`Subscriber.cs:327`) or compare fresh managed objects — all three run in under 1 ms, consistent
  with never reaching native.

Two of these are the cases that would have caught B1 and B2.

### B6 — the drain mirrors pass on the provider's drain, not the Subscriber tier's

*Tests reviewer; verified against `in_process_provider.cpp`.* Six mirrors
(`UnsubscribeWaitsForAnInFlightDelivery`, `DestructorDrainsAnInFlightDelivery`,
`CancellingOnAnother…`, `ADuplicateCancel…`, `ACancelRacing…`, `NoCallbackAfterUnsubscribeReturns`)
each cancel the **only** subscription on their topic, so the last cancel enters
`provider->Unsubscribe` — and `inprocess` holds its mutex across a delivery, a mutex its own comment
calls "the drain". A Subscriber tier with no drain at all would still pass all six. Keeping a
sibling subscription on the topic keeps the provider out of it and makes each test about the tier
it names.

---

## For a ruling, not a finding

### Q1 — the schema-watch pair: the seam now has it; the shim still says it does not

*Native reviewer; verified.* `fl_subscriber_subscribe_schema`/`unsubscribe_schema`
(`binding.cpp:780-802`) answer `kNotSupported` with the message *"the seam does not carry a schema
watch yet (D-BIND-29)"*, and a c-abi test pins that message. **`Subscriber::SubscribeSchema` and
`UnsubscribeSchema` now exist** (`subscriber.hpp:227,245`), from `f33973c` (#128), which reached this
branch through the merge of `main`. D-BIND-29 declared the pair "until the seam provides them"; that
condition has fired. BIND-4's bullet literally says "`kNotSupported` per D-BIND-29", so this
**conforms to the letter** while the message is now false. Implement now, or record as owed — the
maintainer's call.

---

## DEBT

**Native**

- **D1 — `binding.h` hides that `fl_subscriber_unsubscribe` blocks.** *Reported.* It says a delivery
  "may still be running"; outside a delivery the seam blocks until it finishes, for as long as the
  handler takes. A binding that unsubscribes while holding a lock its handler takes deadlocks, and
  the header — meant to be complete for a binding author — does not warn.
- **D2 — leaks on allocation failure.** *Reported, spot-read.* `new BlobOwner()` before
  `make_shared` (`binding.cpp:241-243`); a throw mid-loop in `fl_attachments`'s constructor
  (`handles.hpp:97-112`, run on every delivery) leaks the blocks already pushed; `fl_attachments_builder_build`
  moves the entries out before the constructor can throw, so a failed build empties the builder.
  Contained, not unsafe.
- **D3 — `FL_SUBSCRIPTION_ENDED` is asserted nowhere**, in c-abi or C#, nor "`*out` untouched on
  PENDING/ENDED". *Reported.* Easy to produce: subscribe before the topic exists, unsubscribe, wait.
- **D4 — the refcount tests cannot see a leak and see a use-after-free only by luck.** *Reported.*
  They read memory that is freed if the counting is wrong; CI builds Release with no ASan/LSan lane.
  Also absent: a c-abi test that `FL_REENTRANT_CALL` crosses the shim when `on_delivery` re-enters.

**Managed**

- **D5 — the `_known` cache is wrong after an ordinary unsubscribe.** *Reported.* `Subscriber.cs:302-307`
  claims a provider subscription survives the last local unsubscribe; that holds only on the
  carve-out. After an ordinary last unsubscribe, a re-subscribe from another topic's handler passes
  the managed check and is refused natively — a `FletcherException` instead of the
  `InvalidOperationException` naming `DispatchAfterDelivery`. Not unsafe; the comment is false.
- **D6 — a borrowed `SchemaHandle` can escape the callback.** *Reported.* `new SchemaHandle(*schema,
  owned: false)` (`Subscriber.cs:511`) is a class; a handler can keep it, and a later `Retain()` or
  `ToArrowSchema()` reads what the shim released when `SchemaScope` ended — a native use-after-free
  where invalidating the handle in the thunk's `finally` would give an `ObjectDisposedException`.
  User error, but the cheap guard is not taken.
- **D7 — `GCHandle` leaks on error paths.** *Reported.* `Publisher.cs:304-305` allocates before a
  `BorrowedAttachments` constructor that can throw outside the `try`; `Subscriber.cs:277-287` loses
  `state.Self` if the marshaller throws on a racing `Dispose`.
- **D8 — an owned `SchemaHandle` has no finalizer**, so an undisposed `Wait()` or `Retain()` result
  leaks a native reference. The frozen surface says `SchemaHandle : SafeHandle`. *Reported.*
- **D9 — `Unsubscribe` frees before checking the status** (`Subscriber.cs:342-350`). *Suspected by
  the reviewer:* unsafe only if the seam's `Unsubscribe` can fail before retiring the gate.

**Tests, CI, benchmark**

- **D10 — four publish entry points are never read back.** *Reported.* Batch `Publish`, per-row
  attachments, `PublishRaw` and `RowWriter` payloads are never received by a subscriber in the
  managed suite; `PublishingWithAttachmentsDoesNotEmptyTheCallersBuilder` asserts a managed
  dictionary's count, which cannot see the defect it names (partly covered at builder level).
- **D11 — a deadlock regression hangs CI for hours.** *Reported.* CallerTier cases run the
  possibly-deadlocking call on the test thread; `ci.dotnet.yml` has no `--blame-hang-timeout` and no
  `timeout-minutes`. Several handlers ignore their latch's timeout result — how B5's 10 s case hid.
- **D12 — the integration C# projects get a weaker rulebook and float their transitives.**
  *Verified in part.* Outside `dotnet/.editorconfig` and `Directory.Build.props`, so the format lane
  checks them without the repo's error-level rules; and neither integration project has a lock file
  (BIND-3's D3, **now two projects**).
- **D13 — the dotnet lane's path filter omits `pubsub/**`, `core/**` and the providers**, which it
  builds and whose semantics the CallerTier arm now asserts. *Reported.* Predates BIND-4
  (`934512c`); BIND-4 makes it matter.
- **D14 — benchmark validation does not cover M3 or C4.** *Verified.* The C# validation publishes
  one row via the per-row path and M4's own constant; the batch paths are never read back. The Linux
  reconciliation (M3 = C4 = 5.002 allocations/row) is indirect evidence every row reaches the
  provider. Owed to the benchmark, not to D-BIND-49's verdict.
- **D15 — the transport round trip asserts `NotEmpty`**, not the codec's bytes; XRCE is covered only
  as a typed refusal without an Agent. *Reported.*
- **D16 — timing races.** *Reported.* Fixed sleeps with no proof the handler ran; releaser windows
  started without an at-cancel latch the C++ originals have — a descheduled thread gives a false
  pass, not a false failure.
- **D17 — `c-abi/README.md` is false** ("Only `fl_binding_abi_version()` is implemented"), and the
  tracker's "43 entry points" is stale. *Verified.*

---

## NITs

- `binding.h:823-825` says re-entry "is refused with FL_REENTRANT_CALL", unqualified; the seam serves
  two re-entries (unsubscribe, and subscribe to a held topic).
- `029aed3` began exporting the value tier without a version bump; every later bump matches its
  commit. Pre-1.0 and branch-internal.
- `binding.cpp:274-283`: `fl_schema_release`'s comment sits above `fl_schema_retain`.
- `SchemaArrival.cs:116-119`: the comment says a schema-less result is a `SchemaHandle` with
  `IsNull`; the code returns `null`, as the docs say.
- `TopicPath` checks `/` across a segment before `\0`; the seam finds whichever comes first, so the
  message differs for `"a\0/b"`. Same exception type.
- `ReportAbsorbed` counts a throwing `HandlerFaulted` subscriber as a second failure (documented).
- `CrossTransportTests.cs:292` asserts a count built from itself.
- `alloc_count.c` does not interpose `valloc`/`pvalloc`/`reallocarray`; the benchmark's "well past
  tier-0" claim for 20 000 calls is unmeasured (the two managed instruments agree within 1–2%).

---

## What I did not find

No defect in: the native refcounting (every create/retain/release pair matches the header; the
deleter paths hold under a throwing constructor); containment (every fallible export goes through a
`noexcept` `Contain`, with nothrow allocation for the message); handle ordering under finalization
(publisher and subscriber pin their provider; `fl_rows` pins its codec); the ordinary and
self-cancel lifetime paths (`TryFree` frees exactly once); `TopicPath`'s six rules and the 246-byte
UTF-8 bound; the selector's NAME/PATH rule; struct layouts against `binding.h`; D-BIND-20's timeout
mapping; D-BIND-24 (no `Register`, no `SetPathResolver`); or the benchmark's mechanics.

**The contrast with BIND-3 is the finding worth keeping.** BIND-3's review found no defect in the
handle lifetimes, and said the two riskiest things were "enforced by a type and by a test rather
than by a comment". BIND-4's riskiest things — the sibling carve-out and the per-provider refusal —
were enforced by a comment and by tests that could not fail, and a same-author review read both as
sound.

---

## Resolution log

The findings above are left as found; this section records what happened to each.

| Finding | Resolution |
|---|---|
| **B1** carve-out frees under a sibling | ✅ `1fe40fa`, **D-BIND-50** (maintainer): the free waits for a thread-pool re-cancel that drains |
| **B2** per-Subscriber refusal | ✅ `1fe40fa`, **D-BIND-51** (maintainer): a per-thread frame stack, queried by provider |
| **B3** `pr_gate` ignores the transport lane | ✅ its result is read; `pr_gate` now needs 26 and reads 26 |
| **B4** the checker counts strings | ✅ a mirror must be a live, named, asserting test; C++ macros it cannot parse fail the run; `scripts/test_check_caller_tier_mapping.py` proves ten ways to fake a mirror are refused (the previous checker accepted eight of them) and runs in the lane |
| **B5** nine weak mirrors | ✅ seven now mirror their C++ case — `CancellingOnAnotherSubscriber…` over two `inprocess` instances, the only way to put two deliveries in flight. `UnsubscribeOfAnUnknownIdIsANoOp` now reaches native and checks a live bystander, but cannot cancel a never-issued id (a typed handle makes one unrepresentable), which its comment states. `CancellingASibling…` is declared **structurally weaker** in its XML docs with the reason, as the file already did for two others |
| **B6** drain mirrors pass on the provider's mutex | ✅ five keep a sibling subscription on the cancelled topic, so the provider is never entered. `DestructorDrains…` cannot (Dispose cancels every subscription, so its last cancel always enters the provider) and is declared **structurally weaker** |
| **D16** unsequenced hold windows | ✅ in every rewritten case the hold starts from an at-cancel latch, and latch timeouts are recorded and asserted rather than ignored |
| **D17** stale `c-abi/README.md` and entry-point count | ✅ corrected |
| **Q1** schema-watch pair | ✅ **D-BIND-52** (maintainer): implemented in the shim, ABI 0.4 → 0.5; C# exposure owed to a later ruling. c-abi tests: `inprocess` refuses with the transport's own message, the refusal from inside a delivery wins over `kNotSupported`, and over real Fast DDS a watch is pending, resolves on announce, is idempotent, and its last release ends a still-pending arrival |

**A NEW DEFECT, found by making B5's mirrors faithful — B7.** `Subscriber.Unsubscribe` returned
at once for a subscription already marked retired. After a handler cancelled itself (the
carve-out, which does not drain), another thread's cancel of the same subscription therefore came
back while the handler was still running — the "second exception to a promise that has one" the
seam's ruling of 2026-09-04 removed, reintroduced one tier up. The old
`ACancelRacingASelfCancelWaitsForThatHandler` parked before self-cancelling and so never reached
it. **Fixed:** a repeat cancel reaches native, whose cancel of a retiring id waits for its drain and
of a fully cancelled id is a no-op. The faithful mirror fails against the old code.

**A SECOND NEW FINDING, for a ruling — Q2.** Writing bullet 8's missing test (a payload over the
bound surfaces as `FletcherException(PayloadTooLarge)`) found that it **does not hold on Fast DDS's
default publish path**: a 512-byte row through a provider bounded at 128 bytes published without an
exception. The provider documents this as deliberate and pins it
(`sample_writer.hpp`; `FastDDSPubSubProviderTest.DataSharingOversizedRowDoesNotThrow`): on the
loaned flow an oversized row throws `kPayloadTooLarge`, on the serialize flow it is **dropped and
logged**. From C#, a row over the bound can vanish with no exception. The behaviour came from `main`
(#128), so the choices are an amendment to bullet 8 or a change to the provider; the test is held
out until that is ruled. **Ruled: D-BIND-53** (maintainer) — bullet 8 amended to the binding's
half, proven by `ErrorTests.ARowThatDoesNotFitAFixedWindowIsPayloadTooLarge` on a real native
status; the provider's drop-and-log recorded as its limitation and flagged to `main`'s owner.
**Then amended the same day:** the provider decision was to report the overflow, and the fix is
on this branch (`9255c19`), so bullet 8 now holds over Fast DDS as well
(`CrossTransportTests.ABoundedPayloadOverflowSurfacesAsPayloadTooLarge`).

**Not done here, and not claimed:** the six B6 drain mirrors and the two-provider mirror guard
properties of the NATIVE Subscriber, so falsifying them would take a shim built with the drain
removed; they rest on the reviewer's argument, verified against `in_process_provider.cpp`. The 17
DEBT and 8 NIT items other than D16/D17 are open and, by the round's convention, do not loop the item.

**D15's XRCE half — resolved by D-BIND-56 (maintainer), after close.** The transport lane builds a
MicroXRCEAgent from the recipe the interop lane uses, now shared as
`integration-tests/cmake/MicroXrceAgent.cmake`; the suite's fixture proves it owns the Agent's port;
and `xrce` runs every bucket-4 body. The close-out re-grade had marked bullet 10 met without this —
see the conformance review's correction. D15's other half (the round trip asserts `NotEmpty`, not
the codec's bytes) stays open for the theory rows; the new bridge case compares bytes.
