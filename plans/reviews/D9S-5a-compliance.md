# D9S-5a: conformance review (step 4a), cycle 1

**Verdict: PASS.** No blocking conformance item. No STOP-AND-ASK. Diff `c7220e2..ea49aa3`: 10 files, all under `c-abi/` and `docs/domains/`. Reviewed by reading only, not built.
**Proof of read:** I read `plans/RIX-rulings.md` top to bottom: **491 lines, 37 entries**. The highest is at l.478, **"2026-11-14 — D9S-5: every close ends with one closing delivery; D9S-5 splits"**. No tier-1 collision is open, because that ruling explicitly settled the 2026-11-11 versus d7 L6 one.

## The five points
1. **The edge message rewording is within L12/L13, so the owner is not needed.** `RequireServerBuild` (`binding.cpp:229-238`) changes the end of the subscribe's message from "to subscribe for batches" to "for batched subscriptions".
   - L12 lets the edge build give "named refusals" from batched entry points. L13 counts the batched entry points among those whose answers may differ between builds.
   - What is pinned has not changed: `FL_NOT_SUPPORTED` (D28), `FL_ORIGIN_SEAM` (L12), and a message that names the edge build and `fletcher-c-abi-server` (header). `ExpectRefusedAsEdge`, which holds 4a's four assertions unchanged, still checks all of them.
   - No test, doc or `dotnet/` code holds the old wording, and 0.8 is unreleased. The design asked for one door, and one door forces a shared message ending.
2. **Every flipped mark is true and names its test.**
   - **L12:** `TheBuildAnswersABatchedCloseAsItAnswersTheQuery` sits outside the server-only `#if`, so it runs in the edge, probe and server executables.
   - **L14 and BC-13** are scoped exactly as DEBT-1 orders. They are `[built]` only for "released exactly once, after the callback returns, on the batch thread". G1 checks calls == 1, before_return == 0 and thread == the batch thread for the W1 array, the W1 schema and the W2 schema. After both handles are gone, it imports W0 and W2 on a new thread. L14's "keeps nothing" and BC-13's pool baseline stay `[designed-not-built]` until an ASan/LSan leg runs.
   - **L15 options, BC-8, BC-14 and Q7:** G2 covers all 42 cells. The 27 refused cells match the oracle byte for byte, leave the OUT sentinels untouched and deliver nothing. `INT64_MAX` and the other two no-deadline timeouts stay silent through the 2 s window beside the 10 ms control, then deliver one CLOSING. BC-8's "no subscription left" is checked the only way the ABI can see it: nothing is delivered and the out-id is untouched.
   - **L15 closing and the BC-5 outside half:** F1 covers both the close and the destroy. The in-callback half stays `[designed-not-built]` for 5b.
   - **L6, L13, §2 and §4 "Both builds":** the close exists in both builds (F1, F2).
3. **The d9 §4 "Batched close" entry is not a law change.**
   - It is in §4, the interface section. It amends no §3 text, and L4 still lists two answers.
   - Calling the refusal argument validation matches how the shim already refuses its own malformed non-NULL arguments without counting them under L4: `fl_blob_create` with null data and a size (`binding.cpp:305`), a blob with bytes but no owner (`:483`), and `publish_rows` with count < 0 (`:762`).
   - Bit 63 is how the shim keeps two id spaces apart on one handle (D9S-4 B1, ruling 2026-11-13 "one object"). No seam rule stands behind the refusal, so the refusal does not answer a seam rule again.
   - L4's second exception is a different kind of answer: it answers "can this build do this?", a behaviour question, not a malformed argument.
   - The design's own stop ("if review reads it as a third L4 exception") is not triggered. The 0b audit's condition (the §4 entry must land) is met. One citation needs fixing; see RECORD.
4. **Nothing survives that should not.**
   - 4a's inline refusal is gone, and its message text appears nowhere in the tree.
   - The "only fl_subscriber_destroy ends … for now" sentence (`binding.h:1056-1059`), `fl_subscriber_unsubscribe`'s batched-id sentence (`:944-945`) and the comment "Stage 5's close reuses it" in `binding.cpp` were all rewritten in place.
   - Searching `c-abi/`, d9 and the register finds no "for now", "not built yet", "(stage 5)" or "owed to D9S-5".
   - The design orders no deletions (Files-to-delete is None). 5b's paragraph (`binding.h:867-877`) is untouched.
5. **L15 and scope are clean.**
   - `UnsubscribeBatches` (`batches_server.cpp:148-158`) holds `batches_mu` only while it reads the host pointer, never across `Unsubscribe` (L11), and then forwards the call.
   - It adds no window, timer, wait, flag or options range check. The bit-63 test reads the shim's own id format; it does not check options (BC-8).
   - No `#if` was added; BC-10's site is the only place the build is read.
   - No domain 7, domain 2 or `dotnet/` file changed. The oracle link is server-only, and the edge graph is untouched (L8, "Narrow the docs").
   - The files touched match Files-to-touch exactly. The L6 and §2 edits correct marks that would otherwise have gone stale; they are not scope creep.

## Ruling 2026-11-14: 5a asserts no idle or in-callback close, and 5b has nothing to undo
- No callback closes or destroys anything.
- Every CLOSING the tests assert has rows pending: F1's row 5, F2's one row, G1's W2, and G2's six cells.
- F1's second close hits an id that is no longer live. It is a no-op, and "exactly one" CLOSING still holds after 5b.
- Idle subscriptions (F1's controls, G2's nine cells and its control) are torn down after their last assertion, into sinks that outlive the fixture (DEBT-2). 5b's empty CLOSING therefore cannot turn a count red.
- The close forwards to `SubscriberArrow::Unsubscribe`, so 5b's d7 change reaches it with no shim edit.
- The header's close paragraph promises the closing batch unconditionally, which is where the ruling ends up. The old idle text ("an idle one delivers nothing") and the in-callback text ("no reliable end signal") remain only in destroy's paragraph, which the ruling says 5b supersedes.

## RECORD
- **Brief:** "No existing call changes its answer" should note that the subscribe's edge message ending was reworded (status, origin and names unchanged). Note it in As landed.
- **d9 §4 "Batched close":** its source line cites "its review, 'Rulings asked for'", which reads like an owner ruling. It is a tier-3 reading, and no owner ruling exists; say so. The cited §1 "C spelling" list does not name subscription ids; cite D9S-4 B1 for the tag.
- **Idle close missing from three marks:** the d9 §2 Server-variant row, L15's "Not yet tested" and BC-5's 5b remainder name only the in-callback and row-delivery cases. Ruling 2026-11-14 gives the idle close to 5b too, so add "or idle".
- **Close refusal not named:** d9 L4's mark line and BC-10's status name only the subscribe's edge refusal. The close's refusal (F2) is the same second exception.
- **Entry-point counts:** d9 §2 says "48 today" and §4 says "49 entry points", but `binding.h` now exports 50.
- **d9 §1** still marks the edge refusals and the server variant `[designed-not-built]`, stale since D9S-4a.
- **Evidence:** the design says G1's three mutation controls are "run once and recorded", but they have no record in the tree, and the evidence ledger has no 5a row. Both are owed at close.
