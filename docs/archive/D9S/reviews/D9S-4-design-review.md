# D9S-4 design review — cycle 1

**Verdict: NEEDS-REWORK (3 BLOCKERs, 2 DEBT).** I found no deviation from a ruling, law or locked decision.
B1 reframes owner Decision 1. Before B3's fix lands, the PM must confirm it fits D-D9S-8.

**Reviewed:** `plans/D9S-4-batches-cross.md`, `plans/D9S-4-brief.md`; against d9 §1, §3–§5, §7; d7 §3–§6;
register BC-8–15; `D9S-locked-decisions.md`; ladder rungs 4–10; `D9S-4-audit-0a.md`.
**Proof of read:** I read `plans/RIX-rulings.md` top to bottom. It has 431 lines and 32 entries. The last
heading (l.421) is "2026-11-13 — D9S-3: a test-only Arrow dependency of the edge build is refused too".
I found no tier-1 disagreement.
**Budget:** the design is 300 lines (ripgrep), the brief 55 lines. The new public surface is 4 (B2).
§7b below prices the implied work.

## BLOCKERs

**B1. Decision 1 rests on a P5 that is too broad, and on a false reason for not forbidding H1.**
- **P5 holds only for distinct topics.** One `fl_subscriber` now holds two `Subscriber`s (`subscriber_arrow.cpp:340`),
  and seam §7 clause 4 allows one callback per topic per instance. So when one handle holds a row and a
  batched subscription to the same topic, `inprocess` silently replaces the first (`in_process_provider.cpp:311-315`)
  and Fast DDS refuses the second (`fast_dds_pubsub_provider.cpp:1072-1074`). Neither option line says so.
- **H1's reason is false.** Both id counters start at 1 (`subscriber.cpp:267`). Tag only the batched ids (a high bit, cleared by stage 5's
  close). `fl_subscriber_unsubscribe` is untouched: it sees an unknown id, a no-op, so no edge answer changes (L13).
- **Acceptable fix:** forbid H1 with disjoint ids (about 4 lines, cheaper than an owner question). Narrow P5.
  State the same-topic outcome for each provider in the header and in both options. Offer a same-topic
  refusal only as an owner option (it would be an L4 exception). Register the provider split as a d3/d4 finding.

**B2. The new public surface is 4 against a budget of 3. Ruling: take the cut.**
- **Change:** use `int32_t reason; /* an FL_BATCH_* value */`, with the three values as named constants (an
  untagged enum or `#define`s) and no new type.
- **It violates nothing.** The named values satisfy d9 §1's "C spelling", and no law requires a typedef.
- **It follows the header's own precedent.** `fl_error` already stores `fl_status` and `fl_origin` as
  `int32_t` (`binding.h:319-324`), because C leaves an enum's width to the implementation and bindings
  marshal `fl_batch` by layout.
- **Acceptable fix:** make that cut, which brings the surface to 3, and keep "0 is never a reason".

**B3. Unstated premise: that `pubsub-arrow` and `arrow-bridge` can link into a shared object.**
- **Why it fails:** both are static-library packages, and neither has `fPIC`. `pubsub/conanfile.py:21-33`
  records the same Linux link failure ("R_X86_64_TPOFF32 against `__tls_guard`") on a tree that built
  green on Windows (BIND locked decisions l.635). `pubsub-arrow` has a `thread_local` (`publisher_arrow.cpp:77`).
  It also reaches core's thread-local frame (`subscriber_arrow.cpp:136`). The only evidence is a Windows
  build by hand, so the failure would first show in the owner's PR run, after close.
- **Acceptable fix:** add `fPIC` to both recipes, as `pubsub` does. Put both files in Files-to-touch and in
  the price. **PM:** confirm under D-D9S-8 that this is not "domain 7 and domain 2 code"; otherwise STOP-AND-ASK.

## The eight points

1. **See B1.** The id collision is a design fix, not an owner choice. Once Decision 1 states the same-topic
   collision, it stays a real owner question.
2. **Decision 2 is lawful sequencing, and the question is well formed.**
   - The ladder planned it: rung 8 is red because "domain 7 swallows the throw". L18 stays unbuilt until
     D9S-6, and D-D9S-4 is the round's end state.
   - A thunk that does not throw and sends no no-batch loses rows just as silently. One that does send a
     no-batch is L18 landing before its test.
   - Both options carry consequence lines, and the default is grounded. DEBT-1 widens H3.
3. **See B2.** A bare integer violates nothing and matches `fl_error`.
4. **BC-9:**
   - The scan is structural for its question, and its control can fail.
   - `c-abi/src` has 0 hits today for the seven patterns.
   - `max_rows` and `timeout_ms` are read only to build `BatchOptions`, and rung 7's byte-equal message is
     the net for a range check.
   - I found no window, count, timer, queue or thread.
5. **One site.**
   - The door reads BC-10's function.
   - CMake picks the backend in the same block that sets the constant (`CMakeLists.txt:124-129`).
   - A `static_assert` makes a mispairing fail to compile.
   - The edge backend is unreachable, and its `kInternal` says nothing about the build.
6. **The check stays meaningful and green.**
   - The edge graph does not change.
   - The edge sources name no Arrow type (`shared_ptr<void>`, a C-only `SubscribeBatches`).
   - `-impl` is untouched, and control 4 still finds one `c_abi_tests`.
   - `arrow/*:shared=False` is Arrow's default anyway.
7. **The guard is correct for both the array and the schema.**
   - `ExportRecordBatch` releases its schema on failure and writes the array only on success, so
     zero-initialising and releasing whatever is non-NULL covers every exit.
   - Moved structs and released structs both have a NULL `release`, so a double release cannot happen.
   - A partial move is handled.
8. **Q12 is consistent.**
   - `FL_ORIGIN_SEAM` is the arm that carries every refusal the shim makes itself, and L2 adds no status.
   - D-BIND-54 switches only on `(INVALID_ARGUMENT, CODEC)`.

## §7b — the implied work against the 250 ceiling

- **What the declared +209 leaves out:** B3's two recipes (about +15) and B1's tag (about +4).
- **My estimate:** at this tree's comment density, `batches_server.cpp` is about 110–125 lines (declared
  95) and the entry point about 40 (declared 32). That puts the stage at about **235–265**.
- **The `binding.h` comment exclusion is honest:** the ladder lists those comments but does not count them.
- **Decide the split now, not after implementation.**
  - Re-declare the figure after B1–B3.
  - If it passes 250, take 4a/4b now. 4b (attachments) moves only about 15–20 lines.
  - If it passes about 265, 4b cannot absorb the overrun. That is a planning question the PM takes to the
    owner.

## Tree claims verified

- `binding.cpp:825-832`: the arrival is allocated first. `binding.cpp:947-959`: the timeout is forwarded.
- `CMakeLists.txt:124-129`, `conanfile.py:97-102` and `tests/CMakeLists.txt:8-13`: as cited.
- `tests/CMakeLists.txt:170-173`: the probe tests link Arrow.
- `in_process_provider.cpp:30-39`: P3 holds.
- `subscriber_arrow.cpp:340, 422, 458`: batched ids come from `SubscriberArrow`'s own `Subscriber`.
- Only `ci.c-abi.yml` builds `variant=server`.
- `binding.h:1063-1066`: `fl_publisher_publish_rows` takes `const fl_attachments* const*`, the same shape
  as the design's.

## DEBT (appended to `design-debt.md`)

- **DEBT-1 (H3 now; built in D9S-6):** until D9S-6, thunk step 1 (the per-row views) and the fill step also
  throw into domain 7. BC-12's one catch must cover steps 1–4, so H3 should say "a failed hand-over", not
  "a failed export".
- **DEBT-2 (step 3):** a fourth domain-7 `Reason` (d7 Q4, still open with the owner) must be a compile error
  in the reason `switch`, never a 0 on the wire.

## Cycle 2

**Verdict: APPROVE-WITH-DEBT(3).** B1–B3 are resolved and no BLOCKER stands. No deviation from a ruling,
law or locked decision remains. B3 now sits inside the owner's named exception.
**Proof of read:** `plans/RIX-rulings.md` has 443 lines and 33 entries; ripgrep and Read agree (your brief
said 444). The new last heading (l.433) is "2026-11-13 — D9S-4: position-independent code for
pubsub-arrow and arrow-bridge". It rules "Allow the build option": an `fPIC` option exactly as in `pubsub`,
and no other edit to domains 7 or 2.
**Budget:** the design is 294 lines and the brief 57. The new public surface is 3: the entry point,
`fl_batch` and `fl_batch_fn`.

**B1: resolved.**
- Bit 63 keeps the two id spaces apart, because row ids count up from 1.
- `fl_subscriber_unsubscribe` is unchanged and treats a tagged id as unknown, a no-op, so L13 holds.
- Stage 5's typed refusal of an untagged id answers a question about the shim's own encoding, as `RequireOut`
  does. It is not a seam rule answered again, so it is not an L4 exception.
- P5 is narrowed. The same-topic collision is stated as H1, appears in all three options and is registered
  for domains 3 and 4. One wording slip remains: DEBT-3.

**B2: resolved.** `reason` is now an `int32_t` taking `FL_BATCH_*` values 1, 2 and 3, which matches
`fl_error`.

**B3: resolved by the ruling.** Branch (a) matches the ruling's text. The design and brief still said
"pending"; I brought them up to date as a NIT.

**The split**
- **4a builds nothing that 4b must undo.** 4b appends one field to a struct the shim writes and the binding
  reads through a pointer. That is D9S-6's mechanism for `failure`, and it changes no signature. 4b also
  adds thunk step 1 and its own test.
- **Batches without attachments in 4a are lawful.**
  - The ladder pre-authorises exactly this split ("4b: attachments filled").
  - L14 does not name attachments, and d7 L3 binds domain 7's delivery, which does not change.
  - Nothing ships between 4a and 4b.
  - Appending in 4b is safer than a 4a field that is always NULL, which would break "none NULL".
- The edge refusal stays in 4a with the entry point, as L12 and the ladder require.

**Ceiling.** 4a counts about 236 (+298/−12, less the 50 `binding.h` comment lines the ladder does not
count). My own price is about 232–255, so the declared figure is credible, with about 14 lines of margin.
The swing is `batches_server.cpp`'s comment density. 4b counts about 17 lines in 2 files, which fits the
light lane. No further split exists inside rung 4: if 4a passes 250 at phase A, stop and report to the PM.

**Decisions**
- **Decision 1 is well formed.** Each of (a), (b) and (c) has a Users line and an Architecture line. The
  recommendation rests on the round's purpose and on L4, and the default is (a).
- **Decision 1 is genuinely the owner's.**
  - The collision is inherent while domain 7 stays untouched, because `SubscriberArrow` owns its
    `Subscriber`.
  - (b) breaks the surface budget, and (c) would be a third L4 exception, which only the owner grants.
- **Decision 2 is unchanged and well formed** (cycle 1, point 2).

**DEBT** (register updated): DEBT-1 is struck, because H3 now says "a failed hand-over" and the catch covers
steps 1–4. DEBT-2 stands. New:
- **DEBT-3 (step 3, header doc).** "Its arrival answers `FL_SUBSCRIPTION_ENDED`" holds only for an arrival
  that is still pending. In the default `kAsDeclared` mode the arrival is already Ready
  (`in_process_provider.cpp:329-332`), and the `Subscriber` shares it (`subscriber.cpp:454-460`). So the
  earlier subscription stops with no signal at all. Word the header that way; the brief already does.
- **DEBT-4 (PM, housekeeping).**
  - Annotate D-D9S-8 with the `fPIC` exception (the ruling's "Applies to").
  - The tracker's D9S-4a row still promises attachments, and D9S-5's dependency should name 4b.
  - 4a's doc marks must leave the attachments half of Q6 and BC-13 unbuilt until 4b.
