# D9S-5b design review

## Cycle 1 — NEEDS-REWORK (at `3c7e1d0`; ledger 491 lines, highest entry :478)
- **B1 (STOP-AND-ASK):** a refused Subscribe could deliver rows or a CLOSING. `subscriber.cpp:597-599` publishes
  the entry before the `:324-325` refusal, and `binding.h` would then invite a double free or a use-after-free.
  **Answered by ruling :493, option (a); closed in Cycle 2.**
- **DEBT-1** (brief: "has decoded a row"; the risk line on idle use-after-free), **DEBT-2** (F1 cell (d); D18
  records only the ruling), **DEBT-3** (PM: annotate D-D9S-8). All three are in the register under D9S-5b.
- Pressure tests 1–5 (scope, throw after rows, P3, shape, caps) and the checks (7b, audit 0a carried whole, the
  tree claims, D-D9S-2/-3/-4): no other findings.

## Cycle 2 — APPROVE-WITH-DEBT(3) (at `da4fa84`)
**Proof of read:** `plans/RIX-rulings.md`, 502 lines, read top to bottom. The highest entry is at line 493:
"2026-11-14 — D9S-5b: a refused batched subscribe never delivers".

**(1) B1 is closed, and the gate is sound.** Ruling :493 binds exactly what rules 2–3 do: rows are discarded uncounted
and no closing is sent. It also narrows :478 to accepted subscriptions, which is where L9's "accepted" comes from.
- *No stall.* `registered_` is written and read under `mu_`, and its setter notifies, so the Loop cannot miss the
  wake-up. Every success passes `:450`, the only `return` being `:458`. Every throw from `:422` to `:449` reaches
  `StopOnThrow`, whose unregistered `Stop` wakes the Loop to clear and end. Inside a delivery, `:136-140` detaches and
  the Loop still ends. A deadline reached while gated closes onto the queue and resets `has_deadline_`: no spin.
- *No reordering.* There is one FIFO with one consumer. The gate only delays the head of the queue. L12 holds.
- *Not a third law change.* No law in d7 or d9 promises a batch before `Subscribe` returns. d7 §5's "rows can arrive
  before `Subscribe` returns" is about rows reaching `AddRow`, and it stays true. L13 holds, because nothing new waits.
  L2 needs no clause: it says "The exceptions are L1's" (d7:95). The only text that changes is tier-3 prose at
  `binding.h:1060,1064`, and its new wording promises more than the mechanism gives (DEBT-5).

**(2) F2 under law (b): sufficient.** F2 asserts the whole promise externally. Its green is deterministic, because any
call to a refused sink fails it. Under m2 it is red on **every** attempt: an idle refused close with `registered_` set
queues an empty CLOSING. That gives the closing half an external, deterministic red. Only the caught-rows half takes
its red from D7-2, which is d7 L1's own cited proof at d7's public surface; the shim adds no code on that path. So no
promise rests on a supplementary leg alone. G1 is a d7 test, not an external one. Close wording is owed (DEBT-4).

**(3) DEBT-1 and DEBT-2 are applied.** DEBT-1: brief :8-9 and :40-42; design :22 and :121-123. DEBT-2: F1 cell (d);
d9 L15's clause marked `[built]`; D18/D19 "recording the rulings only". DEBT-3 is still open for the PM, and has
grown: `D9S-locked-decisions.md:30` names only the fPIC exception, and now owes :478 (L6/L9) and :493 (L1).

**(4) 7b fits; no rung split is needed.** Priced from the work: one flag and its setter, the call site, the `Close`
condition, the Loop gate and clear, and rule 3. That is about +20/−4 in one production file, and the shim is 0, against
a ceiling of 250. Tests and doc text are not counted (ladder :15). Even at double that figure, no split is needed.

**(5) Caps.** The design is at its cap (147/150) but not padded. The cap costs precision in one place: "no batch
before Subscribe returns" squeezes "no batch before registration" (DEBT-5). The brief is 52/60 and not cap-sized.

### DEBT (register, D9S-5b)
- **DEBT-4: m4 is an equivalent mutant.** Rule 2's Loop clears the queue whenever a batcher stops while unregistered.
  A closing that m4 lets an unregistered `Stop` queue is therefore cleared, and D7-2's `max_rows` 100 cell stays
  green. *Fix:* delete rule 3 and m4. One mechanism then remains, and m3 (today's Loop restored whole) kills both D7-2
  cells. Or keep rule 3 and declare m4 equivalent. At close, report F2 as the external proof, with its measured
  pre-change red rate (even if 0) and m2's deterministic red. Do not report it as "a guard, with D7-2 carrying the red".
- **DEBT-5: the header promises more than the gate gives.** `registered_` is set at `:450`, inside `Subscribe`, and
  the shim writes `*out_id` after the call (`binding.cpp:913`). So `on_batch` can run before the call returns and before
  `out_id` is written. A binding that trusts "no batch before the call returns" and reads the id from its `ctx` races,
  with no signal. *Fix, in the code commit:* "no batch is delivered unless the call succeeds. The first may arrive while
  it is returning, before `out_id` is written: correlate by `ctx`." Apply it to `binding.h` :1060-1064, the C++ hpp and
  README, and the design and brief risk lines. The C++ phrase the design quotes is not in the hpp or README, so add it.
