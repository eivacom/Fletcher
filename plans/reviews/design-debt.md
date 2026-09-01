# Design-debt register

Debt accepted at architecture review, handed to the implementer. Full text lives
in the per-item review file; this register is the index, not a copy.

## PDA-DEC-1 — conformance suite (APPROVE-WITH-DEBT(6), cycle 2 of 2)

Review: [PDA-DEC-1-design-review.md](PDA-DEC-1-design-review.md).
Cycle-1 DEBT-1..11 were folded into the design revision (`154a1a2`) and are closed.
Cycle-2 items below are open and owed by the implementer.

| Id | Owed | Where |
|----|------|-------|
| DEBT-C2-1 | **Must land in this PR.** `pubsub/include/fletcher/pubsub/provider.hpp:68` still documents re-declaration rejection as optional ("may reject"); the owner's ruling makes it mandatory. One word plus one Files-to-touch line. | review §DEBT register — cycle 2 |
| DEBT-C2-2 | Key *retention* by provider but leave *schema_mode* per subject, or the promised PDA-DEC-3 handoff collides with rung-1 item 3. | ditto |
| DEBT-C2-3 | State that clause 12 observes overlap and cannot force it. | ditto |
| DEBT-C2-4 | Reuse the conflict comparison already shipping above the seam (`pubsub/src/publisher.cpp:46-78`); note the gateway cannot regress, because `Publisher` never forwards a conflicting or identical re-declaration to the provider. | ditto |
| DEBT-C2-5 | Two `Files-to-touch` entries owed, both one line. (`plans/PDA-decouple-interface.md` is already updated by the PM at `ea5287b`.) | ditto |
| DEBT-C2-6 | Informational, nothing owed — the sparse-checkout gotcha was mis-targeted in the design. | ditto |
