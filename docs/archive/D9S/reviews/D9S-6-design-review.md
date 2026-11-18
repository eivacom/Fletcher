# D9S-6 design review

## Cycle 1: NEEDS-REWORK (B1; B2 STOP-AND-ASK). DEBT 3. Full text: this file at `6e7a7d7`.
Proof of read then: ledger 503 lines, highest entry l.493 (D9S-5b).
- **B1** (BLOCKER): `sets`/`set_ptrs` were built before the hand-over's `try`, so a `bad_alloc` there lost the window
  uncounted. Fix: build both inside the `try`, and state "nothing before the `try` can throw".
- **B2** (BLOCKER, STOP-AND-ASK): Decision 1 set a principle (d9 §1, L2) against a detail (L18's sentence, the ladder's
  layout line), framed it as a trade-off, and defaulted to the exception. Fix: owner question, default (b).
- Pressure tests 1-4 passed: D-D9S-4 (no path to a shipped artefact), one delivery per window, `abort()` fits L1, cap.
- Check 7b: fits (production net +18 then). DEBT-1 (package held by name), DEBT-2 (`abort()` in d9 L1), DEBT-3 (one
  batched subscription per cell).

## Cycle 2: APPROVE-WITH-DEBT(2). No BLOCKERs.

**Proof of read:** `plans/RIX-rulings.md`, top to bottom: 514 lines, highest entry l.504, "2026-11-14 — D9S-6: a failed
hand-over still carries its status and message". Also honoured: :363 (borrowed for the call), :383 (D30), :445, :478.

**B1: closed.** Both containers are `std::optional`s, emplaced inside the `try` (l.17-18, l.23). "Nothing before the
`try` can throw" holds against the code: `ReasonOf` (noexcept after step 3), `num_rows()` (an accessor), empty optionals
and trivial aggregates. The thunk's by-value parameters arrive moved, not copied (`subscriber_arrow.cpp:302`). The catch
cannot throw: `Capture` is `noexcept`, rethrows inside its own `try` and allocates `nothrow` (`containment.cpp:29-39`,
`:56-105`). The fresh `fl_batch` is trivial, and its order matches `h:1032-1039` with `failure` last. A `bad_alloc` is
`FL_INTERNAL` at every non-codec origin, as the test expects.

**B2: closed by ruling l.504, and the design matches it exactly.** `const fl_error* failure` follows `attachments`
(h:1038), so the layout is additive. Nothing in the tree asserts `sizeof` or `offsetof(fl_batch)`, and `dotnet/` has no
`fl_batch` mirror. `FL_ORIGIN_HANDOVER = 4` appends to 0-3 (h:263-269), and nothing switches on `fl_origin`. One field
plus one enumerator is the ruling's 2 slots; the `fl_test_*` exports belong to a test-only artefact. No status number,
no second bump (D-D9S-6; minor 8, h:164). Borrowed for the callback and disposed after it (l.30); `const` stops a C caller
disposing it without a cast. The L2 clause is what the ruling authorises; its second sentence restates L2 as it stands.

**Texts to amend: complete except two (DEBT-4).** (a) `binding.h`'s `fl_error` paragraph (h:271-292: "owned by it",
"every binding's throw site is a `finally`") is L2's header form, so it gains the same clause. (b) d9 l.7: "a change to
a law needs an owner ruling, recorded in a decision of record". L2's clause needs D32. For the PM: the ruling's "layout
line" is the ladder's l.93 ("never a signature change") and rung 4's "so no later rung changes a signature" (l.59).

**Check 7b: fits; no split.** Priced from the body: one field, one enumerator, no new type or verb, one restructured
thunk (today's :100-140), a 6-line macro, a test-only TU and target, one two-cell test. +50/−16 is plausible, and
`binding.h`'s comments are doc text, not counted (ladder l.15-17). Counting the seam's CMake as production (ladder l.16;
rung 2 calls `tests/CMakeLists.txt` test-only) gives about +109 net against 250.

**Cap: 150/150, sized by it, not truncated.** l.21 joins two declarations; l.23 folds today's steps into a comment; step
6's `binding.h` line is terse, which is where DEBT-4(a) went. Everything load-bearing is present: B1's shape, the ruling's
surface, the test, M1-M9. Land the debt in code and docs; do not grow the design.

**DEBT** (register, D9S-6):
- DEBT-1 resolved: P2, 4(iii), corner case 7 and the brief's risk now say "held by name". DEBT-2 and DEBT-3 are applied
  in the design (step 6; l.103-104).
- **DEBT-4 (docs):** (a) and (b) above, plus a d9 §6 forward note: C#'s `FletcherOrigin` stops at 3
  (`FletcherException.cs:62-75`).
- **DEBT-5 (code):** if `make_shared` throws mid-loop, `fl_attachments`' constructor leaks its `BlobOwner` blocks
  (`handles.hpp:117-119`). So step 1's "what the attempt built is released" is false for a half-built set. It is a leak
  on OOM, not a loss. Fix it in `handles.hpp` (~5 lines), or register it.
