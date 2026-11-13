# D9S-4 — Batches cross the shim (design)

Round D9S, item D9S-4, ladder rung 4 (`plans/D9S-test-ladder.md`). Tree `8854773`. Full loop.
Authority read: d9 §1, §3 (L1-L4, L8, L11-L18), §4, §5, §7 (Q6, Q7, Q11, Q12); d7 §3 (L1, L3-L7, L9,
L12, L15), §4, §6; `subscriber_arrow.hpp`; register BC-8 to BC-15; `plans/D9S-locked-decisions.md`;
`plans/reviews/D9S-4-audit-0a.md` (CLEAR). **Rulings ledger:** `plans/RIX-rulings.md` read top to
bottom: **431 lines**, last heading **"2026-11-13 — D9S-3: a test-only
Arrow dependency of the edge build is refused too"** (l.421). No tier-1 disagreement found.

## Summary

On the server build, a new entry point, `fl_subscriber_subscribe_batches`, forwards to domain 7's
`SubscriberArrow::Subscribe(segments, RecordBatchCallback, BatchOptions, TopicOptions)` with the
options as given. A thunk then hands each window to the C caller as one `fl_batch`. The `fl_batch`
holds an Arrow C Data array and schema, borrowed for the call and kept only by Arrow's move. It also
holds the reason, the loss count and one `fl_attachments` per row. A window domain 7 could not build
arrives with no array and no schema. On the edge build and the probe shim, the same entry point
answers `FL_NOT_SUPPORTED` at the door, having done nothing. `pubsub-arrow` is required and linked
only under `variant=server`.

## Design

### The C surface (binding.h; ABI stays 0.8, D27: the minor was bumped once, in D9S-1)

```c
typedef enum fl_batch_reason {            /* domain 7's BatchStatus::Reason; 0 is never a reason */
    FL_BATCH_ROW_LIMIT = 1,               /* max_rows reached, or the offset budget (d7 L4)     */
    FL_BATCH_TIMEOUT   = 2,               /* the window's deadline passed (d7 L5)               */
    FL_BATCH_CLOSING   = 3,               /* the subscription is closing (d7 L9)                */
} fl_batch_reason;

typedef struct fl_batch {                 /* written by the shim, read through a pointer        */
    fl_batch_reason reason;
    int64_t rows_dropped;                 /* d7 BatchStatus::rows_dropped, as given             */
    int64_t num_rows;                     /* rows in `array`; 0 when there is no array          */
    struct ArrowArray*  array;            /* NULL: no batch could be built (d7 L7)              */
    struct ArrowSchema* schema;           /* NULL exactly when array is NULL                    */
    const fl_attachments* const* attachments; /* num_rows entries, none NULL; NULL if 0 rows    */
} fl_batch;

typedef void (*fl_batch_fn)(void* ctx, const fl_batch* batch);

FL_ABI_EXPORT fl_status fl_subscriber_subscribe_batches(
    fl_subscriber* subscriber, fl_topic topic, int64_t max_rows, int64_t timeout_ms,
    const fl_topic_options* options, fl_batch_fn on_batch, void* ctx,
    uint64_t* out_id, fl_schema_arrival** out_arrival, fl_error* err);
```

The header comments state, in the header's own words:
- **Ownership (L14, D26, BC-13).** Everything `fl_batch` points to is borrowed for the call. A
  binding keeps the batch only by Arrow's move: it copies each struct and sets the source's
  `release` to NULL. Whatever still has a non-NULL `release` when the callback returns, the shim
  releases then, exactly once. An attachment's blob is kept by `fl_blob_retain`, as on the row path.
- **Threads (L15, d7 L12, D10).** `on_batch` runs on domain 7's batch thread for this subscription,
  one batch at a time, in window order, never inside `Publish`. A batch may arrive before the
  subscribe returns. The callback must not throw (as `fl_delivery_fn`).
- **Options (L15, BC-8, BC-14, Q7).** `max_rows` and `timeout_ms` go to domain 7 as given;
  `INT64_MAX` ms is `kNoTimeout`. An out-of-range value is domain 7's `FL_INVALID_ARGUMENT`, with its
  message, before anything is subscribed. The shim checks neither value.
- **Build (L12, D28, Q12).** On the edge build: `FL_NOT_SUPPORTED`, whatever the arguments, origin
  `FL_ORIGIN_SEAM`, with a message naming the edge build and the server build's file
  `fletcher-c-abi-server`. OUT parameters are untouched and the callback is never stored.
- Otherwise the row subscribe's rules: NULL `options` is all-empty, outputs are written only on
  success, and the caller disposes the arrival.

**Q6 resolved (no new seam vocabulary):** the reason gets a C spelling, as each seam value does
(d9 §1), and loss crosses as `int64_t`. "No batch could be built" is `array == schema == NULL`, a
different case from a zero-row batch (`array != NULL`, `num_rows == 0`; d7 L6, L7). Attachments
reuse `fl_attachments`, one per batch row, row-aligned (d7 L3). A row with none gets an empty set,
never a NULL entry, so a binding keeps the one reader it has for the row path.

**Q7 resolved:** `int64_t` for both options. `std::chrono::milliseconds(timeout_ms)` is exact
(64-bit rep on MSVC and libstdc++), so `INT64_MAX` is `milliseconds::max()` = `kNoTimeout`,
untranslated. The precedent is `fl_schema_arrival_wait` (`binding.cpp:941-959`).

**Q12, edge-refusal half, resolved:** `FL_ORIGIN_SEAM`, the origin every refusal the shim makes
itself already carries (null arguments, `RequireOut`, `RequireRowsMatchTopic`). Bindings switch on
origin only for `(FL_INVALID_ARGUMENT, FL_ORIGIN_CODEC)` (D-BIND-54), so no new enumerator is
needed. The `FL_ORIGIN_SEAM` comment widens to "…or the shim's own refusal of an argument or of a
call this build cannot serve". The hand-over half of Q12 belongs to D9S-6.

**L18's field is designed here and lands in D9S-6,** which appends `const fl_error* failure` to
`fl_batch` (NULL unless the hand-over failed). Appending it with its producer means no field ever
exists without one. The shim writes the struct and a binding reads it through a pointer, so the
append changes no signature (L6, within the one 0.8 bump).

### One door, two backends (BC-10: the refusal reads the one constant)

- **The entry point lives in `binding.cpp`** (shared by all three artefacts), as one
  `Contain(err, FL_ORIGIN_SEAM, …)`:
  1. First statement: `if (fl_binding_variant() != FL_VARIANT_SERVER) throw
     PubSubError(kNotSupported, …)`. The refusal asks BC-10's single site; there is no `#if`.
  2. `RequireOut` on both outputs, then a null `subscriber` or `on_batch` → `kInvalidArgument`, as
     the row subscribe does.
  3. Allocate the arrival **before** the subscription goes live (the row path's ordering,
     `binding.cpp:825-832`), so only noexcept steps follow `Subscribe`.
  4. `internal::SubscribeBatches(*subscriber, segments, max_rows, timeout_ms, topic options,
     on_batch, ctx)` returns `{id, arrival}`; write the outputs.
- **`internal::SubscribeBatches`** is declared in `handles.hpp` and defined in two backend TUs.
  `c-abi/CMakeLists.txt` picks which one in the **same `if (FLETCHER_SHIM_VARIANT …)` block** that
  already sets the per-target constant (`CMakeLists.txt:124-129`):
  - `src/batches_server.cpp` (server only). It gets or creates the subscriber's batch host, a
    `SubscriberArrow` over the subscriber's provider, created lazily under `batches_mu`. It then
    calls `SubscriberArrow::Subscribe(segments, thunk, BatchOptions{max_rows,
    milliseconds(timeout_ms)}, topic_options)` and returns `{subscription_id, schema}`. The lock
    covers only the get-or-create, never the `Subscribe` call or a callback (L11).
  - `src/batches_edge.cpp` (edge, and the probe target). The entry point refuses before it can
    call this, so it is unreachable. It throws `kInternal` ("this artefact was built without the
    batch assembler"), which reports a mis-build and never answers whether this build can batch.
  - **Each backend `static_assert`s the per-target constant** (`FLETCHER_C_ABI_VARIANT ==
    FL_VARIANT_SERVER` or `== FL_VARIANT_EDGE`). A backend linked into the wrong artefact therefore
    does not compile. The assert checks the build and decides no behaviour.
- **`fl_subscriber` gains `std::mutex batches_mu; std::shared_ptr<void> batches;`,** declared
  after `subscriber`, so `~SubscriberArrow` runs first (domain 7 closes every batched subscription,
  L9), then the row `Subscriber`, then the provider. The type is erased, so the edge build's objects
  name no domain-7 type. `batches` stays empty unless the new entry point is called, so a row-only
  caller sees exactly what it saw before (L13).

### The thunk (batches_server.cpp)

It captures only `(on_batch, ctx)`, never the `fl_subscriber`. A destroy made inside the callback
(rung 6) therefore leaves the remaining deliveries nothing to dangle on. Per delivery:
1. Build one `fl_attachments` view per row from domain 7's row-aligned vector (moved in), plus the
   array of pointers to them. This step may throw, and nothing is exported yet.
2. Declare a local `ArrowArray` and `ArrowSchema`, zero-initialised, and give them a scope guard
   that calls each `release` that is still non-NULL on every exit. **This guard is L14's rule** and
   the one site of BC-13's batch half. It needs no "moved" flag: the C Data `release` pointer is the
   only state.
3. If the batch is non-null, call `arrow::ExportRecordBatch(*batch, &array, &schema)`. A non-OK
   status is thrown as `PubSubError(kInternal, status message)`. D9S-6 replaces this throw with the
   L18 delivery (BC-12's one site).
4. Fill `fl_batch` (reason by `switch` over the three enumerators; `rows_dropped`; `num_rows =
   batch ? batch->num_rows() : 0`; the pointers, or NULLs), then call `on_batch(ctx, &b)`.
5. Return. The guard releases whatever the binding did not move, and the views die.

There is no window, count, timer, clock, queue or thread anywhere (BC-9, L15). Order and thread come
from domain 7's single batch thread (d7 L12).

### Build (recipe and CMake)

- **`conanfile.py`:** `if self.options.variant == "server": self.requires("fletcher-pubsub-arrow/0.5.1-alpha")`.
  The `zlib/1.3.1` override, already required under `run_tests`, is shared when either option is on.
  `"arrow/*:shared": False` goes into `default_options`, so the server shim stays one file per RID,
  as the recipe's docstring promises. The edge graph is unchanged (BC-11; `check_edge_arrow_free.sh`
  stays green with no edit). The comment at `conanfile.py:97-102` narrows to "the edge shim".
- **`CMakeLists.txt`:** in the server branch, `find_package(fletcher-pubsub-arrow)`,
  `target_sources(… src/batches_server.cpp)` and a PRIVATE link to `pubsub-arrow`. In the edge
  branch, `src/batches_edge.cpp`. The probe target lists `src/batches_edge.cpp`. Visibility, the
  version script and `--exclude-libs ALL` already hide Arrow's symbols (L7).
- **CI:** `ci.c-abi.yml` gains `pubsub-arrow` in both legs' sparse checkout, and a `conan create
  ../pubsub-arrow` after `arrow-bridge`. `ci.pr.yml`'s c-abi filter gains `pubsub-arrow/**`, because
  a change there changes the server shim.

## Corner cases forbidden

Rung 1 (unrepresentable):
- **Arrow in the edge build:** the server backend and the `pubsub-arrow` link exist only in CMake's
  server branch and the recipe's `variant=server` requirement (check: `check_edge_arrow_free.sh`).
- **A mis-built artefact** (backend and constant disagree): `static_assert`, does not compile.
- **The edge build half-serving a call:** the refusal is the first statement, before any argument
  is read, so nothing is allocated, stored or written. The answer cannot depend on the arguments,
  so BC-10's truth table (query = server ⇔ not `FL_NOT_SUPPORTED`) holds for every call.
- **An `fl_schema` handed to a batch callback:** `fl_batch` has no such field (L14, BC-13).
- **A moved batch released, or an unmoved one kept:** the struct's own `release` pointer decides
  after the callback. There is no flag, so no second state can disagree with it.
- **The shim answering options (BC-8) or batching (BC-9):** nothing reads `max_rows`/`timeout_ms`
  except to build `BatchOptions`. There is no thread, timer, clock or queue (scan guard below).
- **An id in the callback that may not be known yet:** a batch can arrive before `Subscribe`
  returns, so the callback gets no id; a binding correlates by `ctx`.
- **A live subscription whose id never reaches the caller:** the arrival is allocated first, and
  only noexcept steps follow `Subscribe`.
- **A batched subscription outliving its subscriber unseen:** the handle owns the host, and
  destruction closes it through domain 7 (L9; the closing semantics are rung 6's).
- **Attachments misaligned with rows, or a NULL entry:** one view per row of domain 7's aligned
  vector (d7 L3), with an empty set for a row with none.

Rung 2 (refused, typed, at the door): on the server build, a NULL `subscriber`, `on_batch`, `out_id`
or `out_arrival` → `FL_INVALID_ARGUMENT`. On any non-server build → `FL_NOT_SUPPORTED`.

Handled residue:
- **H1. Two numberings on one subscriber.** Row ids and batched ids both count from 1 (one counter
  per `Subscriber`, `pubsub/src/subscriber.cpp:267`). Stage 5's close (below) acts only on batched
  ids, so a number given to the wrong close acts on the other kind. *Why not forbidden:* a separate
  batch-subscriber handle costs four or more entry points (Brief Decision 1), and tagging ids would
  change `fl_subscriber_unsubscribe`'s answer, an edge-surface change (L13; stop-and-ask).
- **H2. A refused subscribe leaves the host behind.** A batched subscribe refused for its options
  leaves the subscriber's batch host created, with no provider subscription. *Why not forbidden:*
  only domain 7 answers the options (BC-8), and it is asked through its host. The host is
  invisible, and the next batched subscribe reuses it.
- **H3. Until D9S-6, a failed export is lost.** It throws into domain 7, which swallows it
  uncounted (d7 L10). *Why not forbidden here:* L18's catch is rung 8's, red first with its
  test-only seam. This is a coexistence window, closed by D9S-6 (Brief Decision 2).

## Premises and stop conditions

- **P1.** `milliseconds::rep` is signed 64-bit on both toolchains, so `INT64_MAX` is `kNoTimeout`.
  *If false:* STOP-AND-ASK. Do not add a translation.
- **P2.** Domain 7 refuses out-of-range options with `PubSubError(kInvalidArgument)` (d9 L15).
  *If another type* (it would become `FL_INTERNAL`): STOP-AND-ASK. Never translate in the shim.
- **P3.** `inprocess`, in its default `kAsDeclared` mode, delivers raw rows on an undeclared topic
  with a null schema, so domain 7 has no decoder and sends a null batch
  (`in_process_provider.cpp:30-39`). *If false:* use a topic whose schema domain 2 rejects (a nested
  dictionary, d7 L8's test shape). That is still "a window domain 7 could not build". Stop only if
  neither gives one.
- **P4.** Arrow and `pubsub-arrow` link statically into the server shim with their symbols hidden.
  On Linux this comes from `--exclude-libs ALL` and the version script. On Windows, static Arrow
  carries no `dllexport`. *If* `check_shim_exports.sh` finds a non-`fl_` export on Linux, or the
  Windows `fl_` set differs: STOP-AND-ASK. Do not widen the export map.
- **P5.** A second `Subscriber` on one provider (inside `SubscriberArrow`) is lawful; two
  `fl_subscriber`s on one provider already exist. *If* a provider refuses: STOP-AND-ASK (domain 3).
- **P6.** `~SubscriberArrow` from outside a callback closes and returns (d7 L9), so the tests tear
  down by `fl_subscriber_destroy`. *If it hangs:* stop; that is rung 6's subject, not a workaround.
- **P7.** Conan resolves `fletcher-pubsub-arrow` in the c-abi graph with the shared zlib override.
  *If* it needs more: stop and report. Do not pin other packages blind.

## Forcing-test mapping

The tests are in `c-abi/tests/test_batches.cpp` (new). It is compiled into `c_abi_tests`, which is
built in the edge and the server package, and into `c_abi_probe_tests`. The expectation comes from
`FLETCHER_EXPECTED_VARIANT`, never from the shim. Tables use int64 and utf8 columns only: no
dictionary column, because d9 Q11 (re-fold vs value type) is open (audit note). Every rung test
calls only `fl_*` and imports with the test binary's own Arrow.

| Test | Turns green by | Red before, for the right reason |
|---|---|---|
| `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` (server binary only) | Declare a topic and subscribe for batches (`max_rows` 2, a finite timeout of at least 1 s, so a descheduled publisher cannot split a row-limit window). Publish 5 rows in one `fl_publisher_publish_rows`, rows 1 and 3 with attachments. Wait one-sided (at most 10 s) for 3 deliveries. Assert ROW_LIMIT, ROW_LIMIT, TIMEOUT and `rows_dropped` 0. `arrow::ImportRecordBatch` of each (a move) equals the published values cell for cell. Attachments match row by row (keys and bytes). Every callback runs on one thread, not the test's own. The first `seq` strictly increases. | Does not compile: no `fl_subscriber_subscribe_batches`, no `fl_batch`. |
| `Batches.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted` (server only) | `fl_publisher_publish_raw` 3 rows on an undeclared topic (P3), then subscribe for batches with a finite timeout. Every delivery has `array == schema == NULL`, `num_rows == 0` and `attachments == NULL`, and Σ`rows_dropped` == 3 (one-sided wait). | Does not compile. |
| `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` (all three artefacts: BC-10's truth table, guard (e)) | One body: subscribe (10 ms timeout), publish one row, then observe for 2 s. **Edge and probe:** `FL_NOT_SUPPORTED`, origin `FL_ORIGIN_SEAM`, a message naming "edge" and `fletcher-c-abi-server`. The sentinel outputs are unchanged and nothing is delivered in the 2 s. A NULL subscriber is also `FL_NOT_SUPPORTED`. **Server (the control):** `FL_OK` and one TIMEOUT delivery inside the same 2 s. This shows the window can see a delivery, and that a refusal reading anything but the build turns an arm red. | Does not compile. Against a refusal-less edge, the edge arm sees `FL_OK`. |

**Guards (re-run, must stay green):**
- (a) `check_shim_exports.sh`: edge = server = header, so both builds export the new declaration.
- (c) `check_edge_arrow_free.sh`: no Arrow in the edge graph or symbols, now that the server links
  it. Both controls are unchanged.
- (d) `c99_probe.c` compiles the new declarations as C99.
- (b) BC-9 scan, **new**: `ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer`
  (`test_shim_batches_nothing.cpp`, `c_abi_tests` only). It reads every file under `c-abi/src`
  (path passed by CMake as a forward-slash literal) and fails on `std::thread`, `jthread`,
  `condition_variable`, `_clock`, `sleep_for`, `sleep_until` or `std::async`. Its control runs the
  same matcher over its own source, which spells each pattern, and requires every pattern to hit.

There are no legs: no test reaches `SubscriberArrow` or a backend directly. Rung 5 may arrive green,
because thunk step 2 is already exact; stage 5 shows it red or declares it a guard (ladder).

## Risks / Unknowns

- **Public surface is 4, one over the budget of 3.** Counted: one entry point
  (`fl_subscriber_subscribe_batches`) and three types (`fl_batch_fn`, `fl_batch`,
  `fl_batch_reason`). Nothing in scope can be retired: Q9's `fl_decode_rows_framed` is domain 10's.
  The cut that would fit 3 is `int32_t reason` with `#define` values: the same vocabulary, less type
  safety, against d9 §1's "C spelling of each seam value". I recommend against it. **Reviewer to
  rule.** Stage 5 adds a fifth item, the close.
- **Stage 5's close is designed here and not declared** (L6):
  `fl_status fl_subscriber_unsubscribe_batches(fl_subscriber*, uint64_t id, fl_error*)`. It forwards
  to `SubscriberArrow::Unsubscribe` behind the same door, the edge build refuses it the same way, and
  with no host it is a no-op, as an unknown row id is. It changes no signature.
- **H3, the L18 window from D9S-4 to D9S-6** (Brief Decision 2). This is a plan-shape fact, not a
  merge request: merging would breach the ceiling and lose rung 8's red-first proof.
- **Size.** About 209 counted production lines against the 250 ceiling. If the implementation
  passes 250, take the ladder's 4a/4b split: attachments (the field and its assertions) are
  appended in 4b. The edge refusal stays with the entry point.
- **Timing.** The server tests wait one-sided. The timeout of at least 1 s keeps back-to-back
  row-limit windows whole. A badly loaded runner could still split one, and then only the reasons
  assertion flakes, never conservation.
- **Windows server DLL.** Static Arrow makes it larger; its size is printed, not enforced (D22).
  Any transitive DLL imports of Arrow are D-BIND-148's question for domain 10.
- **Q11.** A batched dictionary column arrives re-folded. The tests avoid dictionary columns, and
  the header says columns arrive "as domain 7 assembled them". Q11 stays open (domains 2 and 10).
- **No STOP-AND-ASK is open.** Arrow stays out of edge. No edge entry point changes its answer: only
  the new one refuses. No status number is added, nothing changes under `dotnet/`, and no code in
  domain 7 or domain 2 changes. Nothing in the shim checks a window, count, timer, range or thread.

**Declared lines (raw diff):** production about **+262 / −8**, of which about 45 are `binding.h`
doc comments (listed, not counted, by the ladder), so the counted net is about **+209**. Per file:
`binding.h` about 27 code and 45 comment, `binding.cpp` +32, `handles.hpp` +16,
`batches_server.cpp` +95, `batches_edge.cpp` +18, `CMakeLists.txt` +12/−3, `conanfile.py` +10/−4,
`ci.c-abi.yml` +6, `ci.pr.yml` +1. Tests about **+330**; docs about **+60 / −25**. Total raw about
**+650 / −35**. No 4a/4b split.

## Files-to-touch

- **Production:** `c-abi/include/fletcher/abi/binding.h`, `c-abi/src/binding.cpp`,
  `c-abi/src/handles.hpp`, `c-abi/src/batches_server.cpp` (new), `c-abi/src/batches_edge.cpp` (new),
  `c-abi/CMakeLists.txt`, `c-abi/conanfile.py`, `.github/workflows/ci.c-abi.yml`,
  `.github/workflows/ci.pr.yml`.
- **Tests:** `c-abi/tests/test_batches.cpp` (new), `c-abi/tests/test_shim_batches_nothing.cpp`
  (new), `c-abi/tests/CMakeLists.txt` (both files, the source-dir literal; the comment at 6-15
  narrows to the edge shim).
- **Docs, not counted:** `c-abi/README.md` ("Consuming": batched subscribe, server build only). In
  d9: L12's refusals, L14 and L15 `[built]` for the crossing (Windows by hand until CI), §4's server
  rows, and Q6, Q7 and Q12's edge half resolved. In the register: BC-9's shim scan, BC-10's
  refusals and BC-13's batch half `[built]` (rung 5 pending); BC-8 and BC-14 "the shim forwards"
  `[built]`, their grid test owed to D9S-5.

## Files-to-delete

None. Nothing in the tree serves batches through the shim, so there is no predecessor to retire.
What this stage makes redundant, C#'s managed batcher (BC-9's copy), is deleted by the C# switch
round (domain 10, D-D9S-8, out of scope). Stale text is edited, not deleted: `conanfile.py:97-102`
and `tests/CMakeLists.txt:8-13` ("the shipped shim must not link Arrow") narrow to the edge shim,
and the `FL_ORIGIN_SEAM` comment widens (Q12).
