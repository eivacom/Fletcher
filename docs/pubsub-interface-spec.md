# The Pub/Sub Provider Interface — Specification (oracle)

Status: **frozen** (round **PDA-decouple**, token `PDA-DEC`; frozen 2026-09-03 by
PDA-DEC-9). This is the authoritative spec for the pub/sub provider interface — the seam
between Fletcher and a protocol. On any contradiction with the plan or a per-item design,
**this document wins**. **§12 states what "frozen" binds, who may act on each class, and how
each handoff condition was actually verified** — read it before proposing any change here.
Locked-decision digest:
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
the unbounded form, which in C++ is `milliseconds::max()`. **So is any timeout at
or above a large implementation-defined threshold** (~139 years in this tree): a
deadline computed as `now + timeout` overflows well below `milliseconds::max()`
and would return IMMEDIATELY, so a binding spelling "forever" as a very large
finite number must get a wait, never a poll. The unbounded form must be
implemented as a deadline-free wait rather than `wait_for(duration::max())`,
which overflows.

That last clause is normative and **pinned by no test in this tree**, which is
stated rather than left to be discovered: the standard library Fletcher builds
against on Windows clamps the deadline inside `wait_for`, so an implementation
that honours the rule and one that does not behave identically there. It was
measured both ways. It is guarded by being written down; a test asserting it here
would pass for a reason other than the one it states, which is worse than none.

The **semantics** above are pinned; the spelling is illustrative. Each ABI round
writes both sides of its own boundary and chooses its own names and layout — there
is no shared C header (§1).

### §3.5 — Topic segments

`std::vector<std::string>`. Fine as a C++ signature; the C form is a
**pointer-and-count of pointer-and-length pairs, borrowed for the duration of the
call** — a callee that keeps a segment copies its bytes.

**The join is the seam's, not the provider's discretion.** The seam computes the
topic's name once, joining the segments with `/`, and **that name is the topic's
identity**. A driver may map that name into its own transport's namespace — a
transport on which `/` is not a legal topic character is still conformant — but
**only injectively**, and every companion name it derives from a topic's name must
lie in the reserved `__` namespace below. Both obligations are needed: injectivity
alone would still let a driver derive `name + ".meta"` and land on an accepted
topic, which is the collision this section exists to close.

**Refused with `kInvalidArgument` by every method that takes a segment list** — one
check, no default topic, no recovery, no partial mode:

1. an **empty segment list**: it names no topic;
2. a segment containing a **NUL**: the name would not reach the wire whole (XRCE
   hands it to a `const char*` API that has no length form, so the transport would
   see a truncated name);
3. a segment containing **`/`**: the joined name would not split back to the list it
   came from, so `{"a/b"}` and `{"a","b"}` would be one topic on every provider;
4. an **empty segment**: rule 1 one level down — `{""}` reproduces the very name
   rule 1 forbids, and `{"a",""}` names `"a/"`;
5. a segment beginning **`__`**: the reserved namespace derived companion names live
   in — both DDS providers derive `name + "/__schema"`, so `{"a","__schema"}` would
   land on the schema channel of `{"a"}`. The **prefix** is reserved, not any one
   literal name, so a future companion name needs no further amendment here.

The invariant those five establish, for every accepted segment list `L`: `Join(L)`
contains no NUL, `Split(Join(L)) == L`, and `Join(L)` is not a derived companion
name. So two distinct accepted lists are two distinct topics in **every** provider,
no accepted name collides with a derived one, and the name a provider hands its
transport is the whole name.

No trimming, no case folding, no Unicode normalisation, no escaping: identity is
bytes, and there is no normalisation step anywhere on this path for two providers to
disagree about. The checks live once, in `internal::RequireSegments`, which all three
providers already route every topic through.

---

## §4 — Uniform provider selection (requirements §0.1(1) and (2))

Before this round the choice was a hardcoded `if (args.provider == "fastdds")`
in [gateway/src/main.cpp](../gateway/src/main.cpp), and every caller linked every
provider it might select. **As landed** (PDA-DEC-4/5/6) the gateway names no
concrete provider type at all: it calls `RegisterInProcessProvider` and
`RegisterFastDDSProvider`, then one `Create`.

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

   **The rule, as landed** (PDA-DEC-4, `ProviderSelector::Parse`): a **name** is
   a non-empty string of `[A-Za-z0-9_-]` and nothing else; every other non-empty
   string is a **path**; the empty string and a string containing an embedded NUL
   are refused with `kInvalidArgument`. There is no trimming, no case folding, no
   normalisation, and **the rule never consults the registry**, so a given string
   means the same thing in every build. The same predicate validates a
   registration, so a registered name is always selectable.

   **C form of the selector** (§3.5's idiom, so a binding does not have to invent
   one): a pointer and a length, borrowed for the duration of the call, the
   **length authoritative** — the selector is bytes-plus-length, not a
   NUL-terminated string, and the seam copies what it keeps. That is why an
   embedded NUL is refused rather than tolerated: a NUL-terminated reading of
   `"fastdds\0/../evil.so"` would classify as a path and reach a loader
   truncated.

   The two ways a selection can fail are **deliberately different statuses**,
   because they are different operator actions (owner ruling 2026-09-02): an
   unregistered *name* is `kInvalidArgument` listing what is registered — "no
   such protocol here" — and a *path* in a build with no resolver installed is
   `kNotSupported` — "this build cannot load drivers at all". Collapsing them
   would make a typo and a driver path indistinguishable.
2. **PDA-ABI adds a resolver, not a second API.** The registry's **whole public
   surface** is fixed here — `Create`, `Register`, `SetPathResolver`, and nothing
   else. PDA-ABI adds no method: it *calls* `SetPathResolver` with a resolver
   built in its own component, so `dlopen` never enters `fletcher-pubsub`. The
   path branch inside `Create` already exists and is already routed, and
   PDA-DEC-4 proves it end-to-end with a stand-in resolver. If any of this needed
   to change to admit loading, it is underspecified now and that is a
   stop-and-ask against this spec. `Create`'s signature is pinned by a
   `static_assert` on the member-pointer type, not on the return type: a
   return-type check cannot see a defaulted extra parameter or a dropped `const`.

   **The lifetime obligation on both seats is part of this clause.** A resolver
   **or a factory** must keep everything the provider it returns depends on —
   including a loaded module — alive for at least as long as that provider,
   independently of the registry's lifetime and of its own. Clause 3 sanctions
   destroying a registry while its providers run, so a module cache owned by the
   registry would unload code under a live provider. It binds a *factory* because
   a loaded driver is reachable by **name** too — `Register("zenoh",
   factory_that_dlopens)`, which is also how the linkage ruling's static half
   registers a driver on an MCU — and the registry owns `factories_` exactly as
   it owns the resolver.

   **PDA-DEC-4 makes this mechanical rather than advisory.** Both seats are held
   by shared handle and every provider `Create` returns owns a copy of the handle
   that made it, so the callable and everything its closure captured outlive
   every provider handed out from it whatever its author did. PDA-ABI inherits
   the guarantee instead of having to honour a paragraph.

   **The anchor reaches the provider `Create` returned and nothing else**, so
   residues remain the author's. At least these three, and the list is *not*
   exhaustive: a handle the provider mints for itself (`shared_from_this`); a raw
   pointer into module memory handed to something that outlives the provider; and
   **any handle the seam itself carries out of a provider** — `Blob`'s
   `shared_ptr<const void>` owner (§3.2 / decision 6), a `SharedSchema`, a loan
   release. That third one is *not* the provider and does not pass through
   `Create`'s return, so the anchor has no edge to it, while its deleter is module
   code and a caller routinely holds one after the provider is gone. It is what a
   zero-copy receive is made of, and covering it means changing what crosses the
   seam — so it belongs to PDA-ABI, not to the registry.
3. **No global state.** The registry takes and returns explicit objects; multiple
   instances of the same provider with different configs must be ordinary. That
   the ABI round later needs module/instance handles is *its* business; the seam
   must simply not prevent it.

   **As landed** (PDA-DEC-8), and scoped exactly as the owner ruled it on
   2026-09-03: two instances of one provider, created through one registry in
   **one application on one machine** with **different domains**, exchange no rows
   and share no topic declaration, **within a window in which a same-domain
   control measured a real crossing** (`Registry.TwoInstancesTwoDomainsStayIsolated`
   and its `…OneDomainDoInterfere` control, plus
   `…StayIsolatedUnderConcurrentTraffic`, in `conformance_fastdds`). *Separately*,
   two instances with **different payload bounds** each honour their own — a row
   over one instance's bound is dropped there and delivered on the other
   (`Registry.TwoInstancesKeepTheirOwnPayloadBounds`). That second pair **makes no
   crossing claim** in either direction — the bound is part of the registered DDS
   type name, so it could not cross regardless, whatever the registry did.
   **Three exclusions, stated rather than implied:** nothing about
   isolation between machines; nothing about vendor process-wide state both
   instances would set identically; and nothing about the shared memory two
   *separate* processes on one machine use — Fast DDS serves same-process endpoints
   over intra-process delivery (locked decision 12), so what is shown isolated is
   the matching and routing layer, not those segments. The guard is falsifiable and
   was falsified: six mutations to product code, each turning a named **case** above
   red — by a named assertion, or by the typed refusal recorded beside it, three of
   which escape the test helper's constructor before an assertion is reached — all
   recorded verbatim in the suite README. No product code changed for
   this clause — the tree already had the property.
4. **A built-in provider is registered, not hardcoded.** The gateway's
   `--provider` becomes a registry lookup. **As landed** (PDA-DEC-5, PDA-DEC-6):
   the loopback is selectable under the name `inprocess`
   (`RegisterInProcessProvider`), Fast DDS under the name `fastdds`
   (`RegisterFastDDSProvider`) and XRCE-DDS under the name `xrce`
   (`RegisterXrceProvider`, PDA-DEC-7) — each in the provider's own component, so
   **the gateway** names no concrete provider type. Conformance subjects, interop
   tests, `test_package` and the READMEs' examples still construct providers
   directly and legitimately: they are testing or demonstrating a specific
   provider, which is exactly the case selection-by-name does not cover. The gateway registers the two it
   links unconditionally, before the selector is looked at — registration states
   *availability* (a link-time fact), `Create` performs *selection* (a runtime
   string), and branching registration on the selector would put a selector
   branch back above the seam. The gateway does **not** register `xrce`, because
   it does not link the XRCE client: what a build has is a link-time fact, and
   registration is where it is stated. `Registry.FastDdsResolvesAsABuiltIn` and
   `Registry.XrceResolvesAsABuiltIn` (in `conformance_fastdds` and
   `conformance_xrce`, deliberately not in `conformance_registry`, whose narrow
   link line is itself the "no transport SDK is reachable" guard) each prove the
   name resolves and delivers a row through a base-typed handle.

### §4.1 — Configuration: typed core plus opaque document

Configuration at the seam is a small typed core plus an opaque blob:

- **The typed core** is what Fletcher itself must reason about. Both shipping
  providers already have exactly `{max_payload_bytes, domain_id}`, so the core is
  derived from evidence rather than invented. It is **exactly those two fields**
  and it is append-only; a later field never changes `Create`. Widening it
  because one protocol wants a setting typed is a stop-and-ask (owner ruling
  2026-09-02: "Fletcher keeps exactly payload size and domain"). `0` in
  `max_payload_bytes` means *unset* — the provider's own default applies, and it
  is safe to spell it that way because `IsPayloadBound(0)` is false everywhere.
- **The document** is everything else — bytes Fletcher transports and does not
  read. C form: a pointer and a length borrowed for the duration of the call, the
  **length authoritative** (the bytes may contain NUL); a provider that keeps it
  copies it.

**As landed** (PDA-DEC-6), Fast DDS's document is **its own native XML QoS
profiles document, as text** — the setting carries the XML itself, never a
filename (owner ruling 2026-09-02), and Fast DDS parses it through
`get_participant_extended_qos_from_xml` / `get_datawriter_qos_from_xml` /
`get_datareader_qos_from_xml`, which take a *string* and register nothing
process-wide. Reserved profile names are `fletcher_participant` (**mandatory** in
a non-empty document, because "malformed" and "no such profile" share one return
code), `fletcher_writer`, `fletcher_reader`, and a profile named after the
`/`-joined topic for a per-topic override. **A supplied profile is that
endpoint's whole quality-of-service** — no merge, no floor — because the XML API
cannot report which policies a document mentioned. The two settings a QoS profile
cannot express (`fletcher.loan_publish`, `fletcher.max_schema_bytes`) ride as
vendor properties inside the anchor's `<rtps><propertiesPolicy>`, which is native
Fast DDS XML, so there is still exactly one reader and one format. `domain_id`
always wins over an anchor's `<domainId>`, and a non-zero disagreement is refused
rather than silently resolved. **Not every document refusal is a construction-time
refusal, and a provider must say which are not:** the misplaced-`fletcher.*`-property
refusal fires when the profile carrying it is resolved — inside the constructor for the
two role profiles, but on a topic's first `Publish` / `Subscribe` for a profile named
after that topic, which is the first moment its name is known. A constructed provider is
therefore one whose *participant* configuration is good, not one whose whole document has
been read, and the provider's public header states this rather than promising the
stronger thing. The convenience of reading a document out of a file lives in the
**gateway** (`--provider-config FILE`), never in Fletcher.

**As landed** (PDA-DEC-7), XRCE's document is a sequence of `\n`-separated
`key=value` entries with **four** keys: `transport` (`udp` | `tcp`), `agent`
(`HOST:PORT`), `session_key` (decimal `uint32`) and `connect_timeout_ms`
(decimal 0–60000). The address is **one** key, because two would let a document
name only the host and silently keep the default port — a half-specified address
is the same class of silence the no-merge rule above exists to remove. Numbers
are parsed wide and range-checked per key, so **no value is ever narrowed
silently**; `domain_id` is typed core and `uint16_t` on the XRCE wire, so above
65535 it is refused rather than truncated. `transport=serial` is nameable and
refused with **`kNotSupported`**, distinctly from an unknown value's
`kInvalidArgument` (owner ruling 2026-09-02, "accept it, fail distinctly").
**This provider owes nothing under the disclosure clause above, and structurally
so:** its document is read to completion before a buffer is sized, a socket
exists or a session is created, and no key of its is topic-scoped, so there is no
later moment at which one first becomes checkable — a constructed XRCE provider
*is* one whose whole document has been read. The reader is the provider's own
(`src/internal/xrce_document.{hpp,cpp}`), unshared and dependency-free, exactly
as §4.2 requires; the two in-tree `key=value` readers share this section as their
single tolerance oracle and nothing else. **Those rules, stated here rather
than only pointed at:** entries are separated by `\n`, a trailing `\r` on an entry
is stripped so a CRLF document means the same thing in every build, a blank
entry — a blank line or the trailing newline — is skipped, and nothing else is
trimmed; there is no case folding and no comment syntax; an embedded NUL is
refused up front, for the same provider-level reason the loopback gives below.
Both in-tree `key=value` readers refuse every entry containing a byte below
`0x21` or a `0x7F` (DEL); XRCE states that as one rule up front because it is the
only one of the two with a free-form value (`agent`'s host), where the loopback's
closed key and value sets already refuse the same inputs. So `agent= 127.0.0.1:2018` is
unrepresentable rather than accepted with a space in the host and rejected a
layer down by a resolver Fletcher declines to know anything about (PDA-DEC-7 fix
cycle 1). The rule governs bytes inside an entry and never the separators
between them. A third `key=value` reader is judged against this paragraph, not
against either implementation. `XrceConfig` and its transport enum are
**retired**, not deprecated, with no coexistence window (owner ruling
2026-08-31), and five of its twelve fields are **deleted outright**: a payload
cap nothing read, two serial settings reachable only through a transport that
refuses, and the stream depth and pump quantum — which no caller set and no test
could observe, and which are now fixed constants at their previous values. That
last pair is a disclosed narrowing, recoverable only *with* a witness that it
takes effect.

**As landed** (PDA-DEC-5), the loopback is the other document reader in-tree:
its document is a sequence of `\n`-separated `key=value` entries, and the only
key is `schema_carriage` (`as_declared`, the default, or `carried`, §7 clause
1's schema-before-data mode) — e.g. `document = "schema_carriage=carried"`.
Fletcher copies and forwards those bytes exactly as any other provider's; the
only code that reads them is `in_process_provider.cpp`, unshared and
dependency-free, exactly as §4.2 requires. A trailing `\r` on an entry is
stripped (a document authored on this project's primary platform is CRLF, and
the 2026-09-02 configuration ruling requires the same text to mean the same
thing in every build) and a blank entry — a blank line or a trailing newline —
is skipped; nothing else is trimmed. **This provider** refuses a document containing
an embedded NUL, mirroring `ProviderSelector::Parse` — a provider-level rule about what
its own format can represent, not a seam-level one: the seam's document is
length-authoritative (§4.2) and carries a NUL unchanged. An unrecognised entry (unknown
key, unknown value, no `=`, a duplicate key) is refused with
`kInvalidArgument`, quoting the offending entry, so a misconfigured instance
never exists.

**The seam carries no protocol vocabulary — as landed, not as intended.** The
deepest coupling in the system used to be `FastDDSProviderOptions`, which embedded
`eprosima::fastdds::dds::DataWriterQos`/`DataReaderQos` *plus per-topic maps of
them*, so configuring the transport meant compiling against Fast DDS headers.
PDA-DEC-6 **retired** it — not deprecated it, and no coexistence window was
created (owner ruling 2026-08-31, "XML profile config only — one way to do it").
The check is mechanical rather than editorial: `fastdds-pubsub-provider` links
fast-dds **PRIVATE** and its recipe drops `transitive_headers`, so its
`test_package` compiles with no Fast DDS include directories at all and any
eProsima type reappearing in the installed header is a compile error there.
`XrceConfig` was the same shape of change and **is discharged** (PDA-DEC-7): the
installed XRCE header offers exactly two entry points — `RegisterXrceProvider`
and one constructor over `ProviderConfig` — and names no eProsima or `uxr*`
type at all. (It does of course declare the provider class, its own name and
its `PubSubProvider` overrides; the claim is about vendor vocabulary, not about
declarations. The XRCE client is linked PRIVATE and its headers are private to
the component.)

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
- **The numbers are published once, and this spec cites that copy rather than restating it.**
  The *name and number* of every value sit in the *Error taxonomy (published)* table in
  [core/README.md](../core/README.md), beside the header that defines them, because two
  independent bindings need them in prose; the *meaning* stays normatively owned by
  `status.hpp`'s doc comments. `core_tests`' `Taxonomy.PublishedNumbersMatchTheEnum` reads that
  table off disk and compares it row for row to the enum, and the same test's exhaustive
  `switch` — with the unhandled-enumerator diagnostic promoted to an error on that one source
  file — makes **appending a status a compile failure** until its row is published. A second
  enumeration anywhere, including in this document, would be the drift the guard exists to
  stop. The two numbers spelled out in this section (`kOk = 0` above, "taxonomy entry 4" below)
  are **deliberate cited exceptions**: the rules stated there are *about* those numbers. The
  table is the only enumeration.
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
   carries the ones a publisher declared on that instance, and it can be put in
   schema-carrying mode outright — **as landed** (PDA-DEC-5), via the
   `schema_carriage=carried` document key (§4.1), not a second construction
   API — in which case it upholds schema-before-data by refusal —
   `CreateTopic` requires a schema and publishing to an undeclared topic is
   `kTopicNotDeclared`.
2. **Per-writer order**, holding **across the schema handoff**: the buffered
   pre-schema backlog is delivered before, and never interleaved with, samples
   arriving live afterwards.
3. **Idempotent re-declaration** with an identical schema; conflicting
   re-declaration **must** be rejected.
4. **One callback per topic per instance.** Local fan-out is `Subscriber`'s job.
5. **Late joiners** get the schema asynchronously; `Subscribe` never blocks.
6. **After `Unsubscribe` returns**, no further callback for that topic — **at
   every tier this seam publishes**, not only at `PubSubProvider`. §9 hands
   BIND `Publisher`/`Subscriber`, so a guarantee that binds only the provider
   tier is a guarantee a language binding does not have.

   **What "no further callback" means**, made exact by PDA-DEC-A4 (owner ruling
   2026-09-04): *no invocation of that subscription's callback begins after
   `Unsubscribe` returns, and none is in progress when it does.* `Unsubscribe`
   therefore **blocks** while a delivery for that subscription is in flight, and
   on return the caller may free or unpin whatever the callback was using. That
   is the memory-safety property; the accepted cost is that teardown pauses for
   as long as the slowest handler takes, and a handler that never returns blocks
   it forever — the seam cannot bound foreign callback duration.

   **The one shape where a caller may NOT free callback state on return:** an
   `Unsubscribe` **issued from inside a delivery callback on that subscriber**
   does not wait. A cancellation cannot wait for the frame it is already in, and
   waiting for a *sibling* frame on the same subscriber is exactly what lets two
   handlers hang one another. Such a call still gives the first half — no
   invocation begins afterwards — but not the second. Published here rather than
   implied, because a caller who does not know it has a use-after-free.

   **Unsubscribing something that is not live is a no-op, not an error, at every
   tier** (owner ruling 2026-09-04): an unknown id, a fully cancelled id, an
   unsubscribed topic. A foreign-runtime finaliser cancels unconditionally and
   cannot let an exception escape, so teardown must be safe to call blind. The
   deliberate cost is that a mistyped identifier is ignored rather than reported.
   An id that **another thread is cancelling right now** is not that case: it
   waits for the same drain rather than returning early (owner ruling
   2026-09-04), which is what keeps the exception count at exactly one — the
   race is created by the unconditional-cleanup shape idempotence exists to
   serve, and an exception that only bites under a race is the hardest kind for
   an application author to discover.

   **Scoped to each tier's own machinery.** This clause is a promise about the
   tier the call was made on. It says nothing about what a *provider* does with
   an `Unsubscribe` re-entered from inside its own delivery — that question is
   open and is owned elsewhere, and PDA-DEC-A4 neither answers it nor claims it
   away.

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
| Inherits as its oracle | `integration-tests/pubsub-conformance` — `ProviderConformance` (§7's clauses over every subject), `CopyAccounting` (§8's provenance guard), `SeamVocabulary` (what the crossing types make representable), and the `Registry` cases (§4's selection, including the path-selector pair) | the same four, **plus `CallerTier`** (§7 clause 6 and cancellation idempotence at the `Subscriber` tier BIND actually wraps) |

Neither round re-derives §7's rules or re-invents a copy guard: both check themselves against
that suite, and both are **expected to add cases to it** — the contract *text* is frozen, the
test set is not (§12). §8.1's standing requirement travels with it, restated and not
strengthened: *a live negative control ships with it — a guard nobody has made go red is a
guard nobody has measured.*

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

**A record of what the round moved, not a live measurement of the tree** — and that
distinction is normative here, because §12 freezes this document. Past-tense records of work
that landed are what this section is for; a present-tense count of the current tree carrying no
derivation is not representable here — nor anywhere else in this document, by §12.1's rule — and
the ones that were are deleted rather than dated (PDA-DEC-9).
Excluding `docs/archive/**`:

**External consumers of the protocol-typed config**, which a uniform registry retired in
PDA-DEC-6 and PDA-DEC-7. **No count and no per-file ledger is published here**, and the ones
that stood here are deleted rather than dated: the quantity stopped existing when the migration
finished, because after it `ProviderConfig` is how *everything* is configured, so the recipe
that once bounded the blast radius now matches every configuration site in the tree — including
this spec's own prose and the registry header. What survives is the claim a machine keeps:
**`FastDDSProviderOptions` and `XrceConfig` are declared nowhere and constructed nowhere, and
the compile is the check.** They were retired, not deprecated (owner rulings 2026-08-31 and
2026-09-02), so no coexistence window exists in which a stale ledger could be true.

**Provider-internal churn** (part of the work, not external migration):
`fastdds-pubsub-provider/` — done in PDA-DEC-6. Five QoS tests that set a value
and then only checked that a message arrived were retired and replaced by tests
that read back what the endpoint *announced*; the two `max_schema_bytes` tests
were re-anchored onto the `fletcher.max_schema_bytes` document property; and
`internal/qos_defaults.hpp` left the installed tree for `src/internal/`.
`xrcedds-pubsub-provider/` — done in PDA-DEC-7. Four tests that set a struct
field and read it straight back, or asserted only that *something* derived from
`std::runtime_error` came out, were retired and replaced by tests that compare
the parsed document to the code **whole-struct**, read the published defaults out
of `README.md` on disk, and assert the *status* rather than the exception's base
class. The new `XrceConfig.DocumentConfiguresTransport` observes the document's
address arriving at a test-owned socket on an ephemeral port, with no Agent.

**Docs:** the Fast DDS README's configuration section was rewritten and
[gateway/README.md](../gateway/README.md) now documents `--provider-config`, both
in PDA-DEC-6; the XRCE README's configuration section was rewritten in
PDA-DEC-7, and the default document it publishes is read off disk by a test so it
cannot drift from the code.
[docs/architecture-overview.md](architecture-overview.md) and the root
[README.md](../README.md) were inspected in PDA-DEC-6 **for retired vocabulary only**, and
carried none: the direct `make_shared<FastDDSPubSubProvider>()` construction each shows still
compiles, and PDA-DEC-9 added a registry acquisition beside it in
[architecture-overview.md](architecture-overview.md) §7.4. That inspection did not check
*accuracy*, and PDA-DEC-9 found two claims in them that had stopped being true — a subscriber
receiving its schema as an `OwnedSchema` inside `SubscriptionResult` (the seam has carried a
`SchemaArrival` since PDA-DEC-3), and a new transport arriving by "implementing one interface"
alone (it is also registered under a name and reached through §4's registry). Both are restated
in PDA-DEC-9. The one edit PDA-DEC-6 made was §7.4's include path, wrong since #26 (it named the
header unqualified rather than as it is packaged); PDA-DEC-6 corrected it in
passing because the item is what put a machine check on that include tree.

**Consumers of the vocabulary change, which sit ABOVE the seam.** Giving schema arrival a
C-expressible form (§3.4) was not confined to providers. `SubscriptionResult` carries a
`SchemaArrival` and nothing else; the `shared_future` it used to carry was **retired, not
wrapped** (owner ruling 2026-09-01, *"One mechanism only"*), so PDA-DEC-3 rewrote every caller
that waits for a schema — in `pubsub/`, `pubsub-arrow/`, the gateway, both `test_package`
examples and those two components' suites. Same for any change to `Blob` under §3.2. So
PDA-DEC-3 landed in `core/` and `pubsub/` **and** rippled up through `pubsub-arrow` and the
gateway; the round is not "providers only" in either direction. **No per-site count is
published** — with no coexistence window there is nothing for one to bound, and the compile is
the check.

`InProcessProvider` **moved** out of the gateway executable into a real component and
**became** a registered built-in — the first proof that §4's registry works, and later the body
of PDA-ABI's reference driver. The **lift** landed in PDA-DEC-1
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

---

## §12 — The handoff (what is frozen, who may act, and how each condition was verified)

This section is why the document is `frozen` rather than `proposed`. It exists for two rounds
that start the same day and cannot ask each other anything: **PDA-ABI** below the seam and
**BIND-C# / BIND-Rust** above it.

### §12.1 — What is frozen, and who may act

Two classes of **contract text**. **There is no third**, and no "negotiable" text. (§10 and
§11 are records rather than contract, named at the end of this subsection; a record is not a
third class of contract, and what it may license is bounded there.)

**`frozen`** — §2; **§3 entire** (§3.1, §3.2, §3.3, §3.4 including its outcome table and its
`timeout_ms` rules, §3.5 including the empty-segment refusal); §4 and §4.1's *rules*; §4.2;
§5.1's mapping and refusal rules; §5.3's callback rule; §6; §7's clauses; §8's zero-copy
property. **Anything not named `append-only` below is `frozen`** — including §0, §1, §5.2, §8.1
and §9 — **except the two record sections named at the end of this subsection.** *Who may act:*
**nobody
alone.** §1 governs: *"A later round finding the seam insufficient is a **stop-and-ask**
against *this* spec — not a local workaround, and not a change landed inside an ABI round."*

**`append-only`** — the `PubSubStatus` values (§5.1); `ProviderConfig`'s typed core fields
(§4.1); registered provider names (§4). The class constrains only the **shape** of a change:
an append, with nothing renumbered, reordered, reused or removed. *Who may act:* **making an
append is itself a stop-and-ask, and the owner allocates the number or the field** — so two
parallel rounds cannot both take `PubSubStatus = 10`, and no allocation protocol or reserved
range is needed. A `PubSubStatus` append carries its `core/README.md` row **in the same
change**, which §12.2's condition 3 makes mechanical rather than a request. For the typed core,
§4.1 governs verbatim: *"It is **exactly those two fields** and it is append-only; a later field
never changes `Create`. Widening it because one protocol wants a setting typed is a
stop-and-ask (owner ruling 2026-09-02: 'Fletcher keeps exactly payload size and domain')."*

**Two sections are records, not contract, and are named so that correcting a stale record is
not a stop-and-ask:** **§10** (what the round moved) and **§11** (scope). **The permission is
bounded to *facts*:** a statement of what the tree or the round *is or did* may be corrected to
match the tree, and that is ordinary maintenance. It is **not** a licence over the
*prohibitions* those sections carry — §11's "everything ABI is out of scope" list (no
`extern "C"`, no C header, no `dlopen`, no version negotiation, no driver vtable, no
host-callback struct) and its "any change to the interface's method set (§2)" restate §2, §4
and this round's scope, and they are **`frozen`**: relaxing one is a stop-and-ask, never
maintenance. Correcting a stale fact beside them is not.

**One rule binds every section of this document, §12 included: a count carries its derivation
where it stands, or it is not written.** Three forms of derivation and no fourth: the **command**
that re-derives it, inline; or the **members themselves, named here**, so the reader checks the
number against this document (§12.2's "two of the six are mechanical" names which two); or a
**past-tense record of work that landed** and can no longer be re-measured (§4.1's count of a
retired type's deleted fields). A count that claims something about the **current tree** and
carries none of the three is not written at all, a table of counts a machine cannot re-derive is
deleted rather than dated, and no hand-composed per-site ledger is representable anywhere here —
which is why §10 publishes no total and no ledger.

**Contract text is not the test set.** `frozen` binds the *wording* of §7's clauses. The
conformance suite stays extendable and **both rounds are expected to add cases** — that is what
the 2026-08-31 ruling means by the suite pressure-testing the ABI (§9).

### §12.2 — The six handoff conditions, and how each was actually verified

`mechanical` is used **only where a named machine reddens on a named mutation**. By that bar
**two of the six are mechanical end to end** (3 and 4); condition 2 splits into a mechanical
clause and a read one; 1, 2b and 6 name a reader and a date; and 5 has **no machine check** and
says so. The labels were brought down to the evidence, rather than guards invented to meet
them.

| DoD condition | Artifact | The check | How verified |
|---|---|---|---|
| 1. Every crossing type has a normative, C-expressible ownership rule in the header (§3) | `write_buffer.hpp`, `types.hpp`, `owned_schema.hpp`, `schema_arrival.hpp`, `provider.hpp` | Correctness of the *wording* is read. What the types make **representable** is pinned by the `SeamVocabulary` suite — no view-only `Blob`, zero-size normalisation, empty-segment refusal | **by-reading** — PDA-DEC-3 implementer and its two design reviews, 2026-09-01; re-read at close, 2026-09-03 |
| 2a. No second schema-wait mechanism exists (§3.4) | `schema_arrival.hpp`, and the absence of a `shared_future` member on any seam type | Two mutations, two different answers. **Regressing** to a `shared_future` return stops the tree compiling at every waiting call site: mechanical. **Adding one beside `SchemaArrival`** compiles and reddens nothing — only a reader re-running `grep -rn "shared_future" core pubsub pubsub-arrow gateway *-provider --include=*.hpp --include=*.cpp` notices, and no lane runs that grep. Derive the survivors from that command; no tally is published here | **mechanical (regression only)** and **by-reading (addition)** — 2026-09-03. Forward protection against the addition is §3's place in the `frozen` list, not a machine: the same footing as condition 5 |
| 2b. `SchemaArrival` has a coherent C-expressible form (§3.4) | `schema_arrival.hpp` plus §3.4's outcome table and `fl_status wait(...)` sketch | Read. This is the clause both later rounds derive their own `wait` from, weeks apart, without talking — and an absence grep passes whether or not the form is coherent, so this is not labelled as if it settled it | **by-reading** — PDA-DEC-3 implementer and review, 2026-09-01; re-read at close, 2026-09-03 |
| 3. The exception taxonomy is published and stable (§5.1) | `status.hpp` plus `core/README.md`'s *Error taxonomy (published)* table | Per-enumerator `static_assert`s pin the numbering (`status.hpp:67-83`). `Taxonomy.PublishedNumbersMatchTheEnum` reads the table **off disk** and compares it row for row; its exhaustive `switch`, with the unhandled-enumerator diagnostic promoted to an error on that one source file, makes **appending an enumerator a compile failure**; its one-past-the-last assertion holds the suite red until the README carries the new row; and a published row it cannot parse is a failure rather than a skipped line, so an added row does not escape the comparison. The test holds no count and no copy of the numbers | **mechanical**, and *measured*: each named mutation was applied and reverted at close (append → `error C4062`; the `case` added without the row → red; a name edited on either side → red; a row whose `Number` cell does not parse → red), recorded in the progress log. The promotion also proves itself at configure time — CMake `FATAL_ERROR`s if the flag stops rejecting a default-less non-exhaustive `switch`. **Witnessed under MSVC only** — see §12.4 |
| 4. The registry signature admits a path resolver without change (§4 clause 2) | `provider_registry.hpp` | `static_assert` on the **member-pointer type** of `Create` (`provider_registry.hpp:292-297`) — it sees a defaulted extra parameter or a dropped `const`, which a return-type check cannot; plus `Registry.PathSelectorResolvesThroughTheSameCall` (stand-in resolver) with `Registry.PathSelectorWithoutResolverIsRefusedAsUnsupported` as its live negative control | **mechanical** — the model row |
| 5. Nothing above the seam branches on built-in versus loaded (§0.1(2)) | `provider_registry.hpp`, `gateway/src/main.cpp` | Today the distinction is unrepresentable above the seam: the public registry exposes no accessor that reports it, and the gateway names no concrete provider type in selection. **No machine notices a later round adding one** — the frozen-signature assert pins `Create`'s type alone. What protects it forward is §12.1: §4's registry surface is `frozen`, so such an accessor is a **stop-and-ask**. This is the condition PDA-ABI will pressure hardest, because it is the round that adds loading | **by-construction — no machine check** |
| 6. The spec states what is frozen, and changing it is a stop-and-ask (§1) | §1 plus §12.1 | A wording condition: read. §12.1 makes it checkable *in kind* — every normative element sits in exactly one of two named classes, the default is `frozen`, and each class names who may act | **by-reading** — PM and the two design reviews of this item, 2026-09-03 |

A handoff row claiming "a machine watches this" where none does would be worse for two blind
teams than one naming the reader. That is why labels came down in review rather than guards
going up, and why no new guard was invented at close.

### §12.3 — What the two rounds inherit besides the contract

The oracle suites are named in §9's table. Inherited with them:

- **Guards that are real but have never been made to fail** — protection by reading, not by
  measurement, each named so that neither round mistakes it for evidence: PDA-DEC-6's
  discovery-based QoS guard; PDA-DEC-7's unwitnessed document keys; PDA-DEC-7's
  socket-leak probe; PDA-DEC-1H's unreached refusal arm; PDA-DEC-8's
  unreachable-by-construction pair; PDA-DEC-8's `kOk`-on-null-schema wait.
- **One false green, fixed:** a foreign XRCE Agent left listening on the port could serve a
  whole conformance run and certify it (found by PDA-DEC-7, fixed in PDA-DEC-1H, which now
  proves the harness owns the Agent that answers).
- **One false red, and it costs an hour if you meet it cold:** leaked Fast DDS shared-memory
  segments make `create_participant` fault with `0xC0000005`. Clear the
  `eprosima/fastdds_interprocess` directory before a DDS run.
- **Two blind spots that keep their owners by ruling, and are not reopened here:** the
  receive-side data-sharing defect (**PDA-ABI-7**; owner rulings 2026-08-31 and 2026-09-01,
  starting from PDA-DEC-1's evidence table), and §3.4's unbounded-wait clause, which is pinned
  by no test because a conforming and a non-conforming implementation are indistinguishable on
  the standard library this tree builds against on Windows (§3.4 states it; it was measured
  both ways).
- **The standing policy on blind spots:** *a guard may ship with a recorded blind spot* —
  PDA-DEC-1's owner ruling of 2026-09-01 (*"Ship the guard, hunt elsewhere"*), whose own scope
  was that one item, **carried forward as policy by PDA-DEC-9** so that neither later round
  re-poses a settled question. It is a permission, not a requirement, and it does not relieve
  §8.1's negative-control requirement.

### §12.4 — The evidence behind this document, stated exactly

Owner ruling 2026-09-03, *"The handoff states the platform evidence exactly"*:

**Every green cited in the body of this round was produced by a local run on one Windows machine,
plus one WSL compile of the single platform-forked file (PDA-DEC-1H's `/proc` ownership path).**
No automated build had ever run on `feature/protocol-driver-abi` while the round was executing:
the component and integration lanes are `workflow_call` entries invoked from a
`pull_request`-triggered workflow (`.github/workflows/ci.pr.yml`, its `uses:` entries), so
opening the pull request is the only trigger, and that is the owner's step.

**Amended 2026-09-03, after the round closed, because this section's own ruling obliges it to
state the evidence *exactly* and the evidence changed.** PR #126 ran the lanes for the first
time and is green on all 46 checks. That run found **seven defects this round had introduced and
local running could not reach**, which is the number that matters here more than the green does:
a dependency-trait regression that stripped Fast DDS's include directories from both XRCE
harnesses and broke four lanes while a stale cache hid it on Windows; a gateway `in.bad()` check
that could never fire, because `<< rdbuf()` never touches the stream's state, so an unreadable
path and an empty file were indistinguishable on Linux; and — in this round's own headline
evidence — `XrceConfig.DocumentConfiguresTransport` hanging forever, because
`close(fd)` does not wake a thread already blocked in `accept()` on Linux, in a suite with no
ctest timeout to bound it. Two hours of runner time, after every assertion in the test body had
passed.

So, as instructions rather than as a record:

1. **Both later rounds treat Linux as verified once, not continuously.** The evidence is PR #126
   at `08d1b81`, green on both platforms. It is a single point measurement on a branch that has
   not merged; nothing in this document is backed by a *standing* Linux signal, and re-running it
   is a lane away.
2. **Prefer the first lane run to any amount of local green.** On the evidence above, the first
   automated run on a branch is worth more than every local run preceding it — six of the seven
   defects were invisible to a Windows box, and the seventh was invisible because the check
   meant to catch it was itself broken.
3. **A Linux-only difference in seam behaviour is a question for the owner — a stop-and-ask
   against this spec, not a local fix.** A local fix by one round would silently change the
   seam both rounds share, which is exactly the divergence §1 exists to prevent. This clause is
   **unchanged by the amendment** and is the one that mattered: three of the seven defects were
   precisely Linux-only differences, and each was fixed centrally rather than worked around.

This is the fourth consecutive time the owner chose a narrow claim stated honestly over a wide
one implied (2026-09-01 copy-accounting scope, 2026-09-01 conformance blind spot, 2026-09-03
isolation scope, and this).
