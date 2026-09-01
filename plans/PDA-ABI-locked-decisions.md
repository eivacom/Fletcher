# PDA-ABI (`ABI`) — Locked Decisions

Firm choices for the protocol driver ABI. The architect, architecture reviewer,
implementer, and compliance reviewer must honor these; a proposed deviation is a
**stop-and-ask**. Rationale in
[docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md), and — for
anything crossing the seam —
[docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md), which **wins
over both this digest and the ABI spec** on interface matters.

History: designed 2026-08-31 as one round **PDA**; split by the maintainer on
2026-09-01 so that this round and BIND-C#/BIND-Rust can run **in parallel** over a
seam prepared first by **PDA-decouple**. Decisions here are that round's, narrowed
to the C boundary, plus what the split forced (A1, A2).

1. **This round is strictly BELOW the seam, and may not change it.** PDA-decouple
   owns `PubSubProvider` and everything crossing it. A seam that proves
   insufficient is a **stop-and-ask against the seam spec** — never a local
   workaround, never a change landed here. Gate: this round does not start until
   PDA-decouple's Definition of Done holds.

2. **No dependency on the language-binding ABI, in either direction.** BIND runs in
   parallel and mirrors the same seam. **No shared C header between the two** — it
   would couple their release cadence and force one to wait, which is what the
   split exists to prevent. The two will end up with structurally similar types
   because both are views of the same C++ types; that is a consequence, not a
   coupling. Defining this ABI's types in terms of BIND's (or vice versa) is a
   stop-and-ask. Consistency of *idiom* is a review preference only.

3. **This ABI has TWO roles, not three: the driver vtable and the host callbacks.**
   **Provider selection is NOT in this header.** Selecting and configuring a
   provider at runtime is caller-visible — an application reaching Fletcher through
   a language binding must be able to do it — so it is seam surface and therefore
   *binding* surface. A driver never implements selection. This round contributes
   only a **path resolver** to the registry the seam defines.

4. **Public, versioned, third-party-implementable PURE C.** No C++ types, no
   exceptions crossing, explicit version negotiation, append-only structs within a
   major version. A third party must be able to ship a driver built with a
   different compiler and standard library, with no Fletcher rebuild. Downgrading
   to "internal decoupling with a C++ interface" would make this runtime
   *selection* — which the seam already delivers — rather than a driver contract.
   No compatibility promise before 1.0, but that exemption plus a deprecation
   policy must be stated **in the header**.

5. **One entry-point contract; dynamic AND static registration.** A driver's source
   is **identical** in both forms. Dropping the static path is a stop-and-ask: it
   is what keeps XRCE-DDS (which exists *for* MCU targets) portable to a driver,
   and how the ABI is tested with no loader at all.

6. **Discovery: explicit path only.** No directory scan, no manifest, no
   environment variable. A *name* already resolves through the seam's registry; a
   *path* is given explicitly in configuration. Loading executable code from a
   searched directory is an attack surface this round does not need. A search path
   is a later additive feature; adding one here is a stop-and-ask.

7. **The adapter is additive and must not copy.** `DriverProvider : PubSubProvider`
   registers as the path-selector resolver, so `Publisher`/`Subscriber`, generated
   code and the codec are untouched, and a caller cannot tell a driver-backed
   provider from a built-in one. Bridging the seam's shared-ownership types to the
   ABI's handles must cost **no copy in either direction**; if it does, the adapter
   approach is not viable and that is a stop-and-ask. **Retiring the C++
   `PubSubProvider` interface is not in this round.**

8. **Module and instance handles, no global state.** N instances per module; every
   entry point takes an explicit handle; no `fletcher_init()`; no global or
   thread-local "last error" slot — the message belongs to the instance that
   produced it. The load-bearing case is two instances of the *same* driver with
   different configs. **No bridge component**, and nothing may foreclose one.

9. **Zero-copy is inherited, not re-litigated.** Required for rows and attachments;
   the seam's copy-accounting oracle is the arbiter. Accepting a copy anywhere on
   the row or attachment path is a stop-and-ask. Zero-copy *receive* is this
   round's to deliver — the seam made it possible, PDA-decouple did not ship it.

10. **The receive-side data-sharing defect must be answered, not stepped around.**
    Data-sharing is currently off by default on the read side because, with it on,
    a cross-process late joiner silently loses part of the `TRANSIENT_LOCAL`
    backlog. A loaned-sample path that re-enables it without resolving that would
    ship a known defect as a feature. Concluding the answer is upstream and
    deferring is legitimate **if stated**, and the default must stay safe either
    way.

11. **Port all three providers.** InProcess is the reference driver and conformance
    vehicle; Fast DDS exercises the ABI on the path the gateway actually uses; XRCE
    proves static registration and the footprint budget. The Fast DDS **config
    document is unchanged from PDA-decouple** — the protocol-vocabulary problem was
    solved at the seam, so these are ports, not redesigns.

12. **Conformance assertions are reused, never weakened.** This round adds no new
    conformance assertions: it retargets the seam's suite, unchanged, with
    driver-backed subjects, **including a cross-process subject**. Anything the
    suite cannot express through the ABI is a defect in the header or the seam — a
    stop-and-ask, not a relaxed test. Single-process evidence does not cover the
    transport.

13. **The footprint budget is a PROXY.** A desktop link-size test, not an MCU
    build; CI has no MCU lane. Presenting it as MCU verification is a stop-and-ask.
    No JSON/YAML parser may be linked into the edge driver.

14. **The wire format does not change.** No change to encode→decode bytes for any
    input shipping today. A wire-byte change is a stop-and-ask.

15. **Licensing intent is recorded before the ABI is published.** Fletcher is
    LGPL-3.0, and a public, third-party-implementable ABI is what makes proprietary
    third-party drivers practically possible. Whether that is the goal or a thing
    to constrain must be stated explicitly, not settled by silence. **This is the
    round's one genuinely open decision.**
