# PDA-DEC-A5 — code review (step 4b, independent)

Diff reviewed: `git diff 722cc6b 5f04e2c`. Reviewer did not write the change.
Remit: correctness, safety, reachable-and-silent edge cases, simplifications in the
forbidding direction. Design conformance is a parallel reviewer's job.

Built and ran: `pubsub_tests` (23/23 green, the four new `Segments.*` among them);
`conformance_fastdds --gtest_filter=TopicNames.*` (green); `conformance_xrce
--gtest_filter=TopicNames.*` with `-DFLETCHER_CONFORMANCE_XRCE=ON` (green, session
key 0x55000001 against the binary's own Agent). `conformance_xrce
--gtest_list_tests` reports 28 cases, matching the CMake comment.

Counts: **1 blocking · 2 should-fix · 4 nits.**

---

## Did an accepted list still alias or truncate? — YES, one, measured

### B1 (blocking, confidence: measured on this box)

**Two distinct accepted segment lists whose joined names agree on their first 255
bytes are ONE topic on Fast DDS.** A subscriber to `A` receives every row published
to `B`. Silent wrong delivery — the exact defect class this item exists to remove,
still open above 255 bytes.

Root cause is at the sink, not at the door. Fast DDS announces a topic in discovery
as `fastcdr::string_255 topic_name`
(`fastdds/rtps/builtin/data/PublicationBuiltinTopicData.hpp:63`,
`SubscriptionBuiltinTopicData.hpp:63`, `TopicDescription.hpp:37`). `fixed_string`
**truncates silently**: `fixed_size_string.hpp:83`
(`(MAX_CHARS < n_chars) ? MAX_CHARS : n_chars`) and `:331`
(`MEMCCPY(..., MAX_CHARS)`), both `noexcept`, no error, no log. So the name the seam
computed is not the name the transport matches on.

Measured with one `FastDDSPubSubProvider` on domain 156, five rows each way, with a
same-topic control in every run:

| joined name | cross-delivered B to sub(A) | control A to sub(A) | verdict |
|---|---|---|---|
| 32 bytes | 0 | 5 | distinct |
| **255 bytes** | **0** | 5 | distinct |
| **256 bytes** | **5** | 5 | **one topic** |
| 262 bytes | 5 | 5 | one topic |
| 612 bytes | 5 | 5 | one topic |

The threshold is exact: 255 accepted, 256 aliases. Reachable with ordinary
material — eight segments of about 32 characters, or any machine-generated name
built from a device path or a chain of identifiers.

This is not a pre-existing bug the change merely failed to fix; the change *makes
the claim*. `segments.hpp` now states "`Split(Join(L)) == L`, so two distinct
accepted lists are two distinct topics in EVERY provider", and
`docs/pubsub-interface-spec.md` §3.5 states the same normatively. Both are false
above 255 bytes. `Segments.JoinIsInvertible` cannot see it because it is an oracle
over the join, and the join is not where the map breaks.

Secondary consequence of the same truncation, not separately measured: for a data
name of 255 bytes or more, the derived `name + "/__schema"` truncates back onto the
data topic's own announced name, so the reserved-`__` guarantee ("`Join(L)` is not a
name a provider DERIVES") also stops holding there.

**Fix (one line):** bound the joined length in `internal::RequireSegments` — refuse
with `kInvalidArgument` when the joined name would exceed 255 bytes (246 if the
derived `/__schema` companion must also survive intact), which is the one door and
the only place that can make the injectivity claim true. Note for the PM: this is a
*fifth* refusal, so like the other four it is the owner's to rule.

XRCE has the same root cause one hop away — the Agent creates the Fast DDS entities
from the name the client sent — plus a ceiling of its own (`UXR_BINARY_SEQUENCE_MAX`
= 512, `xrce_types.h:37`, into which `uxr_buffer_create_topic_bin` serialises name +
type name). Not separately measured; a length bound at the door covers both.

### What I could not break

Everything else I tried is clean, and I want that recorded rather than padded.

* Injectivity below the length ceiling is *provable*, not merely untested: no
  accepted segment contains `/`, so splitting the joined name on `/` recovers the
  list exactly, at any arity; no accepted segment contains NUL, so `c_str()` sees the
  whole name.
* Exhaustive machine check over all 1- and 2-segment lists with segments of up to 3
  bytes from the alphabet `{a, _, /, NUL, \, 0xC3, 0xA9, %}` — the separator, the
  zero byte, Windows' backslash, a UTF-8 lead+continuation pair, the escape marker
  and an ordinary byte: **63252 accepted lists produced 63252 distinct names and
  63252 distinct `c_str()` names.** No alias, no truncation.
* UTF-8 cannot smuggle a separator: every byte of a multi-byte sequence is at least
  0x80, and 0x2F is not a trail byte in Shift-JIS, GBK or Big5 either.
* `\` on Windows is inert here — no provider turns the joined name into a filesystem
  path; Fast DDS's shared-memory files are named from participant GUIDs, not topic
  names.
* `RequireSegments` genuinely is the only door. All twelve provider entry points
  (`in_process_provider.cpp:180,256,287,321`,
  `fast_dds_pubsub_provider.cpp:289,383,429,533`,
  `xrce_dds_pubsub_provider.cpp:634,774,829,967`) call a join as their first
  statement, before any lock and any side effect; the caller tier (`publisher.cpp:36`,
  `subscriber.cpp:445`) and `pubsub-arrow` do too. `Publisher::Publish` forwards
  without joining, so the provider's join is the only one on the hot path.
* The participant-name sink cannot truncate now: `xrce_dds_pubsub_provider.cpp:675`
  passes the same joined `name.c_str()` the NUL rule covers. No *other* derived name
  is a function of the topic — `FletcherTypeName` derives from the payload bound
  (`payload_bound.hpp:63`), `kSchemaTypeName` is a constant, and the XRCE session key
  and DDS domain come from the config document. The Fast DDS per-topic QoS profile
  lookup takes the joined name as a `const std::string&`, not a `const char*`.
* No in-tree caller uses a `__`-prefixed segment, so the new reservation breaks
  nothing.
* Cost is what the comment claims: one linear pass over bytes the join is about to
  copy anyway, no allocation, no constructed needle. No O(n^2), no re-scan within a
  call.
* Neither new provider-binary case is flaky by construction: no sleeps, no discovery
  wait, no shared domain (Fast DDS 155 against the registry case's 153), and XRCE's
  `NextSessionKey` counter is shared across bases so 0x55xxxxxx cannot collide with
  0x54xxxxxx.
* The peer door's NUL row is correct: `std::string(" \t/\0", 4)` really does carry
  four bytes where a plain literal would have stopped at three.

---

## Should-fix

### S1 — the peer door is not total, and it is a hand-maintained mirror (confidence: high; not reachable today)

`peer_subject.cpp:87` claims "the door is TOTAL: every shape the seam refuses is
unsendable over this pipe by construction". It is not. `RejectUnsendableTopic`
misses:

* the **empty LIST** — the loop body never runs, so `{}` returns `Reply::Ok()` and
  then `internal::JoinSegments` throws `PubSubError` out of `DeclareTopic` /
  `PublishRow`, both declared to return `Reply`;
* the **`__` prefix** — same path, same escaping throw;
* `\n`, `\r`, `\v`, `\f` — the peer reads lines with `std::getline` and tokenises
  with `operator>>`, whose separator set is `isspace`, not the literal `" \t"`. A
  segment carrying a newline splits the request line in two.

Not reachable today: `FreshTopic` produces `{"conf", <clause>, "<pid>_<n>"}` and no
clause constructs any of these shapes for a peer subject. It is a trap for the next
clause that does, and an exception escaping a `Reply`-returning method there is a
hang or a HarnessFailure, not a useful red.

**This is the forbidding-direction fix.** The row is a hand-copied subset of the
seam's rule set that will drift from it every time the seam adds a rule — and B1 is
about to add one. Replace it with the seam's own door plus the pipe's own extra:
`try { internal::RequireSegments(topic); } catch (const PubSubError& e) { return
Reply::HarnessFailure(...); }`, then test only `isspace` on top. The harness then
cannot disagree with the seam by construction, which is what the comment already
claims.

### S2 — the gateway aliases topic names one tier above the seam (confidence: high; pre-existing, outside the diff)

`gateway/src/ws_session.cpp:149` `SplitTopic` **drops empty pieces**
(`if (!seg.empty()) segments.push_back(...)`, twice). So at a shipped caller
`"a/b"`, `"a//b"`, `"/a/b"`, `"a/b/"` and `"//a//b//"` are all the segment list
`{"a","b"}`: a client subscribing to one and a client publishing to another exchange
rows although they named different topics. Same silent alias this item declares
blocking, one tier up, and now the only place on the path that still tidies a name
instead of refusing it.

**Fix, in the forbidding direction: delete the two `if (!seg.empty())` guards.** The
door that refuses empty segments now exists; letting the empty piece through means
`"a/b/"` is refused with `kInvalidArgument` — surfaced as an error frame by the
existing `catch` at `ws_session.cpp:140` — instead of silently becoming `{"a","b"}`.
Strictly less code and strictly fewer representable states.

---

## Nits

* `Segments.AcceptedNamesJoinToTheSameBytesAsBefore`'s escape alphabet covers `%` but
  not `\` or a leading/trailing space, so a backslash-escaping design would move no
  row in the table (trimming is caught, but only over in `JoinIsInvertible`'s `{" a"}`
  row).
* The same test's last two `EXPECT_EQ`s are near-tautological: `EXPECT_EQ(joined,
  "telemetry/depth")` restates a row of the table two lines above it.
* `pubsub-arrow`'s publish path joins at the arrow tier (`publisher_arrow.cpp:55`)
  and again inside the provider, so `RequireSegments` scans the same bytes twice per
  sample there. Harmless; worth knowing before someone measures.
* Nothing pins the `static thread_local` scratch in `FastDDSPubSubProvider::Publish`
  after a refusal — `JoinSegmentsInto` validates before `out.clear()`, so the buffer
  keeps the previous topic's name. Correct, because the call unwinds; untested.

---

## RECORD (for the PM; never blocking)

* `integration-tests/pubsub-conformance/CMakeLists.txt:320` says `conformance_fastdds`
  joins "ten domains"; the tree uses eleven (151, 152, 153, 155, 161-167). The
  off-by-one predates this change, which said "nine" for ten.

---
---

# RE-CHECK after fix cycle 1 (independent, appended — original above is unmodified)

Diff re-reviewed: `git diff 335b016 f526acb`; cumulative `722cc6b..f526acb` also read.
Tree at `f526acb`, clean.

Everything below was re-measured on this box, not read off the implementer's report.
To make that honest I rebuilt the Conan packages from the working tree first
(`conan create pubsub`, `conan create fastdds-pubsub-provider`) and confirmed the new
refusal string is actually inside the compiled provider libraries before running
anything — the cached packages were still carrying the pre-fix header.

**Counts this round: 0 blocking · 0 should-fix · 2 nits · 1 RECORD.**
**Ready to close: YES.**

## 1. The round-1 reproduction, re-run against `f526acb`

Same probe, same shape, same domain-isolated single provider: two lists agreeing on
their first 255 bytes, subscribe A, publish 5 rows to B, count at sub(A), then a
same-topic control.

| case | joined | result |
|---|---|---|
| **the round-1 aliasing pair** | 256 | `CreateTopic(A)` and `CreateTopic(B)` both **REFUSED, `kInvalidArgument`** — the pair is now unreachable |
| just over the bound | 247 | both **REFUSED, `kInvalidArgument`** |
| longest accepted | 246 | accepted; schema `kOk`; **cross(B to subA) = 0**, control 5/5 |
| short control | 40 | accepted; schema `kOk`; cross 0, control 5/5 |

**The finding is closed.** The 5-of-5 wrong delivery that opened it cannot be
constructed any more, because neither name can be declared. The 246-byte row is the
positive control that the bound did not simply refuse everything: at 246 the derived
companion is exactly 255 bytes and the schema still arrived.

`pubsub_tests --gtest_filter=Segments.*`: **5/5 green**, including the new
`NamesThatWouldTruncateOnTheWireAreRefused`.

## 2. Attacking the bound — I tried hard and could not break it

Independent probe compiled straight against the working-tree header, not through the
new test file, so it cannot inherit the test's assumptions.

* **Boundary sweep.** k = 1..6 segments, joined totals 240..252, lengths spread
  across the segments: accepted **exactly** when joined is 246 or less, refused at 247
  and above, and every accepted `JoinSegments(L).size()` equalled the predicted joined
  length to the byte. No off-by-one on either side.
* **Separator accounting.** 1..300 single-byte segments: accepted exactly when
  `2k - 1 <= 246` (k up to 123). A rule that summed segment bytes and forgot the
  separators would have accepted k = 124..247; it does not. The check is on
  `segs.size() - 1 + sum(sizes)`, i.e. genuinely the **joined** length, and it sits
  first in `RequireSegments`, so all twelve entry points inherit it through the two
  joins — nothing measures segments instead.
* **Multibyte straddling.** A 2-byte UTF-8 sequence placed so its second byte lands
  on 245..250: byte-exact, no partial-character acceptance, no off-by-one. Irrelevant
  by construction anyway — the bound is 9 bytes below the ceiling that truncates.
* **Empty list** still hits rule 1 first, so `segs.size() - 1` cannot underflow.
  Overflow of the running sum needs about 2^64 bytes of input; not constructible.
* **Single 246-byte segment vs many short ones** both land on 246, both accepted,
  both join to exactly 246 bytes.

## 3. Is 246 the right number across all three providers? — Yes, and XRCE is not tighter

* **Fast DDS** — 255 is the vendor's, measured last round: `fixed_string<255>`
  truncates silently (`fixed_size_string.hpp:83,331`, both `noexcept`). 246 + 9 = 255,
  and `"/__schema"` is 9 bytes — confirmed the only companion any provider derives
  (`fast_dds_pubsub_provider.cpp:331,494`, `xrce_dds_pubsub_provider.cpp:720,881`; all
  four sites, one suffix, and no other derived name is a function of the topic).
* **XRCE — measured, because reading the client sources alone would not settle it.**
  Own `MicroXRCEAgent` on port 2219, domain 158, a 246-byte topic declared,
  subscribed and published five times:

  ```
  longest-accepted-246: joined=246 companion=255 schemaWait=0 rows=5  ROUND TRIP OK
  short-control-40:     joined=40  companion=49  schemaWait=0 rows=5  ROUND TRIP OK
  ```

  The longest name the seam now accepts, **and its 255-byte `/__schema` companion**,
  cross the XRCE wire intact. XRCE's own ceilings are looser, not tighter:
  `UXR_BINARY_SEQUENCE_MAX` is 512 (`xrce_types.h:37`) and both
  `uxr_serialize_OBJK_Topic_Binary` and `uxr_serialize_OBJK_DomainParticipant_Binary`
  write unbounded CDR strings into it — 246 plus type name plus framing is about 275.
  `DDS_XRCE_REFERENCE_MAX_LEN` (128) applies to the `_ref` creation APIs, which this
  provider does not use. So **246 is right for XRCE too**, and for the same reason it
  is right for Fast DDS: the XRCE Agent builds the real Fast DDS entities from the
  name the client sent.
* **In-process** has no wire, so any bound is safe there.

## 4. Is anything still aliasing? — No

Re-ran the round-1 exhaustive injectivity check with the ceiling in force, alphabet
`{a, _, /, NUL, \, 0xC3, 0xA9, %}` plus 120-, 125-, 246- and 247-byte words so lists
straddle the bound, all 1- and 2-segment combinations, and this time also keying on
**what Fast DDS actually announces** (each name truncated to 255) and on the
**announced companion** (`name + "/__schema"` truncated to 255):

```
accepted=64262  distinct-bytes=64262  distinct-cstr=64262  distinct-companion=64262  CLEAN
```

Every accepted list has a distinct full name, a distinct `c_str()` name and a
distinct announced companion name. No accepted list exceeds the ceiling. Nothing else
on the path can truncate: the XRCE participant name **is** the topic name (246 or
less), the type name derives from the payload bound and not from the topic
(`payload_bound.hpp:63`), `kSchemaTypeName` is a constant, neither DDS provider uses
partitions, and the XRCE session key is a number from the config document.

## 5. Both should-fix items — checked for correctness, not presence

**S1, the peer door: total, confirmed.** `RejectUnsendableTopic` now calls
`internal::RequireSegments` in a `try` and converts a `PubSubError` into
`Reply::HarnessFailure`, then tests `std::isspace` on `unsigned char` (correct
signature usage) for the pipe's own rule. Both `Reply`-returning methods
(`DeclareTopic`, `PublishRow`) call it first, so the `internal::JoinSegments` that
follows **cannot throw** — every shape that would throw has already been converted.
The empty list and the `__` prefix, the two gaps I filed, are now covered by
delegation rather than by a second copy of the rules, and rule 6's length bound
arrived there for free. `Subscribe`/`Unsubscribe` still forward straight to the
provider, but they do not return `Reply` and that is unchanged behaviour. Checked that
no clause can trip the new door by accident: every `FreshTopic`/`Fresh` label in the
suite is a static identifier with no whitespace, longest 19 characters, so a fresh
topic joins to about 37 bytes.

**S2, the gateway: correct, and nothing throws past a handler.** `SplitTopic` keeps
every piece; `""` short-circuits to an empty list, which rule 1 refuses. Both entry
points into the seam sit inside a handler that catches `std::exception`
(`HandleTextFrame`, `HandleBinaryFrame`), and `PubSubError` derives from
`std::runtime_error` (`status.hpp:97`) — so `"a//b"`, `"/a/b"`, `"a/b/"` and a
247-byte name all surface as error frames instead of silently naming another client's
topic. No other gateway path repairs a name: `SplitTopic` is the only
string-to-segments conversion (`ws_session.cpp:189,194,271`) and `ListTopics` returns
names that were already accepted.

## 6. Suites re-run here

* `pubsub_tests --gtest_filter=Segments.*` — 5/5.
* Whole conformance harness rebuilt against the freshly created packages and run with
  **`FLETCHER_CONFORMANCE_XRCE=ON`**: `ctest -C Release` gave **104/105**, the single
  failure being `Registry.TwoInstancesStayIsolatedUnderConcurrentTraffic` (SEGFAULT).
  That is the documented false red — after clearing
  `C:\ProgramData\eprosima\fastdds_interprocess` it passed on re-run, so **105/105
  effective**, independently reproduced. `conformance_xrce` passed as one entry in
  16.5 s.
* `gateway` — 20/20.

## Nits (this round)

* `RefusedCases()`'s renumbered labels are transposed in two rows: the
  NUL-in-a-later-segment row says "rule 3" (NUL is rule 2) and the `b/c` row says
  "rule 2" (`/` is rule 3). Failure-message text only.
* `conan create fastdds-pubsub-provider` fails in `test_package` on this box because
  the test_package build does not carry `cxx_std_20` (`payload_bound.hpp` inline
  variables). Pre-existing and unrelated to this diff — `payload_bound.hpp` is
  untouched — but a local `conan create` needs `-tf=""` to get a package out.

## RECORD (this round)

* `pubsub/tests/test_segments.cpp` `RefusedCases()`: two rule labels are transposed
  (rule 2 / rule 3) after the renumbering that made room for rule 6.

---
---

# FINAL CONFIRMATION after fix cycle 2 (appended — everything above unmodified)

Diff: `git diff 445b4ea b946798`. Tree at `b946798`, clean. Targeted check only; the
boundary sweep, the injectivity check and the XRCE round trip stand from the previous
section and were not re-run.

**0 blocking · 0 should-fix · 1 nit · 1 RECORD. Ready to close: YES.**

## 1. Do my measurements survive the cache purge? — Yes, re-measured, identical

`conan install` on the conformance recipe now resolves three **new** package folders
(`fletc7fa0f4347408a` pubsub, `fletc934411242e12e` fastdds, `fletc27183ac0caf3b`
xrce). I rebuilt my probe against those and re-ran the round-1 reproduction:

```
the-round-1-pair-256: joined=256  CreateTopic(A)=REFUSED(1)  CreateTopic(B)=REFUSED(1)
just-over-247:        joined=247  CreateTopic(A)=REFUSED(1)  CreateTopic(B)=REFUSED(1)
longest-accepted-246: joined=246  schemaWait=0  cross(B->subA)=0  control(A->subA)=5
short-control-40:     joined=40   schemaWait=0  cross(B->subA)=0  control(A->subA)=5
```

Byte-for-byte the same as the numbers I reported last round. Nothing I reported was
taken against a poisoned package, and there is a structural reason as well as a
re-run: **M9 bounds at 255, so M9 accepts 247** — every one of my runs reported 247
REFUSED, which no M9-compiled binary can produce. The probe is behavioural, against
the compiled provider library and a real Fast DDS domain, so it also covers the
harder half of the trap the coordinator describes: a library compiled against a
mutated header that a header inspection would miss.

## 2. The new `gateway-end-to-end` case — correct, non-flaky, on the right path

It drives the shipped gateway exe over its real WebSocket surface and asserts what a
CLIENT observes, which is the claim I made when I filed S2. It covers `"a//b"`,
`"a/b/"` and `"/a/b"` — exactly the three aliases I named — through **both**
`create_topic` and `subscribe`, the two text-frame paths into `SplitTopic`, with the
positive row (`"emptypart/ok"`) first so a refuse-everything gateway cannot be green.

**It cannot race.** I checked the mechanism rather than assuming: the TS client
resolves a pending request on `resp.type === 'error'`
(`gateway-client-ts/src/client.ts:271`) and each call site turns that into a thrown
`Error` carrying the server message, so an error frame **rejects** instead of leaving
`subscribe` waiting for a `subscribed` reply that will never come. The test awaits
each call sequentially, so the pending queue never holds more than one entry and the
"first pending, any type, if error" match cannot mis-correlate. No timers, no sleeps,
no discovery wait, no row delivery, no shared topic across the two provider contexts.
The message it matches (`/empty segment/`) is the seam's own
`"topic: an empty segment names nothing"`.

Green-by-construction at the diff base is the honest description and is fine: the
guards were deleted in cycle 1, so the case is a regression lock, and its red is the
mutation that restores them.

## 3. N4's counterfactual — holds, and it is the right assertion

The product form is correct: longest accepted (246) → companion derived as both DDS
providers derive it (`+ "/__schema"`, 255) → the sink's own truncation modelled as
`substr(0, min(size, 255))` → `announced == companion`, nothing lost, and
`announced != longest`, still a different name from its data topic. That is the
claim, not the arithmetic.

The counterfactual holds: `247 + 9 = 256 > 255`, so 247 is refused *because of the
companion*, not because of the data name — which is exactly what makes 246 the right
number rather than a number that happens to work. `pubsub_tests`: **24/24**, the five
`Segments.*` among them.

## Nit

* The new case covers the two text-frame paths into `SplitTopic` but not the binary
  `publish` path (`ws_session.cpp:271`), which also splits. Low value — a publish to
  an undeclared topic already errors — but it is the one door of the three left
  unwatched.

## RECORD (this round)

* `test_segments.cpp`, the rule-6 counterfactual comment says "Bounded at 255 instead,
  this assertion is what fails". The `EXPECT_GT` beneath it is built from literals, so
  it does not move under M9; what actually reddens under M9 is the `247 is refused`
  row earlier in the same case. The assertion is correct and worth keeping — only the
  sentence about which one fails is off.
