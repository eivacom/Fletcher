# PDA-DEC-1H — code review

Reviewer: independent code review (the only review this item gets — compressed cycle, no design
doc, no architecture cycle). Scope: `git diff 39512da..5af2bfb`, tree clean at `5af2bfb`.
Files: `integration-tests/pubsub-conformance/{CMakeLists.txt,README.md,subjects/xrce_main.cpp}`,
`integration-tests/fastdds-xrce-interop/{CMakeLists.txt,README.md,tests/test_interop.cpp}`.

**Verdict: APPROVE-WITH-DEBT.** No blocking findings. The approach is right: the guard now asks
the OS *who holds the port*, takes that answer **after** the reachability probe (correct
ordering), and the "foreign beats ours" rule is implemented as a two-flag accumulate over the
whole table, so it genuinely cannot be inverted by iteration order. Three should-fix items
below, one of which is a factual correction to the duplication justification.

---

## Soundness of the ownership proof — checked, and it holds

Things the brief asked me to attack, all of which came out clean:

- **`GetExtendedUdpTable` sizing/free/walk.** Size-then-read with `ERROR_INSUFFICIENT_BUFFER`
  retry (4 attempts), buffer is a `std::vector<unsigned char>` so it is freed on every path
  including the early returns; `size` is re-set from `buffer.size()` before the second call
  (over-sizing is legal). `operator new` alignment satisfies `MIB_UDPTABLE_OWNER_PID`. The walk
  is `table->table[i]` for `i < dwNumEntries` — no manual pointer arithmetic, no off-by-one.
  Accepting `NO_ERROR` from the sizing call is harmless because the buffer floor is
  `sizeof(MIB_UDPTABLE_OWNER_PID)`.
- **Byte order.** `((dw & 0xFF) << 8) | ((dw >> 8) & 0xFF)` reads bytes 0 and 1 of the DWORD in
  that order — exactly `ntohs` of the low 16 bits on little-endian, which is what the field
  documents. This is the classic bug and it is *not* present.
- **Wildcard address.** The row's local address is deliberately ignored, so `0.0.0.0:2019`
  matches. That also makes an unrelated `192.168.x.x:2019` count as foreign — a false *refusal*,
  never a false pass. Correct direction.
- **AF_INET only.** The Agent is spawned `udp4`, so an IPv6 row on the port cannot be the
  endpoint being certified. Consistent with the comment, and measured green on Windows.
- **Foreign beats ours.** `ours` and `foreign` are both accumulated over the whole table before
  the decision, and `foreign` is tested first. Order-independent. Correct.
- **`/proc` parsing.** `/proc/net/udp` field layout verified: `tx_queue:rx_queue` and
  `tr:tm->when` are colon-joined, so `inode` really is `token[9]`; `token.size() < 10` skips the
  header and any short line. `rfind(':')` on the local address gives the port for both the IPv4
  (`0100007F:07E3`) and the IPv6 (32 hex nibbles) form. `readlink` truncation is impossible for
  `socket:[N]` at 255 bytes; `value.compare(0, 8, ...)` is safe for short targets and
  `value.back()` is safe because `len > 0`. `readdir` yields `.`/`..`, whose `readlink` fails and
  is skipped.
- **Read errors refuse rather than pass.** `UdpPortInodes` returns false (⇒ `kUnprovable`) only
  when *neither* `/proc/net/udp*` opens; `ProcessSocketInodes` returns false only on `EACCES`. A
  pid that exits mid-read yields an empty inode set, which makes every socket on the port foreign
  ⇒ refusal. A dead child correctly cannot own the port; on POSIX `Alive()` sets `pid_ = -1`
  first, and `ProcessSocketInodes(-1)` returns an empty set, so `DeadChildRefusal` picks the
  leftover-Agent story. Both platforms' dead-child paths converge on the same refusal.
- **The race it exists to fix.** Gone, and for the right structural reason: `ProveOwnership()`
  runs *after* `WaitUntilReachable()`, so "something answered" is no longer allowed to be the
  final word. On POSIX both the ≤1 s spawn reap window and the post-probe table read reach the
  same refusal, so a child that takes longer than 1 s to hit its bind error is still caught.
  Residual window: the proof is a point-in-time snapshot and is never re-taken. That is
  acceptable — a foreign Agent cannot take a port our child already holds without our child
  dying first — but the READMEs and the class comment describe the guarantee without the words
  "at bring-up", which slightly over-sells it (see RECORD).
- **Process lifetime.** `OwnedAgent` is RAII with copy deleted; the two Agents in the forcing
  test are stack objects, so they die on normal return, on `ASSERT_*` early return (the incumbent
  is destroyed correctly), on `GTEST_SKIP`, and on an exception. The fixture holds a `unique_ptr`
  reset in `TearDown`, and googletest runs environment tear-down even when a fatal failure
  occurred in `SetUp`. Windows closes both handles (`hThread` at spawn, `hProcess` in `Kill`);
  POSIX reaps in `Alive()` or in `Kill()`, never both, so no zombie and no SIGTERM to a recycled
  pid.
- **Coverage of the defect class.** `MICRO_XRCE_AGENT_PATH` appears in exactly these two
  harnesses — there is no third Agent-spawning harness left unguarded.
- **Counts.** 12 `TEST_P(ProviderConformance, …)` × 2 `INSTANTIATE_TEST_SUITE_P` + `Registry` +
  `ConformanceXrce` = **26 gtest cases**, and the diff adds/removes no `add_test`, so
  **82 ctest entries** is structurally unchanged. Interop: 4 `TEST(` = **4 gtest cases** in
  **1 ctest entry**. Both reported figures check out at the entries-vs-cases level.
- **`iphlpapi`.** Added inside `if(WIN32)` in both projects (and inside
  `if(FLETCHER_CONFORMANCE_XRCE)` in the conformance one). Cannot reach the Linux lane.
  `winsock2.h` → `windows.h` → `iphlpapi.h` ordering is correct and no `ws2_32` is needed
  because the port swap is done by hand.
- **Probe side effects.** The reachability probe constructs the provider only; participants are
  created in `CreateTopic`/`Subscribe`, so the two extra Agents never create a Fast DDS
  participant and `TerminateProcess` on them leaks no shm segment.

---

## should-fix 1 — a *runtime* failure of the OS query silently re-admits the very defect this item closes (confidence: high; reachability: low)

`PortOwnership::kUnprovable` conflates two different things:

- the `#else` branch — a platform with no way to ask (compile-time, honest), and
- `GetExtendedUdpTable` returning anything other than `NO_ERROR`/`ERROR_INSUFFICIENT_BUFFER`,
  four consecutive `ERROR_INSUFFICIENT_BUFFER`s, `EACCES` on `/proc/<pid>/fd`, or neither
  `/proc/net/udp*` opening — a **runtime failure on a platform that does have the answer**.

Both land in the same arm of the `switch`, which sets `ownership_unprovable_`, prints one
`[   INFO   ]` line to stdout, falls back to bare liveness and **returns success**. On the second
class of cause that is a silent pass: the suite certifies against whatever Agent answers, the
forcing test reports `SKIPPED` (which ctest scores as a pass), and the only trace is one INFO
line in a multi-thousand-line log. That is precisely the failure mode PDA-DEC-7 caught by hand,
back in the tree behind a different door.

It cannot become the *normal* path (the normal path returns `kOurs` on the first table read, and
that is measured), so this is not blocking — but the fallback is machinery that exists to
tolerate a state the code could refuse at the door.

**Fix (also the simplification):** split the two causes, or better, delete the tolerance. Both
supported platforms answer; make the third-platform case a build-time refusal
(`#error "PDA-DEC-1H: no UDP port-ownership query for this platform"`) and make every *runtime*
failure a refusal that names the OS error (`GetLastError()`/`errno`). That removes
`ownership_unprovable_`, the `[   INFO   ]` branch and the `GTEST_SKIP` arm of the forcing test —
about 40 lines from *each* of the two copies — and leaves no path on which the guard passes
without having proved anything.

## should-fix 2 — the interop probe's session-key range collides with the three interop tests' own keys (confidence: high)

`integration-tests/fastdds-xrce-interop/tests/test_interop.cpp:107`

```cpp
constexpr uint32_t kProbeSessionBase = 0xF0F00000u;
uint32_t NextProbeSessionKey() { ... return kProbeSessionBase + (counter.fetch_add(1) & 0x0000FFFFu); }
```

and the three interop cases use the fixed keys `0xF0F00001`, `0xF0F00002`, `0xF0F00003`
(lines 735, 800, 866). The probe loop calls `NextProbeSessionKey()` **once per attempt**, and the
fixture's Agent on 2018 routinely needs several attempts while the Agent binds — so probe keys
`0xF0F00001…3` get handed out on the *same* Agent the three cases later use. Before this commit
the probe used the single constant `0xF0F0FFFF`, deliberately outside the tests' range; the
comment introduced here even states that the base exists to *avoid* key reuse, while the range it
picks walks straight into it. The file's own header invariant ("Each test uses its own XRCE
`session_key`") is thereby weakened.

The consequence is a loud flake, not a silent pass (a stale client key at `create_session` time),
so should-fix rather than blocking. **Fix:** move the base clear of the tests, e.g.
`kProbeSessionBase = 0xF0FF0000u`; or route all four through one `NextSessionKey(base)` helper
with distinct bases the way the conformance file does — that file's single shared counter across
distinct high-octet bases (`0x50`–`0x54`) is the right pattern and is collision-free.

## should-fix 3 — the duplication is defensible, but its stated justification is factually wrong (confidence: high on the premise; medium on the remedy)

I checked the premise against the workflow files rather than taking it on trust, as instructed.

Confirmed: `ci.integration-test.pubsub-conformance.yml` and
`ci.integration-test.fastdds-xrce-interop.yml` each have an `ubuntu-latest` and a `windows-2022`
job with its own `sparse-checkout` block (four blocks), and `ci.pr.yml` has one path filter per
lane (two filters). Confirmed: the two ownership blocks are **byte-identical**, 155 lines each.
Confirmed: both lanes do run Linux, so the `/proc` path is genuinely exercised in CI and a false
refusal there would be loud.

But the claim that a shared file "would have to be added to four sparse-checkout blocks and two
path filters" is **not true**. Both lanes already sparse-checkout `core`, `pubsub`,
`fastdds-pubsub-provider` and `xrcedds-pubsub-provider`, and both path filters already list
`'core/**'`, `'pubsub/**'`, `'fastdds-pubsub-provider/**'` and `'xrcedds-pubsub-provider/**'`. A
shared test-support header under, say, `xrcedds-pubsub-provider/testing/` would be present in
both sparse trees and would retrigger both lanes **with zero workflow edits** — so the
"guard you can forget to arm" argument does not apply to that location.

The real cost is different, and smaller than the one stated: the harnesses consume
`xrcedds-pubsub-provider` as a Conan package and today reach outside their own directory for
nothing (no `../..` in either `CMakeLists.txt` or `conanfile.py`), so sharing means either
exporting a test-only header from a shipped provider package (one `exports_sources` plus one
install line) or breaking that self-containment. That is a legitimate trade to decline — but it
should be declined for *that* reason, not for a CI reason the workflow files contradict.

On the drift guard itself: each lane carrying its own equally-named forcing test does mitigate
the risk, but only partially. Both copies' `kNobody` and `kSomeoneElses` messages contain the two
substrings the test greps for, so a divergence in *which* refusal a copy produces would not be
caught; what the paired tests actually pin is "a foreign Agent is refused with
operator-actionable text", which is the important half.

**Fix:** either move the block to `xrcedds-pubsub-provider/testing/udp_port_ownership.hpp` (both
lanes get it for free), or correct the comment in both files to state the real reason (package
self-containment), so the next reader does not inherit a constraint that does not exist. Not
blocking either way.

---

## Nits (one line each)

- Windows `DeadChildRefusal()`/`UdpPortOwnership()` match by pid after the child has exited; a
  recycled pid could theoretically read as `kOurs`. POSIX clears `pid_` to `-1`; clearing
  `process_id_` in `Alive()` on `WAIT_OBJECT_0` would close it.
- POSIX `Alive()`: `waitpid` returning `-1` (e.g. `ECHILD`) falls through to `WIFEXITED(status)`
  on a `status` waitpid never wrote, reporting "exited immediately with status 0" for an error.
- The interop `Spawn()` now always burns a full 1 s in the POSIX reap window before returning
  (new to that harness, pre-existing in the conformance one) — ~3 s per interop run now that the
  forcing test spawns two more Agents.
- Abnormal termination (ctest `TIMEOUT`, crash, Ctrl-C) still leaks Agents, and this item raises
  the number at risk from 1 to 3 per harness; a Windows job object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` plus `prctl(PR_SET_PDEATHSIG)` in the POSIX child would
  make cleanup structural rather than destructor-dependent. Pre-existing, amplified.
- POSIX `Kill()` does an unbounded `waitpid` after `SIGTERM`; an Agent that ignored SIGTERM would
  hang tear-down. Pre-existing.
- `ADD_FAILURE()` inside `BuildChildEnvBlockWithAugmentedPath()` is now reachable from the
  forcing test's constructor calls, where a non-fatal failure would redden that test rather than
  the fixture. Effectively unreachable (`GetEnvironmentStringsA` returning null).

## RECORD (paperwork for the PM — never blocking)

- `integration-tests/pubsub-conformance/README.md` lines 392–393 and ~439–440: a literal CR
  inside a code span was replaced by a real newline, splitting a markdown table row and a prose
  sentence across two lines. The `InProcessDocumentToleratesCrlfAndBlankLines` table row no
  longer renders. Spell it `` `\r` `` or `` `CR` `` instead.
- Both READMEs and the `OwnedAgent`/`proven()` comments state the ownership guarantee without
  scoping it to bring-up; the proof is a one-shot snapshot taken at `SetUp` and never re-taken.
  One clause ("at bring-up") would make the claim honest.
- `test_interop.cpp` header comment still says "so the two tests can run concurrently" in a file
  that now holds four cases and three interop directions.
