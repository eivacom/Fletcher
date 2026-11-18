# PDA-DEC-7 — code review (independent, step 4b)

Commit `4baac0f`, base `5756e67`, 20 files, +1471/−228. Reviewed the reader
(`xrcedds-pubsub-provider/src/internal/xrce_document.{hpp,cpp}`), the constructor/destructor,
the new tests, the four migrated callers, and the vendored Micro XRCE-DDS Client 3.0.1 sources
under `xrcedds-pubsub-provider/build/_deps/microxrcedds_client-{src,build}/`.

Verdict: **1 blocking, 3 should-fix, 6 nits.** The document reader itself is clean — I could not
break it. Every parse edge in the brief (unterminated input, no `=`, empty key, empty value,
duplicates, prefix keys, CRLF/LF/lone-CR, no trailing newline, embedded NUL, non-ASCII, long
lines, `+`/`-`/`0x`/leading zeros/overflow, locale) is either refused loudly or documented, and
there is no `string_view` anywhere near the document buffer — `XrceSettings` owns its `std::string`
by value and `ParseXrceDocument` returns by value. The `domain_id` path has no narrowing cast
before the `> 65535` refusal. The `kSerial`-less enum is genuinely exhaustive at both switches.

The one defect is not in the reader. It is in the four lines that convert the reader's output
into an XRCE call — the only arithmetic in this item that lives *outside* the pure function the
591-line test file can reach.

---

## Blocking

### B1 — `connect_timeout_ms` in 1–1000 buys **zero** connection attempts; construction can never succeed and the diagnostic blames the Agent

*Confidence: high (read out of the vendored client, not inferred).*

`src/xrce_dds_pubsub_provider.cpp`:

```cpp
const int64_t budget_ms = impl_->settings.connect_timeout.count();
const size_t retries =
    static_cast<size_t>((std::max)(static_cast<int64_t>(0), (budget_ms - 1) / kMsPerAttempt));
if (!uxr_create_session_retries(&impl_->session, retries)) { ... kTransportFailure ... }
```

`retries` is `floor((budget_ms - 1) / 1000)`. In the client, that argument is not a *retry*
count on top of a first try — it is the **total attempt count**
(`session.c: uxr_create_session_retries` → `wait_session_status(..., (size_t) retries)`), and
`wait_session_status` special-cases zero:

```c
session->info.last_requested_status = UXR_STATUS_NONE;
if (0 == attempts) { send_message(session, buffer, length); return true; }   /* session.c:742 */
```

…after which `created = received && UXR_STATUS_OK == session->info.last_requested_status` is
**false unconditionally**, because `last_requested_status` was just set to `UXR_STATUS_NONE` and
nothing ever listened. So `attempts == 0` means *send one datagram and fail*, never "one attempt".

Consequences, with `UXR_CONFIG_MIN_SESSION_CONNECTION_INTERVAL == 1000` (confirmed in the
generated `config.h`):

| `connect_timeout_ms` | attempts | outcome |
|---|---|---|
| `0` | 0 | send once, immediate failure — **intended and correct** |
| `1` … `1000` | **0** | send once, immediate failure — **always**, even against a healthy Agent |
| `1001` … `2000` | 1 | ~1 s of waiting |
| `3000` (the published default) | 2 | ~2 s of waiting for a "3000 ms" budget |
| `60000` | 59 | ~59 s |

Two things are wrong and one of them is silent:

1. A **documented, in-range, accepted** value (`connect_timeout_ms=750`, `=1000`) is dead. The
   operator gets `PubSubError(kTransportFailure)` reading *"failed to create a session with the
   Agent at 127.0.0.1:2018 within 750 ms (is the Agent running?)"* — while the Agent is running.
   The refusal is loud but it misattributes the cause, and the one thing it does not say is the
   true one: this build asked the client for zero attempts. That is the silence.
2. The whole mapping is off by one attempt, so every budget under-spends by ~1 s.

Nothing catches it. `XrceConfig.DocumentConfiguresTransport` is the only guard on this path and
it exercises **only** `connect_timeout_ms=0`, where floor and ceil agree; the Agent-gated suites
use 2000/5000, which land in the "one attempt too few but still enough" band. `README.md`'s claim
*"every value below 1000 ms means one attempt"* and the header's *"everything below 1000 ms means
one attempt"* both describe behaviour the code does not have.

The arithmetic is unchanged from the base commit — but before this item `connect_timeout_ms` was
a typed field only tests set, and now it is one of four operator-facing keys with a published
range of `0–60000`, a third of which is dead.

**Fix** — ceiling division, keeping `0` meaning `0`:

```cpp
const size_t retries = static_cast<size_t>((budget_ms + kMsPerAttempt - 1) / kMsPerAttempt);
```

`0 → 0` (row 1's `EXPECT_LT(elapsed, 1000)` still passes, and the C2-2 mutation still reddens it),
`1..1000 → 1`, `3000 → 3`, `60000 → 60`. `budget_ms` is already range-checked to `0..60000` by the
reader, so the `std::max` and the negative/overflow worry disappear with it. Then correct the
README/header sentence to "`0` sends once without waiting; anything from 1 to 1000 is exactly one
attempt", and add the table above as a row test (see S3).

---

## Should-fix

### S1 — Every constructor throw after transport init leaks the socket; this item multiplies how often that happens

*Confidence: high.*

`Impl` has no user-declared destructor. `uxr_close_udp_transport` / `uxr_close_tcp_transport` are
called **only** from `~XrceDDSPubSubProvider`, which does not run when the constructor throws —
`impl_` is destroyed, `Impl`'s members are destroyed, and the OS socket the client opened inside
`uxr_init_*_transport` is never closed (for TCP, an established connection; on Windows the
client's `WSAStartup` refcount is also never released).

Reachable, routinely, and *by this item's own design*:

- `XrceConfig.DocumentConfiguresTransport` row 1 constructs against a socket that will not speak
  XRCE — leak; row 2 constructs against the default UDP port — leak.
- `XrceConfig.AgentUnreachableIsATransportFailure` — leak.
- `XrceConfig.SerialIsRefusedAsUnsupported` — no leak (refused before init; the ordering claim
  in the header holds).
- The Agent readiness probes in `subjects/xrce_main.cpp` and `tests/test_interop.cpp` construct
  and fail in a loop for up to 15–20 s — one leaked socket per iteration, per run.

The invariant the header asserts ("validate everything first, then touch the world") is right and
holds; what is missing is its mirror image on the way out. Note also that the ordering is only
accidentally safe: the destructor closes the transport after `uxr_delete_session`, which is
correct, but nothing enforces that pairing from the site that opened it.

**Fix** — own the transport where it is created. Give `Impl` a destructor that closes whichever
transport `comm` points at (or a `bool transport_open`), and have `~XrceDDSPubSubProvider` stop the
run-loop and delete the session only. A half-constructed `Impl` then cleans itself up on unwind,
and the two-arm switch exists once instead of twice.

### S2 — Forbidding direction: refuse whitespace and control bytes *inside* an entry rather than hosting `" 127.0.0.1"` as a representable host

*Confidence: high; design economy, not a live bug.*

The non-trimming rule is right, but it is applied asymmetrically, because the checks that follow
happen to differ in strictness:

- `agent =x`, ` agent=x`, `agent=127.0.0.1:2018 `, `transport=tcp\t` — refused, because the *key*
  lookup or the *decimal* parse rejects them.
- `agent= 127.0.0.1:2018` — **accepted**, host `" 127.0.0.1"`, failing much later at
  `getaddrinfo`. So does `agent=127.0.0.1\r:2018` (a mid-entry CR is not the trailing one that
  gets stripped), and `agent=127.0.0.1 :2018`.

So the format's actual rule is not "nothing is trimmed" but "nothing is trimmed, and whether
whitespace is refused depends on which side of the `=` and which key you put it on". That costs a
README paragraph, a header sentence, a test row pinning the oddity, and a class of failure that
surfaces one layer down as `kTransportFailure` instead of at the door. It is safe today only
because `getaddrinfo` happens to reject a leading space — a property of exactly the resolver
Fletcher deliberately declines to know anything about (H1).

**Fix** — one rule, before the `=` split: refuse any entry containing a byte `< 0x21` other than
the trailing `\r` this format strips (space, tab, VT, FF, stray CR — NUL is already refused up
front). A host with whitespace in it becomes *unrepresentable* rather than tolerated; the
`agent=` / `transport=` / `session_key=` whitespace rows collapse into one; the README's
"whitespace inside a value is not trimmed either, and not second-guessed" paragraph and its
carefully-explained exception both go away. No legal hostname, IPv4 literal, decimal number or key
loses anything.

### S3 — The ms→attempts conversion belongs in the pure reader, where it is testable

*Confidence: high.*

B1 is the only arithmetic in this item that lives in the constructor rather than in
`ParseXrceDocument`, and it is also the only arithmetic in this item that is wrong. That is not a
coincidence: everything inside the pure function is pinned by a table in a 591-line test file that
needs no socket, and everything outside it can only be observed through a construction attempt.

**Fix** — move it: either store the derived attempt count in `XrceSettings` beside (or instead of)
`connect_timeout`, or expose a one-line pure `size_t SessionAttempts(std::chrono::milliseconds)`
from `internal/xrce_document.hpp`. The constructor then contains no arithmetic at all, and
`test_xrce_document.cpp` can assert the B1 table directly — `{0→0, 1→1, 999→1, 1000→1, 1001→2,
3000→3, 60000→60}` — seven cheap rows against the knob whose whole purpose is a deadline. This is
the same move the item already made for `kSerial` and for `std::chrono::milliseconds`: push the
fact somewhere it cannot be got wrong quietly.

---

## Nits

- `output_buffer`/`input_buffer` are sized from `UXR_CONFIG_UDP_TRANSPORT_MTU` unconditionally, including for `transport=tcp`; correct today only because `UXR_CONFIG_TCP_TRANSPORT_MTU` also happens to be 512. Size from `impl_->comm->mtu` after transport init, or `static_assert` the two are equal.
- The two-arm transport switch in the constructor has no `default`/`std::unreachable()`: if the enum ever grows, `comm` stays `nullptr` and `uxr_init_session` receives it. (Unreachable today.)
- `transport=udp\ntransport=serial` reports `kInvalidArgument` (duplicate key) while `transport=serial` alone reports `kNotSupported`; the "fail distinctly" status is order-dependent.
- Leading zeros are accepted in every numeric value (`agent=127.0.0.1:02018`, `session_key=007`); harmless, but not what "decimal 1–65535" implies, and `ParseDecimal`'s comment claims one total rule.
- `subjects/xrce_main.cpp`'s readiness probe rebuilds the whole document from scratch two lines after calling `XrceConfigFor`, duplicating the address/session-key construction instead of appending `"\nconnect_timeout_ms=2000"` the way `test_interop.cpp` does.
- `XrceConfig.PublishedDefaultsAreExact` reads `${CMAKE_CURRENT_SOURCE_DIR}/../README.md` at run time, so the test binary is only runnable while the source tree it was built from still exists. It fails loudly and names the path, which is the right failure mode; worth knowing before anyone runs the test binary out of a package.

Test quality otherwise checks out. The four retired tests are each replaced by something strictly
stronger (status instead of exception base class; whole-struct instead of field read-back), and
`EveryKeySetNonDefaultLandsWholeStruct` is not vacuous: all four fields differ from their defaults,
the expectation is built field-by-field rather than from a parse, and
`EXPECT_FALSE(parsed == XrceSettings{})` closes the "indistinguishable from defaults" hole.
`DocumentConfiguresTransport`'s ephemeral port really is unhard-codeable, and its 1000 ms bound is
~2x clear of the C2-2 mutation it exists to catch. `TcpListener` is race-free (the `clients_`
vector is touched only by the accept thread, and the destructor closes the listener before
joining). No migrated caller changed meaning: `xrce_peer_main` builds the same
`127.0.0.1:<--agent-port>` it used to assemble field-wise, and the interop/conformance defaults
(`max_payload_bytes = 0 → 65536`) reproduce the retired `payload_bound` default byte for byte, so
no registered DDS type name moved. `uxr_init_*_transport` resolves host and port at init and
retains neither pointer, so the constructor-local `port_str` does not dangle.

---

## RECORD (paperwork for the PM — never blocking)

- `xrce_dds_pubsub_provider.hpp:4` — "NO XRCE vocabulary may appear in this header, and none does: it declares exactly two things, a registration function and one constructor" — the header names `XrceDDSPubSubProvider`, `RegisterXrceProvider` and the full public method set. The true (and discharged) claim is "no eProsima/`uxr*` type".
- `docs/pubsub-interface-spec.md` §4.2 — "the installed XRCE header … names no XRCE vocabulary at all" — same overstatement.
- `xrcedds-pubsub-provider/README.md` — "**every value below 1000 ms means one attempt**" is wrong: it means zero attempts (B1).
- `xrcedds-pubsub-provider/README.md` test-duration note — "pays the full default 3000 ms budget, which the client spends as two ~1000 ms attempts": two attempts is ~2000 ms, not 3000 (B1).
