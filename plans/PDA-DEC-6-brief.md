# PDA-DEC-6 — Stage Brief (2026-09-02)

**In one sentence:** an operator configures Fast DDS quality-of-service at runtime with a
Fast DDS XML profile, and nothing of theirs compiles against eProsima headers.
**Forcing test:** `FastDdsConfig.ProfileDocumentConfiguresQos` — proves the setting an
endpoint announces on the network is the document's, not merely that the document loaded.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Fast DDS selectable by the name `fastdds` | NEW | The same one call that already resolves `inprocess` |
| Fast DDS QoS as a native XML profile document | NEW | Your 2026-08-31 "XML profile config only" |
| The typed C++ QoS options struct | **DELETED** | Retired, not deprecated — one path, so one path to test |
| Gateway `--provider-config FILE` | NEW | So an operator can still point at a file on disk |

## Deleted
- The typed options struct and its constructor — all seven settings have a named home.
- The gateway's hand-written Fast DDS construction — now one registration call.
- Five QoS tests that set a value then only checked a message arrived — replaced by tests
  that read back what the endpoint announced. Strictly stronger.

## Corner cases forbidden vs handled
**Forbidden:** configuring QoS from C++ without the document; two instances sharing one
profile table; a half-configured endpoint (a profile is the whole QoS for its role, never
a merge); a configurable internal schema channel; and — refused at construction, so a
misconfigured instance never exists — a document that is not a Fast DDS profiles
document, an unknown Fletcher setting, or an unusable payload bound.
**Handled:** a profile named after no real topic is inert (Fast DDS looks a profile up by
name and cannot list what a document defines); an operator's own reader profile can
re-enable the known receive-side data-sharing defect (a Fletcher floor would mean the
document does not really configure QoS, and the defect hunt needs it back on).

## Decisions for you   (3)
1. **Does the setting hold the XML itself, or the name of a file to read?**
   (a) the XML text, with the gateway growing a flag that reads a file for you · (b) a
   filename Fletcher opens · (c) either, by guessing. **Recommendation:** (a) — one
   meaning for one setting. **Default if you don't answer:** (a).
   *Background: the document is opaque bytes; (c) makes `<`-versus-path a silent switch.*
2. **If your profile leaves a setting out, do you get Fast DDS's default or Fletcher's?**
   (a) Fast DDS's — your profile is that endpoint's whole quality-of-service · (b)
   Fletcher's stay underneath, your profile changes only what it mentions.
   **Recommendation:** (a), plus we publish Fletcher's exact profile as the starting
   point, kept true by a test. **Default:** (a). *Background: (b) needs a fact the XML API never reports.*
3. **Two switches are Fletcher's own, not DDS QoS — the zero-copy publish path and the
   internal schema size cap. Keep or drop?** (a) keep, as named entries in the same
   profile document · (b) drop both. **Recommendation:** (a) — dropping zero-copy publish
   removes a capability next round's defect hunt needs. **Default:** (a).
   *Background: they ride as `fletcher.*` properties on the participant profile — native Fast DDS XML, so no new parser.*

## Risks accepted / debt carried
- A profile name matching no topic does nothing and cannot be reported.
- The cross-process Fast DDS harness has stale binaries; it must be rebuilt and shown
  green — it is the only cross-process DDS coverage that exists.
- That Fast DDS parses a profile string without touching process-wide state is the one
  bet here; verified in the shipped headers, stop-and-ask if false.

## Numbers
Declared net lines: +720 / −310 · new public surface: 0 net (+2 / −2) · cycles: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
