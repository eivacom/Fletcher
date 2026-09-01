# Protocol Driver ABI — Specification (oracle)

Status: **proposed** (round **PDA-ABI**, token `ABI`). This is the authoritative
spec for the pure-C protocol driver ABI. On any contradiction with the plan or a
per-item design, **this document wins** — *except* where it concerns the seam,
where [docs/pubsub-interface-spec.md](pubsub-interface-spec.md) wins over this
document. Locked-decision digest:
[plans/PDA-ABI-locked-decisions.md](../plans/PDA-ABI-locked-decisions.md).
Plan + tracker: [plans/PDA-ABI-protocol-driver-abi.md](../plans/PDA-ABI-protocol-driver-abi.md).

---

## §0 — Position: strictly below the seam

Round **PDA-decouple** left `fletcher::PubSubProvider` a specified seam: every
crossing type has a normative C-expressible ownership rule, schema arrival has a
C-expressible form, the exception taxonomy is published, and a registry selects a
provider by **name or path** with built-in versus loaded invisible to callers
([docs/pubsub-interface-spec.md](pubsub-interface-spec.md)).

This round builds the C boundary **below** that seam, so a protocol becomes a
**driver**: implementable by anyone in any language, in a separate binary, and
selected at runtime without rebuilding Fletcher.

```
   application (C++, or via a language binding)
        │
        ▼
   Publisher / Subscriber ──► PubSubProvider ◄─── THE SEAM (decouple's spec)
                                   ▲
                    ┌──────────────┴──────────────┐
              built-in provider            DriverProvider adapter
              (registered)                        │
                                                  ▼
                                        ══ THIS SPEC: the C ABI ══
                                                  │
                                            a driver .so/.dll
                                            or a static registration
```

### §0.1 — What this round inherits and must not redefine

From the seam spec, **by reference, not by restatement**:

| Concern | Where it is defined |
|---|---|
| `WriteBuffer` window + refill semantics, patching rules | seam §3.1 |
| `Blob`/`Attachments` shared-ownership contract | seam §3.2 |
| `SharedSchema`, `ArrowSchema` verbatim | seam §3.3 |
| Schema arrival's C-expressible form | seam §3.4 |
| Topic segments | seam §3.5 |
| Provider **selection** and configuration | seam §4 |
| Exception taxonomy → status mapping | seam §5 |
| Threading and destruction quiescence | seam §6 |
| The delivery contract | seam §7 |
| Zero-copy as a property to preserve | seam §8 |

The driver ABI's C types are **views of those C++ types**. They are derived from
the seam, not from the language-binding ABI, which this round neither consumes nor
provides for (seam §1). **If the seam turns out insufficient, that is a
stop-and-ask against the seam spec** — not a local workaround here.

### §0.2 — Two roles, not three

The driver ABI's surface is exactly:

| Role | Implemented by | Called by |
|---|---|---|
| 1. Driver vtable — `create_topic`, `publish`, `subscribe`, `unsubscribe` | the driver | Fletcher only |
| 2. Host callbacks — the write-buffer window, sample delivery, schema arrival, error reporting | Fletcher | the driver |

**Provider selection is not part of this ABI.** Choosing and configuring a
provider at runtime is caller-visible — an application, including one reaching
Fletcher through a language binding, must be able to do it — so it belongs to the
seam (seam §4) and therefore to the *binding* surface. A driver never implements
selection. This round contributes only a **path resolver** to the registry the
seam already defines.

A **driver written in Rust or C#** implements role 1 and is entirely legitimate.
It is a different artifact from a Rust or C# *application binding*, which calls
the seam. Same language, opposite directions.

---

## §1 — Scope

**In.** The C header for roles 1 and 2; version negotiation and the compatibility
policy; dynamic loading from an explicit path; the static-registration lane;
module/instance handles; the `DriverProvider` adapter and its registry resolver;
retargeting the seam's conformance suite through the ABI; porting all three
providers to drivers; zero-copy receive; the footprint proxy; the driver-author
guide and TD entries.

**Out.** Anything the seam owns (§0.1). Also out: the language-binding ABI, a
protocol bridge, the wire format, the codec, generated code, the gateway's
WebSocket protocol, and driver discovery by search path or manifest.

---

## §2 — Linkage: one contract, two registration paths

Exactly **one** driver entry-point contract. How the driver's code arrives:

| Form | Target | Mechanism |
|---|---|---|
| **Dynamic** | desktop, server | host loads a shared object / DLL from an explicit path and resolves the entry point |
| **Static** | MCU, embedded | the driver's entry point is registered in a link-time table; no loader |

**A driver's source is identical in both forms.** This is what keeps XRCE-DDS —
which exists *for* MCU targets (`<75 KB` Flash; TD-004 rationale, TD-007 context)
— portable to a driver at all, since `dlopen` is a non-starter there. It is also
how the ABI gets tested with no loader involved.

```c
/* The one symbol a dynamic driver exports, and the one function a static driver
   registers. Returns NULL if the driver cannot serve the requested version. */
const fletcher_driver_v1* fletcher_driver_entry(uint32_t abi_version);
```

### §2.1 — Discovery: explicit path only

Fletcher does not scan directories, read a plugin manifest, or consult an
environment variable. Loading executable code from a searched directory is an
attack surface this round does not need: a **name** already resolves through the
seam's registry of built-ins and static registrations, and a **path** is given
explicitly in configuration. A search path is a later, separately designed,
additive feature — adding one here is a stop-and-ask.

### §2.2 — The adapter is a resolver, not a second API

`DriverProvider : PubSubProvider` wraps a loaded driver, and is registered as the
resolver for **path** selectors in the seam's registry (seam §4 clause 2). So:

- `Publisher`/`Subscriber`, generated code and the codec are untouched;
- a caller cannot tell a driver-backed provider from a built-in one;
- the C++ interface remains a supported way to write an in-tree provider, and
  retiring it is **not** in this round.

The adapter must bridge the seam's shared-ownership types to the ABI's handles
**without copying** in either direction (seam §3.2, §8). If it cannot, the
adapter approach is not viable and that is a stop-and-ask.

---

## §3 — The C types

Derived from the seam's vocabulary (§0.1). Two shapes carry the seam's shared
ownership into C, both **self-describing handles carrying their own lifetime
operations**, because memory may belong to the driver, the transport, or the
publishing application — never necessarily to Fletcher:

```c
typedef struct {
    void*          ctx;      /* opaque owner token */
    const uint8_t* data;     /* valid while a reference is held; NULL iff len == 0 */
    size_t         len;
    void (*retain)(void* ctx);
    void (*release)(void* ctx);
} fletcher_blob;

typedef struct {
    void* ctx;
    const struct ArrowSchema* schema;   /* Arrow C Data Interface, verbatim */
    void (*retain)(void* ctx);
    void (*release)(void* ctx);
} fletcher_schema;
```

Ownership follows seam §3.2 exactly — borrowed for the call, retained to keep,
one release per retain, thread-safe, release never throws or re-enters. `retain`
and `release` must be non-NULL even for an empty blob (a no-op pair is fine).

Why this shape rather than a Fletcher allocation: it bridges the seam's
`shared_ptr` with **no copy in either direction** (`ctx` holds a heap `shared_ptr`
one way; a custom-deleter `shared_ptr` calling `release` the other), and `release`
is exactly the hook a transport's `return_loan` needs — which is what makes
zero-copy receive (§6) expressible at all.

The write buffer mirrors the seam's window-plus-refill shape (seam §3.1) — the
driver supplies the window, and only a refill crosses the boundary, so the cost is
**one crossing per refill, not one per append**:

```c
typedef struct fletcher_write_buffer {
    void*    ctx;
    uint8_t* data;      /* the window; Fletcher writes into it directly */
    size_t   capacity;
    size_t   pos;       /* write cursor, and the row's length so far */

    /* Called only when the window is full: append the bytes anyway, updating
       data/capacity/pos to a window with room, or fail. `grow_zeros` appends
       `len` ZERO bytes. Neither may throw or longjmp. */
    fletcher_status (*grow)(struct fletcher_write_buffer* self,
                            const uint8_t* data, size_t len);
    fletcher_status (*grow_zeros)(struct fletcher_write_buffer* self, size_t len);
} fletcher_write_buffer;
```

Bytes already written below `pos` must not move except inside `grow`, which must
preserve them verbatim — Fletcher back-patches there (seam §3.1). A driver over a
bounded DDS payload implements both hooks as an overflow failure.

---

## §4 — Errors and versioning

### §4.1 — Status codes

Exceptions must not cross, in either direction: a driver catches everything at its
entry points, and the host catches everything before returning into a driver. The
mapping from the seam's exception taxonomy (seam §5.1) to these codes is
normative and must be exhaustive — an unmapped exception type is a defect, not a
`FLETCHER_ERR_INTERNAL`.

```c
typedef enum {
    FLETCHER_OK = 0,
    FLETCHER_ERR_INVALID_ARGUMENT,
    FLETCHER_ERR_UNSUPPORTED,
    FLETCHER_ERR_CONFIG,           /* driver rejected the config document */
    FLETCHER_ERR_SCHEMA_CONFLICT,  /* re-declaration with a different schema */
    FLETCHER_ERR_NO_SUCH_TOPIC,
    FLETCHER_ERR_TRANSPORT,
    FLETCHER_ERR_OVERFLOW,         /* row exceeded the payload bound */
    FLETCHER_ERR_INTERNAL,
} fletcher_status;
```

A non-`OK` status may carry a human-readable message, retrievable from the
**instance** that produced it — never a global or thread-local `errno`-style slot,
which would not survive multiple instances per process (§5). The adapter turns
status back into the seam's exceptions, so `PubSubProvider`'s throwing contract is
preserved for callers.

### §4.2 — Negotiation and compatibility

`fletcher_driver_entry(abi_version)` returns `NULL` for an unsupported version.
The vtable's first field is its own version, and the struct is **append-only**
within a major version: fields are added at the end, never reordered, resized or
repurposed, and the host checks the version before reading anything added after
v1.

The ABI is **public and versioned** but carries **no compatibility promise before
1.0**, so its shape can be corrected once a real third-party driver exists. That
exemption and a deprecation policy must be stated **in the header** or they become
permanent by default.

---

## §5 — Handles: module and instance

Two levels, always explicit: a **module** (per opened library or static
registration) and an **instance** (per configuration, **N per module**). No
`fletcher_init()`, no global registry state.

The load-bearing case is not two protocols; it is **two instances of the same
driver with different configs** — two DDS domains in one process. The seam already
requires that of providers (seam §4 clause 3), so this is inheritance, not
invention; it is nearly free now and expensive to retrofit. It is also the
primitive a bridge would compose — **no bridge is built in this round**, and
nothing may foreclose one.

---

## §6 — Zero-copy receive: the win this ABI unlocks

The seam preserves zero-copy on publish and on attachment sharing, and
PDA-decouple made it *possible* for the seam to carry memory it does not own
(seam §3.2, §8). This round delivers it: Fast DDS can hand out transport-owned
memory that must be returned via `return_loan`, and `release` (§3) is exactly that
hook. The ABI is therefore **more** zero-copy-capable than the C++ interface it
wraps, which turns the migration from a cost into a measurable win. The seam's
copy-accounting oracle is the arbiter.

### §6.1 — The known obstacle

Receive-side **data-sharing is currently disabled by default**, deliberately: with
it enabled on both ends, a cross-process subscriber that joins *after* rows were
published intermittently receives only part of the `TRANSIENT_LOCAL` backlog —
often just the newest sample — silently. Measured on Windows / Fast DDS 3.4.0;
evidence, alternatives and the cost are in
[fastdds-pubsub-provider/README.md](../fastdds-pubsub-provider/README.md), and
`MakeFletcherDefaultReaderQos()` carries the reasoning.

This round is the natural home of that question, and it must be **answered, not
worked around**: a loaned-sample receive path that re-enables data-sharing without
resolving the sample loss would ship the defect as a feature. It may legitimately
conclude that the answer is upstream and defer, provided it says so.

---

## §7 — Conformance: the equivalence proof

The seam's conformance suite (seam §7.1) is **retargeted through the ABI** with
its assertions **unchanged**. If it passes against the C++ interface and passes
through the ABI, the ABI is behaviourally equivalent to the seam it implements —
that is the whole claim of the round.

Two rules:

1. **Assertions may not be weakened to pass.** Anything the suite cannot express
   through the ABI is evidence the *header* is underspecified, or that the *seam*
   is — a defect in one of the two specs, and a stop-and-ask either way.
2. **The cross-process subject carries over** (seam §7.2). A driver in a separate
   process is the case third-party drivers will actually be, and single-process
   evidence does not cover the transport.

---

## §8 — MCU footprint

The `<75 KB` Flash figure comes from **TD-004** (rationale) and **TD-007**
(context) — *not* TD-005, which is schema transport via companion topics.

CI builds and tests XRCE-DDS on **ubuntu-latest and windows-2022 only**. There is
**no MCU lane**, so the figure is a design aspiration nothing verifies — real, but
unguarded. A **desktop link-size budget test** is required as a proxy: not an MCU
build, and it must not be presented as one, but it fails when someone adds a
heavyweight dependency to the edge driver. No JSON/YAML parser may be linked into
it (seam §4.2).

---

## §9 — Recorded intent

### §9.1 — Reconciling TD-007 on dynamic linking

TD-007's "alternatives considered" rejects *"dynamic linking with optional Arrow
C++ (fragile, hard to test, platform-dependent)"*. That is narrower than it first
reads: it rejected dynamic linking as a way to make **Arrow C++** optional in the
edge/server tier split — not plugin ABIs in general. This round is nonetheless
Fletcher's first deliberate use of dynamic loading, so a new TD entry must state
the difference and answer the three objections on the record: **fragile** (§4.2's
version negotiation), **hard to test** (§7's conformance suite, plus the static
lane which exercises the ABI with no loader at all), and **platform-dependent**
(platform code confined to the host's loader, with §2's identical-source
guarantee).

### §9.2 — Licensing

Fletcher is LGPL-3.0. A dynamically loaded third-party driver makes this ABI a
**licensing** boundary as well as a technical one, and the combination chosen here
— public, versioned, third-party-implementable — is what makes proprietary
third-party drivers practically possible. Whether that is the goal or a thing to
constrain must be **recorded as explicit intent before the ABI is published**, not
settled afterwards by silence. This is the round's one genuinely open decision.

---

## §10 — Out of scope

- Anything the seam owns — see §0.1. Changing the seam is a stop-and-ask.
- The **language-binding ABI**. It runs in parallel, mirrors the same seam, and
  shares no header with this one (seam §1).
- A protocol **bridge** (DDS↔MQTT routing). §5 keeps it possible.
- **Discovery by search path or manifest** (§2.1 — explicit path only).
- Retiring the C++ `PubSubProvider` interface (§2.2 — the ABI is additive).
- Any **wire format**, codec or generated-code change.
- A real **MCU CI lane** (§8 ships a desktop proxy instead).
