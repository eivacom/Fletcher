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
