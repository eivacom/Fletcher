# D7F-2 code review (step 4b)

Scope: `git diff 7916105 HEAD`, covering `subscriber_arrow.cpp`, `subscriber_arrow.hpp`, `README.md`
and the four new tests. Static review only: no build tree was available to run the tests.

**Verdict: nothing blocking.** The timer change is the single line the design promised. The idle
path waits on `cv_` with no deadline, so there is no busy loop, and `Flush` returns early when
nothing is pending (L6). Deleting the notify in `SetSchema` is safe because nothing waits on schema
arrival any more. Lock order is flat: `SubscriberArrow::mu_` is never held while the batcher's
`mu_` is taken, and the `call_once` body takes only the batcher's `mu_`, which the timer never holds
while it touches the once-flag. A late schema that races a deadline either closes the window as a
null batch first, or lands while `cb_` runs with `mu_` released and assembles the next window.
Either way each row is counted once, in `AddRow`. With `Unsubscribe` during a timer null flush, the
closing flush finds nothing (the timer took the window), and `Stop` joins only after the in-flight
callback returns. DEBT-3 is closed for real: `AcquireCodec` returns owners and nothing is looked up
a second time.

## should-fix

**S1. The forcing tests' W1 margin is 100 ms from `Subscribe`, not from the first row. Confidence:
medium.** `last_flush_` is set when the batcher is constructed, so W1's deadline is
`max(row1 + 1 ms, ctor + 100 ms)`. If 100 ms passes between `Subscribe` and the third
`PublishDirect` (thread start, or a CI preemption), the margin collapses to 1 ms. W1 then splits,
and `ExpectNullTimeoutReport(deliveries[0], 3)` fails on `rows_dropped`. The InProcess variant has
the same exposure, with `CreateTopic` also inside the window. The late-schema phase was already
made stall-proof. Make W1 and W2 stall-proof in the same way: wait until the null `kTimeout`
reports add up to 3 dropped rows (then 4), rather than asserting exactly one delivery. Raising
`kDeadlineTimeout` to around 500 ms would only shrink the risk.

**S2. The "schema decided" state lives in three places, and the caller enforces `SetSchema`'s
at-most-once rule. Confidence: low-medium.** The three places are `schema_set` (once-flag),
`schema_decided` (atomic) and, implicitly, `decoder_`. `SetSchema` documents "called at most once"
but does not refuse a second call; L8 currently rests on the lambda's once-flag. Simpler: keep a
`decided_` bool in the batcher under its own `mu_`, and have `SetSchema` ignore any call after the
first. The door then forbids a second decision, and the lambda can drop the heap-allocated
once-flag. Keep the atomic only if the per-row lock-free fast path is measured to matter.
`AddRow` takes the batcher's `mu_` on every row anyway.

## nit
- N1 (med) `TopicCodec::import_failed` is a field of the map's value type that no map entry can ever have set. Return a separate `{TopicCodec, bool}` result from `AcquireCodec` instead, so the flag cannot appear in `codecs_`.
- N2 (med) `catch (...)` around `ImportArrowSchema` also catches `bad_alloc` from `DeepCopy`, so a transient allocation failure becomes a permanent "undecodable" for the subscription (L8). The loss is still reported. Catch `PubSubError` as the decision and treat everything else as undecided.
- N3 (low) The per-row overload now copies two `shared_ptr`s per row under the process-wide `mu_`: two extra atomic read-modify-writes on a path that already took the lock.
- N4 (low) An `Unsubscribe` racing the timer's null `kTimeout` callback can run `cb_` concurrently on two threads when rows arrived during that callback. This already happened before this change (L12, D7F-3), but schema-less windows now reach it too.
- N5 (low) The idle check (sleep 3×timeout, still 1 delivery) catches spurious deliveries but not a spinning timer. That is acceptable for L6 as worded.

## RECORD
RECORD: cpp:360-363 still says the batcher gets the schema "from the first decodable sample … without dropping the first window for want of a schema". It now gets it from the first row that brings a decidable schema, and the forcing test drops whole windows before it arrives.
RECORD: in plan §3, the snippet decides on `found.arrow_schema || schema`, and the text says `AcquireCodec` returns empty when import fails. As built, a null import is undecided and a throwing import sets `import_failed`. The plan should record this deviation.
