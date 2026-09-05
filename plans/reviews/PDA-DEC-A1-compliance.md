# PDA-DEC-A1 — compliance review (adversarial conformance)

Diff base `a4f2d41`; working tree, uncommitted. Six modified files + one untracked
(`core/tests/test_write_buffer.cpp`). Rulings ledger read in full first:
`grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **45**; last two entries are
*"The seam permits an uncopied row end to end"* (44) and *"The guard claims what the
interface permits, measured with a stand-in"* (45).

**Verdict: ISSUES — two blocking conformance defects, both of the same kind: the
code half landed and the ORACLE half did not.**

---

## ORACLE-WINS TRIPWIRE — read this first

Rulings **44 and 45 are the two most recent entries in the ledger and neither is
implemented in the tree.**

- `docs/pubsub-interface-spec.md` — **not touched at this diff base.** `git status`
  does not list it. §3.1 still ends at clause 5; §8's rows bullet and §8.1's
  measured-interval sentence are byte-for-byte what they were before the design was
  written.
- `integration-tests/pubsub-conformance/README.md` — **not touched.** No claim limit
  for `encode_copies` exists anywhere.

Ruling 44 is an **amendment to frozen text** ("§8's promise scope, amended to the
whole send path… **Twelfth amendment authorised for PR #126**"). Ruling 45 is an
instruction about a specific file ("`integration-tests/pubsub-conformance/README.md`'s
claim limit for the new `encode_copies` measurement"). Both were appended *after* the
design (`d3e6b2c`) and its review (`8c5c013`). The implementation conforms to the
design's *code* half and contradicts the ledger's *published* half.

The design anticipated exactly this state and forbade it, unconditionally, in P3:

> **STOP-AND-ASK / do not guess:** if it is unanswered when implementation reaches the
> spec edit, the item does not close — **do not land §3.1 clause 6 with §8 left saying
> rows are already fine.**

That is the state in the tree right now. The owner answered on 2026-09-04; the answer
was not carried into the artefact it governs.

---

## C1 — BLOCKING · the spec amendments did not land; two ordered retirements survived

`docs/pubsub-interface-spec.md` is unmodified.

**What survived that the design ordered retired** (`Files-to-delete`, items 1 and 2 —
this is the converse check, and both fail):

| Ordered retired | Status | Location |
|---|---|---|
| §8's rows bullet *"already there, via `Publish`'s inversion and `FixedWriteBuffer`"* | **alive, verbatim** | `docs/pubsub-interface-spec.md:812` |
| §8.1's *"the window base after the encoder's last append (§3.1 clause 5)"* as the start of the measured interval | **alive, verbatim** | `docs/pubsub-interface-spec.md:832` |

The first is the sentence the design review verified is **false for a binding** (Claim 5).
It is still published as true. The second is the sentence ruling 44 names as the root
cause ("§8's promise and §8.1's measurement **both begin at the window**").

**Dangling normative citations created by the code.** The implementation cites a spec
clause that does not exist, in four places:

- `core/include/fletcher/core/write_buffer.hpp:40` — "The window has a **write end as
  well as a readable one** (§3.1 clause 6, `AppendInPlace` below)"
- `core/include/fletcher/core/write_buffer.hpp:120` — "── The window's WRITE end (spec
  §3.1 clause 6) ──"
- `core/tests/test_write_buffer.cpp:6` — "Contract: docs/pubsub-interface-spec.md §3.1
  clause 6"
- `integration-tests/pubsub-conformance/src/seam_vocabulary.cpp:206` — "§3.1 clause 6 —
  the window has a WRITE end"

Locked decision 5 requires the normative rule to be written in the header, and it is —
but it is written as a *derivation* of a spec clause that has not been written. A reader
following the citation lands on a five-clause §3.1 whose clause 5 gives the window a
readable end only, and on a §8 that says the property is "already there".

**Also unlanded here:** A1-DEBT-5, which is specific about the wording and about §8.1's
mechanics ("§8's replacement bullet must say so in the sentence itself, and §8.1's
rewrite must move the interval's **start** to the producer's write site while keeping
the window-base sample described as an interior point"). The harness header
(`copy_accounting.hpp`) already carries that framing internally — "Now an INTERIOR point
of the measured interval, not its start" — so the code knows what the spec is supposed to
say and the spec does not say it.

**Acceptable fix:** land the three edits the design's *Spec amendments* section and
ruling 44 specify — (i) §3.1 clause 6: the in-place fill, the C form
`size_t (*writer)(void* ctx, uint8_t* dst, size_t room)`, the one-call borrow window,
`room >= min_bytes`, commit-by-return, and the two `kInvalidArgument` refusals; (ii) §8's
rows bullet rewritten as a **permission** in ruling 44's own terms ("a client that uses
the new call can send without any copy; a client that ignores it still copies and the
seam cannot stop it"), never as an unconditional guarantee; (iii) §8.1 with the interval
starting at the producer's write site, the window-base sample retained as an interior
point, and the staging-producer control named. Nothing about the code needs to change.

---

## C2 — BLOCKING · the harness README did not land; ruling 45 is unimplemented and the guard now contradicts its own published definition

`integration-tests/pubsub-conformance/README.md` is unmodified.

**What survived that the design ordered retired** (`Files-to-delete`, item 3):

> `integration-tests/pubsub-conformance/README.md:177` — "…between the encoder's first
> write and the callback's return. **Not a copy: the encode itself**; anything a transport
> does once the bytes leave the seam; a window refill, below."

The design calls this sentence "the written form of the blindness this item removes".
It is still there, and it is now **actively false about the shipped instrument**:
`Judge()` scores `encode_copies` precisely on the encode, and
`CopyAccounting.StagingProducerIsCaught` pins it at 1. The guard's published definition
of a copy and the guard's behaviour now disagree, in the direction of under-claiming —
which is the shape (a green guard over a lost property) this item exists to remove.

**Ruling 45 is not implemented at all.** It names one artefact and one obligation:

> **Applies to:** `integration-tests/pubsub-conformance/README.md`'s claim limit for the
> new `encode_copies` measurement.

No such sentence exists. The claim limit is currently stated only in a code comment
(`copy_clauses.cpp:154-157`) and in `copy_accounting.cpp:292-294`. Those are correct and
well-worded, but ruling 45 is about what the *harness publishes*, and it is the ninth
consecutive time the owner chose "the narrow claim stated honestly over a wide one
implied" — the whole point being that it is written down where a reader will meet it.

**Consequential omissions inside the same unlanded file:**

- **A1-DEBT-1's owed README line is absent.** The design review's remedy was three-part:
  "name it as the second residue in the ladder, disclose it in the header's normative
  block, **and add one line to the README's claim limit**." The first two landed
  (`write_buffer.hpp:158-172`, residue (b), and it is a good, honest disclosure naming
  the `FixedWriteBuffer`-over-transport-pool case). The third did not. The published
  claim therefore does not mention that a producer may commit bytes it never wrote.
- **Entry count stale in the direction that hides new tests.** `README.md:170` — "`ctest
  -R 'CopyAccounting\.'` runs the whole oracle: **seven entries**". The suite now has 11
  (`TEST_P` × 3 subjects + 5 `TEST`s = the design's predicted 7 → 11, verified by count in
  `copy_clauses.cpp`). The entry table has no rows for
  `InPlaceEncodeWritesIntoTheDeliveredWindow` or `StagingProducerIsCaught` — i.e. the
  forcing test and its live negative control are unpublished.
- **`SeamVocabulary` count stale too.** `README.md:315` — "eight entries", eight-row
  table; the item adds a ninth (`AWriteBufferReferenceCanBeFilledInPlace`). Not in the
  design's Files-to-touch note for the README, but it is the same file and the same edit.

**Acceptable fix:** in one pass over that README — rewrite the copy definition so the
measured interval starts at the producer's write site (retiring "Not a copy: the encode
itself"); add the four new `CopyAccounting` rows and correct 7 → 11; add the ninth
`SeamVocabulary` row and correct 8 → 9; add the ruling-45 claim-limit sentence in the
owner's own terms (the interface **permits** an uncopied send, measured with a stand-in
producer, not that a C#/Rust binding achieves it); add A1-DEBT-1's one line on the
over-report residue.

---

## F3 — NON-BLOCKING · a state the header declares unreadable is silently readable

`copy_accounting.hpp:69-72` states a rule:

> "`produced_at == 0` means the sampler never ran and **NO verdict may be read** — an
> unsampled leg must fail as itself rather than default into `encode_copies == 1`."

`Judge()` does not enforce it. `copy_accounting.cpp:442-446` computes
`verdict.encode_copies = ledger.produced_in_window ? 0 : 1;` unconditionally, so every
pre-existing leg — `PublishAndReceivePerformNoPayloadCopies` ×3, `StagingIsCaught`,
`BorrowedAttachmentCostsNoCopies`, `RefillMovementIsCountedNotFailed` — now silently
carries `encode_copies == 1`, which is exactly the default the comment says must not
exist. The enforcement lives entirely in the test-side macro `COPY_MUST_HAVE_PRODUCED`
(`copy_clauses.cpp:94-100`), which is applied to the two new legs only.

This is the *refusal became a default* shape, in miniature. It is **not blocking**:
A1-DEBT-4's literal remedy was "assert `produced_at != 0` … before any verdict is read",
and no caller reads `encode_copies` on an unsampled leg today, so nothing is currently
wrong. But the invariant is asserted in prose at the type and honoured only by
convention at two of six call sites, which is how the next leg gets it wrong.

**Acceptable fix (either):** make the unsampled case unrepresentable in the verdict — e.g.
`CopyVerdict` carries `bool encode_measured` that `Judge()` sets from `produced_at != 0`,
and the harness's assertion reads that — or narrow the header sentence to say that the
discipline is the harness's and name `COPY_MUST_HAVE_PRODUCED` as its only enforcer.

## F4 — NON-BLOCKING · `std::overflow_error` gains a second, different cause

`write_buffer.hpp:214-216` throws `std::overflow_error("… the refill did not deliver
contiguous room")`. The design ordered this throw (step 3), so this is conformance, not
drift. But `status.hpp:138-144` documents the mapping it feeds:

> "At this seam an overflow_error is **always** `FixedWriteBuffer` refusing a row that does
> not fit the transport's payload bound (write_buffer.hpp is its only thrower)"

"Its only thrower" stays true; "always `FixedWriteBuffer` refusing a payload bound"
becomes false — a subclass whose refill under-delivers now also arrives as
`kPayloadTooLarge`, a number that tells the caller to raise the bound or split the row when
the real cause is a broken subclass. The design's P1 stop-and-ask covers the case existing
at all; nothing covers the number it is reported under.

**Acceptable fix:** one clause in `status.hpp`'s comment naming the second cause, or throw
`std::logic_error` there so it lands on `kInternal` (which is what a subclass defect is).
Either is a one-line change; this is a note for step-4b as much as here.

---

## What was checked and found conformant

Stated briefly, because depth belongs with findings — but these three were named in the
brief for this review and each was verified against the tree rather than the diff summary.

- **`VectorWriteBuffer::Reserve` shadowing:** genuinely closed. There is no
  `WriteBuffer::Reserve` (grep, whole tree); the private helper is renamed
  `ReserveStorage` with both call sites (`AppendSlow`, `Refill`) and the definition
  updated; the only other `Reserve(` in the tree is `arrow::ArrayBuilder::Reserve` in
  `pubsub-arrow`, unrelated. The rename's rationale is written where a reader meets it
  (`write_buffer.hpp:286-290`). No `Reserve`/`Commit` pair was introduced, so rung 1's
  "a reservation that outlives a refill / is committed twice / is never taken" stays
  unspellable.
- **Public surface:** 1 in `core` (`WriteBuffer::AppendInPlace`); counted strictly with
  the harness apparatus (`ProducerMode`, `RunProducerRoundTrip`) it is 3, matching the
  design's declaration and the round budget of 3. Nothing else was added to a public
  header.
- **The measured interval really did widen, for the new leg, and the chain closes.**
  `EncodeProduced` samples `produced_at` *inside* the borrow
  (`copy_accounting.cpp:305-315`), where it is the only place that can be answered; the
  forcing test then requires `encode_copies == 0` **and** `row_copies == 0` on the same
  publish. Since `pos0 == 0` for that leg and the forced refill happens before the writer
  runs, `produced_at == encode_base` and `encode_base == delivered_data`, so the two halves
  compose into one uninterrupted address chain from the producer's write to the
  subscriber's read. The instrument is pinned in both directions (stuck-true reddens
  `StagingProducerIsCaught`, stuck-false reddens all three forcing entries), and
  `JudgeArithmeticIsSound` gained rows proving the halves are independent.
- **Corner-case ladder survived intact.** All four rung-2 rows are implemented as typed
  refusals with nothing committed, and each has its own case:
  `ZeroMinBytesIsRefused`, `OverReportedLengthIsRefused`, `ReEntryFromTheWriterIsRefused`
  (including the nested-`AppendInPlace` variant), `FixedBufferWithoutRoomRefusesAsPayloadTooLarge`
  (asserting the *number* through `TranslateSeamFailure`, not just the C++ type). No refusal
  became a recovery path; no partial commit exists (the commit is one absolute assignment
  after both checks). P4 was honoured — no `PubSubStatus` append.
- **Debts:** A1-DEBT-2 (writer throws → nothing committed, header sentence + pinned case),
  A1-DEBT-3 (patch-below-the-lend-point stated as permitted and pinned; the stash residue
  moved out of rung 1 and beside the disclosed residue), A1-DEBT-4 (the
  `COPY_MUST_HAVE_PRODUCED` macro) are discharged. A1-DEBT-1 is discharged in the header
  and **not** in the README (see C2). A1-DEBT-5 is **not** discharged (see C1).
- **P1 re-verified independently:** `grep` for `public WriteBuffer` finds exactly four
  subclasses — `VectorWriteBuffer`, `FixedWriteBuffer`, the harness's `GrowableProbeBuffer`,
  and the new test-only `RelocatingWriteBuffer`, which is a deliberate relocating refill
  used to exercise steps 2 and 3. No subclass exists that would fire P1's stop-and-ask.
- **Locked decision 14 / round scope:** no `extern "C"`, no C header, no loader, no vtable.
  The C writer signature is stated in prose in the header comment, which is what §3.2
  already does for `Blob`. No `PubSubProvider` method added, removed or reordered; `Publish`
  stays inverted.
- **No coexistence bridge and no unordered deletion.** `Append(const uint8_t*, size_t)`
  is untouched and undeprecated, as the design requires; `EncodeAccounted` and legs 1–3
  are behaviourally unchanged (the `DriveRoundTrip` refactor passes the same encoder), so
  no existing entry lost what it measured.

---

## RECORD (PM corrects in place; never blocking, never a fix cycle)

- `plans/PDA-decouple-progress-log.md` has **no PDA-DEC-A1 entry** — it is in
  `Files-to-touch` and was not written.
- `plans/PDA-DEC-A1-brief.md`'s *"As landed"* footer is still the `<date>` placeholder.
- As-landed size: **+763 / −14** (tracked `git diff --numstat` = +423/−14, plus the
  340-line untracked `core/tests/test_write_buffer.cpp`) against a declared **+640 / −25**.
  Code-only, the declaration was ~+516 and the code came in at +763 (≈1.48×); the total
  sits inside the design's declared +600…+1100 band **only because the three documentation
  deliverables (~+110) are absent**, so the band was not truly tested. `core_tests` alone
  is 340 lines against a declared 185, from nine `WriteBufferInPlace` cases where the design
  named five — the four extras are the A1-DEBT-2/-3 pins and the two rung-2 rows the design's
  table listed but its line estimate did not.
- The design's *Files-to-touch* still says `VectorWriteBuffer::Reserve` has "3 call sites";
  it is 2 plus the definition. Already corrected by the design review (A1-DEBT-3); the code
  is right. No action beyond not re-copying the figure.

---

# RE-REVIEW after fix cycle 1 (adversarial conformance, independent context)

Diff base `3051b94`; working tree, uncommitted. Nine modified files + one
untracked (`core/tests/test_write_buffer.cpp`). Rulings ledger re-read in full:
`grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **47**; the last two are
*"Every protocol driver is C++, so both sides of the protocol ABI are C++"* (46,
scope) and *"Ceremony is expensive; group the remaining work into as few items as
makes sense"* (47, process). Rulings 44 and 45 — the two that blocked cycle 1 —
are entries 44/45 of 47, unsuperseded.

**Verdict: PASS-WITH-FINDINGS. No blocking conformance item. Both cycle-1
blockers (C1, C2) are closed in the tree, not merely described; the flagged
design deviation is justified and minimal; the budget overage is earned; the
skipped `used == 0` refusal is correctly skipped.**

---

## C1 / C2 — CLOSED, verified against the tree

Grepped, not taken on trust. Every construct the design's `Files-to-delete`
ordered retired is gone, and every replacement it ordered is present:

| Ordered retired | Now |
|---|---|
| §8's rows bullet *"already there, via `Publish`'s inversion and `FixedWriteBuffer`"* | **gone** — `grep "already there, via" docs/pubsub-interface-spec.md` returns nothing. The only surviving "already there" is §8's **attachments** bullet (`:857`), which A1 does not touch |
| §8.1's *"window base after the encoder's last append"* **as the interval's start** | **re-anchored, not deleted** (`spec:878`) — retained verbatim and explicitly recast as *"an **interior** point of that interval, not its start"*. That is exactly what A1-DEBT-5 demanded ("keeping the window-base sample described as an interior point"); deleting it would have been the wrong fix |
| README's *"Not a copy: … the encode itself"* | **gone** — `grep "the encode itself"` finds it only inside the new retirement paragraph, which names the retirement and its reason |
| `VectorWriteBuffer::Reserve` (the name) | **gone tree-wide** — `grep -rn "Reserve(" --include=*.hpp --include=*.cpp` returns nothing outside `arrow::ArrayBuilder::Reserve` |

Ruling 44 landed **as the ruling worded it**, not as a paraphrase: §8's rows
bullet now reads *"the seam PERMITS an uncopied row along the whole send path"*
with the ruling's own *"Permits, not guarantees: a client that ignores that call
… and the seam cannot stop it."* The provider half is retained as an
unconditional statement, so the frozen property is **widened, not weakened** —
which is what the ruling's own review note required.

Ruling 45 landed in the artefact it names: harness README, under *"What green
does NOT prove — read before trusting it"*, in the owner's own terms
(**permits** / **stand-in** / no C#/Rust claim), with the ruling cited by date.
It is additionally restated in spec §8.1's **Scope** paragraph — a *narrowing*
disclosure, which the 2026-09-03 licence permits without asking.

The four dangling `§3.1 clause 6` citations cycle 1 flagged now resolve:
`spec:146-179` is a real clause 6. A1-DEBT-1's owed README line landed
(*"A producer is trusted to report what it wrote"*), and the corrected counts are
both **true**: `CopyAccounting` = 2 `TEST_P` × 3 subjects + 5 `TEST` = 11, and
the entry table's rows sum to 11; `SeamVocabulary` = 9 `TEST(` in the file.
Cycle-1 finding F3 is closed at the type rather than by convention —
`CopyVerdict::encode_copies` is `std::optional<size_t>`, empty when
`produced_at == 0`, with `JudgeArithmeticIsSound` gaining an unsampled row that
asserts it. F4 is closed in `status.hpp`'s comment.

---

## Judgment 1 — the `bool lending_` deviation: justified, minimal, and the design is what is wrong

The design's Design section asserts:

> "any mutating re-entry — `Append`, `AppendByte`, `AppendZeros`, **a nested
> `AppendInPlace`** — moves `data_` or `pos_` and is caught by one comparison at
> return"

**That claim is false, by construction. I verified it by tracing the real code
rather than accepting the implementer's account.** On `VectorWriteBuffer` after
`AppendZeros(4)`: `data_=D, capacity_=256, pos_=4` (`Refill` grows by
`kChunk=256`). Outer `AppendInPlace(8)` needs no refill (`8 <= 252`), so
`base0=D, pos0=4, cap0=256`. A nested `AppendInPlace(4)` also needs no refill,
writes over `D+4` — **the same address the outer writer was lent** — and returns
0, so its own commit is `pos_ = 4 + 0 = 4`. All three window fields are
byte-identical to what the outer lend sampled. The outer return comparison
passes, and the outer commits 8 bytes whose first four are the *inner* writer's:
a silently wrong published row, from a mechanism the design declares total.

This is the round's most-repeated failure shape — the owner has stated three
times across A4/A5 that *a loud failure beats a silent wrong answer* — so
tolerating it was not open, and the design's own remedy does not reach it.

**Minimality.** A nested lend leaves **no** observable trace in
`{data_, pos_, capacity_}`, so no state-free predicate can detect it; some state
is forced. What landed is one `bool`, one 9-line private RAII guard, read at
**exactly one site** (`grep lending_` = 3 hits: the guard's two, the door check),
cleared on every exit path including the writer's own throw. It preserves the
design's real constraint — *"no branch on the hot inline append path"* — which a
depth counter or a generation stamp would not improve on. Rung 1 is intact: the
flag is private, unobservable, and cannot be left set, so "a reservation that
outlives a refill / is committed twice / is never taken" remains unspellable.

**And it is not a new prohibition.** The design already ordered a nested fill
refused (rung-2 row 3 names it, `kInvalidArgument`); only the *mechanism*
changed. No owner authorisation is engaged, and the spec text for it lands inside
clause 6 — new text A1 is authorised to write. The flag is scoped to the
`WriteBuffer` object, i.e. the object whose lifetime the guarantee is about,
which is the 2026-09-04 root-cause ruling's own preferred shape.

**Consequence for the record:** `plans/PDA-DEC-A1-writebuffer-constructible.md`
still publishes the false claim. That is a design-document correction, not an
implementation defect.

---

## Judgment 2 — the budget: earned, and the band is met to within 1.3%

Actual **+1114 / −37** (tracked `git diff --numstat 3051b94` = +656/−37, plus the
458-line untracked test file) against a declared **+640 / −25** and a design band
of **+600…+1100**. Per file, against the design's own itemisation:

| File | Declared | Actual | Traceable to |
|---|---|---|---|
| `write_buffer.hpp` | +75/−4 | +176/−4 | code review S2 (nested fill), B1 (`SIZE_MAX` guard), A1-DEBT-1 (residue (b) disclosure), A1-DEBT-2/-3 |
| `core/tests/test_write_buffer.cpp` | +185 | +458 | 9 cases vs 5 named: the extras are the S2 and B1 reproductions and the A1-DEBT-2/-3 pins; `ZeroMinBytesIsRefused` / `FixedBufferWithoutRoomRefusesAsPayloadTooLarge` are rung-2 rows the design's **table** listed and its line estimate omitted |
| `copy_clauses.cpp` | +80 | +137/−1 | A1-DEBT-4 (`COPY_MUST_HAVE_PRODUCED`) plus the F3 `optional` rows |
| `seam_vocabulary.cpp` | +25 | +67 | the declared case, plus ~17 lines pinning the two refusals against the **packaged** core |
| `docs/pubsub-interface-spec.md` | +35/−9 | +69/−9 | clause 6 grew with the nested refusal, the three overflow causes and residue (b) |
| `status.hpp` | *not declared* | +10/−4 | cycle-1 finding F4's own acceptable fix, verbatim |
| `plans/PDA-decouple-progress-log.md` | +25 | **0** | not written (RECORD) |
| harness header · `copy_accounting.cpp` · README · CMake | +55 · +95 · +50/−12 · +1 | +54/−1 · +98/−9 · +43/−8 · +2/−1 | on estimate |

Every line of overage maps to a finding one of the two prior reviews raised, or
to a rung-2 row the design's table already ordered. **No scope crept in beyond
A1's charter**, and both bounds the design set are held:

- **Attachments untouched.** `RunProducerRoundTrip` publishes `Attachments{}` by
  construction; §8's attachments bullet, §3.2, `Blob` and the attachment
  `static_assert`s are byte-identical.
- **The row half of §8 only.** §8's receive bullet and §8.1's attachment sentence
  are unchanged; no provider file is touched; no `PubSubProvider` method is added,
  removed or reordered; P4 honoured — `PubSubStatus` unchanged, no
  `kReentrantCall` grab.

+1114 is 14 lines over the band's top, ~+1139 once the missing progress-log entry
is written. The design pre-authorised this: its stated remedy for exceeding the
band is a split it *itself recommends against*. At 1.3% over, proposing one would
be theatre. Not a finding; recorded below.

---

## Judgment 3 — the two skipped should-fix items

**(a) Refusing `used == 0` — correctly skipped, on two independent grounds, and
the stronger one is not the one the implementer gave.**

1. *Design conformance (decisive).* The design **affirmatively permits**
   `used == 0`. Step 5(c) is `pos_ = pos0 + used` with no lower bound; the rung-2
   table lists exactly four refusals and `used == 0` is not among them; and
   `OnlyTheReportedBytesAreCommitted`'s own comment calls a short report *"a
   variable-length row"*. Adding the refusal would have been a **deviation** —
   forbidding a state the design licenses — not a hardening.
2. *§12.1 (the implementer's stated ground, and it holds).* §3 is `frozen`
   entire; *who may act* is **"nobody alone."** A refusal the design never named
   is a new normative prohibition, and this round has routed precisely that to the
   owner **twice** — the `__` prefix (tenth amendment) and the 246-byte bound
   (eleventh), both new refusals in frozen §3.5 that the 2026-09-03 absorption
   ruling did not name. Neither the absorption ruling nor ruling 44 names a
   `used == 0` refusal.

   One caveat, so the reasoning is not over-applied: ground 2 alone does **not**
   bar every addition to clause 6, since the implementer correctly added the
   nested-fill and `SIZE_MAX` refusals without asking. The discriminator is that
   those two *deliver refusals the design already ordered*, whereas `used == 0`
   would *forbid a state the design permits*. Ground 1 is what separates them.

Substantively it is also benign: `used == 0` commits nothing and publishes
nothing, so the owner's standing "loud refusal beats silent wrong answer"
preference does not engage.

**(b) Splitting the two-subject loops — correctly deferred.** Cosmetic, step-4b's
territory, and no count claim depends on it: the design's forcing-test map pins
`ctest -R '…|WriteBufferInPlace\.'` as a scope, not an entry count, and each loop
carries `SCOPED_TRACE` so a failure names its subject.

---

## Standing items re-checked

- **Public surface: still 1 in `core`.** `AppendInPlace` is the only public member
  added (whole public block enumerated); `lending_` and `Lend` are private; no
  `Capacity()`, no non-const `Data()`, no `Reserve`/`Commit` pair. Counted
  strictly with the harness apparatus (`ProducerMode`, `RunProducerRoundTrip`) it
  is 3, matching the declaration and the round budget.
- **`ReserveStorage` naming: clean**, with the rationale written where a reader
  meets it. No shadowing remains anywhere in the tree.
- **The widened interval matches rulings 44/45 *as landed in the spec*.**
  `EncodeProduced` samples `produced_at` inside the borrow — the only place that
  can answer it — and the forcing test requires `encode_copies == 0` **and**
  `row_copies == 0` on the same publish, so the two halves compose into one
  uninterrupted address chain. The instrument is pinned in both directions
  (stuck-true reddens `StagingProducerIsCaught`; stuck-false reddens all three
  forcing entries), and the design's M1–M5 mutations all still bite under the
  `optional` change — M2 (`encode_copies = 0` unconditionally) now reddens
  `StagingProducerIsCaught` **and** two `JudgeArithmeticIsSound` rows.
- **Corner-case ladder intact.** All five rung-1 states remain unspellable; all
  four rung-2 rows are typed refusals with nothing committed, each with its own
  case, and `FixedBufferWithoutRoomRefusesAsPayloadTooLarge` asserts the *number*
  through `TranslateSeamFailure`. **No refusal became a recovery path** — every
  new check throws and none falls back; the two added checks (nested, `SIZE_MAX`)
  are refusals, not recoveries, and both leave the window intact (pinned by
  `HugeMinBytesRefusesLoudly`'s post-refusal append).
- **Round scope (2026-09-01 split ruling).** No `extern "C"`, no C header, no
  loader, no vtable, no ABI development. The C writer form is stated in prose,
  exactly as §3.2 already does for `Blob` — which is also what ruling 46's test
  ("does this data drill all the way up to the language ABI?") requires here,
  since a row composed by a binding does drill all the way up.
- **P1 re-verified.** `grep "public WriteBuffer"` = four subclasses;
  `RelocatingWriteBuffer` is new and test-only, and its `Grow` delivers contiguous
  room, so P1's stop-and-ask does not fire.
- **No deletion beyond the ordered set; no test deleted.** `EncodeAccounted` and
  legs 1–3 are behaviourally unchanged through the new `DriveRoundTrip` encoder
  parameter; `Append(const uint8_t*, size_t)` stays undeprecated, as ordered.

---

## Non-blocking findings

- **N1 — spec clause 6 publishes "Two refusals" over four `kInvalidArgument`
  throw sites.** `spec:165` reads *"**Two refusals**, both `kInvalidArgument` …
  `min_bytes == 0`, and a writer reporting more than `room`. A **nested** fill,
  and any re-entry that moved the window under the lend, are the same refusal."*
  The code has four such sites and arguably three distinct *kinds* (bad request,
  re-entry, over-report). §12.1's count rule is satisfied — the members are named
  where the count stands — but a binding author mapping causes back from
  `kInvalidArgument` counts two and meets four. *Acceptable fix:* drop the number
  and enumerate ("`kInvalidArgument`, committing nothing, in each of: …"), or say
  "two refusal **classes**" and put the nested and moved-window cases plainly
  under the second.
- **N2 — `PatchU32`'s bound check was hardened without being declared.**
  `write_buffer.hpp:95` changed from `offset + sizeof(value) > pos_` to
  `pos_ < sizeof(value) || offset > pos_ - sizeof(value)`. It is a strict
  hardening — no previously-accepted offset is now refused, and it closes a wrap
  that admitted an out-of-range write — and A1 is what widens who reaches that
  bound, since patching from inside a writer is now a published route. But it is a
  behavioural change to an existing public member that the design's Files-to-touch
  does not list. *Acceptable fix:* none to the code; name it in the as-landed
  delta so the change is not silent.
- **N3 — the design document still asserts a mechanism the tree disproves.** See
  Judgment 1. *Acceptable fix:* the PM corrects the design's Design section to
  record that the return comparison is necessary but not sufficient, and that the
  nested case is refused at the door. **CLOSED 2026-09-05:** a marked CORRECTION
  block is appended after the claim in
  `plans/PDA-DEC-A1-writebuffer-constructible.md`; the original claim is retained.

---

## RECORD (PM corrects in place; never blocking, never a fix cycle)

- `plans/PDA-decouple-progress-log.md` still has **no PDA-DEC-A1 entry** — it is
  in `Files-to-touch` and carries +25 of the declared budget.
- `plans/PDA-DEC-A1-brief.md`'s *"As landed"* footer is still the `<date>`
  placeholder. As-landed: **+1114 / −37** vs declared **+640 / −25**; band
  +600…+1100, so 14 over (≈+1139 once the progress log lands). Fix cycles: 1.
  Undeclared file touched: `core/include/fletcher/core/status.hpp` (+10/−4),
  which is cycle-1 finding F4's own acceptable fix.
- `integration-tests/pubsub-conformance/README.md:192` — *"**Four** addresses per
  publish"*. **CORRECTED 2026-09-05 (PM-directed):** there is no single number —
  six address *roles* exist and each leg samples the ones it has (producer leg 3,
  attachment leg 6, `BorrowedAttachmentCostsNoCopies` 7). The sentence now names
  the roles instead of a count.
- The design's *Files-to-touch* still says `VectorWriteBuffer::Reserve` has "3
  call sites"; it is 2 plus the definition. Already noted at cycle 1; the code is
  right.
