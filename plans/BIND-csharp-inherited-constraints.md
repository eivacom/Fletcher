# BIND-C# — What the seam obliges the binding to do

**Not a round plan, and not reviewed.** A single note, written 2026-09-07 at
`4ad003e`, listing what a C# binding inherits from the pub/sub seam as PR #126
leaves it. No round has opened; there is no tracker here, no Definition of Done
and no gate.

Seam spec (oracle): [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md).
Sibling round: [PDA-ABI-protocol-driver-abi.md](PDA-ABI-protocol-driver-abi.md).

## Provenance, and why that matters for how much this is worth

Two BIND-C# review passes over PR #126 produced twelve findings against the
seam. Eleven were seam defects and are **closed** — nine as items
`PDA-DEC-A1..A8` plus `A9` (rulings of 2026-09-03), and two more by ruling 59
(2026-09-07). One was declined as false. The dispositions live in
`plans/PDA-DEC-rulings.md` and
[reviews/PDA-DEC-PR126-bind-pass2-verification.md](reviews/PDA-DEC-PR126-bind-pass2-verification.md);
the original packets are in PR #126's conversation, dated 2026-09-03 and
2026-09-07. **None of that is re-litigated here** — this file is only what is
left for the binding to do.

**This list has had one author and no review.** The verification pass that
checked the second packet assessed this register for *reachability only* —
whether anything in it was an under-rated defect in shipped behaviour, which it
was not — and explicitly reported nothing else about it. So treat every item
below as a first draft. Two of that same author's second-pass findings were
false, one of them in a way that would have damaged frozen spec text, which is
the calibration to apply here.

**What this list is not:** a claim that the seam is insufficient. Every item is
an obligation arising from behaviour that is documented, owner-ruled and
deliberate. Nothing here is a Fletcher defect, and nothing here needs a ruling
before BIND opens. If an item turns out to need a seam change, §1 governs — it
is a **stop-and-ask against the seam spec**, not a local fix and not a change
landed inside BIND.

---

## 1 — Ship exactly one copy of Fletcher in the process

**Derives from:** `core/include/fletcher/core/internal/delivery_frame.hpp` P1,
as corrected by ruling 59 — *one frame stack per **binary***.

The re-entrancy machinery is an `inline thread_local` in a header-only target
(`fletcher-core` is `INTERFACE`; `fletcher-pubsub` is `STATIC` with no `shared`
Conan option). It is therefore emitted into every consuming binary and
deduplicated only *within* one. Two separately linked binaries carrying Fletcher
get **two stacks**.

A C# binding packages a native shim, which is that second binary if the host
process also links Fletcher — a C++ application hosting the CLR, a second
language binding, an in-process test host.

**What forks when it forks, none of it loudly:**

- §6 clause 6's re-entrancy refusal ([spec:838](../docs/pubsub-interface-spec.md:838))
  stops firing, because a door in one binary cannot see a delivery frame pushed
  in the other. On Fast DDS that restores the listener-mutex hang the clause
  exists to prevent.
- The depth-zero deferred sweep queues onto the wrong thread-local, so
  `Subscriber`'s deferred retirements either never run or run while the other
  binary still believes it is inside a delivery.
- `DeliveryChannel::AbsorbedTotal()` reports one binary's count.

**Owed:** a deliberate packaging decision — Fletcher static inside a single
native shim that is the process's only copy, or a host-side dispatch adapter
with the binding holding no copy — plus a startup check that no second copy is
present. The check is the point: the failure mode is silence, so nothing else
will tell you.

P1's own note is a **STOP-AND-ASK** if this cannot be satisfied. Do not paper
over a fork with a registration handshake.

## 2 — `Dispose` needs a "am I inside my own delivery thunk" flag

**Derives from:** `subscriber.hpp`'s `Unsubscribe` contract (PDA-DEC-A4).

The main promise is what a managed binding needs: once `Unsubscribe` returns,
that callback is not running and will not run again, so the `GCHandle` may be
freed or unpinned. It **blocks** to make that true, for as long as the callback
takes.

The carve-out is the work: an `Unsubscribe` issued *from inside a delivery on the
same Subscriber* does **not** wait. It still guarantees no invocation begins
afterwards; it does not guarantee none is in progress. On that path the handle
must stay pinned.

**Owed:** the binding's own delivery thunk is the only thing that knows which
path a disposal is on. It must set a thread-local the disposal path reads, and
the two paths must free different amounts.

## 3 — Refuse `Dispose` of a Subscriber from inside a handler, in managed code

**Derives from:** §6 clause 5 as widened
([spec:822](../docs/pubsub-interface-spec.md:822)), owner ruling 2026-09-05, and
`subscriber.hpp`'s destructor contract.

Destroying a `Subscriber` from inside a delivery on its own provider **ends the
program**, by design: the destructor reaches `provider->Unsubscribe`, clause 6
refuses it with `kReentrantCall`, and a refusal thrown from a `noexcept`
destructor terminates, naming the cause. The alternative the ruling rejected was
to skip teardown and leak the transport subscription silently.

In C# the reachable spellings are `Dispose()` called from a handler, and a
`using` scope whose exit lands inside one.

**Owed:** throw a managed exception before the call reaches native code. A
managed exception with a stack trace the author can read is strictly better than
a correct process abort — and the abort is correct, so this is the binding's
ergonomics problem, not the seam's.

## 4 — Defer any `Subscribe` issued from inside a delivery

**Derives from:** `subscriber.hpp`'s `Subscribe` contract.

Served when this `Subscriber` already holds a provider-level subscription for
that topic; refused with `kReentrantCall` when it does not, because establishing
one means entering the provider from inside its own delivery frame. The header
states it plainly: *which of the two you get depends on the data, not on the
call.*

**Owed:** always queue such a call past the callback's return, so the managed
surface is deterministic rather than a function of subscription history.

## 5 — Give C# an async path for acting on the seam from a handler

**Derives from:** §6 clause 6 ([spec:838](../docs/pubsub-interface-spec.md:838)) —
all four `PubSubProvider` methods refuse a re-entrant call, uniformly, with no
per-protocol exception and no method carved out.

So a handler cannot publish a reply inline. Request/reply is an ordinary C#
pattern, and the seam's own answer is *"copy what it needs and act after the
callback returns"*.

**Owed:** a first-class async dispatch helper, so the common pattern has a
supported spelling rather than users discovering `kReentrantCall`. Note the
refusal is per *thread* and per *provider instance*, so handing the work to
another thread is sufficient and is the sanctioned route.

## 6 — `AppendInPlace` reaches generated encoders only, never application code

**Derives from:** §3.1 clause 6's last bullet
([spec:189](../docs/pubsub-interface-spec.md:189)) — *"A producer must report what
it wrote."*

`used` is trusted against `room`, not against what was actually touched, so bytes
in `[written, used)` are committed and published having been written by nobody.
On a `FixedWriteBuffer` in this tree that span is a transport's payload —
recycled pool memory that may still hold **the previous sample's bytes**. The
header's refusal to `memset` is right: it would cost O(room) on precisely the
zero-copy path the member exists to open.

The managed hazard is an ordinary bug, not an exotic one: returning a character
count where a byte count was wanted over-reports for every non-ASCII field.

**Owed:** `AppendInPlace` is not part of the public managed surface. Only
generated encoders reach it, and a generated encoder returns **its own cursor
delta** — never a separately computed length.

## 7 — Choose the in-place writer's failure convention, and write it down

**Derives from:** §3.1 clause 6 ([spec:158](../docs/pubsub-interface-spec.md:158)).
This is the residual of a finding that was declined as false; the decline was
correct and this is what survived it.

Two mechanisms already exist, and the stronger one is normative:

- **Raise inside the writer's frame.** The C form
  ([spec:166](../docs/pubsub-interface-spec.md:166)) is
  `size_t (*writer)(void* ctx, uint8_t* dst, size_t room)` — three arguments —
  while `AppendInPlace` invokes its writer with **two**. A C function pointer
  therefore cannot *be* the `Writer`; a C++ adapter closing over `ctx` is
  structurally mandatory, and that adapter has clause 6's full channel: *an
  exception from the writer commits nothing and propagates unchanged*. So the
  managed thunk catches its own exception, sets `ctx->failed`, returns; the
  adapter throws; nothing commits; the publish fails. **No managed exception
  ever crosses into native code.** This is the one to use.
- **Return more than `room`.** A spec-normative refusal
  ([spec:179](../docs/pubsub-interface-spec.md:179)) that also commits nothing, so
  returning `SIZE_MAX` aborts the publish from inside the C form. Available, but
  the diagnostic message is misleading for this use.

**Owed:** pick the first, document it, and make it the only route the generated
encoders use. Unstated *for the boundary* is exactly why this is BIND's.

## 8 — Do not let the absorbed-failure count read zero

**Derives from:** §5.3 ([spec:791](../docs/pubsub-interface-spec.md:791)) and
`Subscriber::AbsorbedCallbackFailures()`.

A callback must not throw; if one does, the seam absorbs it at a `noexcept`
dispatch site, reports it to nobody, and **counts** it. The count is the whole
difference between containment and a silent wrong answer.

But it counts what crosses into C++ as an exception. A thunk that catches its own
managed exceptions — which item 3's process-safety argument requires it to do —
increments nothing, so the property §5.3 was built for is lost at exactly the
boundary it was built for.

**Owed:** either rethrow as `PubSubError` so the seam counts it, or expose the
binding's own counter with the same semantics — monotonic, per-instance, readable
so a test can bracket an operation and assert the delta. Do not ship containment
with no observable.

---

## 9 — A refused `Subscribe` still walks the rollback path (inherited debt)

**Derives from:** `AG1-DEBT-21`, migrated at PDA-DEC's close from
`plans/reviews/design-debt.md` (archived with that round's reviews). Raised by
PDA-DEC-AG1 cycle-3 code review (should-fix S2), 2026-09-05, and **addressed to
"the next item to touch `subscriber.cpp`'s subscription bookkeeping"** — which is
the caller tier this binding wraps, not the driver tier PDA-ABI re-cuts.

`Subscriber::Subscribe` consumes a subscription id, inserts into
`subscription_topic` and `topics`, and republishes the fan-out snapshot **before**
reaching the caller-tier door. A re-entrant call that is then refused unwinds
through the retirement-and-deferred-sweep path that PDA-DEC-A4 needed four fix
cycles to settle, for an entry no caller ever observed.

**Not a correctness defect today** — two independent reviewers traced the unwind and
neither blocked on it; the rollback takes no gate under `mu` and the entry is
removed before the throw leaves the tier. It is a hazard because it makes the most
delicate code in the item reachable from a path that has no reason to touch it.

**Why it was not fixed in-round.** It is a concurrency change to that same rollback
path, arriving in fix cycle 3 of a bounded-at-three budget. Landing it would have
required a fourth code review of the item's highest-risk code to verify a finding
neither reviewer considered blocking. The published contract was narrowed instead to
state what the code actually does (the local record is rolled back on the way out),
so nothing false ships.

**Acceptable fix:** look `key` up under `mu` and refuse before `try_emplace`, so the
refused call never enters the rollback path at all.

**Owed here:** item 4 tells the binding to queue any `Subscribe` issued from inside
a delivery, which is what keeps this path out of reach from managed code. If the
binding instead lets such a call through and catches `kReentrantCall`, it is
exercising this path on purpose.

---

## Standing rulings this list rests on

Migrated at PDA-DEC's close (2026-09-08) from `plans/PDA-DEC-rulings.md`, which is
archived with that round's execution corpus. Ruling numbers are positions in that
ledger (ruling *n* = its *n*-th `## 2026` entry). These are not seam text and not
new obligations; they are the owner's own words, and the first one is the premise
several items above rest on.

**BIND are full pub/sub clients, not read-only accessors** — ruling 5(2),
2026-08-31, the owner's own prose: *"They're full pub/sub clients."* Asked about
what BIND-C# and BIND-Rust are meant to do. It is the load-bearing premise under
the `Unsubscribe`-idempotence ruling (a finaliser cancels unconditionally and cannot
let an exception escape), and items 2, 3 and 8 all rest on it. Seam §9's table says
BIND *"implements the caller side of `Publisher`/`Subscriber` + §4 selection"* —
which is a scope statement, not this ruling. A binding that publishes as well as
subscribes is what was asked for; a read-only accessor is not.

**Fix a leaking guarantee at the scope the guarantee is about — do not publish the
leak** — ruling 39, 2026-09-04. Given after PDA-DEC-A4 had scoped one promise four
ways (process-wide → per-`Impl` → per-frame → per-gate, the last being the first
scope whose lifetime matched the guarantee). The preference is conditional: it
applies **provided the root cause has been NAMED** rather than another instance
patched. **Its stop condition stands for any future item:** *a further case after
the root-cause fix means the premise is broken and the item stops.* BIND will meet
this the first time a published promise leaks at the managed boundary — items 2 and
8 are both promises of exactly that kind.

**Narrowing a published claim may be inferred; widening it never may** — rulings 32
(2026-09-03) and 37 (2026-09-04). Ruling 32 granted a licence to infer the owner's
standing preference for a narrow claim stated honestly over a wide one implied, so a
design need not ask again. Ruling 37 bounds it: that licence *"permits **narrowing**
without asking, never **widening**"*. Seam §12.4 records the preference but not the
asymmetry, and the asymmetry is the half that decides whether an item must stop and
ask.

**A Linux-only difference in seam behaviour is a question for the owner** — ruling
32's third instruction, frozen into seam §12.4: it is *a stop-and-ask against that
spec, not a local fix*, because a local fix by one round would silently change the
seam both rounds share. No automated build ever ran on `feature/protocol-driver-abi`
before PR #126, so eight items of Linux-side correctness exist as local Windows runs
plus one WSL compile: **treat Linux as unverified.** This is not theoretical for
BIND — three of the seven defects the PR #126 packets raised were exactly
Linux-only differences.

---

## Two mapping details that bite once each

**`Timeout.Infinite` is `-1`, and the seam refuses it.**
`timeout_ms < 0` is refused with `kInvalidArgument`
([spec:339](../docs/pubsub-interface-spec.md:339)), deliberately, so that
"negative means forever" cannot be invented by one boundary and not another.
Every .NET wait API spells forever that way. `TimeSpan.MaxValue` maps correctly —
it lands above the ~139-year threshold that means unbounded — but `-1` does not,
and a `Task`-based wrapper is the first thing that will try it.

Carried with it: §3.4's unbounded-wait clause is **pinned by no test, by ruling**
— a recorded blind spot in §12.3, because a conforming and a non-conforming
implementation are indistinguishable on the standard library this tree builds
against on Windows. BIND inherits an unmeasured guard on the first API it
touches. That is a permission the owner granted, not an oversight; it belongs on
BIND's risk list rather than in a bug report.

**Every string bound is bytes, never characters.** The joined topic name is
capped at 246 **UTF-8 bytes** (`kMaxJoinedTopicBytes`, from Fast DDS's 255-byte
announced ceiling less `"/__schema"`), and `Attachments` keys are ordered by
`memcmp` over bytes — which `core/include/fletcher/core/types.hpp` already notes
differs from C#'s ordinal UTF-16 order. Neither is visible from `string.Length`.
**A boundary never sorts; it reads the sequence Fletcher publishes.**

---

## One thing to know about the ground under items 6 and 7

`WriteBuffer::AppendInPlace` — the member PDA-DEC-A1 added specifically so a
language binding could fill a buffer it was handed — has **no production caller
in this tree**. Every caller is a test or a conformance subject
(`core/tests/test_write_buffer.cpp`,
`integration-tests/pubsub-conformance/src/copy_accounting.cpp`,
`.../seam_vocabulary.cpp`).

So BIND-C# will be its first production consumer, and the guards around it —
including the residue item 6 describes — have never met real generated-encoder
code. Its unit and conformance coverage is thorough; it is simply not the same
thing as having been used.
