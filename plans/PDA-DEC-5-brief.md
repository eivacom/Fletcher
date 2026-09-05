# PDA-DEC-5 — Stage Brief (2026-09-02)

**In one sentence:** the built-in loopback transport stops being wired in by hand
and becomes selectable by name through the one call every future protocol —
built-in or loaded — will use; the gateway switches onto it.
**Forcing test:** `Registry.InProcessResolvesAsABuiltIn` — asking for `inprocess`
yields a working transport and nothing else knowable about it; plus the gateway's
battery over `--provider inprocess`, unchanged.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Selecting the loopback by the name `inprocess` | NEW | first protocol reachable by runtime selection |
| Loopback setting `schema_carriage` (`as_declared` \| `carried`), in that protocol's own settings text | NEW | its one knob, moved where every protocol's settings now live |
| Choosing that mode at compile time, and the gateway's own list of valid `--provider` values | **DELETED** | one way, not two; the registry is the single list |

## Deleted
- The gateway's "is it fastdds or inprocess" construction → one selection call;
  its own name check and message → the registry's two refusals.
- The compile-time way to pick the loopback's schema mode → the setting. No test
  is deleted.

## Corner cases forbidden vs handled
**Forbidden:** picking the loopback's schema mode by any route other than its
settings text; a half-configured transport existing at all; the gateway asking
which kind of protocol it got, or keeping its own list of names. **Refused:** a
misspelled setting, a duplicate registration, an unknown name, a file path in a
build that cannot load files. **Handled:** the loopback ignores the two generic
settings (payload size, domain) as it documents today, and tolerates a Windows
line ending so one text means the same everywhere.

## Decisions — both PM-decided, NOT asked (2026-09-02)
*Neither reached the owner: (1) is already answered by spec §4.2 and the 2026-08-31
configuration ruling, as the design review established; (2) has no viable alternative,
since keeping today's message requires the gateway to retain the very name list this
item removes. Both taken as recommended. Recorded here so the reasoning is visible.*

1. **Should the loopback read its own one-line settings text, given that Fletcher
   itself parses nothing?** Options: (a) yes — ~25 lines inside the protocol, no
   library · (b) no — publish two protocol names, one per mode.
   **Recommendation:** (a) — the shape every other protocol uses next; (b) turns a
   setting into permanent vocabulary. **Default:** (a).
   *Background: decision 8 bans a config parser in Fletcher; this reader belongs
   to the provider, which happens to ship inside `fletcher-pubsub`.*
2. **A bad `--provider` value now gets a different message. Acceptable?** Options:
   (a) the registry's wording — a typo lists the protocols this build has, a
   file-path-shaped value says this build cannot load drivers · (b) keep today's
   text, which needs the gateway to keep its own name list.
   **Recommendation:** (a) — (b) is a second list that will drift; the exit code
   (2) is unchanged either way. **Default:** (a).

## Risks accepted / debt carried
- `--domain-id` stays silently ignored with `--provider inprocess`, as documented
  today; refusing it would be a behaviour change this stage forbids itself.
- The gateway harness does not rebuild the gateway, so a stale binary passes the
  old battery silently — covered by two refusal cases no old binary can pass.
- Fast DDS keeps its compiled-in settings one more stage (PDA-DEC-6 moves it).

## Numbers
+270 / −60 · new public surface: **0 at its strictest** (+2/−2; the earlier "net −1"
charged the retired constructor but not the added one) · design cycles: 1/2

---
*As landed (2026-09-02):* +626 / −78 (code + spec, excl. `plans/`) vs declared +270/−60;
both reviewers counted it — prose and mutation evidence, no scope creep. Surface **0**
at its strictest, as declared once corrected. 1 design cycle, **2 fix cycles**, 3
implementer launches. Two things the brief did not predict: the document reader's own
tolerance rules had no guard (17/17 green under either mutation), and memoising one
built-in per registry passed **19/19 and 80/80** — the property PDA-DEC-8 depends on.
