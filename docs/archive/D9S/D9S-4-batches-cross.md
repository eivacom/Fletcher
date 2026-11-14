# D9S-4 — Batches cross the shim (design)

Round D9S, item D9S-4, ladder rung 4 (`plans/D9S-test-ladder.md`). Tree `8854773`. Full loop.
Authority read: d9 §1, §3 (L1-L4, L8, L11-L18), §4, §5, §7 (Q6, Q7, Q11, Q12); d7 §3 (L1, L3-L7, L9,
L12, L15), §4, §6; `subscriber_arrow.hpp`; register BC-8 to BC-15; `plans/D9S-locked-decisions.md`;
`plans/reviews/D9S-4-audit-0a.md` (CLEAR). **Rulings ledger:** `plans/RIX-rulings.md` read top to
bottom: **431 lines**, last heading **"2026-11-13 — D9S-3: a test-only Arrow dependency of the edge
build is refused too"** (l.421). No tier-1 disagreement. **Cycle 2** answers review B1-B3
(`plans/reviews/D9S-4-design-review.md`). DEBT-1 and DEBT-2 stand for the implementer.

## Summary

On the server build, `fl_subscriber_subscribe_batches` forwards to domain 7's
`SubscriberArrow::Subscribe(segments, RecordBatchCallback, BatchOptions, TopicOptions)` with the
options as given, and returns an id tagged as batched. A thunk hands each window to the caller as
one `fl_batch`. It holds an Arrow C Data array and schema, borrowed for the call and kept only by
Arrow's move, plus the reason and the loss count; 4b adds one `fl_attachments` per row. A window
domain 7 could not build arrives with no array and no schema. The edge build and the probe shim
answer `FL_NOT_SUPPORTED` at the door. `pubsub-arrow` is linked only under `variant=server`.
**Split now:** 4a carries everything except attachments, and 4b appends them (Risks).

## Design

### The C surface (binding.h; ABI stays 0.8, D27: the minor was bumped once, in D9S-1)

```c
/* domain 7's BatchStatus::Reason, as fl_batch.reason values; 0 is never a reason (review B2) */
#define FL_BATCH_ROW_LIMIT 1              /* max_rows reached, or the offset budget (d7 L4)     */
#define FL_BATCH_TIMEOUT   2              /* the window's deadline passed (d7 L5)               */
#define FL_BATCH_CLOSING   3              /* the subscription is closing (d7 L9)                */

typedef struct fl_batch {                 /* written by the shim, read through a pointer        */
    int32_t reason;                       /* an FL_BATCH_* value, int32_t as fl_error's fields  */
    int64_t rows_dropped;                 /* d7 BatchStatus::rows_dropped, as given             */
    int64_t num_rows;                     /* rows in `array`; 0 when there is no array          */
    struct ArrowArray*  array;            /* NULL: no batch could be built (d7 L7)              */
    struct ArrowSchema* schema;           /* NULL exactly when array is NULL                    */
    /* 4b appends: const fl_attachments* const* attachments; (num_rows entries, none NULL)     */
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
  releases then, exactly once. (4b: an attachment's blob is kept by `fl_blob_retain`, as on the row path.)
- **Ids (B1).** A batched id has bit 63 set and a row id never does: a counter starting at 1 would
  need 2^63 subscriptions to reach it. Stage 5's close refuses an untagged id.
  `fl_subscriber_unsubscribe` is unchanged; it treats a tagged id as unknown, a no-op (L13).
- **Same topic, one provider (seam §7 clause 4: one callback per topic per instance).** Batched
  subscriptions run on a second `Subscriber`, so a row subscription and a batched one to the same
  topic through one provider collide, as two subscriber handles do today. On `inprocess` the later
  silently replaces the earlier: the earlier stops receiving, and its arrival answers
  `FL_SUBSCRIPTION_ENDED`. On `fastdds` the later is refused with `FL_INVALID_ARGUMENT` ("already
  subscribed"). Batched subscriptions to one topic share one provider subscription, as row ones do.
- **Threads (L15, d7 L12).** Domain 7's batch thread, one batch at a time, in window order. A batch
  may arrive before the subscribe returns, and the callback must not throw.
- **Options (L15, BC-8, BC-14).** Passed as given, with `INT64_MAX` = `kNoTimeout`. An out-of-range
  value gets domain 7's `FL_INVALID_ARGUMENT` and message; the shim checks neither.
- **Build (L12, D28).** The edge build answers `FL_NOT_SUPPORTED` whatever the arguments, with
  `FL_ORIGIN_SEAM` and a message naming the edge build and `fletcher-c-abi-server`. Its OUTs are
  untouched and nothing is stored. Everything else follows the row subscribe's rules.

**Q6 resolved (no new seam vocabulary).** The reason is named `int32_t` values (d9 §1), as
`fl_error` stores its enums (`binding.h:319-324`; C leaves an enum's width open), and loss is
`int64_t`. "No batch could be built" is `array == schema == NULL`, which differs from a zero-row
batch (`array != NULL`, `num_rows == 0`; d7 L6, L7). In 4b, attachments reuse `fl_attachments`:
one per row, aligned with the rows (d7 L3), with an empty set rather than a NULL entry.
**Q7 resolved:** both options are `int64_t`. `milliseconds(timeout_ms)` is exact (64-bit rep), so
`INT64_MAX` is `kNoTimeout`, untranslated, as in `fl_schema_arrival_wait` (`binding.cpp:941-959`).
**Q12, edge-refusal half, resolved:** the refusal carries `FL_ORIGIN_SEAM`, as every refusal the
shim makes itself does. D-BIND-54 switches only on `(INVALID_ARGUMENT, CODEC)`. The comment on
`FL_ORIGIN_SEAM` widens to "…or the shim's own refusal of an argument or of a call this build
cannot serve". The hand-over half belongs to D9S-6.
**L18's field is designed here and lands in D9S-6.** D9S-6 appends `const fl_error* failure` (NULL
unless the hand-over failed) with its producer. The shim writes the struct and a binding reads it
through a pointer, so appending changes no signature (L6, within the 0.8 bump).

### One door, two backends (BC-10: the refusal reads the one constant)

- **The entry point is in `binding.cpp`** (all three artefacts), one `Contain(err, FL_ORIGIN_SEAM, …)`:
  1. First statement: `if (fl_binding_variant() != FL_VARIANT_SERVER) throw kNotSupported`. It reads
     BC-10's single site; there is no `#if`.
  2. `RequireOut` on both outputs, and a null `subscriber` or `on_batch` → `kInvalidArgument`.
  3. The arrival is allocated before the subscription goes live (`binding.cpp:825-832`).
  4. `internal::SubscribeBatches(…)` returns `{id, arrival}`. Write the outputs with
     `*out_id = id | kBatchedIdTag` (bit 63; a file-local constant, reused by stage 5's close; B1).
- **`internal::SubscribeBatches`** is declared in `handles.hpp` and defined in two backend TUs.
  `c-abi/CMakeLists.txt` picks which one in the **same `if (FLETCHER_SHIM_VARIANT …)` block** that
  already sets the per-target constant (`CMakeLists.txt:124-129`):
  - `src/batches_server.cpp` (server only). It gets or creates the subscriber's batch host, a
    `SubscriberArrow` over the subscriber's provider, created lazily under `batches_mu`. It then
    calls `SubscriberArrow::Subscribe(segments, thunk, BatchOptions{max_rows,
    milliseconds(timeout_ms)}, topic_options)` and returns `{subscription_id, schema}`. The lock
    covers only the get-or-create, never the `Subscribe` call or a callback (L11).
  - `src/batches_edge.cpp` (edge and probe) is unreachable, because the door refuses first. It
    throws `kInternal` ("built without the batch assembler"): a mis-build report, never an answer
    to "can this build batch".
  - **Each backend `static_assert`s the per-target constant**, so a backend in the wrong artefact
    does not compile. The assert checks the build and decides no behaviour.
- **`fl_subscriber` gains `std::mutex batches_mu; std::shared_ptr<void> batches;`,** declared
  after `subscriber`, so `~SubscriberArrow` runs first (domain 7 closes every batched subscription,
  L9), then the row `Subscriber`, then the provider. The type is erased, so the edge build's objects
  name no domain-7 type. `batches` stays empty unless the new entry point is called, so a row-only
  caller sees exactly what it saw before (L13).

### The thunk (batches_server.cpp)

It captures only `(on_batch, ctx)`, never the `fl_subscriber`. A destroy made inside the callback
(rung 6) therefore leaves the remaining deliveries nothing to dangle on. Per delivery:
1. (4b) Build one `fl_attachments` view per row from domain 7's aligned vector, plus a pointer
   array. This may throw, and nothing is exported yet. In 4a the vector is dropped.
2. A local `ArrowArray` and `ArrowSchema`, zero-initialised, with a scope guard that calls each
   `release` still non-NULL on every exit. **This guard is L14's rule**, BC-13's one batch site. It
   needs no "moved" flag: the `release` pointer is the only state.
3. For a non-null batch, `arrow::ExportRecordBatch(*batch, &array, &schema)`. A non-OK status is
   thrown as `PubSubError(kInternal, …)` until D9S-6 replaces the throw (BC-12).
4. Fill `fl_batch`: the reason by a `switch` over domain 7's three values (DEBT-2),
   `rows_dropped`, `num_rows`, and the pointers or NULLs. Then call `on_batch(ctx, &b)`.
5. Return. The guard releases whatever was not moved.

There is no window, count, timer, clock, queue or thread (BC-9, L15). Order and thread are domain
7's (d7 L12).

### Build (recipe and CMake)

- **`c-abi/conanfile.py`:** `variant == "server"` requires `fletcher-pubsub-arrow/0.5.1-alpha`. The
  `zlib/1.3.1` override (today under `run_tests`) is shared by both, and `"arrow/*:shared": False`
  keeps one file per RID. The edge graph is unchanged (BC-11). The comment at `:97-102` narrows to
  the edge shim.
- **`CMakeLists.txt`:** the server branch gets `find_package(fletcher-pubsub-arrow)`,
  `batches_server.cpp` and a PRIVATE link. The edge branch and the probe target get
  `batches_edge.cpp`. Visibility, the version script and `--exclude-libs ALL` hide Arrow (L7).
- **CI:** `ci.c-abi.yml` adds `pubsub-arrow` to both legs' sparse checkout and `conan create`s it
  after `arrow-bridge`. `ci.pr.yml`'s c-abi filter gains `pubsub-arrow/**`.
- **PIC for a shared object (B3; owner ruling 2026-11-13, "Allow the build option": branch (a), a named exception to D-D9S-8).**
  `pubsub-arrow` and `arrow-bridge` are static packages without `fPIC`. Linking them into the Linux
  `.so` fails as `pubsub` once did (`pubsub/conanfile.py:21-33`, `R_X86_64_TPOFF32`; `pubsub-arrow`
  has a `thread_local`).
  - **(a)** Add `fPIC` to `pubsub-arrow/conanfile.py` and `arrow-bridge/conanfile.py` exactly as
    `pubsub` does: in `options` and `default_options` (True), deleted on Windows in
    `config_options()`, with a comment citing `pubsub`. About +8 / −2 each. These are recipe lines,
    not domain-7 or domain-2 code. Windows package IDs are unchanged; Linux IDs change and rebuild.
  - (b), a flag in the Linux profile, was not taken.

## Corner cases forbidden

Rung 1 (unrepresentable):
- **Arrow in the edge build:** only CMake's server branch and the recipe's `variant=server`
  requirement name it (`check_edge_arrow_free.sh`).
- **A mis-built artefact:** a `static_assert` means it does not compile.
- **The edge build half-serving a call:** the refusal comes before any argument is read, so nothing
  is allocated or written, and BC-10's truth table holds for every call.
- **An `fl_schema` in a batch callback:** `fl_batch` has no such field (L14, BC-13).
- **A moved batch released, or an unmoved one kept:** the `release` pointer decides; no flag.
- **The shim answering options (BC-8) or batching (BC-9):** the options are only used to build
  `BatchOptions`. There is no thread, timer, clock or queue (scan guard).
- **An id in the callback that may not be known yet:** none is given; a binding correlates by `ctx`.
- **A live subscription whose id never reaches the caller:** the arrival is allocated first, and
  only noexcept steps follow `Subscribe`.
- **A batched subscription outliving its subscriber unseen:** the handle owns the host, which is
  closed through domain 7 (L9; rung 6).
- **Attachments misaligned with rows, or a NULL entry (4b):** one view per aligned row (d7 L3).
- **A row id equal to a batched id (B1):** bit 63 is set on batched ids only. Stage 5's close
  refuses an untagged id, and the row unsubscribe sees a tagged id as unknown, a no-op (L13).

Rung 2 (refused, typed, at the door): on the server build, a NULL `subscriber`, `on_batch`, `out_id`
or `out_arrival` → `FL_INVALID_ARGUMENT`. On any non-server build → `FL_NOT_SUPPORTED`.

Handled residue:
- **H1. Same-topic collision.** A row and a batched subscription to one topic through one provider
  collide: on `inprocess` the later replaces the earlier silently, and on `fastdds` the later is
  refused. *Why not forbidden:* "one callback per topic per instance" is domain 3's rule, enforced
  differently per provider for any two subscribers. A shim refusal would be a third L4 exception
  with per-topic bookkeeping, so it is offered as owner option (c) of Brief Decision 1. The split
  is registered as a d3/d4 finding.
- **H2. A refused subscribe leaves the host behind.** A subscribe refused for its options leaves an
  empty batch host. *Why not forbidden:* only domain 7 answers options (BC-8), asked through the
  host; the host is invisible and reused.
- **H3. Until D9S-6, a failed hand-over is lost.** It throws into domain 7, which swallows it
  uncounted (d7 L10; DEBT-1). *Why not forbidden here:* L18's catch is rung 8's, red first with its
  test-only seam. It is a coexistence window, closed by D9S-6 (Brief Decision 2).

## Premises and stop conditions

- **P1.** `milliseconds::rep` is signed 64-bit. *If not:* STOP-AND-ASK; add no translation.
- **P2.** Domain 7 refuses with `PubSubError(kInvalidArgument)`. *If not:* STOP-AND-ASK; no translation.
- **P3.** `inprocess` (`kAsDeclared`) delivers raw rows on an undeclared topic with a null schema,
  so domain 7 sends a null batch (`in_process_provider.cpp:30-39`). *If false:* use a schema domain
  2 rejects (a nested dictionary). Stop only if neither works.
- **P4.** Arrow links statically with its symbols hidden (`--exclude-libs` and the version script;
  no `dllexport` in static Arrow). *If* the export check finds a non-`fl_` Linux export, or the
  Windows `fl_` set differs: STOP-AND-ASK. Do not widen the export map.
- **P8 (B3).** With PIC (branch (a)), the Linux server `.so` links. *If not:* stop and report.
- **P5 (narrowed, B1).** A second `Subscriber` on one provider is lawful **for distinct topics**;
  for the same topic it collides (H1). *If* a distinct topic is refused: STOP-AND-ASK (domain 3).
- **P6.** `~SubscriberArrow` from outside a callback closes and returns (d7 L9). *If it hangs:* stop.
- **P7.** Conan resolves `pubsub-arrow` with the shared zlib override. *If not:* stop and report.

## Forcing-test mapping

The tests are in `c-abi/tests/test_batches.cpp` (new), compiled into `c_abi_tests` (edge and server
packages) and `c_abi_probe_tests`. The expectation comes from `FLETCHER_EXPECTED_VARIANT`, never
from the shim. Columns are int64 and utf8 only: Q11 is open, so no dictionary columns. Tests call
only `fl_*` and import with the test binary's own Arrow.

| Test | Turns green by | Red before, for the right reason |
|---|---|---|
| `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` (server binary only; 4a) | Declare a topic and subscribe for batches (`max_rows` 2, a finite timeout of at least 1 s, so a descheduled publisher cannot split a row-limit window). Publish 5 rows in one `fl_publisher_publish_rows`. Wait one-sided (at most 10 s) for 3 deliveries. Assert ROW_LIMIT, ROW_LIMIT, TIMEOUT and `rows_dropped` 0. `arrow::ImportRecordBatch` of each (a move) equals the published values cell for cell. Every callback runs on one thread, not the test's own. The first `seq` strictly increases. `out_id` has bit 63 set (B1). | Does not compile: no `fl_subscriber_subscribe_batches`, no `fl_batch`. |
| `Batches.EachBatchRowCarriesItsOwnAttachments` (server only; 4b) | The same 5 rows, rows 1 and 3 with attachments. Every delivery's attachment sets match the published rows by position (keys and bytes), with an empty set for a row without any. | Does not compile until 4b appends `attachments`. |
| `Batches.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted` (server only) | Subscribe for batches on an undeclared topic with a finite timeout, then `fl_publisher_publish_raw` 3 rows to it (P3; `inprocess` keeps nothing for a topic with no subscription). Every delivery has `array == schema == NULL` and `num_rows == 0`, and Σ`rows_dropped` == 3 (one-sided wait). | Does not compile. |
| `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` (all three artefacts: BC-10's truth table, guard (e)) | One body: subscribe (10 ms timeout), publish one row, then observe for 2 s. **Edge and probe:** `FL_NOT_SUPPORTED`, origin `FL_ORIGIN_SEAM`, a message naming "edge" and `fletcher-c-abi-server`. The sentinel outputs are unchanged and nothing is delivered in the 2 s. A NULL subscriber is also `FL_NOT_SUPPORTED`. **Server (the control):** `FL_OK` and one TIMEOUT delivery inside the same 2 s. This shows the window can see a delivery, and that a refusal reading anything but the build turns an arm red. | Does not compile. Against a refusal-less edge, the edge arm sees `FL_OK`. |

**Guards (re-run, must stay green):**
- (a) `check_shim_exports.sh`: edge = server = header.
- (c) `check_edge_arrow_free.sh`: unchanged, controls included.
- (d) `c99_probe.c`.
- (b) **New:** `ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer` (`c_abi_tests` only). It scans
  `c-abi/src` (path passed as a forward-slash literal) for `std::thread`, `jthread`,
  `condition_variable`, `_clock`, `sleep_for`, `sleep_until` and `std::async`. Its control runs the
  same matcher over its own source and requires every pattern to hit.

There are no legs. Rung 5 may arrive green, because thunk step 2 is exact; stage 5 shows it red or
declares it a guard.

## Risks / Unknowns

- **Public surface is 3 (B2):** `fl_subscriber_subscribe_batches`, `fl_batch` and `fl_batch_fn`.
  The `FL_BATCH_*` values are constants, not a type. Stage 5 adds the close.
- **Stage 5's close is designed here and not declared (L6):**
  `fl_subscriber_unsubscribe_batches(fl_subscriber*, uint64_t id, fl_error*)`. It sits behind the
  same door, with the same edge refusal. An untagged id gets `FL_INVALID_ARGUMENT` ("not a batched
  subscription id"). A tagged id has its bit cleared and goes to `SubscriberArrow::Unsubscribe`;
  with no host it is a no-op.
- **H3, the L18 window to D9S-6** (Decision 2; DEBT-1: the catch encloses thunk steps 1-4).
- **4a/4b split, taken now.** The counted figure is about 254 after B1-B3(a), over 250.
  - 4b appends `attachments` to `fl_batch`, adds thunk step 1, and adds
    `EachBatchRowCarriesItsOwnAttachments`: about +20 production lines in 2 files, so it fits the
    light lane.
  - The edge refusal stays with the entry point in 4a (L12).
  - **Disclosed:** in 4a, attachments are not delivered. Nothing ships between the two stages.
- **B3 ruled:** branch (a) (owner, 2026-11-13, "Allow the build option").
- **Timing.** The waits are one-sided, and a timeout of at least 1 s keeps the row-limit windows
  whole. On a loaded runner only the reason assertion can flake, never conservation.
- **Windows server DLL.** Static Arrow makes it larger; the size is printed, not enforced (D22).
  Arrow's DLL imports are D-BIND-148's question (domain 10).
- **Q11.** Batched dictionary columns arrive re-folded. The tests avoid them; Q11 stays open.
- **No STOP-AND-ASK is open.** B3 is ruled (branch (a)).
  - Arrow stays out of the edge build, and only the new entry point refuses.
  - No new status number, no change under `dotnet/`, and no domain-7 or domain-2 code.
  - Nothing in the shim checks a window, count, timer or range, or owns a thread.

**Declared lines (raw diff), after B1-B3(a):**
- **Unsplit:** production about +318 / −12. About 52 of those lines are `binding.h` doc comments,
  which the ladder does not count, so the counted figure is about **254**, over 250.
- **4a:** production about **+298 / −12**, counted about **236**.
  - `binding.h`: about 22 code and 50 comment.
  - `binding.cpp` +42, `handles.hpp` +16, `batches_server.cpp` +108, `batches_edge.cpp` +18.
  - `CMakeLists.txt` +12/−3, `c-abi/conanfile.py` +10/−4, two recipes +8/−2 each, `ci.c-abi.yml`
    +6, `ci.pr.yml` +1.
  - Tests about +270, docs about +65/−25. Raw total about **+635 / −40**.
- **4b:** production about +20 (counted about 17), tests about +60.

## Files-to-touch

- **Production:** in `c-abi/`: `include/fletcher/abi/binding.h`, `src/binding.cpp`,
  `src/handles.hpp`, `src/batches_server.cpp` (new), `src/batches_edge.cpp` (new), `CMakeLists.txt`
  and `conanfile.py`. Also `.github/workflows/ci.c-abi.yml` and `ci.pr.yml`, and, for B3(a),
  `pubsub-arrow/conanfile.py` and `arrow-bridge/conanfile.py` (the ruled exception, `fPIC` only).
  4b touches only `binding.h` and `batches_server.cpp`.
- **Tests:** `c-abi/tests/test_batches.cpp` (new), `c-abi/tests/test_shim_batches_nothing.cpp`
  (new), `c-abi/tests/CMakeLists.txt` (both files, the source-dir literal; the comment at 6-15
  narrows to the edge shim).
- **Docs, not counted:**
  - `c-abi/README.md`: "Consuming" covers the batched subscribe.
  - d9: L12's refusals, L14 and L15 marked `[built]`; §4; Q6, Q7 and Q12's edge half resolved.
  - Register:
    - BC-9's scan, BC-10's refusals and BC-13's batch half `[built]`.
    - BC-8 and BC-14: the shim forwards; their grid test is owed to D9S-5.
    - H1's provider split recorded as a d3/d4 finding.

## Files-to-delete

None. Nothing in the tree serves batches through the shim. The C# managed batcher this makes
redundant is deleted by the C# switch round (D-D9S-8). Stale text is edited:
- `conanfile.py:97-102` and `tests/CMakeLists.txt:8-13` narrow to the edge shim.
- The `FL_ORIGIN_SEAM` comment widens.
