# D9S-4a code review (step 4b)

Diff `5b36a63..e766c3e`. Scope: binding.h, binding.cpp door, handles.hpp host, batches_server.cpp,
batches_edge.cpp, CMake/Conan/CI, test_batches.cpp, test_shim_batches_nothing.cpp.

**Verdict: no blocking findings. 2 should-fix, 5 nits.**

Checked and sound (one line each, so the next cycle does not re-derive them):
- Destruction order: `batches` goes first (declared last). `~SubscriberArrow` stops every batcher, then
  its own `Subscriber`. The provider is co-owned by that `Subscriber` anyway.
- In-callback destroy: the thunk captures only `(on_batch, ctx)`, and the batcher is co-owned by its
  thread (d7 `Start`), so nothing in the shim dangles.
- L11: `batches_mu` covers only get-or-create and is never held across `Subscribe` or a callback.
  Re-entrant subscribe from a batch callback cannot deadlock on it.
- Guard: zero-init structs; the guard is declared after them, so it is destroyed first. The `release`
  pointer is the only state. `ExportRecordBatch` releases its own partial schema on failure, so there
  is no double release. Moved means not released; unmoved means released once on every exit.
- `milliseconds(INT64_MAX)` == `kNoTimeout` (64-bit rep). Domain 7 guards the deadline sum. No
  overflow.
- Bit 63: `fl_subscriber_unsubscribe(tagged)` reaches `Subscriber::Unsubscribe` as unknown, which is
  an accepted no-op (see S1).
- Only noexcept steps follow `SubscribeBatches`. All argument conversions run before the subscription
  goes live.
- C99: `struct ArrowArray;` and `struct ArrowSchema;` are already forward-declared at
  binding.h:107-108. `fl_batch` uses `#define` constants and fixed-width fields.
- ODR: one `SubscribeBatches` per artefact; helpers are in an anonymous namespace. The test exe's
  second Arrow meets the shim's only through C Data. On Linux, `--exclude-libs` and `local: *` stop
  interposition.
- Tests: `inprocess` delivers synchronously, so the `INT64_MAX` row is deterministic. All waits are
  one-sided. Every path that destroys from outside a callback joins the batch threads, so no threads
  leak.

## Should-fix

**S1. A tagged id passed to `fl_subscriber_unsubscribe` answers FL_OK and does nothing; refuse it at
the door instead.** Confidence: medium (the design sanctions the no-op, B1/L13).
`binding.cpp:927` forwards the tagged id to the row `Subscriber`, whose unknown-id path is a silent
no-op. A binding that "unsubscribes" a batched id gets FL_OK, frees `ctx`, and keeps receiving
batches on domain 7's thread: a use-after-free with no signal. The tag exists precisely so the shim
can tell the two apart. Checking `subscription_id & kBatchedIdTag` and throwing `kInvalidArgument`
("a batched subscription id; close it with the batched close") is three lines. It changes no answer
for any id a row subscribe can return, so L13's "only adds" is kept in substance. The finaliser
idempotence that motivated the 2026-09-04 no-op ruling concerns row ids only. Owner call, since it
amends a design line.

**S2. The header does not state the destroy contract the thunk was built for.** Confidence: medium.
binding.h:1050 says only "fl_subscriber_destroy ends both kinds". `fl_subscriber_destroy`'s own text
(binding.h:858-864) still promises process termination from inside a delivery, and says nothing about
these three facts:
- (a) It now BLOCKS for running batch callbacks and runs the CLOSING delivery on another thread
  before it returns. A binding holding a lock its batch callback takes will deadlock.
- (b) From inside a batch callback it returns WITHOUT waiting for that subscription. Remaining
  batches and CLOSING arrive afterwards, so `ctx` must live until CLOSING. That is a silent
  use-after-free for a binding that frees `ctx` on return, as the row-side contract teaches it to.

Rung 6 owns the test. Make sure that rung also lands both sentences in the header, or forbid the
in-callback destroy outright.

## Nits

- N1. Thunk: on an export failure the code builds and throws a `PubSubError` only to catch and
  discard it 10 lines later. A `return;` says the same, the guard still runs, and nothing is
  allocated. (D9S-6 replaces it anyway.)
- N2. The `catch (...)` also encloses `on_batch`. When D9S-6 turns the handler into L18's no-batch
  hand-over, a callback that throws would get a second delivery for the same window. Scope D9S-6's
  catch to steps 1-4 (before the call).
- N3. No 4a row hands an UNMOVED batch to the guard: every sink moves. The "release exactly once"
  path is exercised only by reasoning until rung 5. Even a sink that ignores the batch would show a
  double release under ASan.
- N4. `TheMatcherFindsEveryPatternItIsGivenInItsOwnSource` only proves `std::string::find` works on
  the literals it lists. The control cannot fail for any reason the guard cares about. It is harmless,
  but it is not a control.
- N5. Linux (not yet run): the cached Arrow recipe defaults `with_mimalloc=True`. Static mimalloc uses
  initial-exec TLS inside a `.so` that .NET will `dlopen`. It is usually fine (pyarrow does the same),
  but confirm on the first Linux server run, or set `arrow/*:with_mimalloc=False` beside `shared=False`.

## RECORD
- RECORD: D9S-4-batches-cross.md test row 1 says "finite timeout >= 1 s ... ROW_LIMIT, ROW_LIMIT,
  TIMEOUT, wait for 3". The test uses `INT64_MAX`, waits for 2, and gets CLOSING via destroy.
  Update the plan to match the (better) test.
- RECORD: The design's H3 says the thunk "throws into domain 7, which swallows it". The code catches
  locally (per D-D9S-4) and throws nothing into domain 7.
- RECORD: The handles.hpp:180-181 comment credits declaration order with keeping the provider alive.
  `SubscriberArrow`'s own `Subscriber` co-owns it, so the order only sequences the closes.
