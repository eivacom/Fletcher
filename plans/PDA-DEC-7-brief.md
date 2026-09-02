# PDA-DEC-7 — Stage Brief (2026-09-02)

**In one sentence:** an operator configures the edge (XRCE) client at runtime with a plain
`key=value` document — Agent address included — no XRCE type in their build, no parser added.
**Forcing test:** `XrceConfig.DocumentConfiguresTransport` — the client dials the address
the document names, proved by a socket that sees the connection arrive.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| XRCE selectable by name (`xrce`), configured by a `key=value` document | NEW | Your 2026-09-02 "into the document"; same call that resolves `inprocess` and `fastdds` |
| The typed C++ XRCE options struct (and its transport enum) | **DELETED** | Retired, not deprecated — one path, so one path to test |
| Agent address, transport kind, session key, stream depth, timeouts | CHANGED | Now document lines; the address is **one** line (`agent=host:port`), so it cannot be half-given |

## Deleted
- The options struct, its transport enum and its constructor — every live setting has a
  named document key; payload size and DDS domain move to Fletcher's two typed settings.
  Plus a payload-size setting that never capped anything and the two serial-port settings,
  reachable only through a transport that refuses (decision 1).
- Four tests setting a struct field and reading it back → tests that read the *published
  defaults out of the README* and assert a typed refusal, not merely "it threw".

## Corner cases forbidden vs handled
**Forbidden:** configuring the client from C++ without a document; a half-specified Agent
address; a number silently truncated to fit (a domain id above 65535 is refused, never
narrowed — truncation means the wrong DDS domain, no error, no data); a partly-configured
client, since every document error is refused before a socket opens. So refused at
start-up: an unknown or duplicate key, a key with stray spaces, an address with no port,
an unhonourable stream depth or pump interval.
**Handled:** an unresolvable hostname (a transport failure — rejecting hostnames would
break setups that work today); an empty document meaning "all defaults"; a session key
clashing with another client on one Agent (only the Agent can see that).

## Decisions for you   (3)
1. **A setting that never did anything: delete it or make it real?** The edge client
   documents a 512-byte payload cap; nothing reads it, and rows are bounded only by
   Fletcher's own payload setting. (a) delete it · (b) keep it as a document key that
   genuinely caps sends. **Recommendation / default:** (a) — (b) is new behaviour, not a
   migration. *Background: `XrceConfig::max_payload`, read nowhere.*
2. **Serial: still a thing an operator can ask for?** It has never been implemented.
   (a) yes — refused distinctly, "this build cannot do serial" · (b) no — an unknown value,
   refused like a typo. **Recommendation:** (a), mirroring your ruling that an unloadable
   driver fails distinctly from a typo. **Default:** (a).
3. **How forgiving is the document file?** (a) strict — exactly the rules the in-process
   provider uses: one setting per line, Windows line endings and blank lines fine,
   **everything else refused, including comments and stray spaces** · (b) also allow `#`
   comments and trim spaces. **Recommendation / default:** (a) — two tolerant readers
   drift, and silence is how a misconfigured edge device ships.

## Risks accepted / debt carried
- The format is better covered than the transport: all but two guards need no live Agent.
  Transport coverage is the 27 existing end-to-end cases, now document-configured, unweakened.
- One substrate fact is load-bearing (the client's connect happens up front, so the
  address is observable without an Agent); if false, the design stops rather than weakens.

## Numbers
Net lines: +1400 / −380 · new public surface: −1 net (+2 / −3) · design cycles: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
