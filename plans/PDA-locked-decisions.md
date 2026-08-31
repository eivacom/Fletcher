# PDA — Locked Decisions

Firm choices for the protocol-driver ABI round. The architect, architecture
reviewer, implementer, and compliance reviewer must honor these; a proposed
deviation is a **stop-and-ask**. Full rationale in
[docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md) (the
oracle — it wins on any conflict with this digest or a per-item design).

Decisions L1–L7 were answered by the maintainer on 2026-08-31; L8–L10 on the same
day after the scope questions. Nothing here is an architect's assumption.

1. **One C entry-point contract; two registration paths.** Dynamic discovery on
   desktop/server, **static registration** on MCU/embedded. A driver's source is
   identical in both forms. Dropping the static path is a stop-and-ask — it is
   what keeps XRCE-DDS (which exists *for* MCU targets) portable to a driver at
   all, and it is how the ABI gets tested with no loader involved. See spec §2.

2. **Configuration = typed common core + opaque per-driver document.** The typed
   core is `{max_payload_bytes, domain_id}` — derived from what both shipping
   providers already have, not invented. Everything else is an opaque blob only
   the driver parses. **The document format is driver-defined** (Fast DDS → its
   native XML QoS profile; XRCE → `key=value`; Zenoh → JSON5), which is what
   dissolves the L1/L2 footprint conflict. **Fletcher gains no config parser and
   no config dependency** — this is an explicit non-goal, and adding one "for
   convenience" is a stop-and-ask. See spec §5.

3. **Public, versioned, third-party-implementable PURE C ABI.** No C++ types, no
   vtables-as-classes, no exceptions crossing, explicit version negotiation.
   Third parties must be able to ship a driver built with a different compiler
   and standard library, with no Fletcher rebuild. Downgrading this to "internal
   decoupling with a C++ interface" would make the round runtime *selection*
   rather than a driver contract, and is a stop-and-ask. No compatibility promise
   before 1.0, but that exemption plus a deprecation policy must be stated **in
   the header**. See spec §6.

4. **Zero-copy is REQUIRED for rows AND attachments.** This is the single biggest
   complexity driver and it was chosen deliberately. It forces self-describing
   **fat handles** carrying their own lifetime ops (`fletcher_blob`,
   `fletcher_schema` with `retain`/`release`) and a callback-based
   `fletcher_write_buffer`, because Fletcher cannot own the allocation. Accepting
   "one copy at the boundary" anywhere on the row or attachment path is a
   stop-and-ask. Zero-copy must be **falsifiable by a copy-accounting oracle**
   (PDA-2), not asserted. See spec §3, §4, §8.2.

5. **BIND-C#/BIND-Rust are full pub/sub clients, so the shared vocabulary is
   load-bearing, not cosmetic.** The two ABIs are **directional opposites**
   (driver ABI: Fletcher calls, driver implements; binding ABI: Fletcher is the
   callee, app calls). **No driver data-plane function is callable by a binding,
   and none should be.** Bindings need only the *management* role. What is shared
   is the **type, ownership, error and version vocabulary** — specifically the
   buffer struct, blob lifetime protocol, `ArrowSchema` verbatim, and the
   error/version conventions. Consequence: **the roadmap does not need
   reordering.** Designing a second, divergent buffer or blob representation for
   BIND is a stop-and-ask. See spec §9.

6. **Multiple driver instances per process must be architecturally trivial.**
   Module handle (per library / static registration) vs instance handle (per
   config, N per module); every entry point takes an explicit handle; **no
   `fletcher_init()`, no global registry state.** The load-bearing case is two
   instances of the *same* driver with different configs (two DDS domains), not
   two protocols. **No bridge component this round** — but nothing may foreclose
   one. See spec §7.

7. **Driver discovery: explicit path from configuration only.** No directory
   scan, no manifest, no environment variable. Loading executable code from a
   searched directory is an attack surface the round does not need. A search path
   is a later additive feature, and adding one here is a stop-and-ask. See
   spec §2.1.

8. **The ABI goes BELOW `PubSubProvider`, not in place of it.** A
   `DriverProvider : PubSubProvider` adapter wraps a loaded driver, so
   `Publisher`/`Subscriber`, `PublisherArrow`/`SubscriberArrow`, generated code
   and the codec are **untouched**, and the C++ interface stays a supported way
   to write an in-tree provider. Retiring the C++ interface is **not** in this
   round. See spec §0.2.

9. **Port all three providers; Fast DDS config becomes XML-profile-only
   (breaking).** InProcess is promoted out of `gateway/src/main.cpp` into a real
   component and becomes the reference driver; Fast DDS proves the hard config
   case on the path the gateway actually uses; XRCE proves static registration
   and the footprint budget. `FastDDSProviderOptions` is **retired**, not
   deprecated — one configuration path, so there is one path to test. Measured
   blast radius: 4 external files / 19 occurrences, plus substantial
   provider-internal test rewrite (spec §10). Keeping the typed options struct
   alive alongside the document path is a stop-and-ask.

10. **Guard-first, and cross-provider divergences are FIXED in-round.** The
    conformance suite (PDA-1) and the copy-accounting oracle (PDA-2) land
    **before** any ABI work, written against the existing C++ interface and run
    against all three providers. Divergences are **expected** — three providers
    written to a prose contract have never been mechanically compared — and they
    are **fixed in-round so all three agree before the ABI wraps them**, rather
    than pinned as known-divergent. Rationale: the ABI must be defined over
    consistent behaviour, not freeze an inconsistency into a public contract.
    Consequence accepted: PDA-1's size is not fully knowable up front. A
    divergence whose fix changes **wire bytes** is a stop-and-ask.

11. **Every behavioural item ships a red-first test; guard items ship a green
    oracle.** PDA-1/PDA-2 are the guards. PDA-5 and PDA-9 are equivalence
    proofs. Migration items (PDA-6..PDA-8) must keep PDA-1's suite and PDA-2's
    oracle green — a port that regresses either is red by definition.

12. **The wire format does not change.** No change to encode→decode bytes for any
    input shipping today. The ABI moves *where* bytes are handed off, never what
    they are. A wire-byte change is a stop-and-ask (this includes any fix landing
    under decision 10).
