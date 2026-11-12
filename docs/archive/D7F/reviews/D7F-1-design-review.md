# D7F-1 — design review, cycle 1 of 2

**Verdict: APPROVE-WITH-DEBT(2).** No BLOCKERs. No STOP-AND-ASK.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: 169 lines, 14 entries. The last entry
is "2026-11-10 — Queued windows are delivered before the closing batch". Every entry was treated as
tier 1. No ruling, law or locked decision (D-D7F-1..7) is contradicted.

**Budget.** (a) The design is 221 lines (cap 300), with 0 new public surface and declared net lines.
(b) The implied work is 0 new types and 0 new verbs. It deletes one method (`NoteDropped`), one gate
(cpp:210) and one dead branch (cpp:69-71), and restructures one lambda (cpp:365-381). I price it at
about −15 net production lines, which matches the declared figure and is far under the 250 ceiling.
No split is needed.

## Verified against the tree (the implementer can rely on these without re-checking)

- **P1 holds.** `Subscriber`'s dispatch passes `schema` straight through to the entry callback.
  Nothing filters a null schema (`pubsub/src/subscriber.cpp:393-420`).
- **P2 holds** (test:50-57). `PublisherArrow::PublishDirect` and `Publisher::Publish` go straight
  to the provider, and neither refuses an undeclared topic (`publisher_arrow.cpp:92-96`,
  `publisher.cpp:95-98`).
- **P3 holds.**
  - `kAsDeclared` is the default (`in_process_provider.cpp:85`).
  - `Subscribe` latches the schema once (`:329-332`), and `CreateTopic` does not update it (`:236-246`).
  - `Publish` delivers `subscription_schema` synchronously under `mu` (`:276-295`). So F3 and F4
    are deterministic: all 3 rows reach `AddRow` before `Unsubscribe`.
- **P4 holds.** `in_process_provider.cpp` is built into `fletcher-pubsub` (`pubsub/CMakeLists.txt:13`),
  which `fletcher-pubsub-arrow` links PUBLIC (`pubsub-arrow/CMakeLists.txt:25-26`).
- **The null batch already exists in the finishing half** (cpp:216-236). Once cpp:210 is gone, a
  window with `ready_` false and drops reaches it with `decoder_` null, so `batch` stays null. In
  that state `atts_` is always empty, because attachments are pushed only after a decoder `Append`.
- **`NoteDropped` and `AddRow`'s no-decoder branch behave the same** (`stopped_` check, `++dropped_`,
  `ArmTimer`). Folding one into the other changes nothing observable except the report at close.
- **Every other existing batched test, and FastDDS `BatchedRecordBatchDeliveredAcrossDdsBoundary`,
  declares the topic before it subscribes.** None of them can gain an extra `kClosing` null batch.
  The step-0a audit's CLEAR stands.

## BC-3: is it lawful to leave `AcquireCodec` returning `nullptr`? Yes, for this stage.

- **Tier.** BC-3 is a census output, so it is tier 3. D-D7F-7 locks C1, C2 and C4, not C3. Ladder
  rung 1 (ladder:42) does not commission the state. BC-3's structural test, "every state ×
  {timeout, closing}", is assigned to stage 2 (ladder:77).
- **What BC-3 requires.** Its principle is that one site answers whether rows are reported, and that
  no state suppresses reporting. After this stage, "awaiting schema", "import failed", "rejected"
  and the a3 race all end in the same no-decoder branch, and all are reported at close. Every
  `ImportArrowSchema` failure is therefore reported.
- **Why the missing state hides nothing.** A topic's schema cannot change from one non-null value to
  another. A conflicting re-declaration is refused (`in_process_provider.cpp:219-226`). So an import
  failure is effectively permanent, and the missing state hides no L8 case.
- **What is left to decide.** The mechanism detail, "becomes a state", is unbuilt. So is "fed by one
  usability decision", which waits on d7 Q7 and which no D7F rung commissions. Both are tier-3
  planning text. Neither is a deviation, but the register must say who owns them (DEBT-1).

## DEBT

**DEBT-1: BC-3 is missing from the design's register edits.** The design updates BC-2 and BC-4 only.
BC-3 still names D7F-1 for "`AcquireCodec`'s import failure becomes a state". The D7F-2 architect
could read that as already done or as already owed. Fix: add to Documents/Register a BC-3 status
note with four parts:
- cpp:210 reporting gate removed (D7F-1);
- import failure counted through the no-decoder branch (D7F-1);
- state vs decoder-presence, and `ready_`, left to D7F-2;
- "one usability decision" waits on d7 Q7.

**DEBT-2: the proposed d7 L1 build mark over-claims during the declared interim.** d7's violation 1
has two halves: the timer parks forever (cpp:198), and `Flush` discards the count at close (cpp:210).
The ladder cites "L1 (violation 1)" as rung 2's authority too (ladder:43). The proposed mark
("built for … a schema that never becomes usable (D7F-1)") closes violation 1 while the timer half is
still open. Fix: mark it "violation 1: closing half built (D7F-1); deadline half open (D7F-2)", and
D7F-2 flips it. Doc text only, with no effect on code or tests.
