# PDA-DEC-2 — Stage Brief (2026-09-01)

**In one sentence:** "we do not copy your data" stops being prose and becomes a test
that fails when it stops being true — rows and attachments, outbound and inbound.
**Forcing test:** `CopyAccounting.PublishAndReceivePerformNoPayloadCopies` — the bytes a
subscriber gets are the *same bytes, in the same memory*, the publisher wrote.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Ability to read where an outgoing message buffer currently sits | NEW | Nothing can check "were these bytes moved?" without it. |
| A second suite, `CopyAccounting`, in the existing conformance harness | NEW | The standing zero-copy guard both later ABI rounds inherit. |

## Deleted
Nothing — add-only guard; no existing test measured copying, so nothing is superseded.
No configuration key, wire format or operator-visible behaviour changes either.

## Corner cases forbidden vs handled
**Forbidden:** a copy check across processes or off-thread (it is address identity, so
unbuildable there); a protocol declaring its own "expected copies"; a silently skipped
check; mistaking corrupted bytes for copied ones; passing because *nothing* arrived.
**Handled:** *a send buffer that grows mid-message* — counted, not failed, since the spec
permits growth (Decision 1); *the one copy the current attachment type forces on receive*
— pinned at exactly one, named for the next stage, whose job it is.

## Decisions for you   (3)
1. **When a large message is written into a send buffer that runs out of room, the
   already-written bytes move. Is that a violation?**
   Options: (a) fail it — bans growable send buffers, and the in-process loopback is
   non-conforming until reworked · (b) permit it, publish the number, fail every *other*
   movement. **Recommendation / default:** (b) — no growable buffer reaches zero for an
   arbitrary message size, and the check that catches real regressions is unaffected.
   *Background: your 2026-08-31 ruling says a copy anywhere is a violation; spec §3.1
   clause 1 says bytes "must not move ... except inside a refill". Raised, not designed around.*

2. **How much should this guard claim about DDS?**
   Options: (a) run the receive-side check against Fast DDS now · (b) scope it to the
   interface, stating in the README that it proves nothing about a transport's internals
   · (c) enable DDS shared-memory receive to test it now. **Recommendation / default:**
   (b) — DDS zero-copy receive does not exist yet and its enabler is off, so (a) measures
   the copying path and calls it evidence; (c) reopens work assigned elsewhere.
   *Background: receive-side data-sharing is off by default; that defect is owned by
   PDA-ABI-7 per your 2026-09-01 ruling.*

3. **Pin the known receive-side copy so the next stage must come back and update this
   guard?** Options: (a) pin at one — removing it turns this test red · (b) report only.
   **Recommendation / default:** (a) — it is the one deficiency the next stage exists to
   remove, and silence is how such a fix gets forgotten or half-landed.

## Risks accepted / debt carried
- Green is evidence about the interface, not about any protocol's internal copying —
  written into the harness README, not implied. The guard runs only where publisher
  and subscriber share a process, which is where the interface itself lives.
- Answering (a) to Decision 1 adds ~40 lines of loopback rework this round.

## Numbers
Declared net lines: +560 / −5 · new public surface: 1 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
