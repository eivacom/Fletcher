# PDA-DEC-4 — Stage Brief (2026-09-02)

**In one sentence:** the protocol stops being a compile-time choice — an application names it
in configuration, hands over that protocol's own settings as a document Fletcher never reads,
and gets a working provider without knowing which protocol it got or where from.
**Forcing test:** `Registry.SelectsByNameWithoutCallerKnowingTheProvider` — a name read at
runtime picks one of two protocols, proved by which one delivers a published row.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The protocol setting: one string, either a built-in protocol name or a driver file path | NEW | Charter requirement (a): choose the protocol at run time. |
| Protocol settings: payload size + domain, plus an opaque document for everything else | NEW | Requirement (b): configure it at run time, without Fletcher learning its vocabulary. |
| The act of making a provider from those two | NEW | One call, identical for built-in and (later) loaded protocols. |

## Deleted
Nothing. What this makes obsolete — the Fast DDS typed settings struct, the XRCE one, the
gateway's hardcoded switch — is retired by the three stages that own those migrations.

## Corner cases forbidden vs handled
**Forbidden:** anything above this seam being *able* to ask whether a protocol is built in
or loaded; a name meaning two things in two builds; two registrations of one name silently
replacing each other; an unknown name resolved by guessing (it fails at startup, naming
what *is* available).
**Handled:** *"payload size not stated"* — Fletcher cannot check a protocol's valid sizes, so
it forwards "unset" and the protocol applies its own default; *a protocol that fails to
start* — reported at startup with a stable cause.

## Decisions for you   (3)
1. **Configuration can name a driver file that nothing can load yet. What does an operator
   see?** (a) it is accepted as a valid selection and fails with a distinct "this build
   cannot load drivers" message, and this stage proves with a stand-in that a real driver
   arrives through the identical path · (b) treat it as an unknown protocol name for now.
   **Recommendation / default:** (a) — under (b) the round's central promise ("the next
   round adds loading and changes nothing here") stays untested prose.

2. **How does one configuration string say "built-in protocol" versus "driver file"?**
   (a) the shape decides — a plain word (`fastdds`) is a name, anything else (`/opt/x.so`)
   is a path · (b) an explicit prefix, `file:/opt/x.so` · (c) two separate settings.
   **Recommendation / default:** (a) — one setting, same rule in every build. (c) is
   disqualified: it lets an application see which kind it has, the thing this round prevents.

3. **Does the XRCE agent's address stay a first-class Fletcher setting, or become a line in
   that protocol's own document?** (a) the document · (b) keep it typed at the seam.
   **Recommendation / default:** (a) — Fletcher keeps exactly payload size and domain.
   Visible cost: XRCE settings that are struct fields today become document lines (and the
   same happens to Fast DDS QoS two stages later).

## Risks accepted / debt carried
- Nothing shipping selects through this yet: the gateway keeps its hardcoded switch for one more stage, by the plan's split.
- Green here says nothing about any transport — selection and configuration routing only.
- A protocol must still be built in to be selectable: the *choice*, not the *availability*,
  moves to run time in this round.

## Numbers
Declared net lines: +640 / −25 · new public surface: 5 (waiver requested) · cycles 1/2

---
*As landed (2026-09-02):* +1411 / −9 (code + spec, excl. `plans/`) vs declared +640/−25;
counted by the code reviewer, no unexplained bucket. Surface **5**, exactly the waiver.
1 design cycle, 2 fix cycles, 5 implementer launches — **2 wrote nothing** (a wedged
stream, then machine sleep), so 3 were productive. Six behaviours were asserted by
nothing when first written, all found by mutation, all on the branch PDA-ABI fills;
the lifetime rule became mechanical rather than prose.
