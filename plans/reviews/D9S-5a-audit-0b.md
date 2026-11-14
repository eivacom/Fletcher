# D9S-5a — step 0b test-authority audit (new-test)

**Verdict: CLEAR.** MUST-DELETE(0). No scheduled restoration. No tier-1 disagreement. HEAD c7220e2, working-tree diff.
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **491 lines, 37 entries**; highest at l.478, **"2026-11-14 — D9S-5: every close ends with one closing delivery; D9S-5 splits"**. Yardstick: every ruling; d9 §3 L1–L18; d7 L4–L6, L9, L15.
**Scope:** F1, G1 (+`MoveOrLeaveSink`), G2 (+`Domain7Refusal`), F2, the helpers at l.283-423, and `tests/CMakeLists.txt` +8. The out-of-tree mutation controls were judged from the design.

| Test | Class | Authority |
|---|---|---|
| F1 | LAWFUL | d7 L9 ("The pending window is delivered once, as `kClosing`. After `Unsubscribe` … returns, no further batch is delivered"); ruling 2026-11-10 "Queued batch, then closing"; 2026-11-14 "exactly one closing delivery". The second close hits a dead id (d9 L4, a no-op); it is not an idle close |
| G1 | LAWFUL | d9 L14 / ruling 2026-11-13 "what is not moved the shim frees when the callback returns"; L3 (either destroy order) |
| G2 | LAWFUL | d7 L15; ruling "Refuse both" ("the shim passes the refusal through"); d9 L15; L3 (OUT untouched); d7 L5 ("never earlier than its timeout says"); d9 L4 (topic-option refusal forwarded) |
| F2 | LAWFUL, one arm UNMOORED | d9 L12 ("refuses … having done nothing") for edge, probe and NULL. **UNMOORED:** the server arm where an untagged id gives `FL_INVALID_ARGUMENT`. No law states it; it rests on the design review's tier-3 reading. DEBT-1's d9 §4 entry must land |
| Helpers; `ExpectRefusedAsEdge` | LAWFUL | 4a's four edge assertions, moved verbatim. The OUT-sentinel and nothing-delivered checks stay, so **4a is not weakened** |
| CMakeLists | LAWFUL | Server-only link; the edge graph is untouched (d9 L8; ruling "Narrow the docs") |

**Ruling 2026-11-14 (idle or in-callback close):** no callback in scope closes or destroys anything. Every asserted CLOSING has rows pending: F1 row 5, G1 W2, F2's one row, and G2's six no-deadline cells with one unflushed row each. Every live idle subscription is torn down unasserted, after its last assertion, and its sink outlives the fixture: F1's control, G2's nine ROW_LIMIT/TIMEOUT cells and its control, F2's row subscription. G2's "exactly one delivery" for a live idle cell rests on d7 L6's unamended half ("A window with nothing pending delivers nothing"); no close is involved.

**Law (b), the G2 oracle, observes.** It drives a separate domain-7 copy only to compute the expected string, and every exercise of the system under test is an `fl_*` call (ladder rung 7 names this oracle). The rule "refused exactly when `max_rows < 1 || timeout < 0`" is hard-coded from d7 L15, so a clamp is caught without the oracle. The oracle is the **sole** proof against one mutation: a shim range check with its own message. That is acceptable, because a check with a byte-identical message cannot be seen at the ABI. The oracle cannot pass vacuously: if domain 7 accepts the options, it returns "<domain 7 accepted…>".

**No smuggled value subject.** H1 (a dead tagged id gives `FL_OK`) is d9 L4. The untagged-id refusal is argument validation (UNMOORED above, not a violation). G1's counting decoy observes only the `release` pointer, which is all the state the C Data Interface has.

**The `sleep_for(2s)` is lawful** under the ladder ground rule ("one-sided … fixed observation window … control arm: a 10 ms timeout must give a `kTimeout` within 2 s"). No tier-1 text governs timing. The positive cells share the window with the control.

**Laws without a test after 5a:** d9 L1 (the designed termination); L2 (the enums agree); L14 "keeps nothing" (G1 counts releases rather than measuring memory; DEBT-1 scopes the mark). L17 and L18 are correctly `[designed-not-built]`. No false green.
