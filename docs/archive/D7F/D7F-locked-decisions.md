# D7F — Locked decisions (digest)

The digest every architect, reviewer and implementer in round D7F must honour. The laws themselves
are in [docs/domains/d7-batch-assembly.md](../docs/domains/d7-batch-assembly.md) §3, and the owner's
words are in [RIX-rulings.md](RIX-rulings.md); both win over this digest. A proposed deviation from
any line here is stop-and-ask.

- **D-D7F-1 — The round fixes L1 and L12 and implements L9 as revised.** L1: no lost row is ever
  silent. That covers rows lost before any usable schema arrives, and the over-budget row lost
  around an in-callback `Unsubscribe`. L12: batches reach the callback one at a time, in window
  order. L9: after an `Unsubscribe` from inside a callback, windows already queued are delivered,
  then `kClosing`, then nothing. ("2. Fix both"; L9 rulings "Deliver after it returns" and "Queued
  batch, then closing".)
- **D-D7F-2 — Every batch callback runs on the subscription's batch thread** (domain 7 D10). The
  transport's delivery thread never runs or waits on a callback. Full windows queue in order. There
  is no back-pressure; that cost is accepted, so do not add a bound or a blocking queue without a
  ruling.
- **D-D7F-3 — A null batch means "no batch could be built for this window"** (domain 7 D9). The
  header (`subscriber_arrow.hpp:103-111`) and README (`:94-98`) take the ruled wording verbatim:
  "batch is null when no batch could be built for this window: no usable schema has arrived yet,
  the schema was rejected (permanent), or finishing the batch failed; rows_dropped counts the
  window's rows in every case".
- **D-D7F-4 — The ~256 MiB test is labelled `heavy` and is not in the default run** (domain 7 D11).
  It is excluded through ctest labels and `ctest -LE heavy` in `pubsub-arrow/conanfile.py`, never
  by disabling the test.
- **D-D7F-5 — Public surface unchanged.** `BatchOptions`, `BatchStatus`, `RecordBatchCallback` and
  the `Subscribe` / `Unsubscribe` signatures stay as they are. What changes is the thread the
  callback runs on, when it runs, and what a null batch means, all ruled above.
- **D-D7F-6 — No index logic.** The row index (domain 7 L14, domain 8) belongs to round RIX.
  D7F adds no index arrays, counters or attachments handling.
- **D-D7F-7 — One window closer, one count entry, one delivery site** (census C1, C2, C4 in
  [D7F-pre-round-audit.md](D7F-pre-round-audit.md)). A second place that delivers to `cb_`, counts a
  drop, or closes a window is a defect.
