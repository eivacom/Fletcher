# The Pub/Sub Provider Interface — Specification (oracle)

Status: **proposed** (round **PDA-decouple**, token `PDA-DEC`). This is the authoritative
spec for the pub/sub provider interface — the seam between Fletcher and a
protocol. On any contradiction with the plan or a per-item design, **this document
wins**. Locked-decision digest:
[plans/PDA-decouple-locked-decisions.md](../plans/PDA-decouple-locked-decisions.md).
Plan + tracker: [plans/PDA-decouple-interface.md](../plans/PDA-decouple-interface.md).

**This document is also the meeting point for two later rounds that do not
otherwise touch each other** — see §1.

---

## §0 — Why this spec exists

Fletcher has one seam between the codec and a transport:
`fletcher::PubSubProvider`
([pubsub/include/fletcher/pubsub/provider.hpp](../pubsub/include/fletcher/pubsub/provider.hpp)),
four virtuals, three implementations (Fast DDS, XRCE-DDS, and an
`InProcessProvider`, which PDA-DEC-1 lifted out of the gateway executable into
[pubsub/src/in_process_provider.cpp](../pubsub/src/in_process_provider.cpp);
it is not yet *registered* — that is PDA-DEC-5).

Two things are queued **on opposite sides of that seam**:

- **PDA-ABI** — a pure C ABI *below* it, so a protocol becomes a driver chosen and
  configured at runtime ([docs/protocol-driver-abi-spec.md](protocol-driver-abi-spec.md)).
- **BIND-C# / BIND-Rust** — a C ABI *above* it, so an application in another
  language can publish and subscribe.

Neither can be built cleanly against the seam as it stands, for reasons that are
the same in both directions: the types crossing it are C++ types with no stated
C-expressible ownership model, failures cross as exceptions, the delivery
contract is prose, and *which* provider is in use is a compile-time decision
baked into every caller.

**This round changes the seam, and nothing else.** It writes no C, defines no
ABI, and loads nothing at runtime. Its whole purpose is to leave a seam that both
ABI rounds can then build against **independently and in parallel**.

### §0.1 — The three requirements this serves

1. A provider is selected and configured **at runtime**, by name or by path.
2. Whether a provider is **built in or loaded through an ABI is invisible** to
   everything above the seam — including the language bindings.
3. Every type and every failure that crosses the seam has a **documented,
   C-expressible** ownership and error model, so a C boundary can be built on
   either side without inventing vocabulary or copying data.

---

## §1 — The parallelism contract (the point of the round)

After this round closes, **PDA-ABI and BIND-C#/BIND-Rust proceed in parallel and
meet only here.** That is only safe if the seam is genuinely frozen, so:

- **This document is the contract.** Both later rounds mirror it; neither owns it.
- **Neither ABI round may change the seam.** A later round finding the seam
  insufficient is a **stop-and-ask** against *this* spec — not a local
  workaround, and not a change landed inside an ABI round. That rule is what
  keeps the two from silently diverging.
- **Neither ABI mirrors the other.** They mirror *this*. The two C boundaries
  will end up with structurally similar types — both are views of the same C++
  types — but that is a **consequence of both mirroring the seam, not a
  dependency between them**. Neither may be defined in terms of the other's
  types, and neither may assume the other exists.
- **No shared C header between the two ABIs.** Sharing one would couple their
  release cadence and force one round to wait on the other, which is exactly what
  this split exists to avoid. Consistency of *idiom* (§5.2) is required;
  shared *code* is not.

### §1.1 — Why the binding ABI must not consume the driver ABI

Because of requirement §0.1(2). A built-in provider hands `RowEncoder` a plain
C++ `WriteBuffer&` — a `VectorWriteBuffer` or a `FixedWriteBuffer` — and there is
no driver-ABI object anywhere in that path. A binding ABI defined as pass-through
of driver-ABI types would therefore work only when the provider happens to be a
loaded driver, and break for a built-in one.

So the binding ABI's buffer is a C view over **`WriteBuffer`**, obtained the same
way whichever kind of provider is underneath. Zero-copy is preserved either way,
because both are views over the same bytes (§8). The same argument applies to
attachments: the vocabulary is `Blob`/`Attachments` and its ownership model
(§3.2), not any driver-ABI handle.

---

## §2 — The interface

The method set is **stable and not up for revision** in this round:

```cpp
virtual void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) = 0;
virtual void Publish(const std::vector<std::string>& segments, const RowEncoder& encoder,
                     const Attachments& attachments = {}) = 0;
[[nodiscard]] virtual SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                                   SubscribeCallback callback) = 0;
virtual void Unsubscribe(const std::vector<std::string>& segments) = 0;
```

What this round *may* change is the **types** in those signatures, and only where
a type has no C-expressible form (§3). Adding, removing or reordering methods is
a stop-and-ask.

**`Publish` is inverted, and stays inverted.** The provider supplies the buffer
and Fletcher encodes into it. That inversion is the entire zero-copy encode path
and the reason `FixedWriteBuffer` exists; a change that has the provider hand
back a finished buffer instead would forfeit it and is a stop-and-ask.

---

## §3 — The vocabulary: every crossing type needs a C-expressible model

This is the substance of the round. For each type crossing the seam, the spec
must state — in the C++ header, normatively — who owns the memory, for how long,
and what a C boundary on either side must do to honour it. **A type whose
ownership is only implied by C++ semantics is not done.**

### §3.1 — `WriteBuffer` — a window plus a refill hook

Already in this shape as of the Fast DDS modernization merge
([core/include/fletcher/core/write_buffer.hpp](../core/include/fletcher/core/write_buffer.hpp)):
`Append`/`AppendByte`/`AppendZeros`/`Position`/`PatchU32`/`PatchByte` are
non-virtual and write straight into `{data_, capacity_, pos_}`; only running out
of room reaches the two virtuals, `AppendSlow`/`AppendZerosSlow`.

This is the shape both C boundaries want, and the reason is worth stating: a
window means **one crossing per refill, not one per append**. Normative points a
C view must honour:

1. **Random-access, not a stream.** Fletcher back-patches length prefixes and
   null bitfields at offsets below `pos`, so bytes already written must not move
   or be flushed except inside a refill, which must preserve them verbatim. This
   is inherent to the positional wire format (TD-002).
2. **`PatchByte` ORs; `PatchU32` overwrites.** Not interchangeable.
3. **Bounds by subtraction, never addition** (`len > capacity - pos`), so a
   hostile length cannot wrap.
4. A fixed-capacity buffer reports overflow; a growable one refills.
5. **The window base is readable** — `Data()`, over `[Data(), Data() + Position())`
   only, invalidated by any refilling append and by `VectorWriteBuffer::Finish()`.
   The window is already declared `{data, capacity, pos}`, so this adds no
   obligation; without it §8.1 is not implementable from outside a provider.

### §3.2 — `Blob` / `Attachments` — shared ownership across the seam

`Blob` is an **owner plus a span** — `{shared_ptr<const void> owner, const uint8_t* data,
size_t size}` — and `Attachments = unordered_map<string, Blob>`
([core/include/fletcher/core/types.hpp](../core/include/fletcher/core/types.hpp)).
Shared ownership is what gives zero-copy publisher → provider → subscriber, and
naming the bytes separately from what keeps them alive is what lets the seam
carry memory Fletcher did not allocate.

It was `shared_ptr<const vector<uint8_t>>` until PDA-DEC-3. That could only ever
name bytes Fletcher had allocated, which is why the "Consequence" paragraph below
was not met.

The C-expressible form of shared ownership is a **handle plus retain/release**.
This round does not write that C form — it **states the contract it must satisfy**
so that both ABI rounds derive the same one:

1. A blob crossing the seam as an argument is **borrowed for the call**. A callee
   that keeps it must take its own reference.
2. Bytes are **immutable** once they cross.
3. Reference operations must be safe from **any thread**, concurrently.
4. Release must not throw and must not re-enter the seam.
5. Empty is representable with a null data pointer and zero length, and Fletcher
   **enforces** it: a zero-size blob normalises its pointer to null however it was
   built, so a C view may test `size == 0` and `data == NULL` interchangeably. A
   zero-size blob also needs no owner — there is no byte to keep alive.

**Consequence, DELIVERED by PDA-DEC-3:** the seam carries memory it does not own —
a transport's loaned sample, say — without copying it into a `vector`. The one
general constructor is `Blob(owner, data, size)`; there is deliberately **no
view-only form** (non-null `data` with a null `owner` is refused with
`kInvalidArgument`), so clause 1 above is exactly true and a C boundary implements
"keep it" as `retain(owner)`. There is also deliberately **no conversion from the
retired `shared_ptr` alias**: that would have left every call site compiling and
its copy in place, a coexistence window in a change whose whole point is that
there is none.

The C form is **conceptual, never a memory image**. `shared_ptr<const void>` is
two words and a control block; a C `{void*, const uint8_t*, size_t}` is not a
reinterpretation of it, and no layout compatibility is implied or permitted — a
boundary *constructs* a `Blob` from the three fields. This is also why the two ABI
rounds need no shared C header: they never exchange a struct with each other, they
each wrap the same C++ value from their own side, so their C spellings may differ
freely.

### §3.3 — Schemas

`ArrowSchema` is already the Arrow C Data Interface — a stable C ABI with its own
release callback — so schema *content* crosses for free and **no Fletcher schema
format is to be invented**. But the C Data Interface's release is *unique*
ownership while `SharedSchema = shared_ptr<const ArrowSchema>` is shared and is
documented as storable by callbacks across threads
([pubsub/include/fletcher/pubsub/owned_schema.hpp](../pubsub/include/fletcher/pubsub/owned_schema.hpp)).
So `SharedSchema` needs §3.2's treatment; the schema it points at does not.

`CreateTopic` transfers ownership *in* (by-value `OwnedSchema`); delivery to a
subscriber callback **borrows**.

### §3.4 — The schema future — the one type with no obvious C form

`SubscriptionResult::schema` was a `std::shared_future<SharedSchema>`. A future is
the least C-expressible thing at the seam: both ABI rounds would otherwise have
invented their own bridge, and they would have invented different ones.

PDA-DEC-3 replaced it with `SchemaArrival` — a copyable, thread-safe waitable
handle with a single-use `SchemaResolver` on the write end
([pubsub/include/fletcher/pubsub/schema_arrival.hpp](../pubsub/include/fletcher/pubsub/schema_arrival.hpp)).
There is **one waiting mechanism**: the `shared_future` is retired, not kept as a
C++ convenience beside it, so the path a C#/Rust caller uses is the path the
tree's own tests exercise.

`Wait(timeout, out)` returns a **typed** outcome, never a bare bool:

| Outcome | Meaning | `*out` |
|---|---|---|
| `kOk` + non-null | the schema arrived | written |
| `kOk` + null | **RESERVED**: this transport carries no schemas at all (§7 clause 1) | written null |
| `kPending` | not yet, within `timeout` | untouched |
| `kSubscriptionEnded` | no schema will ever arrive — the subscription is gone | untouched |
| anything else | the provider failed to produce one | untouched |

The second and the fourth are different facts that demand opposite handling at a
subscriber, and §7's failure mode for guessing wrong is silent wrong-slot
decoding. So `SchemaArrival::Ready(nullptr)` is the **only** producer of
`kOk` + null; `SchemaResolver::Resolve` refuses a null schema. A resolver is
move-only with two `&&`-qualified terminal calls and a destructor that is itself
the third outcome, so "resolved twice" and "resolved never" are both
unrepresentable — which is what makes an unbounded wait safe to offer.

C form: `fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)`, where
`fl_schema` is §3.2's owner-handle pair `{owner, const ArrowSchema*}` and **not**
a bare `ArrowSchema*`. On `kOk` the handle is a **new reference the caller
releases**. A boundary releases the *owner handle* and must **never** call the
Arrow C Data Interface `release` on a shared schema — that destroys it under every
other holder. `timeout_ms < 0` is refused with `kInvalidArgument` (so "negative
means forever" cannot be invented by one round and not the other); `INT64_MAX` is
the unbounded form, which in C++ is `milliseconds::max()`. So must any timeout at
or above a large implementation-defined threshold (~139 years in this tree): a
deadline computed as `now + timeout` overflows well below `milliseconds::max()`
and would return IMMEDIATELY, so a binding spelling "forever" as a very large
finite number must get a wait, never a poll. It must be implemented
as a deadline-free wait rather than `wait_for(duration::max())`, which overflows.

The **semantics** above are pinned; the spelling is illustrative. Each ABI round
writes both sides of its own boundary and chooses its own names and layout — there
is no shared C header (§1).

### §3.5 — Topic segments

`std::vector<std::string>`, so the provider may join with any separator. Fine as a
C++ signature; the C form is a **pointer-and-count of pointer-and-length pairs,
borrowed for the duration of the call** — a callee that keeps a segment copies its
bytes. **An empty segment list is illegal** and is refused with
`kInvalidArgument` by every method that takes one: there is no default topic and
no recovery. The check lives once, in `internal::RequireSegments`, which all three
providers already route every topic through.

---

## §4 — Uniform provider selection (requirements §0.1(1) and (2))

Today the choice is a hardcoded `if (args.provider == "fastdds")` at
[gateway/src/main.cpp:183](../gateway/src/main.cpp#L183), and every caller links
every provider it might select.

This round introduces a **provider registry**: one function that turns a
*selector* plus a *configuration* into a `PubSubProvider`, with **built-in and
loaded providers indistinguishable to the caller.**

```
selector + config  ──►  registry  ──►  shared_ptr<PubSubProvider>
                          │
                          ├── built-in, statically registered   (this round)
                          └── loaded through the driver ABI     (PDA-ABI adds this)
```

Normative:

1. **A selector is a name or a path.** A name resolves against the static
   registry; a path is a driver library. Nothing above the registry may branch on
   which — that is requirement §0.1(2), and it is what lets a deployment move a
   protocol from built-in to loaded without touching a caller.
2. **PDA-ABI adds a resolver, not a second API.** The registry's signature is
   fixed here. If it needs to change to admit loading, it is underspecified now
   and that is a stop-and-ask against this spec.
3. **No global state.** The registry takes and returns explicit objects; multiple
   instances of the same provider with different configs must be ordinary. That
   the ABI round later needs module/instance handles is *its* business; the seam
   must simply not prevent it.
4. **A built-in provider is registered, not hardcoded.** The gateway's
   `--provider` becomes a registry lookup.

### §4.1 — Configuration: typed core plus opaque document

Configuration at the seam is a small typed core plus an opaque blob:

- **The typed core** is what Fletcher itself must reason about. Both shipping
  providers already have exactly `{max_payload_bytes, domain_id}`, so the core is
  derived from evidence rather than invented.
- **The document** is everything else — bytes Fletcher transports and does not
  read.

**The seam must carry no protocol vocabulary.** `FastDDSProviderOptions` embeds
`eprosima::fastdds::dds::DataWriterQos`/`DataReaderQos` *plus per-topic maps of
them*, so configuring the transport today means compiling against Fast DDS
headers. That is the deepest coupling in the system and it cannot survive a
uniform registry: a caller selecting `"fastdds"` by name must be able to
configure it without linking eProsima.

### §4.2 — Fletcher parses nothing (explicit non-goal)

**Fletcher gains no configuration parser and no configuration dependency.** The
document's format is the *provider's* — Fast DDS's native XML QoS profile,
`key=value` for XRCE, JSON5 for a future Zenoh. One mechanism, no uniform format,
and no provider carrying a parser it cannot afford on a `<75 KB` Flash target
(TD-004, TD-007). Adding a JSON or YAML dependency "for convenience" would
quietly re-couple Fletcher to config semantics it must not know, and is a
**stop-and-ask**.

---

## §5 — Failure

### §5.1 — The seam throws; each boundary translates

Recoverable errors throw (HARD's H-INV-2), and that stays: it is the idiom the
whole C++ surface uses. Exceptions cannot cross C, so **each** C boundary
translates — independently, in its own round.

For that to yield the same statuses on both sides, the seam publishes a **stable
exception taxonomy**, and PDA-DEC-3 delivered it as **one error type carrying one
stable numbered cause** rather than a set of types plus a prose map (owner ruling
2026-09-01): `PubSubError` over `PubSubStatus`
([core/include/fletcher/core/status.hpp](../core/include/fletcher/core/status.hpp)).
Two independent bindings cannot mirror an assortment of standard exception types
without drifting, which is the drift this round exists to stop.

- `PubSubStatus` values are **fixed integers, appended only, never reordered or
  reused**, pinned one enumerator at a time by `static_assert` — a reorder fails
  the build rather than silently re-labelling every error an already-deployed
  binding has seen. `kOk = 0` exists because both C boundaries need a success
  value in the same enum (§5.2).
- `PubSubError` **refuses** `kOk`, `kPending` and `kSubscriptionEnded`: the first
  would let a boundary report a failed call as a success, and the other two are
  §2 wait outcomes rather than failures. "Refuses" means **coerces to
  `kInternal`**, not "throws on construction" — a boundary must reproduce that,
  and throwing from inside a throw expression would be worse than a mislabelled
  status. The property is the same either way: no failure ever carries a
  non-failure number.
- **Every seam entry point translates.** Each provider wraps its four methods, so
  the only exception that leaves is a `PubSubError`; anything else — including
  `std::bad_alloc` or a transport SDK's own type — becomes `kInternal` carrying
  the original `what()`. A taxonomy that lets an untyped exception through is not
  one.
- **One mapping rule, and it is normative** because a C driver author must
  reproduce it or drift: a `std::overflow_error` escaping a seam entry point
  becomes **`kPayloadTooLarge`**, not `kInternal`. `WriteBuffer`'s fixed-capacity
  overflow is its only source in the tree, and it is the only producer of that
  status — without the rule, taxonomy entry 4 is unreachable and a row that does
  not fit the transport's bound degrades to "internal", which tells a caller
  nothing it can act on. The rule is by TYPE, so it is stated rather than
  narrowed: a `RowEncoder` that throws `std::overflow_error` for reasons of its
  own is reported the same way. A `SubscribeCallback` or `RowEncoder` throwing a
  non-`std::exception` type loses its identity entirely and arrives as
  `kInternal` — the price of a boundary that cannot let an untyped exception
  through.
- `PubSubError` derives from `std::runtime_error`, so existing
  `catch (const std::exception&)` sites are unaffected. Messages are unchanged;
  what moved is branching on the error *type*, which is now branching on a number.

### §5.2 — Consistency of idiom, not of code

Both boundaries should use the same *shape* of status enum and the same
version-negotiation convention, because two idioms in one codebase is a wart.
This is a **review preference, not a dependency** (§1) — it must never become a
reason for one round to wait on the other.

### §5.3 — Callbacks must not throw across a transport

A provider invokes `SubscribeCallback` from its own thread, often from inside a C
callback in the transport library (XRCE's session pump, a Fast DDS listener),
where an escaping exception is a process termination rather than an unwind. The
seam must state that a callback **must not throw**, and say what a provider does
if one does anyway.

---

## §6 — Threading and lifetime (normative)

Existing behaviour, with the implicit parts made explicit — both ABI rounds
depend on these being written down:

1. Delivery is **serialized per subscription**; the thread may differ between
   samples.
2. Delivery **may run concurrently across different subscriptions**, including
   across provider instances.
3. Callbacks must be assumed callable from **any thread**, including a
   transport-owned one.
4. Reference operations on shared types (§3.2, §3.3) are thread-safe.
5. **Destruction requires quiescence**: no call in flight, no callback able to
   re-enter. Destruction is *not* a synchronization boundary. This mirrors what
   both concrete providers already document, and HARD-4's rule that teardown must
   not hold the provider lock while the transport waits on in-flight callbacks.

---

## §7 — The delivery contract, made executable

Today "schema before data" and "per-writer order" are **prose** in
`provider.hpp`, honoured by three providers under our own review. Two independent
ABI rounds cannot build against prose, and the failure mode is **silent
wrong-slot decoding at the subscriber, not a crash**, because the positional
format carries no per-field metadata to catch a mismatch (TD-002 risks).

The contract:

1. **Schema before data.** A callback is never invoked with a null schema. A
   subscriber may subscribe before any publisher exists; data arriving ahead of
   the schema is **buffered** and delivered once the schema is known. A transport
   that carries no schemas for a topic — the gateway's in-process loopback in its
   default mode, where the client brings its own — passes null throughout
   instead.

   **The two are never mixed WITHIN ONE SUBSCRIPTION.** Restated per subscription
   by PDA-DEC-3 (owner ruling 2026-09-01), and stronger than the instance-wide
   reading it replaces: a subscription's schema is fixed when `Subscribe` returns
   and is exactly what its `SchemaArrival` reports, so **a declaration made after
   a subscription exists never reaches that subscription** — it reaches only new
   ones. The loopback used to cache a `CreateTopic` schema and hand it to whatever
   subscription was live, which flipped a live subscription from null to non-null
   mid-stream: a client decoding one stream two ways with no signal, which is
   silent wrong data rather than an error. The clause also binds the DDS
   providers, at no cost — schema-before-data already makes every delivery there
   non-null.

   The loopback is **not** a transport that "carries no schemas at all": it
   carries the ones a publisher declared on that instance, and it can be
   constructed schema-carrying outright
   (`InProcessPubSubProvider::SchemaCarriage`), in which case it upholds
   schema-before-data by refusal — `CreateTopic` requires a schema and publishing
   to an undeclared topic is `kTopicNotDeclared`.
2. **Per-writer order**, holding **across the schema handoff**: the buffered
   pre-schema backlog is delivered before, and never interleaved with, samples
   arriving live afterwards.
3. **Idempotent re-declaration** with an identical schema; conflicting
   re-declaration **must** be rejected.
4. **One callback per topic per instance.** Local fan-out is `Subscriber`'s job.
5. **Late joiners** get the schema asynchronously; `Subscribe` never blocks.
6. **After `Unsubscribe` returns**, no further callback for that topic.

### §7.1 — The conformance suite is the deliverable

§7's clauses are encoded as a suite run against **all three** providers. It is
what makes the seam a contract rather than a description, and it is what each ABI
round later checks itself against without re-deriving the rules.

Divergences between the three are **expected** — they have never been
mechanically compared — and are **fixed in this round**, so both ABI rounds build
over consistent behaviour instead of freezing an inconsistency.

### §7.2 — The suite needs a cross-process subject

**A single-process suite cannot see transport behaviour.** Fast DDS serves
same-process endpoints over intra-process delivery, which bypasses data-sharing
and much of the transport entirely. This is not hypothetical: the Fast DDS
modernization merge shipped a receive-side data-sharing defect that lost
`TRANSIENT_LOCAL` samples for cross-process late joiners, and the provider's
entire 70-test suite was green throughout, because every test ran in one process
(see `fastdds-pubsub-provider/README.md` and
[plans/PDA-decouple-progress-log.md](../plans/PDA-decouple-progress-log.md)).

So the suite must have a **cross-process subject** for the DDS providers. A
conformance suite that cannot observe the transport would certify the seam on
evidence that does not cover it.

**This clause requires a cross-process subject; it does not claim that such a
subject observes every transport defect, and PDA-DEC-1 measured that it does
not.** The suite landed with the required cross-process subjects, but repeated
falsification could not make it fail against a provider with the receive-side
data-sharing defect deliberately re-enabled, while
`integration-tests/gateway-fastdds-ts` failed in the same session against the same
build. Two hypotheses were refuted by measurement; the remaining candidate is the
mix and count of data-sharing endpoints per participant, which needs a subscribing
peer this harness does not have. The evidence table lives in
`integration-tests/pubsub-conformance/README.md`, the defect is owned by
**PDA-ABI-7**, and the owner ruled on 2026-09-01 that the suite ships with the
blind spot documented (`plans/PDA-DEC-rulings.md`). **Do not read a green
conformance run as evidence about that defect class.**

---

## §8 — Zero-copy is a property of the seam

Zero-copy is required for rows **and** attachments, and it is a property *of this
seam* — both ABI rounds inherit it and neither can restore it if the seam loses
it.

- **Rows:** already there, via `Publish`'s inversion and `FixedWriteBuffer`.
- **Attachments:** already there publisher → provider → subscriber, via
  `shared_ptr`.
- **Receive:** the *seam* no longer stands in the way — `Blob` is an owner plus a
  span, so a provider hands over borrowed transport memory where it lies (§3.2,
  delivered by PDA-DEC-3). What is still not there is the transport half: Fast
  DDS's loanable read path materialises one owning copy per sample **that carries
  attachments** (down from one per attachment; an attachment-free sample is
  untouched) because the loan is returned when `Take` returns and a buffered
  pre-schema backlog can outlive it, and `Envelope::row` is still a `vector` copy
  on every XRCE and gateway receive. §11 assigns the loaned-sample path to
  PDA-ABI by name.

### §8.1 — It must be falsifiable

"Zero-copy" is unverifiable by inspection and regresses silently, so this round
ships a **copy-accounting oracle** — `integration-tests/pubsub-conformance`, the
`CopyAccounting` suite — which both ABI rounds inherit rather than reinvent.

The mechanism is **address provenance**, not counting: the window base after the
encoder's last append (§3.1 clause 5) must equal, **strictly and span for span**,
what the subscriber callback receives, and each delivered `Blob`'s `data()` the
published one; content equality is checked first, so "garbled" and "at a second
address" are different failures. It binds only where delivery is **synchronous on
the publishing thread** and the encode window stays **allocated until the callback
returns** — a subject breaking either is not measurable this way. Refill movement
is permitted (§3.1 clause 1) and **reported as a number**; every other byte
movement is a violation, and the copy §3.2 used to force on a provider's own
borrowed memory is pinned at **zero** — it was pinned at exactly one so that its
removal would turn the guard red, which is what happened when PDA-DEC-3 removed
it (`CopyAccounting.BorrowedAttachmentCostsNoCopies`, renamed from
`…CostsExactlyOneCopy`). A third suite, `SeamVocabulary`, pins what the crossing
types make representable rather than what a provider does.

**Scope.** Green is evidence about *this seam* and nothing else — not about a
transport's data-sharing, loaned samples or receive-side zero-copy. A live
negative control ships with it: a guard nobody has made go red is a guard nobody
has measured.

---

## §9 — What each later round owes this one

| | **PDA-ABI** (below) | **BIND-C#/BIND-Rust** (above) |
|---|---|---|
| Mirrors | §3 vocabulary, §5 taxonomy, §6, §7 | §3 vocabulary, §5 taxonomy, §6, §7 |
| Implements | the driver side of the seam | the caller side of `Publisher`/`Subscriber` + §4 selection |
| Adds to §4 | a resolver for path selectors | nothing |
| May change the seam | **no** (stop-and-ask) | **no** (stop-and-ask) |
| Depends on the other | **no** | **no** |

Note where **§4 selection** lands: choosing and configuring a provider at runtime
is binding-visible — a C# application must be able to do it — so it is part of
the *seam's* surface and therefore of the binding ABI, **not** of the driver ABI.
The driver ABI's own surface is only what a driver implements and what the host
calls back; a driver never implements selection.

A **driver written in Rust or C#** is entirely legitimate and is unrelated to a
Rust or C# *application binding*: the former implements the driver ABI, the
latter calls the seam. Same language, opposite directions — worth naming, because
the two are constantly conflated.

---

## §10 — Migration and blast radius

Measured, excluding `docs/archive/**`:

**External consumers of the protocol-typed config** that a uniform registry
retires — 4 files, 19 occurrences:

| Site | Occurrences |
|---|---|
| [integration-tests/fastdds-xrce-interop/tests/test_interop.cpp](../integration-tests/fastdds-xrce-interop/tests/test_interop.cpp) | 9 |
| [integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp](../integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp) | 8 |
| [gateway/src/main.cpp](../gateway/src/main.cpp) | 1 |
| [integration-tests/gateway-fastdds-ts/src/fastdds_peer.cpp](../integration-tests/gateway-fastdds-ts/src/fastdds_peer.cpp) | 1 |

**Provider-internal churn** (part of the work, not external migration):
`fastdds-pubsub-provider/` 39 occurrences across 7 files — 24 in its QoS test TU
alone, so those tests are substantially rewritten against profile documents;
`xrcedds-pubsub-provider/` 10 across 4 files.

**Docs:** both provider READMEs, plus the "implementing one interface" claims in
[docs/architecture-overview.md](architecture-overview.md) and the root
[README.md](../README.md).

**Consumers of the vocabulary change, which sit ABOVE the seam.** Giving schema
arrival a C-expressible form (§3.4) is not confined to providers: `SubscriptionResult`
and its `shared_future` are consumed by 10 sites outside `provider.hpp` —
`pubsub/src/subscriber.cpp` (5), `pubsub-arrow/src/subscriber_arrow.cpp` and its
header (4), `gateway/src/{main,ws_session}.cpp` (3), plus both `test_package`
examples and the `pubsub` / `pubsub-arrow` test suites. Same for any change to
`Blob` under §3.2. So PDA-DEC-3 lands in `core/` and `pubsub/` **and** ripples up
through `pubsub-arrow` and the gateway; the round is not "providers only" in
either direction.

`InProcessProvider` moves out of the gateway executable into a real component
and becomes a registered built-in — the first proof that §4's registry works, and
later the body of PDA-ABI's reference driver. The **lift** landed in PDA-DEC-1
(the conformance suite cannot link a type in an anonymous namespace inside a
`main.cpp`); PDA-DEC-5 is the **registration**.

---

## §11 — Out of scope

Everything ABI. Specifically: no `extern "C"`, no C header, no `dlopen`, no
version negotiation, no driver vtable, no host-callback struct, no static
registration table for *drivers* (a registry of *built-ins*, §4, is in scope).

Also out: the wire format (byte-identical is a hard invariant), the codec,
generated code, the gateway's WebSocket protocol, a protocol bridge, and any
change to the interface's method set (§2).

Deliberately deferred with a named home: **zero-copy receive** is enabled here
(§3.2) but delivered in PDA-ABI, where the loaned-sample path and the data-sharing
defect live.
