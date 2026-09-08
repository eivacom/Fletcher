# PDA-DEC-A1 — architecture review, cycle 1 of 2

Design: `plans/PDA-DEC-A1-writebuffer-constructible.md` (268 lines, `d3e6b2c`).
Brief: `plans/PDA-DEC-A1-brief.md` — **57 lines**, inside the 60 cap. Untouched.
Rulings ledger: `grep -c '^## 2026' plans/PDA-DEC-rulings.md` = **43**, as expected; read in full first.

**Verdict: APPROVE-WITH-DEBT(5). No BLOCKERs. Nothing goes to the owner beyond the
brief's own two decisions**, both of which are correctly framed (see Claim 5).

---

## Rulings on the six claims

### Claim 1 — "one defect, one class" — the root cause is REAL, not a tidy story

Verified against the text, not the design's paraphrase:

- §8.1 (`docs/pubsub-interface-spec.md:831-833`): *"the window base after the encoder's
  **last append** (§3.1 clause 5) must equal … what the subscriber callback receives"*. The
  measured interval starts **after** the encode.
- `integration-tests/pubsub-conformance/README.md:177-179`: *"Not a copy: **the encode
  itself**; anything a transport does once the bytes leave the seam; a window refill"*. The
  blindness is written down in the guard's own definition.
- §3.1 clause 5 (`:142-145`) gives the window a **readable** end only — `Data()` is
  `const uint8_t*` (`write_buffer.hpp:83`); `data_` is `protected` (`:120`); there is no
  `Capacity()`; every public advance of `pos_` supplies bytes from elsewhere
  (`Append` memcpy `:47`, `AppendByte` `:56`, `AppendZeros` memset `:65`). The window's
  **write end is genuinely absent from the contract.**

So the two halves are one omission seen twice, and the owner's 2026-09-04 ruling ("fix at
the scope the guarantee is about, **provided the root cause has been NAMED**") licenses both
halves landing in one item. Landing only the API would leave a promise no machine can
falsify; landing only the oracle would pin a property no client can reach. **Ruled: the
root cause is correctly named and the single-item shape is right.**

One nuance the design may keep as-is: the Summary's "cannot put a byte into it without
supplying that byte from somewhere else" is true of *advancing `pos_`*, but
`AppendZeros` + `PatchByte`/`PatchU32` is a real in-place back-patch route — the
verification document already calls it the pseudo-workaround that is *worse* than one copy
(one crossing per 4 bytes; `PatchByte` only ORs). This does not weaken the item: §3.1's own
rationale is "one crossing per refill, not one per append", so that route is disqualified by
the spec's own reasoning, not by its impossibility.

### Claim 2 — `AppendInPlace`: the mechanism holds; "every" and "zero" are one word too strong each

Walked every route against the real header and the three real subclasses.

**"Zero hot-path cost" — TRUE.** No flag, no depth counter, no state; nothing is added to
`Append`/`AppendByte`/`AppendZeros`. The whole cost is two comparisons inside the new call.
This is materially better than A4's `thread_local`/counter shape and should not be traded.

**The commit being absolute (`pos0 + used`) plus check (a) is sound.** `pos_` is
monotonic — no public member reduces it except `VectorWriteBuffer::Finish()`, which also
nulls `data_` — so no re-entrant mutating append can restore both sampled values. Nested
`AppendInPlace`, `Append` with or without a refill, `AppendByte`, `AppendZeros` and `Finish`
each move `data_` or `pos_` and are caught. **A refill during the callback cannot occur
except through a re-entrant append**, which check (a) refuses; the *permitted* refill (owner
ruling 2026-09-01) happens in step 2, before `base0`/`pos0` are sampled, so the ruling is
honoured and the lent pointer is never invalidated under the writer's hand. Step 2's
arithmetic was checked against all three subclasses (below) and delivers `room >= min_bytes`
in every case.

**Writer throws** — safe by construction, because the commit is the last statement: `pos_`
is still `pos0`, so nothing is committed and the exception propagates. The tree already
relies on exceptions leaving an encoder (`sample_writer.hpp:126`). But it is **unstated**,
and it is a contract a binding author must be able to read → **A1-DEBT-2**.

**Two over-claims** (neither is a hazard, both would become wrong header text if copied
verbatim) → **A1-DEBT-3**:
- *"any mutating re-entry … is caught"* — `PatchU32`/`PatchByte` from inside the writer move
  neither `data_` nor `pos_` and are **not** caught. They are also harmless: both are bounded
  by `offset < pos_ == pos0`, which is disjoint from the lent span. The correct claim is
  "every re-entry that could invalidate the lend", and patching below the lend point should
  be stated as permitted.
- *"the pointer exists only for the writer's frame … cannot be spelled"* (rung 1) — a C++
  lambda can capture `dst`, and a C writer can stash it in `ctx`. Using it after return is a
  use-after-free of exactly the same genus as the design's one disclosed residue. It belongs
  **beside** that residue, not on rung 1.

**One genuine second residue, and it is the sharpest finding in this review** → **A1-DEBT-1**:
a writer that **reports more than it wrote** (`used <= room`, but bytes `[written, used)`
never touched) commits bytes nobody wrote, and they are published with no signal. The design
says this class is *"the same exposure `Append(const uint8_t*, size_t)` has carried since the
file was written — no new class."* That is right for over-*writing* and **wrong for
over-reporting**: today **no** public member advances `pos_` over a byte it did not itself
write, so `AppendInPlace` creates the class. It is benign on both growable subclasses (a
`resize`/value-initialised vector leaves the lent span zeroed), and it is **not** benign on
`FixedWriteBuffer`, which in this tree wraps a transport payload — `SampleBody(payload.data …)`
(`fastdds-pubsub-provider/src/internal/sample_writer.hpp:119`,
`fletcher_sample_pub_sub_type.hpp:86`), i.e. **recycled pool memory holding the previous
sample's bytes**. That is the zero-copy DDS path, so it is the path that matters. It cannot
be forbidden and it should **not** be memset away (see the DEBT text) — it must be disclosed
and scoped, which is the shape the owner has now chosen eight consecutive times.

### Claim 3 — one member, not four — the forbidding is GENUINE and the capability is complete

`Capacity()` is not withheld, it is **answered**: the writer is handed `room`. A non-const
`Data()` is a bound-free writable pointer — the unsafe shape itself. `Reserve`/`Commit`
creates a reservation object whose staleness across a refill is the state rung 1 removes;
with one call there is no such object to spell. All three are genuine unrepresentability,
not absent capability.

Confirmed a full pub/sub client can encode with this alone: variable-length rows work
(`room` is the whole remaining window, so no second crossing to ask "how much is left");
unknown-length rows work by reporting what was written and calling again (a fixed buffer
then refuses loudly with `kPayloadTooLarge`); back-patching works either inside the writer's
own `dst` or afterwards through the already-public `WriteLengthPlaceholder` /
`PatchU32` / `PatchByte` / `Position()` (`write_buffer.hpp:86-110`). **Ruled sound.**

Locked decisions checked: D4 (method set frozen, types in scope, `Publish` stays inverted) —
`WriteBuffer` is a crossing *type*, so this is squarely the in-scope half. D5 — the
normative comment block is required, and the design carries it. D13 — no wire bytes move.
D14 / §11 — stating the C writer signature in prose and in a header comment is **not**
building an ABI; it is exactly what §3.2 already does for `Blob` ("This round does not write
that C form — it states the contract it must satisfy"). **No deviation from any locked
decision.**

### Claim 4 — the oracle extension is load-bearing, and the mutation set survives the in-tree/packaged split

Verified the split myself, per the A4/A5 precedent:

- `core/tests/CMakeLists.txt:6-9,62-64` — `core_tests` links `fletcher-core`, which
  `core/CMakeLists.txt:7` defines as an **INTERFACE** (header-only) in-tree target. A
  mutation to `core/include/fletcher/core/write_buffer.hpp` therefore reddens `core_tests`
  with no package step. **M1, M3, M4, M5 are all live in-tree — none is inert.**
- `integration-tests/pubsub-conformance/CMakeLists.txt:7` — the harness is a **separate
  project** doing `find_package(fletcher-pubsub CONFIG REQUIRED)`, so it compiles against
  the **packaged** `core` headers. The design says so and requires a `core` package rebuild
  before the three forcing entries move. That is the honest statement A5's inert mutation
  cost the round, and it is made here without being asked.
- **M2 is unaffected by the split**: `Judge` lives in `src/copy_accounting.cpp:…`, compiled
  straight into `conformance_copy_accounting`, so it reddens directly.

**Would the control genuinely redden?** Yes, for the failure modes that matter:
- API lands but does not lend the window (M1) → `WriterReceivesTheWindowCursor` red in
  `core_tests` immediately, forcing entries red after the package rebuild.
- Oracle goes blind (M2, `encode_copies = 0` unconditionally) → `StagingProducerIsCaught`
  **and** `JudgeArithmeticIsSound` red. This is the half-landing case, and it is covered.
- Sampler stuck true → `StagingProducerIsCaught` red; stuck false → all three forcing
  entries red. The instrument is pinned in **both** directions, which is more than the round
  has usually achieved.
- Full revert → the forcing test does not compile. The design says this plainly and does
  not dress a compile failure as a behavioural red; the substantive standing evidence is the
  control, written first, green today over the lost property. **That is the correct handling
  for an added capability, and I accept it.**

`7 → 11` entries verified: `copy_clauses.cpp` has one `TEST_P` × 3 subjects + 4 `TEST`s = 7
today, and README:170 already publishes "seven entries" with its own re-derivation command.
Subject labels `SeamProbe` / `InProcessLoopback` / `InProcessViaPubSub` confirmed
(`copy_accounting.cpp:530-545`). **Ruled: the oracle extension is genuinely load-bearing and
the mutation set is not inert.** One hardening owed → **A1-DEBT-4** (an unsampled
`produced_at` must fail as itself rather than default into `encode_copies == 1`, which would
make the staged control pass for the wrong reason — the same discipline
`COPY_MUST_DELIVER_CLEANLY` already applies to "arrived MISSING").

### Claim 5 — the §8 amendment makes the property ACHIEVABLE; P3 is correctly framed and the PM carries it

§8's rows bullet today (`:812`) reads *"already there, via `Publish`'s inversion and
`FixedWriteBuffer`"* — which the verification pass confirmed is **false for a binding**. So
the frozen text is currently wrong in the *wide* direction, and A1 does not weaken it:
option (a) makes the wide claim true by supplying the missing capability; option (b) narrows
the published sentence to what is provable and names the gap.

**P3 needs the owner, and the design could not have settled it.** Three independent reasons,
any one sufficient:
1. §12.1: §8's zero-copy property is `frozen`, *who may act:* **nobody alone**.
2. The 2026-09-03 absorption ruling authorises **amending** §3.1/§8/§8.1 for A1 — the
   verification's *Contract gap* section names A1 as "(§3.1 clauses + §8's property)" — but
   it does not choose the **direction**. Option (a) is a **widening** of a published promise,
   and the 2026-09-04 ruling states the inference licence "permits **narrowing** without
   asking, never **widening**."
3. Option (b) is a narrowing, but it concedes a copy on the row path, which locked decision 7
   makes a stop-and-ask in its own right ("Accepting a copy anywhere on the row or attachment
   path is a stop-and-ask").

**Ruled: P3 is correct as framed, the no-default is correct, and I cannot save the owner this
decision.** What the *implementation* still owes is the sentence itself → **A1-DEBT-5**: under
(a) the promise is only true of a client that fills the window in place, so §8 must read as a
**permission** ("a client that composes into the lent window …"), never as an unconditional
end-to-end guarantee — otherwise A1 reinstates, at the wording level, exactly the green-guard-
over-a-lost-property defect it exists to remove. Decision 2's recommendation (a) already
carries this framing for the README, so the pair is coherent; it is the §8 sentence that is
not yet shown to anyone.

### Claim 6 — the budget declaration is defensible, and the band is a hedge with a consequence

Design 268 ≤ 300; brief 57 ≤ 60; new public surface **1** (3 counted strictly, still inside
budget) — all verified by count, not by claim. Declared +640/−25 with the apparatus itemised
at +385 and the reason named: the last two items under-counted **exactly** the test
apparatus. That is a correction aimed at the measured failure mode rather than a round-up.
The band +600…+1100 is not a way to avoid being wrong: it carries a **named consequence** at
breach (propose the oracle-first split) and a recommendation against taking it. I read the
+640 point estimate as low at the harness end (a new driver, two producers, a scoring path
and four rows for +255 is optimistic on A4/A5 evidence), which is what the band is for.
**No finding.**

---

## Premises — discharged so nobody re-derives them

- **P1 verified TRUE for all three in-tree subclasses**, with the arithmetic, so the
  stop-and-ask should not fire on a re-reading:
  `VectorWriteBuffer::AppendZerosSlow` → `Refill(n)` sets `capacity_ = pos0 + max(n,256)`
  and preserves bytes below `pos0`, so after the design's `pos_ = pos0` restore,
  `capacity_ - pos0 = max(n,256) >= n` (`write_buffer.hpp:164-175`);
  `GrowableProbeBuffer::AppendZerosSlow` → `Grow(n)` allocates `pos_ + max(n,128)` and
  memcpys the prefix (`copy_accounting.cpp:83-95`), same conclusion;
  `FixedWriteBuffer` throws `std::overflow_error` (`:198-200`), which
  `TranslateSeamFailure` maps to `kPayloadTooLarge` by hand
  (`core/include/fletcher/core/status.hpp:138-144`) — step 2's claim is exact.
  Access is fine: `AppendZerosSlow` is `protected` on the base, so a base member may call
  it however the override is declared.
- **P2 verified TRUE**: `Blob(owner, data, size)` is public and already used from outside
  core (`copy_accounting.cpp:151`). A1's scope does not double.
- **P4 — RULED, do not stop-and-ask.** `kInvalidArgument` is the right refusal for both A1
  cases and A1 must not append a status. The owner's 2026-09-03 allocation of
  `kReentrantCall = 10` is about a caller re-entering a **provider** from inside a delivery
  callback, and it is A3's to land; A1 cannot use a value that does not exist yet without
  making its own close depend on A3's. Both A1 refusals are loud and typed, so nothing is
  silent either way. Note for A3, not for A1: since **nothing has shipped** — all eleven
  amendments land inside PR #126 — moving `AppendInPlace`'s re-entry refusal to
  `kReentrantCall` at the moment A3 lands the enum costs nothing and changes no published
  behaviour. Decide it once, there, with both sites in view.
- No unstated premise found. The design states its substrate assumptions (subclass refill
  behaviour, attachment constructibility, its authorisation, its status budget) and each
  carries a stop condition.

## Deletion, end-state, buildability

`Files-to-delete` is present and real (four items, each with its replacement, all four
verified to exist in the tree). **No coexistence bridge**: `Append(data, len)` stays as the
honest call for a producer forwarding bytes it was handed, not as a legacy path with a
scheduled deletion — so there is no merge-the-stages finding to make here. `Files-to-touch`
is plausible and complete; `core/tests/test_write_buffer.cpp` really is new (the directory
holds three test files today) and `core/tests/CMakeLists.txt` lists sources explicitly, so
"one source line" is right. No hidden cross-cutting change: the member is additive, no
in-tree caller must change, no wire byte moves. No hand-composed post-change ledger anywhere
in the document.

**One corrected tree fact:** `VectorWriteBuffer::Reserve` has **two** call sites
(`write_buffer.hpp:153` in `AppendSlow`, `:172` in `Refill`) plus its definition at `:178` —
not "3 call sites". Same one file, so the rename is unchanged in shape.

---

## DEBT register (5) — none of these loops the design

Copied to `plans/reviews/design-debt.md` under PDA-DEC-A1.

**A1-DEBT-1 — the second handled residue: a writer that reports more than it wrote.**
Committing `pos0 + used` publishes bytes nobody wrote when `used` exceeds what the writer
actually filled. Correct the design's "no new class" line: over-*writing* is indeed the
existing `Append(ptr,len)` exposure, but over-*reporting* is new, because no public member
today advances `pos_` over a byte it did not write. Scope it honestly — benign on both
growable subclasses (the lent span is already zeroed by `resize` / value-init), **not**
benign on `FixedWriteBuffer`, which in this tree wraps a transport payload
(`sample_writer.hpp:119`, `fletcher_sample_pub_sub_type.hpp:86`), i.e. pool memory that may
still hold the previous sample's bytes. Owed: name it as the second residue in the ladder,
disclose it in the header's normative block, and add one line to the README's claim limit.
**Do not "fix" it with a memset of the lent span** — that is an O(room) cost on precisely the
zero-copy path the item exists to open, and it buys nothing on the two subclasses where the
span is already zero.

**A1-DEBT-2 — state the writer-throws behaviour, and pin it.** Safe by construction (the
commit is the last statement, so `pos_` is still `pos0`), but unstated. One sentence in the
header contract — an exception from the writer commits nothing and propagates unchanged —
and one `WriteBufferInPlace.*` case. A binding author cannot infer it, and the C form cannot
produce it, so it is a C++-caller contract that must be written down.

**A1-DEBT-3 — two ladder claims are one word too strong; fix the words, not the mechanism.**
(a) `PatchU32`/`PatchByte` from inside the writer move neither `data_` nor `pos_`, so check
(a) does not catch them; they are harmless (bounded by `offset < pos0`, disjoint from the
lent span) and should be stated as **permitted**, with the claim restated as "every re-entry
that could invalidate the lend". (b) Stashing the lent pointer past the writer's return is
**not** rung-1 unrepresentable — a lambda capture or a `ctx` field spells it in either
language — and it is a use-after-free of the same genus as the disclosed residue; move it
beside that residue. Also correct "3 call sites" for `VectorWriteBuffer::Reserve` to two plus
the definition.

**A1-DEBT-4 — an unsampled ledger must fail as itself.** If `produced_in_window` defaults
false, a leg where the sampler never ran scores `encode_copies == 1` and the staged control
passes for the wrong reason — the "control that cannot fail" shape this round has logged five
times. Assert `produced_at != 0` (and `produced_len == row_bytes`) before any verdict is
read, in the same place and for the same reason `COPY_MUST_DELIVER_CLEANLY` asserts an
attachment did not arrive MISSING.

**A1-DEBT-5 — the §8 sentence must publish a permission, not an unconditional promise.**
Under brief decision 1(a), zero-copy across the whole send path is true only of a client that
composes into the lent window; a client that still stages pays one copy and the guard reports
it. §8's replacement bullet must say so in the sentence itself, and §8.1's rewrite must move
the interval's **start** to the producer's write site while keeping the window-base sample
described as an interior point — the provider-half check (`row_copies`) is not being replaced,
only preceded. PM-facing half: when presenting decision 1, one clause makes (a) unambiguous —
*"(a) whole path — the seam **permits** an uncopied row from the client's own write to the
subscriber's read, for a client that uses the new call; the guard reports which kind of client
it measured"*. Without it the owner rules on a promise wider than the mechanism, which is the
A4 defect at the wording level.
