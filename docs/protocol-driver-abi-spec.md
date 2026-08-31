# Protocol Driver ABI — Specification (oracle)

Status: **proposed** (round **PDA**). This is the authoritative spec for the
protocol-driver ABI. On any contradiction with the plan or a per-item design,
**this document wins**. Locked-decision digest:
[plans/PDA-locked-decisions.md](../plans/PDA-locked-decisions.md). Plan + tracker:
[plans/PDA-protocol-driver-abi.md](../plans/PDA-protocol-driver-abi.md).

---

## §0 — Context & motivation

Fletcher already decouples encoding from transport. `fletcher::PubSubProvider`
([pubsub/include/fletcher/pubsub/provider.hpp](../pubsub/include/fletcher/pubsub/provider.hpp))
is a four-method abstract interface — `CreateTopic`, `Publish`, `Subscribe`,
`Unsubscribe` — and three implementations exist: Fast DDS, XRCE-DDS, and an
`InProcessProvider` that currently lives **inside**
[gateway/src/main.cpp](../gateway/src/main.cpp) rather than in a library.

The interface is honest, but it is a **C++** interface, and that has three
consequences this round exists to remove:

1. **Protocol choice is a compile-time decision.** The gateway hardcodes
   `if (args.provider == "fastdds")` at [gateway/src/main.cpp:183](../gateway/src/main.cpp#L183)
   and must link every provider it might select. Adding MQTT or Zenoh means
   rebuilding Fletcher.
2. **Protocol configuration leaks protocol vocabulary into consumers.**
   `FastDDSProviderOptions` embeds `eprosima::fastdds::dds::DataWriterQos` and
   `DataReaderQos` *plus per-topic maps of them*. Configuring the transport
   therefore requires compiling against Fast DDS headers. This is the deepest
   coupling in the system. `XrceConfig`, by contrast, is entirely POD (ints,
   strings, one enum) — the two shipping providers sit at opposite extremes,
   which usefully brackets the design.
3. **A third party cannot ship a protocol.** Any new transport must be built
   inside the Fletcher tree, with Fletcher's compiler and standard library.

**Goal.** Define a **pure C ABI** below `PubSubProvider` so that a protocol
becomes a *driver*: selected and configured **at runtime**, implementable by
anyone in any language, without rebuilding Fletcher.

### §0.1 — The two end-user requirements

- **(a)** The protocol driver is chosen at **runtime**, not compile time.
- **(b)** Protocol-specific setup is supplied at **runtime**, at the driver level.

Both must be satisfiable from a config file, with no Fletcher rebuild and no
consumer compiling against a protocol SDK.

### §0.2 — What is *not* changing

The ABI goes **below** `PubSubProvider`, not in place of it (**D-PDA-1**). A
`DriverProvider : PubSubProvider` adapter wraps a loaded driver. Therefore
`Publisher`/`Subscriber`, `PublisherArrow`/`SubscriberArrow`, all generated code,
and the codec are **untouched**, and the C++ interface remains a supported way to
write an in-tree provider. Retiring it later becomes optional rather than a
prerequisite for this round.

---

## §1 — Scope

**In scope.** The C header and its contract; host-side loading (explicit path)
and static registration; the module/instance handle split; the `DriverProvider`
adapter; conformance and copy-accounting suites; ports of all three providers to
drivers; the Fast DDS configuration change; multi-instance support; zero-copy
loaned-sample receive.

**Out of scope.** A protocol *bridge* component (routing between two drivers) —
§7 requires the ABI make one trivial, but no bridge is built this round. Also
out: the wire format (unchanged), the codec, generated code, the gateway's
WebSocket protocol, and the language *binding* ABI (BIND-C#/BIND-Rust — see §9).

---

## §2 — Linkage: one contract, two registration paths (L1)

There is exactly **one** driver entry-point contract. How the driver's code
arrives in the process has two supported forms:

| Form | Target | Mechanism |
|---|---|---|
| **Dynamic** | desktop, server | host loads a shared object / DLL from an explicit path and resolves the entry point |
| **Static** | MCU, embedded | driver's entry point is registered in a link-time table; no loader involved |

A driver's source is **identical** in both forms. This is what keeps XRCE-DDS —
whose reason for existing is MCU targets (TD-004 rationale, TD-007 context:
`<75 KB` Flash) — portable to a driver at all, since `dlopen` is a non-starter
there.

The single entry point:

```c
/* The one symbol a dynamic driver must export, and the one function a static
   driver registers. Returns NULL if the driver cannot serve the requested
   ABI version. */
const fletcher_driver_v1* fletcher_driver_entry(uint32_t abi_version);
```

### §2.1 — Discovery (L7)

Discovery is **explicit path from configuration only**. Fletcher does not scan
directories, read a plugin manifest, or consult an environment variable. This is
deliberate: loading executable code from a searched directory is an attack
surface, and the round does not need it to satisfy §0.1. A future search-path
mechanism must be an additive, separately designed feature.

---

## §3 — The fat-handle primitive (L4)

**Zero-copy is required for rows *and* attachments.** Combined with third-party
drivers, that forbids Fletcher from owning the allocation: memory may belong to
the driver, to the transport, or to the publishing application. Blobs therefore
cross the boundary as **self-describing handles carrying their own lifetime
operations**:

```c
typedef struct {
    void*          ctx;      /* opaque owner token */
    const uint8_t* data;     /* valid while a retain is held */
    size_t         len;
    void (*retain)(void* ctx);
    void (*release)(void* ctx);
} fletcher_blob;
```

### §3.1 — Ownership rules (normative)

1. A `fletcher_blob` passed as a **function argument** is borrowed for the
   duration of the call. The callee must call `retain` if it keeps it longer.
2. Every `retain` is matched by exactly one `release`. `data` may be dereferenced
   only while the caller holds at least one retain.
3. `retain`/`release` must be callable from **any thread** and must be safe to
   call concurrently for the same `ctx`.
4. `release` is the only teardown hook. It must not throw, must not re-enter the
   ABI, and must tolerate being the last release (that is when it frees).
5. `data` may be `NULL` **iff** `len == 0`. `retain`/`release` must be non-NULL
   even for an empty blob (a no-op pair is acceptable).
6. Once handed across the boundary, the underlying bytes are **immutable**.

### §3.2 — Why this is the right primitive, not a compromise

- **It bridges today's `Blob` with no copy in either direction.** `Blob` is
  `shared_ptr<const vector<uint8_t>>` ([core/include/fletcher/core/types.hpp](../core/include/fletcher/core/types.hpp)).
  C++ → C: `ctx` holds a heap-allocated `shared_ptr` copy; `retain`/`release`
  are copy/destroy of it. C → C++: a `shared_ptr` with a custom deleter that
  calls `release`. So the existing C++ providers and the new ABI interoperate
  zero-copy — which is what makes the adapter in §0.2 viable rather than a
  copying shim.
- **It expresses zero-copy receive that today's interface cannot.** Fast DDS can
  hand out transport-owned memory that must be returned via `return_loan`.
  `release` *is* that hook. Today's `Blob` forces a copy into a `std::vector` on
  every receive. The ABI is therefore **more** zero-copy-capable than the C++
  interface it wraps (see §8.2 and PDA-10).

### §3.3 — Schemas

`ArrowSchema` is already the Arrow C Data Interface — a stable C ABI with its own
release callback — so schema *content* crosses for free and **no Fletcher schema
format is invented**. But the C Data Interface release is **unique** ownership,
while `SharedSchema` is `shared_ptr<const ArrowSchema>` and is explicitly
documented as storable by callbacks across threads
([pubsub/include/fletcher/pubsub/owned_schema.hpp](../pubsub/include/fletcher/pubsub/owned_schema.hpp)).
Sharing therefore needs the same fat-handle treatment:

```c
typedef struct {
    void* ctx;
    const struct ArrowSchema* schema;   /* Arrow C Data Interface, verbatim */
    void (*retain)(void* ctx);
    void (*release)(void* ctx);
} fletcher_schema;
```

§3.1's rules apply unchanged. `create_topic` transfers ownership *into* the
driver (matching today's by-value `OwnedSchema` parameter); delivery to a
subscriber callback **borrows**, and the host retains if it stores.

---

## §4 — The write buffer: a pass-through vtable (L4, L5)

`PubSubProvider::Publish` is **inverted**: the *driver* supplies the buffer and
Fletcher encodes into it, via `RowEncoder = std::function<void(WriteBuffer&)>`.
That inversion is the entire zero-copy encode path — `FixedWriteBuffer` exists
precisely to wrap a DDS payload
([core/include/fletcher/core/write_buffer.hpp](../core/include/fletcher/core/write_buffer.hpp)).

`WriteBuffer` is a **window plus a refill hook**, not a set of per-operation
virtuals: `Append`/`AppendByte`/`AppendZeros`/`Position`/`PatchU32`/`PatchByte`
are non-virtual and write straight into `{data_, capacity_, pos_}`, and only
running out of room reaches the two virtuals, `AppendSlow`/`AppendZerosSlow`.
The ABI mirrors that shape, which is what makes it cheap — **one crossing per
window refill, not one per append**:

```c
typedef struct {
    void*    ctx;
    uint8_t* data;      /* the window; Fletcher writes into it directly */
    size_t   capacity;  /* bytes available at `data` */
    size_t   pos;       /* write cursor, also the row's length so far */

    /* Called only when the window is full. Must append the bytes anyway —
       updating data/capacity/pos to a window that has room — or report
       failure through the error channel (§6). `grow_zeros` appends `len`
       ZERO bytes (null-bitfield placeholders). Neither may throw or
       longjmp across the boundary. */
    fletcher_status (*grow)(struct fletcher_write_buffer* self,
                            const uint8_t* data, size_t len);
    fletcher_status (*grow_zeros)(struct fletcher_write_buffer* self, size_t len);
} fletcher_write_buffer;
```

Rules (normative), each inherited from the C++ semantics a driver must not
reinterpret:

1. **Patching is done by Fletcher, in the window** — `patch_u32` overwrites four
   bytes at an earlier offset (length prefixes) and `patch_byte` **ORs** bits
   into one (null bitfields). A driver therefore may not move or reallocate
   bytes already written **below `pos`** except inside `grow`, and `grow` must
   preserve them verbatim.
2. **Bounds are computed by subtraction, never addition** — `len > capacity - pos`,
   so an attacker-supplied length cannot wrap.
3. A fixed-capacity driver (a DDS payload of a bounded type) implements both
   hooks as an overflow failure; a growable one refills the window.

### §4.1 — Consequence for driver authors (normative, must be documented)

The window is a **random-access writable region, not a stream**: Fletcher
back-patches length prefixes and null bitfields at offsets below `pos`. A naive
socket-stream driver cannot supply that without internal buffering. This is
inherent to the positional wire format (TD-002), so it is a real requirement on
driver authors, not an accident of the design — and it is the reason `grow` must
carry the already-written bytes across a refill rather than flushing them.

### §4.2 — Why the struct must be shared with the binding ABI

For a Rust or C# client publishing a row, Fletcher passes the **driver's** buffer
handle through to the binding *unchanged*, so the caller's writer appends
directly into transport memory: zero copies across two ABI hops, with Fletcher a
pure conduit. That only works if both ABIs use this **identical struct**. This is
the concrete content of "shared vocabulary" in §9 — and the reason the two ABIs
cannot be designed independently even though their function sets are disjoint.

---

## §5 — Configuration: typed core + opaque document (L2)

Configuration splits in two:

```c
typedef struct {
    uint32_t    max_payload_bytes;  /* bounds the full serialised envelope */
    uint32_t    domain_id;          /* protocol-agnostic endpoint identity */
    const char* config_document;    /* driver-defined format; may be NULL */
    size_t      config_document_len;
} fletcher_provider_config;
```

- **The typed core** is the small set Fletcher genuinely needs to reason about.
  Both existing providers already have exactly these two fields
  (`FastDDSProviderOptions::{domain_id, max_payload_bytes}`,
  `XrceConfig::{domain_id, max_payload}`), so the core is derived from evidence,
  not invented.
- **The opaque document** is everything else. Only the driver parses it.

### §5.1 — Document format is driver-defined (normative)

L2 fixes one config *mechanism*; it does **not** mandate a uniform format. Each
driver declares its own:

| Driver | Format | Parsing cost |
|---|---|---|
| Fast DDS | its native **XML QoS profile** | zero new code — Fast DDS already loads XML profiles |
| XRCE-DDS | `key=value` lines | a few hundred bytes |
| Zenoh (future) | JSON5, its native format | zero new code |

This is what dissolves the apparent L1/L2 conflict: forcing a JSON parser onto a
`<75 KB` Flash target would be a footprint problem, and driver-defined formats
mean no driver carries a parser it cannot afford.

### §5.2 — Fletcher parses nothing (explicit non-goal)

**Fletcher gains no configuration parser and no configuration dependency.** The
document is transported as bytes and handed to the driver. Adding a JSON or YAML
dependency "for convenience" would quietly re-couple Fletcher to config
semantics it must not know, and is a **stop-and-ask**.

### §5.3 — Consequence for the Fast DDS provider

Fast DDS QoS moves from `FastDDSProviderOptions` (eProsima types in a public
header) to an XML profile named by the config document. Per-topic QoS overrides,
today `unordered_map<string, DataWriterQos>`, become per-topic profile names —
which is Fast DDS's own native idiom. The typed options struct is **retired**
(L9; blast radius in §10).

---

## §6 — Error channel and versioning (L3)

Exceptions must not cross a C boundary, and today's providers signal failure by
throwing. Every ABI function therefore returns a status code:

```c
typedef enum {
    FLETCHER_OK = 0,
    FLETCHER_ERR_INVALID_ARGUMENT,
    FLETCHER_ERR_UNSUPPORTED,
    FLETCHER_ERR_CONFIG,          /* driver rejected the config document */
    FLETCHER_ERR_SCHEMA_CONFLICT, /* re-declaration with a different schema */
    FLETCHER_ERR_NO_SUCH_TOPIC,
    FLETCHER_ERR_TRANSPORT,
    FLETCHER_ERR_OVERFLOW,        /* payload exceeds max_payload_bytes */
    FLETCHER_ERR_INTERNAL,
} fletcher_status;
```

Rules (normative):

1. **No exception may propagate across the boundary, in either direction.** A
   driver must catch everything at its entry points; the host must catch
   everything before returning into a driver.
2. A non-`OK` status may be accompanied by a human-readable message retrievable
   from the **instance** that produced it — never a global or thread-local
   `errno`-style slot, which would not work with multiple instances per process
   (§7).
3. The adapter translates status → exception on the C++ side, so
   `PubSubProvider`'s existing throwing contract is preserved for consumers.

### §6.1 — Version negotiation

`fletcher_driver_entry(abi_version)` returns `NULL` for an unsupported version.
The vtable's first field is its own version, and the struct is **append-only**
within a major version: fields are added at the end and never reordered,
resized, or repurposed. The host must check the version before reading any field
added after v1.

The ABI is **public and versioned** but carries **no compatibility promise before
1.0**, so its shape can be corrected once a real third-party driver exists. That
exemption must be stated in the header itself, together with a deprecation
policy, or it becomes permanent by default.

---

## §7 — Handles: module and instance (L6)

Two handle levels, always passed explicitly:

- a **module** handle — one per opened library or static registration;
- an **instance** handle — one per `(module, config)` pair, **N per module**.

There is **no** `fletcher_init()` and no global registry state. Every entry point
takes an explicit handle.

The load-bearing case is not two different protocols; it is **two instances of the
same driver with different configs** — two DDS domains in one process. That is
needed even with a single protocol, it is the primitive a bridge would later
compose, and it is nearly free now and expensive to retrofit.

### §7.1 — Threading (normative)

Carrying over `PubSubProvider`'s existing semantics and making the implicit parts
explicit:

1. Delivery is **serialized per subscription** (implied by per-writer order, §8).
2. Delivery **may run concurrently across different subscriptions**, including
   across instances.
3. **Host callbacks must be assumed callable from any thread**, including a
   driver-owned transport thread.
4. `retain`/`release` on both fat handles are thread-safe (§3.1).
5. **Destruction requires quiescence.** Destroying an instance requires that no
   thread is executing or about to enter an ABI call on it, and no driver
   callback into the host is still in flight. This mirrors the precondition
   already documented on both concrete providers' destructors; destruction is
   **not** a synchronization boundary.

---

## §8 — The delivery contract (the part a public ABI must enforce)

Today "schema before data" and "per-writer order" are **prose** in
`provider.hpp`, honoured by two in-tree providers under our own review. Publish
this ABI and a third-party driver will get them wrong — and the failure mode is
**silent wrong-slot decoding at the subscriber, not a crash**, because the
positional format carries no per-field metadata that could catch a mismatch
(TD-002 risks).

The contract, stated normatively:

1. **Schema before data.** A subscriber callback is never invoked with a null
   schema. A subscriber may subscribe before any publisher exists; the driver
   **buffers** data arriving ahead of the schema and delivers it only once the
   schema is known.
2. **Per-writer order.** Samples from one writer reach the callback in publish
   order. This holds **across the schema handoff**: the buffered pre-schema
   backlog is delivered before — and never interleaved with — samples arriving
   live afterwards.
3. **Idempotent re-declaration.** `create_topic` with an *identical* schema
   succeeds, so several publishers may share a topic.
4. **Conflicting re-declaration** may be rejected with
   `FLETCHER_ERR_SCHEMA_CONFLICT`.
5. **One callback per topic per instance.** Local fan-out is `Subscriber`'s job,
   not the driver's.
6. **Late-joining subscribers** receive the schema asynchronously; the schema
   future resolves non-null, and `subscribe` never blocks to obtain it.
7. **After `unsubscribe` returns**, no further callback for that topic is
   delivered (subject to §7.1's quiescence precondition).

### §8.1 — The contract must be executable, not prose (D-PDA-2)

A public ABI whose semantics live only in comments is a documented interface, not
a contract. §8's clauses are therefore encoded as a **conformance suite** that a
driver author runs. The suite is written **first, against the existing C++
`PubSubProvider`**, and run against all three providers — turning today's prose
into assertions and (expectedly) revealing divergences between three
implementations that have never been mechanically compared. It is then
**retargeted through the ABI** unchanged, which is the ABI-equivalence proof.

Anything the suite cannot express is evidence the ABI is underspecified.

### §8.2 — Zero-copy must be falsifiable (D-PDA-3)

"Zero-copy" (L4) is unverifiable by inspection and will regress silently. A
**copy-accounting oracle** — an instrumented blob and buffer that count copies —
makes L4 a test rather than an aspiration. It lands with the conformance suite,
before the ABI.

---

## §9 — Relationship to the binding ABI (BIND-C#/BIND-Rust) (L5)

BIND-C# and BIND-Rust are queued and each needs a C boundary over Fletcher. They
do **not** consume this one, because the two ABIs point in opposite directions:

```
  DRIVER ABI                            BINDING ABI
  Fletcher is the CALLER                Fletcher is the CALLEE
  driver IMPLEMENTS                     application CALLS

  Rust/C# app ──► binding ABI ──► Fletcher C++ ──► driver ABI ──► Fast DDS
                                  (Publisher/       (DriverProvider
                                   Subscriber)        adapter)
```

A Rust app publishing a row goes `fletcher_publisher_publish(...)` → Fletcher's
C++ `Publisher` → the adapter → the driver's `publish`. **No driver data-plane
function is callable by a binding, and none should be.**

### §9.1 — Three roles, not two

Requirements §0.1(a) and (b) *are* binding-visible: a C# app must be able to pick
and configure a driver at runtime. Those are **host** functions, not driver
functions. The header therefore has three roles:

| Role | Implemented by | Called by |
|---|---|---|
| 1. Driver vtable — `create_topic`, `publish`, `subscribe`, `unsubscribe` | driver | Fletcher only |
| 2. Host callbacks — buffer vtable, sample delivery, error reporting | Fletcher | driver |
| 3. Driver management — load, instance create/destroy, config, last error | Fletcher | **applications, incl. bindings** |

Bindings need **only role 3** — the same verbs a C++ application uses.

A **Rust or C# _driver_ is entirely legitimate**: it implements role 1. That is a
different artifact from a Rust *application binding*, which calls role 3. Same
language, opposite directions — worth naming explicitly, because the two get
conflated.

### §9.2 — Where the ABIs genuinely couple: types, not calls

Four items. If they diverge, the cost is paid in copies:

1. **Buffer representation** (§4) — share it and a Rust publish composes with
   zero copies; use two and you `memcpy` at the junction, losing exactly the
   property `WriteBuffer` exists to provide.
2. **Blob lifetime** (§3) — the receive path for a Rust client hits the
   retain/release protocol at *both* hops.
3. **`ArrowSchema`** — free; both use the C Data Interface verbatim (§3.3).
4. **Error and version conventions** (§6) — two idioms in one header is a wart;
   two version-negotiation schemes is worse.

**Therefore:** share the *type, ownership, error and version vocabulary*; keep the
*function sets* separate and directional. This round defines the vocabulary plus
the driver direction; BIND reuses the vocabulary for the app direction. **The
roadmap does not need reordering.**

---

## §10 — Migration and blast radius

Measured, not estimated (excluding `docs/archive/**`, which is history):

**External consumers of the retired typed options** — 4 files, 19 occurrences:

| Site | Occurrences |
|---|---|
| [integration-tests/fastdds-xrce-interop/tests/test_interop.cpp](../integration-tests/fastdds-xrce-interop/tests/test_interop.cpp) | 9 |
| [integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp](../integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp) | 8 |
| [gateway/src/main.cpp](../gateway/src/main.cpp) | 1 |
| [integration-tests/gateway-fastdds-ts/src/fastdds_peer.cpp](../integration-tests/gateway-fastdds-ts/src/fastdds_peer.cpp) | 1 |

**Provider-internal churn** (part of the port itself, not external migration):
`fastdds-pubsub-provider/` 39 occurrences across 7 files — of which
[tests/test_fast_dds_pubsub_provider.cpp](../fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp)
alone has 24, so the QoS-configuration tests are substantially rewritten against
XML profiles; `xrcedds-pubsub-provider/` 10 across 4 files.

**Docs:** [fastdds-pubsub-provider/README.md](../fastdds-pubsub-provider/README.md)
(7), [xrcedds-pubsub-provider/README.md](../xrcedds-pubsub-provider/README.md)
(2), plus the transport-agnostic claims in
[docs/architecture-overview.md](architecture-overview.md) and the root
[README.md](../README.md), which both describe adding a transport as
"implementing one interface" and must be restated in driver terms.

### §10.1 — `InProcessProvider` is promoted

`InProcessProvider` is lifted out of [gateway/src/main.cpp:72](../gateway/src/main.cpp#L72)
into a real component. It becomes the reference driver and the conformance-suite
vehicle: the artifact that proves the ABI is implementable and that a driver
author reads first.

---

## §11 — Recorded intent: TD reconciliation, footprint, licensing

Three things must be recorded explicitly rather than decided by omission.

### §11.1 — The existing TD stance on dynamic linking

TD-007's "alternatives considered" rejects *"dynamic linking with optional Arrow
C++ (fragile, hard to test, platform-dependent)"*. That is a narrower stance than
it first appears: it rejected dynamic linking as a way to make **Arrow C++**
optional, in the context of the edge/server tier split — not plugin ABIs in
general. This round is nonetheless the first deliberate use of dynamic loading in
Fletcher, so a new TD entry must state the difference and answer the three
objections on the record: **fragile** (answered by the version negotiation of
§6.1), **hard to test** (answered by the conformance suite of §8.1 and by the
static registration path, which exercises the ABI with no loader at all), and
**platform-dependent** (answered by confining platform code to the host's loader,
with §2's identical-source guarantee).

The MCU `<75 KB` Flash figure cited throughout comes from **TD-004** (rationale)
and **TD-007** (context) — *not* TD-005, which is schema transport via companion
topics.

### §11.2 — Footprint is an unguarded aspiration

CI builds and tests XRCE-DDS on **ubuntu-latest and windows-2022 only**. There is
**no MCU lane**, so `<75 KB` is a design aspiration nothing verifies — the
constraint is real but unguarded, and a regression would ship unnoticed. A
**desktop link-size budget test** is required as a proxy: not a real MCU build,
but it fails when someone adds a heavyweight dependency to the XRCE driver.

### §11.3 — Licensing

Fletcher is LGPL-3.0. A dynamically loaded third-party driver makes the ABI a
**licensing** boundary as well as a technical one, and the combination chosen here
(public + third-party-implementable, L3) is what makes proprietary third-party
drivers practically possible. Whether that is the goal or a thing to constrain
must be **recorded as explicit intent before the ABI is published**, not decided
afterwards by silence.

---

## §12 — Out of scope

- A protocol **bridge** component (DDS↔MQTT routing). §7 makes it trivial to
  build later; building it is a different round.
- The **binding** ABI function sets (BIND-C#/BIND-Rust) — §9 fixes the shared
  vocabulary only.
- **Driver discovery by search path or manifest** (L7 — explicit path only).
- Any **wire format** change; any **codec** or generated-code change.
- Retiring the C++ `PubSubProvider` interface (§0.2 — the ABI is additive).
- A Fletcher-side **config parser** (§5.2 — an explicit non-goal).
- A real **MCU CI lane** (§11.2 ships a desktop proxy instead).
