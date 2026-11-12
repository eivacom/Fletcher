# D7O — Locked decisions (digest)

The digest every architect, reviewer and implementer in round D7O must honour. The laws themselves
are in [docs/domains/d7-batch-assembly.md](../docs/domains/d7-batch-assembly.md) §3, and the owner's
words are in [RIX-rulings.md](RIX-rulings.md) (2026-11-12 entries); both win over this digest. A
proposed deviation from any line here is stop-and-ask.

- **D-D7O-1 — A batched subscription may have no timeout** (domain 7 D14, L4, L5). Its windows close
  only on `max_rows`, the offset budget or closing, and it never receives `kTimeout`. How "no
  timeout" is spelled in `BatchOptions` is the architect's design, and is the round's one new piece
  of public surface. It must be expressible by a C ABI caller as a plain value (the shim round
  passes it through), so a spelling that only C++ syntax can express is a design defect.
- **D-D7O-2 — A deadline is never earlier than its timeout says** (L5). A finite timeout too long
  for `steady_clock` to represent means no deadline, never an early or wrapped one. The overflow at
  `last_flush_ + timeout_` (`subscriber_arrow.cpp:187`) is removed. (Confirmed by the owner 2026-11-12, LC-1.)
- **D-D7O-3 — Out-of-range options are refused, never clamped** (L15, D15). `max_rows < 1` or a
  negative timeout throws `PubSubError(kInvalidArgument)` from the batched `Subscribe` **before the
  batcher's thread starts and before any provider subscription exists**. A zero timeout is accepted,
  and the 1 ms floor applies. The clamp at `subscriber_arrow.cpp:36` is deleted.
- **D-D7O-4 — One answer to "are these options acceptable?"** (register BC-8). The batched
  `Subscribe` is the only place that decides. No second check is added anywhere in C++.
- **D-D7O-5 — Loss with no timeout is reported at closing** (audit F2). With no timeout, a window
  holding only lost rows is delivered when closing ends it. L1 still holds, and the header and
  README say so.
- **D-D7O-6 — Nothing else changes.** `BatchStatus`, its reasons, `RecordBatchCallback`, the
  per-row `Subscribe`, the threading of D7F (D10) and every other law stay as they are. BC-2's
  per-row gap, BC-7, Q4 (`kCapacity`) and anything in `c-abi/` or `dotnet/` are out of scope (owner
  rulings 2026-11-12).
