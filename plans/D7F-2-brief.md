# D7F-2 — Stage Brief (2026-11-10)

**In one sentence:** a subscriber whose rows arrive without a usable schema is now told, at each
timeout while it stays subscribed, how many rows it lost. Today it learns this only when it
unsubscribes. If a usable schema turns up later, normal batches follow, and rows received plus rows
reported lost still equal rows sent.
**Forcing tests:**
- `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` (mock provider). 3 rows, then
  1 row, sent before the topic has a schema, give two timeout reports ("no batch, 3 lost", then
  "no batch, 1 lost") and nothing during a quiet pause between them. Then the schema is declared,
  3 good rows arrive as normal batches in order, and the totals add up to 7.
- `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` (in-process): one report per window.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batched subscribe, before a usable schema arrives | CHANGED | Lost rows are reported at each timeout, like any other window, not only at close |
| Header/README sentence on when a timeout batch comes | CHANGED | It said "timeout after the batch started filling". The confirmed law (L5) is "timeout after the previous delivery, at least 1 ms after the window's first row". The text now states L5, schema or not |

No public type, method or wire shape changes. New public surface: 0.

## Deleted (no tests are deleted or rewritten)
- The wait that held a timeout report back until a schema arrived. Silence while subscribed is gone;
  that is the fix.
- The internal "schema has arrived" flag. No replacement: what it answered is already known elsewhere.
- A second lookup of the topic's schema, which a badly timed unsubscribe elsewhere could make miss,
  leaving a subscription schema-less for life. Replaced by keeping the result of the first lookup.

## Corner cases forbidden vs handled
**Forbidden** (cannot happen by construction):
- staying silent at a timeout after losing rows;
- reporting a window twice, or reporting a window in which nothing happened;
- losing the schema for good because another subscriber left at the wrong moment;
- **a schema that arrives but cannot be read is final**, like one the decoder rejects (law L8).
  Today it is retried on every row. You would see a difference only if a transport sent an
  unreadable schema and later a readable one to the same subscription. None in the tree does.

**Handled:** a timeout that falls between the lost rows and the schema's arrival gives one
"no batch, N lost" report, then normal batches. Forbidding it would mean holding the window open
past its timeout, which is the silence this stage removes.

## Decisions for you
None. Settled by "2. Fix both" (L1) and the confirmed laws L5 (cadence), L6 (loss is reported,
idleness is not), L8 (rejection is permanent) and L13 (subscribing never waits for a schema).

## Risks accepted / debt carried
- Lost rows never count towards the row limit, so with a small row limit a schema-less
  subscription still hears of its loss at the timeout, not sooner. This is already the rule for
  undecodable rows, and it is the ladder's "one report per window".
- "Unreadable schema is final" may have no test (it needs a hand-made malformed schema).
- Usability is still decided in two steps (read, then decode), now with one permanent outcome.
  Merging them is open question d7 Q7.

## Numbers
Declared net lines: +20 / −40 (production) · new public surface: 0 · design cycles used: 1/2

---
*As landed (2026-11-11):*
2026-11-11: production +64/−76 (net −12) vs declared +20/−40; adds >50% over, mostly D9 doc wording in hpp/README — disclosed, no owner note. Tests +285. Fix cycles 1 (4a blocker: unimportable-schema guard cell). DEBT-6 (decided-state in three places) carried to D7F-3.
