# D7F-1 — Stage Brief (2026-11-10)

**In one sentence:** a subscriber whose rows arrive without a usable schema is now told, when it
unsubscribes or is destroyed, exactly how many rows it lost. Today that count is silently discarded.
**Forcing test:** `NoUsableSchemaDropsReportedAt{Unsubscribe,Destruction}` and
`SchemaDeclaredAfterSubscribeDropsReportedAt{Unsubscribe,Destruction}`. They prove that 3 rows sent
to a subscription that never gets a usable schema produce one closing report: no batch, 3 rows lost.
They run on the mock provider (topic never declared) and on the in-process provider (topic declared
only after the subscriber joined).

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batched-subscribe header and README text on when a batch is "null" | CHANGED | Takes your approved wording verbatim: null means no batch could be built (no usable schema yet, schema rejected, or finishing failed), and the lost-row count covers the whole window in every case |
| Batched-subscribe behaviour at close | CHANGED | A schema-less subscription that received rows now gets one closing report instead of nothing |

No public type, method or wire shape changes. New public surface: 0.

## Deleted
- A second, private way of counting a lost row. Every incoming row now goes through one counting
  path, so the "no schema yet" loss and the "schema rejected" loss are reported the same way.
- A row-limit flush at schema arrival that could never fire. Nothing replaces it, and no behaviour
  is lost.
- The check that discarded the loss count when no schema had arrived. Its behaviour, silent loss,
  is gone. That is the fix.
- No tests are deleted or rewritten.

## Corner cases forbidden vs handled
**Forbidden** (cannot happen by construction):
- one row counted twice, or counted along two different paths;
- a "null" report that means "nothing happened". A null closing report always carries at least
  one lost row (guard test `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose`);
- an idle schema-less subscription getting a report at close;
- a second closing report.

**Handled:** none.

## Decisions for you
None. Your rulings already settle what this stage needs: "Fix both", the null-batch wording you
approved, and laws L1, L6, L7 and L9. Of the mechanisms, the design takes the one that deletes code
rather than adds it.

## Risks accepted / debt carried
- **Interim, for one stage.** Until D7F-2 lands, a subscription that stays *open* without a usable
  schema learns its loss only when it closes, not at each timeout. D7F-2 removes that wait. The
  round ships with both stages, so users never see the interim.
- The plan estimated 30–50 added lines. The stage is net negative, because the "no batch" report
  already existed and was only being blocked.
- Whether a schema is usable is still decided in two places (import, then decoder). Both now lead
  to the same report. Merging them is an open domain question (d7 Q7), not this round's work.

## Numbers
Declared net lines: +10 / −25 (production) · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
