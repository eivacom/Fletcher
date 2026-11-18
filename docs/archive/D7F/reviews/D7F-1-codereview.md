# D7F-1 — Code review (step 4b)

Diff `b2ef5ae..a2c8b97`. Scope: `subscriber_arrow.cpp`, `subscriber_arrow.hpp`, `README.md`, F1–F4/G1.
Verdict: **no blocking findings**. 0 blocking / 2 should-fix / 3 nit. Tests read, not executed (no build tree here).

Checked and clean: lock order (the lambda takes `SubscriberArrow::mu_` and the batcher's `mu_` one after
the other, never nested; `call_once` → `SetSchema` holds no other lock; `Flush` still drops `mu_` around
`cb_`). `lock_guard` in `SetSchema` is fine because nothing there unlocks any more. The timer cannot reach
`Flush` without a decoder unless `ready_` is set, and `ready_` with no decoder means a rejected schema,
which behaves as before. `Stop` is still idempotent through `stopped_`. Destructor order is unchanged
(batchers stop, then `subscriber_.reset()`). Conservation holds: a row with no decoder is counted once
at `AddRow`, and the count leaves only through `Flush`. F1–F4 are deterministic: the Mock and the
in-process provider both deliver synchronously on the publishing thread, so the 10-minute timeout and
`max_rows=100000` can't fire, and nothing is in flight at `Unsubscribe` or destruction.

## Findings

**S1 — should-fix (medium): `SetSchema(nullptr)` should be impossible, not handled.** Inside the
`call_once`, the lambda looks `codecs_[key]` up a second time (cpp:358-362) after `AcquireCodec` has
already succeeded. In the batched path, `codec` is only checked for null and never dereferenced. If the
entry is gone (the d7 a3 race: another subscription's `Unsubscribe` erases `codecs_[key]` before this
one is in `sub_topic_`), `SetSchema(nullptr)` latches "no schema" for good. Rows are then counted but,
with `ready_` false, reported only at close. Have `AcquireCodec` return the `arrow_schema` it found or
built (or a `TopicCodec*`), and pass that to `SetSchema`, which then takes a schema that is never null.
That removes the second lookup, the `if (schema)` branch and the `ready_ = (schema != nullptr)` coupling.
It is in scope for D7F-2, which deletes `ready_` anyway.

**S2 — should-fix (medium): no test covers the schema-arrives-later transition the fold relies on.**
F1–F4 never get a schema; the older tests always have one. Nothing checks the case where rows are
dropped before the schema and then good rows follow, which is exactly where "no codec ⇒ no decoder" has
to hold. Mock case: 3× `PublishDirect` before `CreateTopic`, then `CreateTopic`, then 2× `Publish`, then
`Unsubscribe`. Assert one `kClosing` batch with `num_rows==2`, `rows_dropped==3` and 2 attachments (the
Mock looks the schema up again on every publish).

## Nits
- N1: `AddRow` cpp:110 `ready_ && decoder_->num_rows()`: `ready_` is redundant after the `!decoder_` early return (D7F-2 removes it).
- N2: `SetSchema` cpp:64 `decoder_.reset()` in the catch is a no-op, since `decoder_` is always null on entry (call_once).
- N3: Possible F5: a row-limit flush (`max_rows=2`) on a schema-less subscription delivers nothing until close. This pins the declared coexistence behaviour so that D7F-2 flips it on purpose.

## Conformance
Matches the design. All three production changes landed as specified. `NoteDropped`, the `ready_` gate and
the `SetSchema` row-limit flush are gone, and none survives. Header and README carry D9's wording. F1–F4
and G1 match the design's table and assertions.

RECORD: hpp:95-98 still promises a timeout flush "since the first row/drop"; until D7F-2 a schema-less open subscription reports only at close (declared coexistence, but the header contradicts the tree).
