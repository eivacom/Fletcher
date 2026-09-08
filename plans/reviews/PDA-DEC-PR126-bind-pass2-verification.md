# Verification of the second BIND-C# pass on PR #126

**Read receipt.** `plans/PDA-DEC-rulings.md` carries **58** entries
(`grep -c '^## 2026' plans/PDA-DEC-rulings.md` -> 58). Last two entry titles:
**57 — "A zero-byte label is refused on arrival as well as on attachment"** (`:816`) and
**58 — "§5.1 states the general escape rule, not one class"** (`:840`). Ruling **46** is at
`:608` exactly as the document cites — *"Every protocol driver is C++, so both sides of the
protocol ABI are C++"*, scope class `(scope)`.

**Tree state verified.** `HEAD` is `c7bf5dc` — the exact commit the packet is raised against —
and the working tree is **clean** (`git status --porcelain` empty). Every line citation below was
read at that commit.

**Verdict summary.**

| # | Claim | Verdict | Scope | Ruling already answers it? | Reachable shipped defect? |
|---|---|---|---|---|---|
| B1 | The writer's C form has no failure channel | **FALSE** on its premise | belongs to **BIND** (risk register, beside R2) | §3.1 clause 6 already states the channel | No |
| B2 | Ruling 46 not carried into the seam spec | **PARTLY-TRUE** — headline true, all three consequences false | in scope procedurally (frozen §0/§5.2); substance mostly dissolves | Ruling 46 settles the substance and states its own precedence | No |
| B3 | P1's stop-and-ask misses BIND | **PARTLY-TRUE** — mechanism right, "by construction" wrong | `core/` comment; **not an owner ask at all** | No ruling needed | No |

**Nothing in this packet — nor in its risk register — is a reachable defect in shipped
behaviour.** The author's own framing is correct on that point, understated in one place (B1) and
overstated in another (B3).

---

## B1 — FALSE on the premise. The C form is never the writer.

### The citations are exact; the inference off them is not

Every line the packet cites is right, character for character:

- `docs/pubsub-interface-spec.md:154` — `size_t (*writer)(void* ctx, uint8_t* dst, size_t room)`
- `core/include/fletcher/core/write_buffer.hpp:188` — `void AppendInPlace(size_t min_bytes, Writer&& writer) {`
- `write_buffer.hpp:256` — `if (used > room) {`
- `write_buffer.hpp:261` — `pos_ = pos0 + used;`

So the packet did not misread a moved line. **It misread the type.** That is this round's
documented dominant failure mode, now appearing in the review of the review.

### Read off the type

`AppendInPlace` invokes its writer with **two** arguments:

```cpp
const size_t used = std::forward<Writer>(writer)(data_ + pos0, room);   // write_buffer.hpp:242
```

The normative C form takes **three** (`ctx, dst, room`). A `size_t (*)(void*, uint8_t*, size_t)`
**cannot be the `Writer`** — it is not callable with `(dst, room)`. Every call site in the tree
confirms the shape: `test_write_buffer.cpp:95,117,127,...`, `copy_accounting.cpp:309`,
`seam_vocabulary.cpp:232` all pass `[&](uint8_t* dst, size_t room) -> size_t`. A C boundary is
therefore **structurally obliged** to install a C++ callable that closes over `ctx` and forwards
to the foreign function pointer. That C++ callable *is* the writer, and it has clause 6's full
exception channel:

> **An exception from the writer commits nothing and propagates unchanged.** (spec §3.1 clause 6;
> `write_buffer.hpp:162-163` states the same, and `:261` makes it true by putting the commit last.)

The header says so in as many words, and the sentence is only coherent under this reading:

> `write_buffer.hpp:136-138` — *"Note what the C writer does NOT receive: a buffer handle.
> Foreign code therefore cannot re-enter this buffer at all, and the re-entry refusal below is a
> **C++-only concern**."*

Re-entry can only be "a C++-only concern" if the thing `AppendInPlace` calls is C++. And §3.2's
governing sentence, which clause 6 explicitly points at (*"exactly as §3.2 does for Blob"*), makes
the derivation rule general:

> `:270-272` — *"The C form is **conceptual, never a memory image** ... a boundary **constructs**
> a `Blob` from the three fields."*

So the correct convention for a managed boundary is already normative, and it is the better one:
the thunk catches its own managed exception, sets `ctx->failed`, returns; the shim's C++ adapter
**throws inside the writer's own frame**; nothing commits, the throw propagates through the
`RowEncoder` and `TranslateSeamFailure` (`fast_dds_pubsub_provider.cpp:391`), and the publish
fails. No managed exception ever crosses into native code — exactly the process-safety property
the packet says it needs.

### A second, in-band channel the packet also missed

`used > room` is not merely "the only return check" — it is a **spec-normative refusal**:

> §3.1 clause 6 — *"**Two refusals, both `kInvalidArgument`, both committing nothing:**
> `min_bytes == 0`, and a writer reporting more than `room`."*

A foreign writer returning `SIZE_MAX` therefore aborts the publish and commits nothing, from
inside the C form, today. The diagnostic message is misleading for that use, but the claim
*"The C writer has none"* is false off the type as well as off the spec.

### What survives, and where it belongs

One residual point is fair: **which** of the available conventions a boundary picks is a boundary
decision, and it is unstated *for the boundary*. That is a BIND-round design note of exactly the
class of the packet's own **R2** ("a generated encoder returns its own cursor delta") — and by the
packet's own taxonomy it belongs in the risk register, not in a stop-and-ask against frozen §3.1.

**The proposed append must not land as drafted.** It asserts *"The C writer returns a count and
**cannot refuse**"* and prescribes raising **after** `AppendInPlace` returns. The first sentence
contradicts clause 6's own "two refusals" bullet four bullets above it; the second prescribes the
weaker of two mechanisms while the stronger one is already normative. Landing it would create a
fresh internal inconsistency inside frozen §3.1 — the very thing B2 complains about.

### Cost, against the packet's estimate

- The estimate — *"None in product code ... a wording ask"* — is right about product code and
  **wrong about the total**, because it prices the sentence and not the ask. Amending frozen §3.1
  costs an owner ruling (§12.1: §3 entire is `frozen`, *"who may act: nobody alone"*), plus the
  guard the packet itself proposes: one `CopyAccounting`/`SeamVocabulary` case **plus a live
  negative control**, and conformance clauses run **x3 subjects** (Fast DDS, XRCE, in-process —
  `integration-tests/pubsub-conformance/README.md:254`). That is not a wording cost.
- Checked: `AppendInPlace` has **no production caller in the tree** (`grep -rn AppendInPlace` ->
  `core/tests/`, `integration-tests/pubsub-conformance/`, and one comment in `status.hpp`). The
  urgency is nil.
- **True cost of the correct disposition: zero.** Decline the amendment; move the residual note
  into BIND's register beside R2.

---

## B2 — headline TRUE, and a real internal inconsistency. Every consequence drawn from it is FALSE.

### The internal inconsistency is real. Both sentences, verbatim

Frozen seam spec, `docs/pubsub-interface-spec.md:29`:

> - **PDA-ABI** — a pure C ABI *below* it, so a protocol becomes a driver chosen and
>   configured at runtime

Sibling oracle, `docs/protocol-driver-abi-spec.md:3-6`:

> This is the authoritative spec for the protocol driver ABI, **both sides of which are C++** —
> see §0 and the owner's ruling of 2026-09-05. **(This line read "the pure-C protocol driver ABI"
> until PDA-DEC-AG2 carried that ruling into the same file's §0**; the two are now one answer
> rather than two.)

Decisive on the fact: AG2 judged the phrase *"pure-C ... ABI"* to be the phrase ruling 46
contradicted, and amended it — in the sibling file, while the **identical phrase** survived at
seam `:29`. `grep -n '2026-09-05\|ruling 46\|pure C++' docs/pubsub-interface-spec.md` returns
**only** the three unrelated 2026-09-05 rulings (§5.3, §6 clauses 5 and 6). Ruling 46 was never
carried into the seam spec, and ruling 46's own **Supersedes** clause names only
`protocol-driver-abi-spec.md` §0.2 and `:21-22` — so the ledger's supersession list is itself
incomplete against the tree. **Confirmed.**

*Severity, stated honestly:* this is stale description, not a live ambiguity. Ruling 46 says
*"until amended, the spec contradicts this ruling and **this ruling wins**"* (`:617-619`), and the
driver spec is the authority on the driver ABI's own shape. A reader of frozen text alone is
misled; a reader of the ledger is not. It is an inconsistency **inside a `frozen` section**, which
is why correcting it needs a ruling rather than maintenance (§12.1's records carve-out is bounded
to §10 and §11; §0 and §5.2 are `frozen`).

### All three "why this is not tidiness" consequences are false against the tree

The packet's urgency rests entirely on these. Each fails against
`docs/protocol-driver-abi-spec.md` **as amended by AG2 — that is, after ruling 46 was carried
in**:

1. **"§5.2 has lost its counterparty."** FALSE. The driver spec still publishes a status enum of
   exactly the shape §5.2 is about (`:238-253`):
   `typedef enum { FLETCHER_STATUS_OK = 0, ... } fletcher_status;`, under the heading
   *"`fletcher_status` **is** `PubSubStatus`, number for number"*, with the rationale
   *"both boundaries need them in the same enum"* (`:248`). The counterparty is present, in C,
   today.
2. **"Every C form in §3 now has exactly one consumer ... the driver side no longer derives any of
   them. Nobody will find a defect in those forms before BIND-C# does."** FALSE, and this is the
   load-bearing claim. `protocol-driver-abi-spec.md` **§3 is titled "The C types"** and derives
   them right now: `fletcher_blob` and `fletcher_schema` (`:167-181`, C structs with
   `retain`/`release`) and `fletcher_write_buffer` (`:194-...`), the latter explicitly
   *"mirrors the seam's window-plus-refill shape (seam §3.1)"*. Ruling 46 changed the
   *implementation language of drivers* and the *language-agnosticism test*; it did not delete the
   driver ABI's C forms, and it explicitly preserved the constraints that force them:
   *"**Does NOT relax:** binary-stability constraints ... exceptions may not cross, layout may not
   be assumed"* (`:622-626`). The second reader **exists**. The argument that B1 must be settled
   now because that reader is gone therefore collapses.
3. **"§3.2's reason for having no shared C header is now half-true — one side wraps nothing; it
   *is* the C++ value."** FALSE. Spec `:274-275` is quoted correctly, but the driver spec's own
   rationale for `fletcher_blob` is precisely a wrap: *"`ctx` holds a heap `shared_ptr` one way; a
   custom-deleter `shared_ptr` calling `release` the other"* (`:189-190`), with *"no layout
   compatibility is implied or permitted"* (seam `:272`) holding on both sides. Both sides wrap.
   The sentence is fully true, not half-true.

### Scope

Not ABI work — it is text, so the charter's ABI prohibition (§11: no `extern "C"`, no C header,
no `dlopen`, no vtable, no host-callback struct; `frozen` per §12.1) is not engaged. Procedurally
the packet is right that §0 and §5.2 are `frozen` and this is the owner's call; it is the same
class as rulings 54, 56 and 58, each of which authorised one specific, explicitly
non-generalising frozen-text amendment.

### Cost — materially higher than estimated, and the estimate names the wrong sites

The packet estimates *"a note plus four local edits"* (§0, §3.1, §5.1, §5.2). The tree carries the
claim at **eight** sites; the packet's list misses four:

| Site | Text | In packet's list? |
|---|---|---|
| `pubsub-interface-spec.md:29` | *"a pure C ABI below it"* | yes |
| `:65` | *"They mirror *this*. **The two C boundaries** will end up with structurally similar types"* | **no** |
| `:130` | *"the shape both C boundaries want"* | yes |
| `:690` | *"both C boundaries need a success value in the same enum"* | yes |
| `:750` | *"so **the two boundaries** derive one rule rather than two"* | **no** |
| `:764-768` | §5.2 entire | yes |
| `core/README.md:31` | *"Present because **both C boundaries** need a success value in the same enum"* — inside the **published** *Error taxonomy* table | **no** |
| `core/include/fletcher/core/status.hpp:33` | *"Success. Present because **both C boundaries** need a success value in the ..."* — and §5.1 makes `status.hpp`'s doc comments the **normative** owner of meaning | **no** |

So the real edit is roughly 2x the estimate and reaches (a) a machine-read published table
(`Taxonomy.PublishedNumbersMatchTheEnum` — safe: it guards name+number only, per debt C1-2) and
(b) normative header text.

**And two of the four missed sites are arguably still correct as written.** Given that the driver
ABI keeps its C types, `:65` and `:750` remain true in the sense the spec means them — two
boundaries at which C forms appear. A blanket "there is one C boundary" edit would replace a stale
sentence with a **wrong** one. The minimal correct change is narrower than the packet's note:
bring `:29`'s *"pure C ABI"* into line with the sibling spec's own amended phrasing, and leave the
parallelism argument and the idiom preference alone.

---

## B3 — mechanism right; "by construction" wrong; and it is not an owner ask.

### Citations and premise, checked

- `core/include/fletcher/core/internal/delivery_frame.hpp:21-26` is P1, and the STOP-AND-ASK is
  quoted accurately (the packet omits only the trailing *"a forked thread-local must not be
  papered over with a registration handshake"*).
- `:42` is `inline thread_local std::vector<const void*> g_delivery_stack;` — exact.
- `pubsub/CMakeLists.txt:11` is `add_library(fletcher-pubsub STATIC` — exact.
- `core/CMakeLists.txt:7` is `add_library(fletcher-core INTERFACE)` — exact.
- **No `shared` option in any Fletcher recipe.** `pubsub/conanfile.py:18` declares
  `options = {"run_tests": [True, False]}` and nothing else; `.conan-profiles/*:10-13` state
  *"No blanket `*:shared` here"*; every `add_library` in the repo outside `third_party` is
  `STATIC`, `INTERFACE` or `OBJECT` — **no `SHARED` anywhere**. The packet's premise concession is
  **correct**.

### The linkage mechanics are correct, and P1's own sentence is what over-claims

The packet is right that static/`INTERFACE` linkage dedups **per linked binary**, not per process.
So P1's stated reason —

> `:21-23` — *"Every Fletcher component is a STATIC library ... so this `inline thread_local` has
> **exactly one instance however many components include it**."*

— is an over-claim: one instance per *binary image*, which is one per process only because the
tree builds one image. The same applies to `DeliveryChannel::AbsorbedTotal()`, whose storage is a
function-local static in a static library (`pubsub/src/delivery_channel.cpp:16-17`) while its
header documents it as *"**Every absorption in this process**, across every channel"*
(`delivery_channel.hpp:107`). **Two doc sentences say "process" where the mechanism says
"binary".** That is the true, narrow content of B3, and it is worth a comment.

### But BIND does **not** trip it "by construction", and the note does not miss BIND

1. **The note's first clause is generic.** *"if **any component** becomes a shared library"* is not
   scoped to PDA-ABI; only the second clause names PDA-ABI. A BIND shim that packages Fletcher
   into `Fletcher.Native.dll` is squarely inside the first clause. The packet reads the second
   clause as if it were the whole note.
2. **"By construction" is false, and the packet's own R1 says so.** A shim that is the process's
   *only* copy of Fletcher has exactly one stack and one total — no fork, nothing to ask about.
   R1 concedes it: *"B3's fork is BIND's to avoid, not the seam's to survive."* The trigger is a
   **second** copy in the process, which is a packaging choice, not a construction.

### Two of the three enumerated consequences do not follow

- **"§6 clause 6 stops firing."** Does not follow for the realistic shape. Clause 6 refuses
  re-entry *on the same provider instance* (`delivery_frame.hpp:122-125`;
  `fast_dds_pubsub_provider.cpp:396` passes `static_cast<const PubSubProvider*>(this)`). A given
  instance's push (`DeliveryChannel::Deliver`, `delivery_channel.cpp:58`) and its door both
  execute in the binary that compiled that instance's methods, so both touch the *same*
  thread-local. Two copies of Fletcher in one process yield two independent, internally consistent
  worlds; a call from binary B into binary A's provider dispatches through A's vtable into A's code
  and A's stack. The refusal keeps firing. Breaking it requires passing a **non-virtual** C++ seam
  object (a `Subscriber`, whose out-of-line methods relink per binary) across two separately linked
  copies — which *"layout may not be assumed"* (`protocol-driver-abi-spec.md:30`) independently
  forbids, and which no binding does by construction.
- **"A4's depth-zero sweep misfires."** Same argument, same answer: the deferral
  (`DeferUntilDeliveryDepthZero`) and the sweep (`~DeliveryScope`, `:99-113`) are both reached from
  the same binary's code on the same thread.
- **"`AbsorbedTotal()` forks too."** **This one is true**, and it is the only one that is: the
  counter is per-binary and the header claims per-process.

### Scope and cost

`core/include/fletcher/core/internal/delivery_frame.hpp` is an internal header comment, **not
frozen text** — §12.1's classes bind this document's sections, not implementation comments. So
**this is not an owner ask at all**; it is ordinary maintenance the PM or an implementer may make.
The packet's cost estimate ("a comment") is **accurate** — with one addition it did not name:
while editing, P1's *"exactly one instance however many components include it"* and
`delivery_channel.hpp:107`'s *"in this process"* should both be corrected to say **per linked
binary**, because those two sentences are the actual over-claims. The recommended edit is narrower
than the packet's draft: name the binding case under the existing first clause, correct the two
per-process sentences, and drop the two consequences that do not follow.

**Not registered as debt.** A grep of `plans/reviews/design-debt.md` for `shared library`,
`thread_local`, `forked`, `ruling 46`, `failure channel` and `AppendInPlace` finds none of
B1/B2/B3. AG1-DEBT-19 (re-permitting re-entry), AG1-DEBT-20 (re-entrancy identity typed at one
end), AG1-DEBT-21 (refused `Subscribe` walks the rollback path) and AG2-DEBT-11 (unbounded
attachment count) are all different subjects. No double-registration, and none of the three makes
an existing debt worse.

---

## The risk register at l.234 — reachability only

Skimmed as instructed; it carries no ask and nothing else about it is reported here. **Nothing in
it is a reachable defect in shipped behaviour that the author has under-rated.** Spot checks:

- **R2** (over-reporting writer discloses the previous sample's bytes) is the closest call and is
  **correctly rated**. It is disclosed normatively at spec `:177-182` and
  `write_buffer.hpp:174-186`, and `AppendInPlace` has **no production caller** in the tree — so it
  is reachable only from code that does not exist yet. The packet rates it BIND's; that is right.
- **R5** is the only register item whose consequence in shipped code is **process termination**
  (`subscriber.hpp:46-53`). Not under-rated: it is the authorised answer to a forbidden act under
  ruling 49 (2026-09-05, *"Destroying another subscription from inside a delivery is forbidden, and
  said so"*), and the packet says so.
- **R3/R4/R6** are binding-side obligations against documented, owner-ruled behaviour, not
  Fletcher defects.
- The carried-forward `Timeout.Infinite` note is **correct off the type**:
  `pubsub/include/fletcher/pubsub/schema_arrival.hpp:63,69` — `int64_t timeout_ms`, and
  *"`timeout_ms < 0` is refused with `kInvalidArgument`"*, so `-1` is refused and
  `TimeSpan.MaxValue` maps to the unbounded form. A real BIND mapping obligation; not a defect.

---

## Disposition

- **B1 — decline.** FALSE on its premise; the channel it asks for already exists, twice, and the
  drafted text would contradict clause 6. Move the residual "which convention does the boundary
  pick" note into BIND's register beside R2. **No ruling needed.**
- **B2 — the inconsistency is real; grant a narrow correction, not the note.** `:29`'s
  *"pure C ABI"* contradicts the sibling oracle's own amended title line and should be brought
  into line with it. Decline the §0 note and the "one C boundary" framing: the driver spec's §3
  still publishes C types and a C status enum, so both the counterparty and the second reader
  exist, and a blanket edit would replace stale text with wrong text. If `:130`/`:690` are edited,
  `core/README.md:31` and `status.hpp:33` must move with them or the published table and the
  normative doc comment drift from the spec.
- **B3 — maintenance, not an ask.** Correct the two per-process sentences to per-binary and name
  the binding case under the existing first clause. Reject "by construction" and the clause-6 and
  A4 consequences.
- **Nothing here blocks approval on shipped-behaviour grounds.**

## RECORD

- `plans/PDA-DEC-rulings.md:617-619` — ruling 46's **Supersedes** clause names only
  `protocol-driver-abi-spec.md` §0.2 and `:21-22`; `pubsub-interface-spec.md:29` carries the same
  superseded phrase and is unnamed.
- `docs/pubsub-interface-spec.md:29` — *"a pure C ABI below it"* is the phrase AG2 amended in the
  sibling spec's own title line as contradicting ruling 46, still present here.
- `core/include/fletcher/core/internal/delivery_frame.hpp:22-23` — *"exactly one instance however
  many components include it"* is per linked binary, not per process.
- `pubsub/include/fletcher/pubsub/delivery_channel.hpp:107` — *"Every absorption in this process"*
  is per linked binary.
- The packet's B2 edit list (4 sites) is incomplete against the tree (8 sites).
