# D9S-6 step-0b test-authority audit (MODE=new-test): CLEAR

**Verdict: CLEAR.** Nothing in scope is FORBIDDEN or FORBIDDEN-BY-TOLERANCE. Nothing schedules a restoration, and no tier-1 texts disagree. Working tree on HEAD d43eb0b; no behaviour code yet.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom. It has 514 lines (`wc -l`) and 39 entries. The highest, at l.504, is "2026-11-14 — D9S-6: a failed hand-over still carries its status and message". Rulings :383 (D30), :445(2), :478 and :504 govern.

**Yardstick:** all rulings; d9 §1 ("every C++ failure into a status plus message"), L1, L2, L6, L7, L11, L14, L15, L18, Q12; D-D9S-4 ("Any fault hook used to prove it is test-only and absent from the shipped server build"); ladder rung 8.

**Scope and classification:**
- `handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`, with its cell helper (`test_batches.cpp`, k=2 and k=3). **LAWFUL.**
  - **Law (a):** every assertion agrees with a law. 3 deliveries, the window's reason kept, and its rows counted lost come from L18. "Exactly one CLOSING, last" and "nothing after the close" come from :478. "No array, schema or attachments" comes from L14 ("NULL iff num_rows == 0"). The failure carrying `FL_INTERNAL`, `FL_ORIGIN_HANDOVER` and a copied message comes from :504 ("carries the failure's status and message, borrowed for the callback") and L11. Σ received + Σ lost == 5 is BC-12/L18's own structural test.
  - **Law (b):** the test injects the fault, then observes the result. Rung 8 says so outright: "This is observation, and only arranges the failure: the exercise is the subscribe and the publish." L18 itself prescribes a test "with the export failing on a chosen window". Every assertion reads the public `fl_batch` through the callback. `fired == 1` is the only internal read, and it guards against a vacuous run, not a subject. **So it is still an external proof of L18.**
  - **No new value subject.** `FL_INTERNAL` with the message equal to `what()` is L2's existing `Capture` rule (`containment.cpp:88-90`). The field and code 4 are :504's two slots. `failure == NULL` on deliveries that were handed over is just what the field means under :504, not a new rule.
- **Control C1** (the `Batches` suite in `c_abi_handover_tests`, unarmed). **LAWFUL, and a real control.** It goes red if the unarmed point fires, or if the fault shim differs from the shipped server shim (variant literal 2, same sources, visibility and version script). Each TEST runs in its own ctest process (P4), so an arm cannot leak into another test.
- **Seam** (`fletcher-c-abi-handover-fault`, `handover_fault.cpp`, the macro). **LAWFUL; D-D9S-4 holds.** The seam cannot reach a shipped artefact:
  - the target is server-only, and it exists only under `FLETCHER_BUILD_TESTS`;
  - the macro is PRIVATE to that target, and CMake compiles `batches_server.cpp` once per target, so the shipped copy expands to `((void)0)`;
  - `handover_fault.cpp` refuses to compile without the macro (`#error`);
  - `package()` names the stem, and `*fletcher-c-abi-server.dll` and `*libfletcher-c-abi-server.so*` do not match `...-handover-fault`. This is held by name only (registered risk);
  - `check_shim_exports.sh` refuses any shipped `fl_test_*` export.
- **`BatchSink::Record`** now copies `failure`. **LAWFUL; earlier proofs are unchanged.** The copy is made inside the callback (L11). It adds no assertion to any other test, and today's `out = {}` (`batches_server.cpp`) leaves the field NULL.
- **Arm before publish** (the design says "publish 5 rows, arm hand-over k"). **Lawful; the subject is unchanged.** The windows are handed over on the batch thread while the rows are published, so arming afterwards would race and leave k undefined. Nothing is handed over between subscribe and publish (no rows, `INT64_MAX` timeout). This is tier-3 text drift: the PM corrects the design line. It is not a finding against the test.

**Notes (no finding):** (1) The fault point is the last statement of the hand-over scope. An earlier throw (building the sets, or the export) is proven by structure (B1), not exercised; the half-built-export release (ASan/LSan) and DEBT-5 stay owed. (2) If the binary runs outside ctest and a cell fails before the fault fires, the arm stays set; today the forcing test is the file's last test. (3) The test is red for the right reason: the fault is swallowed, so k=2 sums to 3 and k=3 has no CLOSING.
