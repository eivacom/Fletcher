# D10C-2 — step 0b new-test audit (light lane)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test in scope. Base `b07eb54`; tests uncommitted, RED, no production code.

**Proof-of-read:** `plans/RIX-rulings.md` top to bottom: 603 lines, 45 `## ` entries, last "## 2026-11-15 — D10C ladder: the domain-7 port tests" (:595). d10 §3 L1, L2 (:123-144), L6 (:217-230), D26, D34, Q11; ladder ground rules (:21-40) and rung 2; `binding.h` :176-187, :264-269, :1013-1015 (read only).

**Yardstick:** d10 L2 "`NativeMethods` and `AbiTypes` are a mechanical mirror of `binding.h`: same names ... Every number the shim hands out (`fl_status`, `fl_origin`, `FL_BATCH_*`, `fl_variant`) has a C# value equal to it, never renumbered"; :551 (3) "origin `Handover` (4) ... C#'s origin enum gains code 4"; D34 "`BatchReason` is renumbered to the C values 1-3"; L1 (handshake, so no C# range check, L3); L6 "learns its build by asking, never from a missing symbol".

**Scope and classification:**
- `AbiNumberTableTests.EveryNumberTheShimHandsOutHasTheSameValueInCSharp` (`AbiNumberTableTests.cs`:49), rows `fl_status` (declared guard), `fl_origin`, `FL_BATCH_*`, `fl_variant` — LAWFUL. Asserts exactly L2's four tables, both directions (no C#-only member: "authors none of it"). Requires no C# range check (L3/L1 respected) and touches no `default(BatchReason)`. Missing header fails, never skips.
- `ErrorTests.OriginFourIsHandover` (`ErrorTests.cs`:108) — LAWFUL. Asserts the name `Handover` and value 4 of :551 (3)/D26 and status `Internal` (d9 D32). A leg on internal `Errors.ThrowIfFailed`, as the ladder's rung-2 row itself names it.
- `AbiNumberTableTests.BindingVariantAnswersADefinedMember` (:88) — LAWFUL leg. Asserts only "defined", not which build: it does not pre-empt rung 4's subject.
- csproj `<None Include=".../binding.h" Link="binding.h">` — LAWFUL. Reads d9's header, edits nothing.

**Law (b) — the internal reflection, judged:** the forcing test's `fl_variant` row reflects on the internal `FlVariant` type (it does not call `NativeMethods`; `typeof(...).Assembly` runs no initializer). That is acceptable *because L2 is itself a law about the internal mirror* ("Holds at: `NM:1-43`"), so a structural mirror check is its natural proof, and ladder rung 2 lists "and the variant values". **But the brief's claim (Risks, :63) that "the variant's behaviour is proven from outside at rung 4 and rung 7" is only half true:** rung 4 lists `fl_binding_variant` as "**Observation only**", and rung 7 proves batch availability, not the variant's numbers. L6 also says C# "does not refuse from the variant query itself". So **while `FlVariant` stays internal, this row is the SOLE proof of the variant's number equality.** That is not forbidden: no external behaviour depends on those numbers yet. **Note for the PM (non-blocking):** correct the brief's wording, and have rung 4's design re-check this if it makes the variant public or routes a decision through it.

**New VALUE subject beyond rung 2:** none. Status 0-10, origin 0-4, reasons 1-3 and variant values are all rung 2's. The legs add no value.
**Scheduled restoration:** none. **Tier-1 disagreement:** none.
**Laws vs build state:** L2 becomes tested here (closes Q11). No `[designed-not-built]` law (L6) is asserted as built.
