# D10C-1 — Stage Brief (2026-11-15)

**In one sentence:** Fletcher's one batcher, the C++ one, gets its own proof of the four behaviours that only C#'s copy is tested for today, so deleting C#'s copy later (stages 7-8) loses no proven behaviour.
**Forcing test:** none red. Four declared guards, expected green on arrival (ladder rung 1): rows split across two messages are both counted as lost and nothing shifts; an empty attachment key arrives with its row; a callback that throws does not stop later batches; a row that would overflow a batch closes it and arrives in the next batch.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| none | none | Test files only. No product code, API, file format or setting changes (ruling 2026-11-15, :595 (1)). |

## Deleted
Nothing, by design: the C# tests these cases come from retire later (re-anchored at stages 3 and 7; the C#-window test is deleted at 7). Deleting them before these four land is forbidden (ruling :203, d10 L4).

## Corner cases forbidden vs handled
**Forbidden:** a test hook in the batcher (:595 (2): case 4 runs at real size). Any test reaching past the batcher's public publish and batched subscribe. Unbounded waits: every wait is bounded inside the ctest cap, so a red run ends red rather than hanging. Asserting C#'s failure count, which stays C#'s (d10 L9).
**Handled:** case 4's memory (about 1 GiB peak). This cannot be forbidden because the overflow point is Arrow's fixed 2^31-element limit, so the `heavy` label carries it (:140, :595 (2)).

## How
New tests only, in `pubsub-arrow/tests`. Each is driven through `PublisherArrow` and the batched `SubscriberArrow::Subscribe`, using the suite's existing sink and provider. d2's public `Codec`/`BatchDecoder` only build bytes or check a premise; neither is the proof.
| # | New test, and what it asserts | File | d7 clause |
|---|---|---|---|
| 1 | `SubscriberArrowBatchTest.RowsSplitAcrossMessagesAreDroppedNotMisaligned`: two encoded rows, cut 3 bytes into the second, sent as two messages with attachments. A good row with its own attachment follows. At closing: 1 row (the good one, its attachment at index 0) and 2 dropped. | `test_pubsub_arrow.cpp` | L1, L2 (L3) |
| 2 | `SubscriberArrowBatchTest.AnEmptyAttachmentKeyArrivesWithItsRow`: row A carries an empty key ({7,8}) beside a named key; row B carries none. One batch of 2 rows: A's two attachments at index 0, B's empty set at index 1. 0 dropped. | same | L3 |
| 3 | `SubscriberArrowBatchTest.AThrowingCallbackDoesNotStopTheNextBatch`: max_rows 1. Call 1 throws a std exception and call 2 a non-std value. 3 rows give 3 row-limit batches in order (x = 1, 2, 3), then exactly one closing. | same | L10, L12 |
| 4 | `SubscriberArrowBatchHeavyTest.ARowThatMeetsTheBudgetClosesTheWindowAndOpensTheNext`: two ~128 MiB `list<bool>` rows, each of 2^30 null elements (each fits alone, not together), max_rows 1000, no timeout. First delivery: row-limit, 1 row (A's attachment), 0 dropped. Then Unsubscribe gives closing: 1 row (B's attachment, 2^30 elements), 0 dropped. | `test_pubsub_arrow_heavy.cpp` | L4, L2 |
| 4c | Control `SubscriberArrowBatchHeavyTest.TwoHalfBudgetRowsFitAloneButNotTogether`: the decoder takes B alone, and after A it refuses B for the budget, not as malformed. | same | premise of 4 |
**Heavy:** the heavy executable already carries the `heavy` label (`tests/CMakeLists.txt:64-65`), and the default run (`ctest -LE heavy`) skips it. CMake changes only if case 4's measured time needs more than the 60 s cap; the comment then records the measurement. Elements are bool, not int8, because null int8 elements zero-fill about 1 GiB per decoded row.
**If red:** none is expected. A red case is a d7 (or d2) defect: STOP and go to the owner, because fixing it is d7 production code.
**Verify** (from `pubsub-arrow/`): `conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True" -c tools.build:jobs=1` runs cases 1-3. For case 4: `conan build .` with the same arguments, then under `build\generators\conanrun.bat` run `ctest --test-dir build -C Release -L heavy` (4, 4c and the 2 existing heavy tests).

## Decisions for you
None. Where the ports land and how case 4 is proven were both ruled at :595.

## Risks accepted / debt carried
- **No CI job runs heavy tests** (no workflow names the label). The "arrives in the next batch" proof is therefore a hand run recorded at close. A later regression there goes unnoticed until the next hand run. Accepted by :140 and :595 (2).
- d7's own document still says a throwing callback has no test, and it names none of the four. It picks them up at its next round (:595 (1)). d10 §8 rule 6 and Q9 name the four tests at close; that is doc only, not counted.
- Case 1 cuts at one fixed point (3 bytes into row 2), as its C# source does. It guards that both halves are refused, not every possible split.

## Numbers
Declared net lines: production +0 / −0 (0 files; light-lane admission met) · tests about +190 / −0 in 2 files (plus at most 3 comment lines in `tests/CMakeLists.txt` if the cap moves) · new public surface: 0 · design cycles used: 1/2

---
**As landed (2026-11-15):**
Production +0/−0 (light-lane admission held). Tests +280/−11 in the 2 named files vs ~+190 declared (+47%, under the 50% flag; extra assertions + banners, review N1 removed the duplicate helper). Added beyond the brief: none; control 4c as declared; `tests/CMakeLists.txt` unchanged (heavy label 0.92 s). Fix cycles 1 (review S1 lifetime, N1 dedupe; non-blocking). 0a MUST-DELETE(1) = C# T38, deletion stays at rung 7 per :203; 0b CLEAR.
