# PDA-DEC-6 — Stage Brief (2026-09-02, rev 2)

**In one sentence:** an operator configures Fast DDS quality-of-service at runtime with a
Fast DDS XML profile, and nothing of theirs compiles against eProsima headers.
**Forcing test:** `FastDdsConfig.ProfileDocumentConfiguresQos` — the setting an endpoint
announces on the network is the document's, not merely that the document loaded.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Fast DDS selectable by name (`fastdds`), configured by a native XML profile document | NEW | Your 2026-08-31 "XML profile config only"; same call that resolves `inprocess` |
| The typed C++ QoS options struct | **DELETED** | Retired, not deprecated — one path, so one path to test |
| Gateway `--provider-config FILE` | NEW | So an operator can still point at a file on disk |

## Deleted
- The typed options struct, its constructor, and the gateway's hand-written Fast DDS
  construction — all seven settings have a named home; the gateway now registers instead.
- Five QoS tests that set a value then only checked a message arrived → ones reading back
  what the endpoint announced, setting-for-setting. Strictly stronger.

## Corner cases forbidden vs handled
**Forbidden:** configuring QoS from C++ without the document; two instances sharing one
profile table; a half-configured endpoint (a profile is the whole QoS for its role, never a
merge); a configurable internal schema channel; and — refused at start-up, so a
misconfigured instance never exists — a document that is not a Fast DDS profiles document,
an unknown Fletcher setting, an unusable payload size, or **a profile naming a DDS domain
disagreeing with the one Fletcher was given** (else: domain 0, no error, no data).
**Handled:** a profile named after no real topic is inert (Fast DDS looks a profile up by
name, and cannot list what a document defines); an operator's own receiving profile can
re-enable the known data-sharing defect (a Fletcher floor would mean the document does not
really configure QoS, and the defect hunt needs it on).

## Decisions for you   (2)
1. **Does the setting hold the XML itself, or the name of a file to read?**
   (a) the XML text, with the gateway growing a flag that reads a file for you · (b) a
   filename Fletcher opens · (c) either, by guessing. **Recommendation:** (a) — one meaning
   for one setting. **Default:** (a). *Background: (c) makes `<`-vs-path a silent switch.*
2. **If your profile leaves a setting out, do you get Fast DDS's default or Fletcher's?**
   (a) Fast DDS's — your profile is that endpoint's whole quality-of-service · (b)
   Fletcher's stay underneath, your profile changes only what it mentions.
   **Recommendation:** (a), plus we publish Fletcher's exact profile as your starting
   point, kept true setting-for-setting by a test. **Default:** (a).
   *Background: (b) needs a fact the XML API never reports — which policies your file set.
   Cycle 1's third question (keep zero-copy publish and the schema cap?) review settled:
   both kept, same document, not re-asked.*

## Risks accepted / debt carried
- A profile name matching no topic does nothing and cannot be reported. The cross-process
  Fast DDS harness has stale binaries; it is rebuilt and shown green with no intermittent
  row loss — the only cross-process DDS coverage that exists.
- Two unprovable-from-the-package facts — that reading a profile touches no process-wide
  state, and that one broken profile rejects the whole document — each get a test here.

## Numbers
Declared net lines: +780 / −310 · new public surface: 0 net (+2 / −2) · cycles: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
