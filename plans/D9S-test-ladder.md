# D9S — test ladder

Round **D9S**, domain 9 (the C ABI shim), branch `feature/shim-server-variant`. Purpose (owner): C#,
and any other binding, can get columnar batches straight from Fletcher's one batcher through the
shim, while the build that runs on embedded devices stays as small and Arrow-free as it is today
("we should only have one and it should be the C++ one"; "Arrow C++ should never be in the native
library, because it needs to be able to run on embedded devices"; "Let's build one shim in two
variants").

Citations: `d9` = `docs/domains/d9-c-abi-shim.md`, `d7` = `docs/domains/d7-batch-assembly.md`,
`rul` = `plans/RIX-rulings.md`, `aud` = `plans/D9S-pre-round-audit.md`,
`reg` = `docs/domains/bounded-context-register.md`, `h` = `c-abi/include/fletcher/abi/binding.h`,
`sa` = `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`. Tree: `7561c71`.

Ceiling: **250 net production lines per stage** (tests not counted). Sizes are estimates. Recipe,
CMake, CI YAML and `scripts/` count as production. Doc text (`binding.h` comments, `c-abi/README.md`,
d9 build marks) is listed but not counted. **Files** are the production files a stage touches. The
config uses them to decide which stages qualify for the light lane (at most 60 touched lines and 3
files).

**Out of scope (rul:353-381, d9 §6):** the C# switch, the managed batcher, the NuGet server package
and its name, size ceiling, notice, lock and dependency lists (Q5a, R-9), D-BIND-59/R-3, R-4, Q9,
Q11, and every change to domain 2 or 7. The only domain-10 edit is `HeaderVersionMinor` in
`dotnet/src/Fletcher.Interop/NativeLoader.cs`, made in the bump's commit (D27).

## Ground rules for every rung

- **Driven from outside:** every rung test calls `fl_*` entry points declared in `binding.h`. They are
  called from gtest executables in `c-abi/tests/`, each linking **one** shim artefact (precedent:
  `c_abi_probe_tests` links only the probe shim, `tests/CMakeLists.txt:146-157`), from `c99_probe.c`,
  from the C# lanes, or from CI steps over the built artefacts and their Conan graphs.
  `fletcher-c-abi-impl`, `SubscriberArrow` and `RecordBatchBatcher` are never called by a rung test.
  Supplementary legs may call them, but no leg is ever the only proof of a rung.
- **Publishing:** through the same shim. The `inprocess` provider, `fl_publisher_declare` and
  `fl_codec_publish_rows` for rows with a schema, as in
  `Subscriber.TheChainDeliversThroughTheAbiInBothDirections`. `fl_publisher_publish_raw` on an
  undeclared topic for rows that have no schema.
- **Importing a batch:** the test imports what it was handed with its own Arrow C++
  (`arrow::ImportRecordBatch`). Test binaries may link Arrow (L8, aud T10). The shim's Arrow is a
  second, hidden copy, so a successful import proves the batch crossed as C Data and nothing else.
- **Every server-only subject is also asserted on the edge build and on the probe shim**, as BC-10's
  truth table: there, the same call answers `FL_NOT_SUPPORTED`, with a message that names the edge
  build and the server package. It never calls the callback, leaves any OUT parameter untouched, and a
  later publish delivers nothing.
- **Timing assertions are one-sided**, as in D7O. "Nothing delivered" is asserted over a fixed
  observation window, followed by "exactly X at closing". Every such arm has a control arm: a 10 ms
  timeout must give a `kTimeout` within 2 s. No upper bound tighter than 2 s is asserted.
- **Names and shapes are the architect's:** the query, the batched entry points, the C delivery
  (status, reason, loss count, attachments: Q6), the Conan switch, the artefact file name (with
  domain 10, Q5a). This ladder names none of them.

## The ladder

| # | Subject (what becomes TRUE) | External path | Internal observation | Builds on | New work (est. net prod. lines / files) | Authority |
|---|---|---|---|---|---|---|
| **1** | **A caller can ask the loaded shim which build it is.** It can ask before any other call and from any thread. Today's shim and the test-only probe shim both truthfully answer **edge**. Both report the new ABI minor, and every lane that loads the shim, including C#'s, still loads it. | The query, called from `c_abi_tests` and `c_abi_probe_tests` first, before any other call, and then from several threads at once. `BindingAbi.VersionMatchesHeader` / `.VersionIsPackedMajorMinor` (T1). `c99_probe.c` compiles with the new declaration (T18). `test_package`. The dotnet lane's `NativeLoaderTests` (T2), with the expected minor moved. | None. | The BIND shim as built. `fl_binding_abi_version` is the template for "callable first, from any thread" (`h:158-172`). | One declaration and the minor bump in `binding.h`. One function in `c-abi/src` that answers from one per-artefact constant: this is **BC-10's single site**, created here, with edge as its only value so far. The `HeaderVersionMinor` line in `NativeLoader.cs`, **in the same commit**. **~15-30 lines, 3 files** (4 if the function gets a new source file, which adds a `CMakeLists.txt` line). Light-lane candidate. Docs: d9 L16 and L6 build marks, README "Consuming". Red: does not compile (no query). Control: without the `NativeLoader.cs` line, the dotnet lane refuses the shim. | **L16**, **L6** (D27), **L12** (the edge build gains the query); D23; reg BC-10 |
| **2** | **A second build of the shim exists, made from the same sources, and says it is the server build.** A caller that uses the edge surface sees the same behaviour on it. Its exported `fl_*` set equals the edge build's, and both equal the header's declarations. CI's size, dependency and strip checks still measure the **edge** shim, even when the server shim sits in the same Conan cache. | The query on the server artefact answers server. The edge-surface rows of `c_abi_tests` (`BindingAbi.*`, `BindingEntryPoints.*` including `TheShippedShimHasNoProbe` (T11), `Subscriber.*`, `Attachments.*`, `TopicOptions.*`, `Publisher.*`) are built a second time as an executable linked to the server artefact. `test_package` runs against both packages. CI compares the export sets: edge = server = `FL_ABI_EXPORT` set (Linux, as T4; on Windows the `fl_*` subset). CI picks the edge shim for the size ceilings (T3), the dependency and strip checks (T5, T19) by package identity (the switch), never with `find -print -quit` on the file name. | None. | Rung 1: the per-artefact constant becomes a definition set per target. The server target sets "server". The probe shim's two-artefacts-from-one-object-library pattern (`CMakeLists.txt:164-211`, `conanfile.py:30-34`). | A build switch **kept in the package ID** (aud F13). A server shim target, made the way the probe shim is. The per-target constant. The c-abi lane builds both variants. Artefact selection re-anchored in `ci.c-abi.yml` (win and linux legs) and `ci.dotnet.yml` (linux and windows legs). An export-set comparison step or script. **No Arrow link yet.** **~90-140 lines, 4-5 files** (`conanfile.py`, `CMakeLists.txt`, two workflows, one script). Test-only: `tests/CMakeLists.txt`. Docs: d9 L12/L13 marks, README "Building". Red: no server artefact, so nothing can answer server. Controls: an `fl_` export added to one target only turns the comparison red. A server package placed in the cache ahead of the edge one must not change which file CI measures. | **L12**, **L13**, **L16**, **L7**; D19, D22; reg BC-10 (export-set half); aud F7, F8, F13; T3, T5, T19 re-anchored |
| **3** | **The edge build is shown to carry no Arrow C++, by name rather than by size.** A check fails if any path brings `arrow`, `fletcher-arrow-bridge` or `fletcher-pubsub-arrow` into the edge configuration's host graph (outside test requires), or any `arrow::` symbol into the **unstripped** edge shim. | CI step(s) in the c-abi lane: `conan graph info` of the edge configuration, and a symbol listing of the unstripped edge artefact (`nm -C` on Linux; on Windows, by graph or by PDB/link map, at the architect's choice). The size ceilings stay as the coarse check (L12: no size check changes). | None. | Rung 2: the switch exists, so "the edge configuration" names something, and both configurations are built in CI. | One script plus the CI steps that run it. **~40-70 lines, 2 files** (`scripts/<check>.sh`, `ci.c-abi.yml`). Light-lane candidate if it stays at 60 lines or fewer. Docs: d9 L8 `[built]` (no longer "no code owner"), Q4 resolved. **Green on the pre-stage tree by construction**, because nothing links Arrow yet. Its red is the control: the same check pointed at the `c_abi_tests` binary (which links Arrow, T10) must fail by name. It lands **before** rung 4 brings Arrow into the recipe. | **L8**, **L12** ("no link"); the owner's "never in the native library"; reg BC-11; Q4 |
| **4** | **On the server build, a C caller subscribes for batches and receives each window domain 7 closes.** Each window arrives as an Arrow C Data array and schema that the caller can import, with the window's reason, its loss count and each row's attachments, on one thread, one at a time, in window order. A window domain 7 could not build arrives as a **no-batch delivery** with its rows counted. **On the edge build and the probe shim the same call answers `FL_NOT_SUPPORTED`** and does nothing. | Server: declare a topic, publish 5 rows (2 of them with attachments), subscribe with `max_rows` 2 and a finite timeout. The caller gets 2 `kRowLimit` batches and a `kTimeout` batch. Each is imported and compared value for value with what was published, attachments row by row, loss 0. Every callback runs on one thread, and first-row values strictly increase (d7 `EveryCallbackRunsOnTheBatchThreadInWindowOrder`, replayed through the ABI). Raw rows published to an undeclared topic give a no-batch delivery whose loss equals the rows published. Edge and probe: BC-10's truth table (ground rules). | None. | Rung 1 (the refusal asks BC-10's site; no second `#if`). Rung 2 (the server artefact and its edge-surface suite). Rung 3 (the Arrow link added here is checked to reach the server configuration only). | Declarations: batched subscribe and the C delivery shape (Q6, Q7, Q12), designed here **in full**, so no later rung changes a signature. Server-only source: forwards to `SubscriberArrow::Subscribe(segments, RecordBatchCallback, BatchOptions, TopicOptions)` (`sa:163-165`) with options and topic options **as given**. A batch thunk exports the batch with Arrow's C exporter, puts the attachments in C form (reusing `fl_attachments`), calls the binding, and releases whatever was not moved when the callback returns. Edge source: the named refusal. `pubsub-arrow` is required and linked under the switch only. **~150-220 lines, 5-7 files** (`binding.h`, `binding.cpp` and/or one server-only and one edge-only source, `handles.hpp`, `CMakeLists.txt`, `conanfile.py`). Docs: d9 L14/L15 marks, README. Red: does not compile (no entry point). Edge red: no refusal to observe. | **L14** (crossing), **L12**, **L15** (thread, order, no batching), **L13**, **L1**, **L3**, **L7**; d7 L3, L7, L12; D23, D28; reg BC-10, BC-9 |
| **5** | **A batch is the caller's only for the duration of its callback, unless the caller moves it.** A moved batch stays valid and importable after the callback returns, on any thread, until the caller releases it, even after the subscriber and provider handles are gone. Whatever the caller did not move is released by the shim exactly once when the callback returns. The shim keeps nothing of any batch, and the callback is never handed an `fl_schema`. | Same subscription as rung 4. The callback moves some batches' array and schema (copies the structs, sets the source `release` to null) and leaves others alone. After later batches, the caller closes, then destroys the subscriber and provider in either order (L3), then imports the moved batches on another thread and releases them. | The shim's live Arrow allocations return to baseline once every moved batch is released. This is observation only: a test-only allocation count, or a leak-checking CI leg (ASan/LSan); the architect picks. | Rung 4 (the thunk and its release on return). | The thunk's "released only if not moved" rule, if rung 4 did not already make it exact. **~0-15 lines, 1 file.** Docs: d9 L14 `[built]`, BC-13 batch half `[built]`. **May arrive green with rung 4.** The architect shows red on the pre-stage tree (a moved batch released by the shim, or an unmoved one leaked) or declares the arm a guard. | **L14** (D26), **L11**, **L3**; reg BC-13 |
| **6** | **Closing a batched subscription delivers every finished batch and then the closing batch before the close returns, and nothing after.** Destroying the subscriber does the same. Closing or destroying it from **inside its own batch callback** neither terminates the process nor waits: the remaining batches, then the closing batch, still arrive. On the edge build, any close entry point the design adds refuses as rung 4's does. | The batched close (whatever the design has) and `fl_subscriber_destroy`, called from outside and from inside the callback: d7 `UnsubscribeFromInsideACallbackNeverDeliversNested` cases a-e, through the ABI. The closing batch is always last. Under the ctest TIMEOUT, a deadlock fails the test. | None. | Rungs 4 and 5 (deliveries, and batches that are released, so closing leaves nothing). | A close entry point and its edge refusal, if the design has one. Destroying a subscriber tears its batched subscriptions down through domain 7's `Stop`, and a destroy made inside a batch callback is never routed into the row path's designed termination (d9 L1). **~25-50 lines, 2-3 files.** Docs: d9 L1 (F4) and L15 marks; BC-5 shim note `[built]`. | **L15** (d7 L9, D12), **L1** (F4), **L3**, **L12**; reg BC-5 |
| **7** | **Batch options reach domain 7 exactly as given.** `INT64_MAX` ms is "no timeout": windows close only when full or at closing. A finite timeout gives `kTimeout`. `max_rows` is used as given. An out-of-range value is refused with `FL_INVALID_ARGUMENT` carrying domain 7's message byte for byte, and leaves no subscription, no callback call and no delivery. Topic options are forwarded as they are for the row subscribe. | BC-8's 6 x 7 grid, driven through the ABI. BC-14: `INT64_MAX` gives no `kTimeout` within the observation window and exactly one closing batch at close; -1 is refused. The expected message comes from domain 7 itself, which the test binary links as an oracle (test-only, as `arrow-bridge` is for T13). Control: 10 ms gives `kTimeout` within 2 s. | None. | Rung 4 (options forwarded as given) and rung 6 (closing, for the no-timeout arm). | Normally none: rung 4 forwards and checks nothing (BC-8: the shim answers nothing). **~0-10 lines, 0-1 file.** Docs: BC-8 and BC-14 shim halves `[built]`, d9 Q7 resolved. **Expected green on arrival.** The architect confirms each arm on the pre-stage tree and declares the green ones as guards. | **L15**, **L4**; D12, D-D7O-1; rul "Refuse both"; reg BC-8, BC-14 |
| **8** | **When the shim cannot turn a window domain 7 delivered into its C form, the caller still gets that window.** It arrives as a no-batch delivery with its rows counted as lost, its reason kept, and the failure's message beside it. Nothing is thrown into domain 7, later windows arrive normally, and Σrows + Σlost seen by the caller equals the rows published. | A batched subscribe on a server **test** artefact, with the hand-over made to fail on window k. Publish N rows over several windows. Assert conservation, assert window k's reason is the one domain 7 gave, and assert that windows after k are ordinary batches. | The fault seam reports that it fired on window k. This is observation, and only arranges the failure: the exercise is the subscribe and the publish. | Rung 4 (the no-batch delivery shape this reuses) and rung 5 (what a half-built export must release). | The thunk's one failure site (BC-12): it catches, delivers no-batch with the rows counted, and never rethrows. Q12's origin (an appended `fl_origin`, which rides on rung 1's bump). A test-only fault seam that is **never compiled into the shipped server artefact**; see Findings 3. **~25-45 production lines plus ~15-30 for the test-only seam, 2-4 files.** Docs: d9 L18 `[built]`, BC-12 `[built]`. Red: without the catch, window k is lost silently (domain 7 swallows the throw, d7 L10), so conservation fails. | **L18** (D30), **L1**, **L14**; d7 L1, L7; reg BC-12 |
| **9** | **A process that loads the edge build and the server build, in either order, gets one working shim.** The later one refuses every fallible call and names both modules. The first keeps working. In the refused one, the version and the variant query still answer. | A test executable linked to one artefact loads the other by path, calls through it, and asserts the refusal and the named modules. Both orders. The existing decoy rows stay (T12). | None. | Rungs 1 and 2: both artefacts exist and can be reached from one process. Nothing later is needed, so the architect may move this rung forward. | Normally none: both builds export the marker from the same source. The marker text stays the same in both builds, or L13's list of differing answers would be wrong (aud A32). **~0-10 lines, 0-1 file**, plus test-side loading. Docs: d9 L17 `[built]`, BC-15 `[built]`. **Expected green on arrival; declared.** | **L17**, **L5**, **L16**; D22; reg BC-15 |
| **10** | **A C# caller written against the edge surface gets the same bytes and answers from the server build as from the edge build:** the binding-abi-conformance lane passes against both. | `ci.integration-test.binding-abi-conformance.yml`, run once against each artefact (`FletcherNativeShim`), with the emitter resolving the matching package. | None. | Every earlier rung: this is L13 checked on the finished server build. Rung 1 (C# loads the bumped version). | The lane runs a second time with the server switch. `integration-tests/binding-abi-conformance/conanfile.py` passes the switch through. **~15-30 lines, 1-2 files.** No `dotnet/src` change. Docs: none. | **L13**; T14 |

**Rungs: 10. Stages: 7.**

| Stage | Rungs | Est. prod. lines | Files | Light lane? |
|---|---|---|---|---|
| D9S-1 | 1 | 15-30 | 3 (4) | candidate |
| D9S-2 | 2 | 90-140 | 4-5 | no |
| D9S-3 | 3 | 40-70 | 2 | candidate if ≤60 |
| D9S-4 | 4 (+ BC-9 scan) | 150-220 | 5-7 | no |
| D9S-5 | 5, 6, 7 | 25-75 | 2-3 | no (one stage, three subjects) |
| D9S-6 | 8 | 40-75 | 2-4 | no |
| D9S-7 | 9, 10 | 15-40 | 1-3 | candidate |

**Why stage 5 has three rungs, and stage 7 two (flagged; not a ceiling exception).** Rungs 5, 7 and 9
prove laws the earlier rungs' code may already satisfy (borrow-and-move, pass-through, the marker
scan). On their own they would be stages with no red work. They stay separate rungs, so each law has
its own test. The architect orders the red-first work so each rung is shown red on the pre-stage tree
where it can be, and declares any arm that is green both ways as a guard.

**Rung 4 is the one near the ceiling.** If the architect's estimate passes 250 lines, split it into
4a and 4b, and do not carry it over the ceiling. 4a: the shape designed in full; rows, reason, loss
count and no-batch; the edge refusal. 4b: attachments filled. Never split the edge refusal from the
first batched entry point: L12 requires the two builds to export the same set at every commit.

**Accumulation and deletion checks.** Rung 1 creates BC-10's single site, rung 2 gives it a second
value, and rung 4's refusal reads it, so no second `#if` is ever written. The Arrow-free check
(rung 3) is in place before the first Arrow link (rung 4). The C delivery shape is designed once, at
rung 4, and rungs 5-8 add behaviour behind it, never a signature change. The no-batch delivery exists
(rung 4) before L18 reuses it (rung 8). Closing (rung 6) exists before the no-timeout arm needs it
(rung 7). No rung builds something a later rung deletes.

## Tests other than rungs

- **Guards (declared).** (a) The export-set comparison (stage 2) re-runs at every later stage, so a
  batched entry point that is missing from either build is red at once. (b) The BC-9 source scan
  (stage 4): `c-abi/src` contains no `std::thread`, `std::condition_variable`, clock arithmetic or
  timer. (c) BC-11 (stage 3) re-runs at every later stage. (d) `c99_probe.c` (T18) catches a batched
  declaration that names a C++ type. (e) BC-10's truth table runs over edge, server and probe for
  every batched entry point, at stages 4 and 5.
- **Controls.** 1: no query, so the build fails; without the `NativeLoader.cs` line, the C# lane
  refuses the shim. 2: no server artefact; an export added to one target turns the export-set step
  red. 3: the check pointed at a binary that links Arrow is red by name. 4: no entry point; the
  edge has no refusal. 5: the shim releases a moved batch, or leaks an unmoved one. 6: a close inside
  the callback terminates or deadlocks. 7: a 10 ms timeout gives `kTimeout` (timing control); a
  shim-side range check would change the message, which the byte-equality assertion catches.
  8: without the catch, conservation fails. 9 and 10: green on arrival, declared.
- **Existing tests (aud part C).** T1, T11 and T13 also run against the server artefact (stage 2).
  T2's minor moves at stage 1. T3, T5 and T19 are re-anchored at stage 2. T4 is extended at stage 2.
  T12 is extended at stage 7. T14 is extended at stage 7. T15-T17 are untouched. **T6, T7, T8 and T9
  are not this round's** (Findings 4). Nothing is deleted.

## Findings and owner questions

1. **Plan finding: the version bump goes first, not last.** The handed-down order put "the C# lane
   still passes with the bumped version" last. L6 says a change to the ABI is never silent. If the
   bump comes last, stages 1-9 commit a changed header that still reports 0.7, and a 0.7 loader (C#,
   T2) accepts those shims. If it comes first, no 0.7 consumer ever accepts a changed shim, and
   later appends (the batched entry points, any `fl_origin` from Q12) ride on the same unreleased
   minor. That is D27's "one version … both bump with the shim round", with the one `NativeLoader.cs`
   edit in the bump's commit (stage 1). **Owner (low):** confirm that one bump at the first append is
   the reading you intended. The alternative is one bump per stage, BIND's precedent, which would
   need the `NativeLoader.cs` line moved more than once and so more than D27's one edit.
2. **Plan finding: the edge refusal and the Arrow-free check move earlier.** The handed-down order
   had "edge build refuses with not-supported and stays Arrow-free" as one late rung. L12 requires the
   edge build to export every batched entry point the server build does, so the refusal must land in
   the same rung as the first batched entry point (rung 4). The structural Arrow-free check must be in
   place before the rung that first links Arrow (rung 3 before rung 4).
3. **L18 has no natural trigger from outside (for the architect; owner only if this fails).** I found
   no batch that domain 7 builds but Arrow's C exporter refuses, and allocation failure cannot be
   forced reliably. Rung 8 therefore needs a test-only fault seam, kept out of the shipped server
   build (L7: the export table is exactly the surface; L13). The precedent is the probe shim, a
   test-only artefact (D-BIND-62). If the seam can only live in the shipped server artefact, that is
   an owner question.
4. **T6, and the server halves of T7, T8, T9 and R-9, are not this round's.**
   `check_dotnet_packages.sh` checks NuGet packages, and this round produces no server package
   (packaging is domain 10's, Q5a). Re-anchoring T6 now would give it a subject that does not exist
   and an arm that cannot be exercised. It moves with the round that adds the server package. The
   brief listed T6 among the re-anchors; this ladder does not.
5. **Q5a's native file name is decided at stage 2**, together with domain 10. Rung 9 needs both
   artefacts loadable by one process, and if both files share a name the stage-2 re-anchor matters
   more. Either choice is lawful.
6. **Design questions that are due at a named stage:** Q6 (how attachments, reason and the no-batch
   case cross) and Q7 at stage 4; Q12 (the `fl_origin` of the edge refusal and of a failed hand-over)
   at stages 4 and 8. A new `fl_origin` enumerator is the shim's own (aud F5), not a seam question.
   aud A32: the marker text must stay the same in both builds.
7. **No tier-1 disagreement found.** L6, L12-L18, D22-D31 and BC-8-BC-15 agree with one another and
   with every rung's subject. The one constraint they put on the plan's order is in Findings 1 and 2.
