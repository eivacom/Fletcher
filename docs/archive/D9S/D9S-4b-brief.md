# D9S-4b — Stage Brief (2026-11-13)

**Lane:** light. This brief is the design; there is no separate design doc. Split from D9S-4 at design; no ruling is needed.
**In one sentence:** on the server build, every batch a C program receives carries each row's attachments beside that row: one set per row, in row order, and an empty set for a row that had none. This closes the gap 4a disclosed ("batches arrive without their rows' attachments").
**Forcing test:** `Batches.EachBatchRowCarriesItsOwnAttachments` proves that each row's attachments arrive beside that row, as the same keys and bytes that were published, and that a row without attachments gets an empty set, never a missing one.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The batch a callback receives (4a's batch type) | CHANGED: one field appended, giving the row-by-row list of attachment sets | d9 L14 (attachments clause), d7 L3; reuses the row path's attachment set and its readers |

**New public surface: 0.** 4b widens one existing type. It adds no call, no type and no status. The added field is purely additive: the shim writes the struct, a program only reads it through a pointer, no existing field moves, and the ABI version stays as bumped in stage 1 (D-D9S-6).

## Deleted
Nothing. Only 4a's "attachments: 4b" placeholder goes.

## Corner cases forbidden vs handled
**Forbidden** (unrepresentable):
- A row whose set is missing (NULL): each row gets a set built from that row's own entry in domain 7's list, and a row with nothing gets an empty set.
- Sets out of row order, shifted across windows, or filtered: the shim builds one set per entry of domain 7's row-aligned list, in that order (d7 L3). It drops, adds, reorders and re-checks nothing (L15).
- A set that outlives its callback unseen: the sets live only for the call and are torn down on every exit, exactly as the row path's are. To keep a value longer, a program retains it, the row path's rule (L11).
- Half a delivery: the sets are built before the batch is exported, so a failure partway hands nothing over.

**Handled**, each with why it could not be forbidden:
- A window whose sets cannot be built (out of memory) is lost uncounted until D9S-6, then reported by L18. *Why not forbidden:* your ruling of 2026-11-13, "Accept until stage 6", covers every failed conversion in stages 4–5, and L18 keeps its red-first test in stage 6.

## How
- **binding.h:** append `const fl_attachments* const* attachments;` to `fl_batch`, after `schema`; D9S-6's `failure` comes after it. The doc says it has `num_rows` entries, none NULL, and is NULL exactly when `num_rows == 0` (a no-batch or zero-row window). Every set and every value is borrowed for the call: a program never disposes them and keeps a value only by `fl_blob_retain`. This is the same type the publish side takes (`fl_publisher_publish_rows`'s `atts_per_row`).
- **batches_server.cpp** is 4a's thunk step 1, inside the existing `try`, before the export. The thunk takes domain 7's `std::vector<Attachments>` by value and moves each element into its own `fl_attachments`. The sets are kept in a container whose elements never move, such as `std::deque`, because `fl_attachments` can be neither copied nor moved. The thunk collects their addresses, then sets `out.attachments` to that list, or to NULL when it is empty. Each set takes its entry's existing blobs, so no attachment bytes are copied (d9 D7). Everything is destroyed with the frame after `on_batch` returns.
- **The shim adds no check of its own.** It adds no count, window, timer or range check, and does not compare the set count with the row count; alignment is domain 7's law (L15, BC-9). The edge build never reaches this code.
- **Forcing test** (server binary only, `test_batches.cpp`). Rows `seq` 100–104, windows of 2 rows, no timeout, closed by destroying the subscriber, as 4a's test does. Rows 1 and 2 are published with attachments through `atts_per_row`: row 1 `{trace: r1}`, row 2 `{origin: o2, trace: r2}`. Row 1 is the last row of window 1 and row 2 is the first row of window 2, so the test catches a set landing on the wrong row inside a window and a set slipping across a window boundary. The sink copies each set's keys and bytes during the callback. Expected: `[{}, {trace:r1}]`, `[{origin:o2, trace:r2}, {}]`, `[{}]`, with the list non-NULL and no entry NULL.
- **Red before:** the test does not compile, because the batch has no `attachments` field. **Controls:** none separate. The test checks rows with attachments and rows without, using distinct keys, values and set sizes, so an all-empty, all-same, shifted or swapped answer fails it.

## Decisions for you
None. I checked every stop trigger and none fires:
- no edge entry point changes its answer;
- no new status or error channel;
- no window, timer or range check in the batched path;
- no domain 7, domain 2 or `dotnet/` edit;
- the ABI change is purely additive.

## Risks accepted / debt carried
- **Premise (stop and ask if false):** domain 7 hands over exactly one attachment entry per batch row, and none for a no-batch window (d7 L3; `subscriber_arrow.cpp:181-183, :258`). If the implementer finds a path where the counts differ, stop. The shim does not absorb or re-check it.
- Keeping a batch's attachment values past the callback uses the row path's retain rule, which the row path's tests already prove. The proof of a batch's lifetime is rung 5's (D9S-5, BC-13).
- Evidence will be Windows, run by hand; the CI runs come from your draft PR.

## Numbers
- **Touched production lines (non-comment):** about +12 / −1 in **2 files**, `binding.h` (+1) and `batches_server.cpp` (+11/−1). With comments the raw count is about +22 / −2. This is within the light-lane admission of 60 lines in 3 files.
- **Tests:** about +70 in `c-abi/tests/test_batches.cpp`, covering the sink's copy of the sets and the new case.
- **Docs (not counted):** d9 L14's attachments clause, L13's note, §2 Server row, §4 "not yet built" and Q6's attachments half; register BC-13's attachments half; `c-abi/README.md` "Consuming".
- New public surface: 0. Design cycles used: 1/2.

---
*As landed — 4b, 2026-11-13:*
Production: 12 non-comment lines touched (+11/−1) in 2 files, against a declared ~+12/−1; light lane held. Tests: the forcing test, plus one NULL-list assertion added to 4a's no-batch test (0b note). d9 L14's attachments clause is `[built]` (Windows). BC-13 gained no clause, because its text never names attachments; the brief's prediction was wrong. The record fix corrected d9 L14's stale `fl_decode_rows` pointer. Cycles: design 0 (brief only), fix 0, implementer launches 1 (resumed once).
