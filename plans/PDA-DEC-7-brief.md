# PDA-DEC-7 — Stage Brief (2026-09-02, rev 1)

**In one sentence:** an operator configures the edge (XRCE) client at runtime with a plain
`key=value` document — Agent address included — no XRCE type in their build, no parser added.
**Forcing test:** `XrceConfig.DocumentConfiguresTransport` — the client dials the address the
document names, proved by a socket that sees the connection arrive.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| XRCE selectable by name (`xrce`), configured by a `key=value` document | NEW | Your 2026-09-02 "into the document"; same call that resolves `inprocess` and `fastdds` |
| The typed C++ XRCE options struct (and its transport enum) | **DELETED** | Retired, not deprecated — one path, so one path to test |
| Four settings — transport kind, Agent address, session key, connect timeout | CHANGED | Now document lines; the address is **one** line (`agent=host:port`), so it cannot be half-given |

## Deleted
- The options struct, its transport enum and its constructor — every setting an operator can
  still choose has a named key; payload size and DDS domain become Fletcher's typed settings.
- **Five settings go away entirely.** A payload cap that never capped anything; two serial-port
  settings reachable only through a transport that refuses; and — new in this revision — the
  **stream depth** and **pump interval**, which nothing in the tree ever set and no test could
  observe. Those two become fixed values — *behaviour gone, disclosed*; if either is ever
  wanted it returns with a test proving it took effect, the thing neither had.
- Four tests that set a struct field and read it back → tests that read the *published defaults
  out of the README*, assert a typed refusal rather than "it threw", and — new in this revision
  — assert that a document setting every remaining setting non-default lands **all four**
  exactly, so a build that validates a value and then ignores it fails.

## Corner cases forbidden vs handled
**Forbidden:** configuring the client from C++ without a document; a half-specified Agent
address; a number silently truncated to fit (a domain id above 65535 is refused, never
narrowed — truncation means the wrong DDS domain, no error, no data); a setting accepted then
discarded; a partly-configured client, since every document error is refused before a socket
opens; IPv6 Agent addresses, which never worked and are now refused rather than half-accepted.
Refused at start-up: an unknown or duplicate key, a key with stray spaces, an address with no
port, a timeout out of range.
**Handled:** an unresolvable hostname (a transport failure — rejecting hostnames would break
setups that work today); an empty document meaning "all defaults"; a session key clashing with
another client on one Agent (only the Agent sees that).

## Decisions for you   (none — three recorded as decided, 2026-09-02)
1. **The payload cap that never capped anything is deleted.** *Authority:* verified P5 —
   nothing sets or reads it — plus your 2026-09-01 rewrite-the-provider-code ruling.
2. **Serial stays nameable but is refused distinctly** — "this build cannot do serial", not
   "unknown setting". *Authority:* your ruling that an unloadable driver fails unlike a typo.
3. **The document is strict** — one setting per line, Windows line endings and blank lines fine,
   everything else refused, comments and stray spaces included. *Authority:* spec §4.1 as landed.

## Risks accepted / debt carried
- The format is better covered than the transport: all but two guards need no live Agent.
  Transport coverage is the 24 existing end-to-end cases, now document-configured, unweakened.
- One substrate fact is load-bearing (the client's connect happens up front, so the address is
  observable without an Agent); unverifiable here, so it is confirmed before the test is
  written — if false the design stops rather than weakens. Nine review-debt items ride with
  the implementation; none behaviour-visible.

## Numbers
Net lines: +1900 / −400 · new public surface: −1 net (+2 / −3) · design cycles: 2/2

---
*As landed (2026-09-02, PM):* **+1979 / −247** over 20 files excluding `plans/` (+2922/−256 with
the design and review artifacts) vs declared +1900/−400 — **adds within 5% of declared**, the
first item this round to cost what it said. Production `src/`+headers **+715/−121**. Surface
**net −1 (+2/−3)** as declared. Two fix cycles, both from findings the brief did not predict:
`connect_timeout_ms` was parsed and then dropped for a hard-coded value, so **every budget from
1–1000 ms bought zero attempts and could never connect** (found independently by both step-4
reviewers, proved live both ways); and the socket-leak fix then shipped with **no witness**,
its probe deleted after measuring. Both now have tests that redden on regress. Added beyond the
brief: refusal of whitespace and DEL inside an entry (unrepresentable, not documented), and the
attempt arithmetic moved into the pure reader. Suite 10→15 gtest / 11→16 ctest; `conformance_xrce`
24→25. Cycles: design 2/2 · fix 2 · implementer launches 3/5 · owner touches 0.
**Routed out, unowned:** the harness cannot tell whose Agent it is talking to (debt ROUND-1).