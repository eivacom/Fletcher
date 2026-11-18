# D7O-3 — conformance review (step 4a), cycle 1

**Verdict: CONFORMS — ISSUES(2, 0 blocking).** Diff `957a514..c18e95e` (working tree clean apart from the
untracked 4b file) against `plans/D7O-3-any-row-count.md` (approved `957a514`, APPROVE-WITH-DEBT(4)), the
brief, D-D7O-1..6, d7 §1/§3, register BC-3/BC-8, ladder rung 4 and the sweep, and the plan's D7O-3 row.
**Proof-of-read:** `plans/RIX-rulings.md` read top to bottom: **295 lines, 24 entries, last heading
"2026-11-12 — D7O-2: the deadline definition says some windows have none"**. The diff contradicts no
ruling. No tier-1 collision, no tier-2-vs-tier-1 conflict, no principle-vs-detail conflict for the owner.

## Answers to the brief's pressure points (verified in the tree, not from the implementer)
- **No reset path survives.** `SetSchema` (`cpp:77-88`) returns on a constructor throw. It calls
  `PreSize()` only after the decoder exists. `git grep "decoder_.reset|decoder_ = "` finds only the
  constructor assignment (`cpp:83`). `Loop`'s end (`cpp:290`) is the only release. Both Files-to-delete
  blocks are gone. The single `Reserve` call is `cpp:222`.
- **The bound neither leaks nor re-checks.** `kMaxPreSizeRows` is read only in `PreSize`. `max_rows_` is
  read at `cpp:38/42/45` (the one range check), `:119` (the window compare, unchanged) and `:222` (the
  hint) only. Nothing refuses or closes on the bound, and rung 4 (c) guards that from outside. D-D7O-4 holds.
- **Header.** `hpp:124` says "Any other value is used as given" again (D7O-2 4a N1 is discharged). `hpp:78-81`
  carries the brief's two disclosures: room is set aside up to 65 536 rows, then follows the rows received
  (brief "Disclosed"); a window that outgrows memory is lost and counted (brief "Risks"). See N2 for one word.
- **README** `:79-80` has the design's one sentence. Its "Anything else is refused" became "A value out of
  range is refused", which the inserted sentence required.
- **Build-state marks are true.** d7 header `:7`, L15 `:251-252` and BC-8 ("sweep `[built]`") are backed by
  green runs: the implementer's log shows 58/58, `heavy` 2/2 and fastdds 4/4, with all three D7O-3 tests passing.
  L15 is `[built]` clause by clause. **Keep it scoped:** do not collapse it to a bare `[built]`. L15's binding
  clause ("a binding passes the refusal through") is still unmarked on purpose, because C#'s copies
  survive until the switch round (BC-8 copies; D7O-1 4a record).
- **The allocation residue is recorded honestly.** L8 `:165-169` says "One residue is open". Findings (d)1
  is headed "Reported at D7O-3, not fixed". Q8 asks a question and claims no fix, and BC-3 records a
  "Registered gap". None of them claims a fix, and L8's letter ("an undecodable schema is permanent")
  still holds, as the design review's Point 3 also found.
- **Design debt landed.** DEBT-1/2 landed in the brief at `957a514`. DEBT-3 landed as
  `AFailedPreSizeDoesNotMakeTheSchemaUnusable`, matching the review's cell. DEBT-4 (a) landed as BC-3's gap,
  Findings (d)1 and Q8, (b) as BC-8's clause and (c) as Findings (d)2.
- **Scope.** No existing test was touched (the test diff is a pure append). Nothing changed under
  `arrow-bridge/`, `c-abi/` or `dotnet/`, in this stage or across the round (`063ab3b~1..c18e95e`). There is
  no new public surface. The production overage is comments: code-only lines are +14/−13.

## N1 (non-blocking): a clamp is ruled out by convention and a guard, not by the type
- The design's "Corner cases forbidden", **Rung 1 (unrepresentable)**, lists "a clamp ... disguised as the
  bound" and says "`max_rows_` is never rewritten". In the code `max_rows_` is a mutable member
  (`cpp:301`), so a later `max_rows_ = std::min(...)` would compile. Only rung 4 (c) would catch it.
- The code meets the design's own mechanism (never rewritten, plus guard (c)), so this is not drift from
  the sketch. The rung-1 label is stronger than what the code guarantees.
- **Fix (one token each, no line delta):** `const int64_t max_rows_;` (and `const` on `timeout_`). Both are
  set only in the constructor's initializer list, and copy and assignment are already deleted.

## N2 (non-blocking): the header's "lost whole" says more than the code does
- `hpp:80-81`: "A window that outgrows memory before it closes is lost whole and counted". When an
  `Append` fails on allocation, `TryAppend` → `DiscardPending(1)` (`cpp:170-171`, `:181-188`) counts the
  rows held so far, plus the failing row, but the window stays open. Later rows in the same window
  are delivered in its batch. Only a failed `Finish` (`cpp:242-246`) loses the window whole.
- The word "whole" comes from the design (`:128`, "dropped whole") and DEBT-2. The brief, which the owner
  read, says only "is lost and counted". d7 §2 defines a window as everything since the previous flush,
  so "whole" is untrue under the domain's own term.
- **Fix (docs only):** "...when memory runs out while a window fills, the rows it holds are lost and counted
  in `rows_dropped`." Or drop "whole" to match the brief. Non-blocking: the error overstates loss, and the
  count is right.

## Round definition of done (`plans/D7O-plan.md:52-59`): what is still missing at close
- Tracker: D7O-3 still reads ⚪, although the item is in step 4. It should read 🔴 now and 🟢 at close.
- Evidence ledger: there is no D7O-3 row yet. The runs exist only in the implementer's log
  (`scratchpad/d7o3_full.log`: 58/58, heavy 2/2, fastdds 4/4). The PM must check them and record them.
- L4, L5, L15 and BC-8 read `[built]`, §2's Deadline was amended at D7O-2, and nothing changed under
  `c-abi/` or `dotnet/`: met.
- **Not in the DoD, but owed at round close:** the owner note on line-estimate overruns that the D7O-1 log
  requested. D7O-3 makes it three of three items more than 50 % over on declared adds (+26 vs +16).
  The design-debt ledger also needs reconciling (see RECORD).

## RECORD
- RECORD: d7 Mode line `:9` lists the `cpp:N` shifts of D7O-1 and D7O-2 but not D7O-3's (about +1 after
  `SetSchema`, about +11 after `ArmTimer`, about −6 after `Close`'s pre-size). L8's "Holds at" (`cpp:65-77`, `:67`), which
  this stage edited, is now `cpp:77-88` (`:79`). L15's `cpp:34-47` is `cpp:35-48`.
- RECORD: L8 `:165` "the `BatchDecoder` constructor alone decides" reads against the bullet above it (an
  import failure decides too), and BC-3's new clause does the same. Suggested: "for a schema that
  imported, the constructor alone decides".
- RECORD: the design's Docs bullet (`D7O-3-any-row-count.md:92-94`) still calls for "one disclosure
  sentence". It also cites brief "Decision 1/2" (`:34`, `:94`, `:131`, `:231`, `:260`), but the brief now has
  no decisions, only disclosures (`957a514`).
- RECORD: `design-debt.md:830-833` (D7O-3 DEBT-1..4) and `:826-827` (D7O-2 DEBT-2, the BC-4 site text,
  landed at `7a0cc64`) are not struck, though they have landed.
- RECORD: production +26/−16 against the declared +16/−10. Code-only is +14/−13; the rest is comments.
