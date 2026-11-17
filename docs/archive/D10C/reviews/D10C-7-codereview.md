# D10C-7 code review (step 4b) — diff base 4542e78, working tree

**Verdict: 0 blocking · 3 should-fix · 6 nit.**

**Verified sound:**
- Release offsets: 64 for ArrowArray and 56 for ArrowSchema, correct on LP64 and LLP64. The thunk test at `SubscriberArrowTests.cs:1886` cross-checks them with `Marshal.OffsetOf` on both CI legs.
- `FlBatch` mirrors `binding.h:1048`: offsets 0/8/16/24/32/40/48, size 56.
- The move is atomic: it runs on the batch thread before the shim's post-return release, so nothing races it.
- No double release: `Free` releases only a non-NULL `release`, and the importer moves its input.
- The GCHandle is freed only at Closing or under `!accepted`. Nothing between FL_OK and `accepted = true` can throw.
- BC-14: negatives floor (−1 tick becomes −1), and `TimeSpan.MinValue`/`MaxValue` do not overflow.
- No lock is held across a native close.
- 32-bit has no explicit guard. It cannot reach the thunk because only win-x64/linux-x64 shims ship and the loader resolves by RID.

## should-fix
1. **A take-over failure before the handler loses the window silently, Closing included.** (confidence high; reachable only out of memory)
   - `SubscriberArrow.cs:606-607`: `CArrowArray.Create()` and `CArrowSchema.Create()` sit outside the `try`.
   - If the second one throws, the first struct leaks. The exception then reaches the thunk's `catch (Exception)`, so `_handler` is never called.
   - Effect: that window's rows are not counted anywhere. On a Closing, a consumer waiting for Closing never sees it.
   - This contradicts the design's residue (a): a failed take-over should arrive as a null batch.
   - Fix: allocate inside the `try` (null-initialised, `Free` on non-null). Any failure before the handler then takes the null-batch + `dropped += NumRows` path.
2. **The packed-assembly CI step can pass with zero tests run.** (confidence high)
   - `ci.dotnet.yml`, the step "Check the managed batcher is gone from the packed assemblies".
   - Under VSTest with xunit 2, a `--filter FullyQualifiedName=…` that matches nothing exits 0. Renaming the test or its class turns the step green while running nothing.
   - Fix: append `-- RunConfiguration.TreatNoTestsAsError=true`.
3. **Two reentrancy paths are untested and undocumented.** (confidence medium)
   - `SubscriberArrow.Dispose()` from inside a batch handler goes to `fl_subscriber_destroy` from a batch callback (`binding.h:882-884`). No test covers it, on either leg.
   - Closing a *sibling* batched subscription from a batch handler blocks, and two handlers that do it to each other hang (`binding.h:1118-1119`). The `Unsubscribe` docs do not warn about this.
   - Fix: add a server-leg test for in-handler `Dispose` (one Closing per subscription, then nothing). Add one sentence to `Unsubscribe`'s remarks about the sibling hang.

## nit
- Throw after acceptance: an out-of-memory failure in `new Subscription`/`new SchemaArrival` (`Subscriber.cs:327`) leaves native live. The handler keeps firing, the claim count leaks, and the caller has nothing to cancel; only `Dispose` frees the handle. Fix: call `fl_subscriber_unsubscribe_batches(id)` on that path.
- `ArrayReleaseOffset`/`SchemaReleaseOffset` hard-code 8-byte pointers. Use `Marshal.OffsetOf<CArrowArray>("release")` as the test does, or refuse `IntPtr.Size != 8` at the door.
- `ATenMillisecondTimeoutClosesTheWindowWithinTwoSeconds`: the 2 s upper bound may flake on loaded shared runners. The `Quiet` negative waits are one-sided and fine.
- The metadata/source scans miss a deadline built on `Environment.TickCount64`/`DateTime`/`Task.Delay`, and a hand-made batch `Thread`. The packed run does not assert the expected target-framework (TFM) count.
- `Subscription.Retired` is written on the batch thread with a plain store. Use `Volatile` to match `BatchDelivery.Subscription`.
- `TimeSpan.FromMilliseconds(-1)` equals `InfiniteTimeSpan`, so it crosses as "no timeout" rather than being refused. This is inherent to .NET; worth one line in the `BatchOptions.Timeout` doc.

## Linux-only, not verifiable by reading
- Transport-conformance Linux job:
  - the `variant=server` `conan create`;
  - `locate_shim.sh` uniqueness inside the container;
  - whether `libfletcher-c-abi-server.so` resolves its Arrow/Boost dependencies when loaded from the conan cache path;
  - the escaped `\$OUT` `test -f`.
- Pack job (docker, ubuntu): `dotnet test tests/Fletcher.Tests --no-restore` building in the sparse checkout. Is the whole project-reference closure present, and did `Fletcher.slnx` restore cover it?
- `unzip` of the nupkg glob.

## RECORD
- Design D7 (i)/(ii) (`plans/D10C-7-switch.md:83-85`) scopes the scans to `SubscriberArrow`/`SubscriberArrow.cs`. The tree scans all of `Fletcher.dll` and `dotnet/src/Fletcher` (widened by C2-1).
