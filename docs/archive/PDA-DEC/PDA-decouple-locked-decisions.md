# PDA-decouple — Locked Decisions

Firm choices for preparing the pub/sub seam. The architect, architecture reviewer,
implementer, and compliance reviewer must honor these; a proposed deviation is a
**stop-and-ask**. Full rationale in
[docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) (the oracle — it
wins on any conflict with this digest or a per-item design).

History: this round is the first half of what was designed on 2026-08-31 as a
single round **PDA**. The maintainer split it on 2026-09-01 into **PDA-decouple**
(the seam) and **PDA-ABI** (the C boundary below it), so that PDA-ABI and
BIND-C#/BIND-Rust can then run **in parallel**. Decisions below are that round's,
restricted to the seam, plus the ones the split itself forced (D1, D2, D3).

1. **The seam is the meeting point, and this round is the only one that may
   define it.** PDA-ABI builds below it, BIND-C#/BIND-Rust build above it, they
   run in parallel, and they meet **only** at
   [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md). A later
   round finding the seam insufficient is a **stop-and-ask against that spec**,
   not a local workaround and not a change landed inside an ABI round.

2. **Neither ABI mirrors the other; both mirror the seam.** The two C boundaries
   will end up with structurally similar types, because both are views of the same
   C++ types — that is a **consequence, not a dependency**. Neither may be defined
   in terms of the other's types, neither may assume the other exists, and there
   is to be **no shared C header between them** (it would couple their cadence and
   force one to wait, which is what the split exists to prevent). Consistency of
   *idiom* is a review preference; shared *code* is not permitted.
   *(This supersedes the original round's L5, which claimed the buffer struct had
   to be literally shared and that the two ABIs "cannot be designed
   independently". That was wrong: a built-in provider hands the encoder a plain
   C++ `WriteBuffer&` with no ABI object in the path, so a binding defined over
   driver-ABI types would work only for loaded drivers.)*

3. **Built-in versus loaded is invisible above the seam.** One registry, one
   creation signature, selector = **name or path**; nothing above may branch on
   which. A protocol must be movable from built-in to loaded without touching a
   caller. PDA-ABI adds a **resolver**, not a second API — if the signature has to
   change to admit loading, it was underspecified here and that is a stop-and-ask.

4. **The interface's TYPES are in scope; its METHOD SET is not.** Changing a
   crossing type — to give it a C-expressible ownership model, or to give schema
   arrival a C-expressible form — is the work. Adding, removing or reordering
   `CreateTopic`/`Publish`/`Subscribe`/`Unsubscribe` is a **stop-and-ask**.
   `Publish` stays **inverted** (provider supplies the buffer, Fletcher encodes
   into it) — that inversion is the entire zero-copy encode path.

5. **Every crossing type gets a normative, C-expressible ownership rule, written
   in the header.** A type whose ownership is only implied by C++ semantics is not
   done. Covers `WriteBuffer` (window + refill), `Blob`/`Attachments` (shared →
   handle + retain/release), `SharedSchema`, topic segments, and schema arrival.
   Shared ownership must be safe from any thread; release must not throw or
   re-enter the seam.

6. **The seam must be able to carry memory it does not own.** Today's
   `Blob = shared_ptr<const vector<uint8_t>>` forces a copy into a `vector`, so a
   provider that could hand over a transport's loaned sample cannot. Fixing that is
   the largest single thing standing between the seam and zero-copy receive. How
   is PDA-DEC-3's design call; that it is fixed is not optional. Zero-copy receive
   itself is **delivered in PDA-ABI**, not here.

7. **Zero-copy is required for rows AND attachments, and must be falsifiable.**
   It is a property *of the seam*: both ABI rounds inherit it and neither can
   restore it if the seam loses it. A copy-accounting oracle (PDA-DEC-2) makes it a
   test rather than an aspiration. Accepting a copy anywhere on the row or
   attachment path is a stop-and-ask.

8. **Configuration = typed core + opaque document, and the seam carries no
   protocol vocabulary.** Typed core is `{max_payload_bytes, domain_id}` — derived
   from what both shipping providers already have. Everything else is bytes
   Fletcher transports and never reads; the format is the **provider's** (Fast DDS
   → native XML QoS profile, XRCE → `key=value`, Zenoh → JSON5). **Fletcher gains
   no config parser and no config dependency** — an explicit non-goal, and adding
   one "for convenience" is a stop-and-ask.

9. **`FastDDSProviderOptions` is retired, not deprecated.** It embeds
   `eprosima::fastdds::dds::DataWriterQos`/`DataReaderQos` plus per-topic maps of
   them, so it cannot survive a uniform registry: a caller selecting `"fastdds"`
   by name must configure it without linking eProsima. One config path, so there
   is one path to test. Blast radius: 4 external files / 19 occurrences, plus
   substantial provider-internal test rewrite. Keeping the typed struct alive
   alongside the document path is a stop-and-ask.

10. **The seam throws; each boundary translates independently.** Recoverable
    errors throw (HARD's H-INV-2) and that stays. Because exceptions cannot cross
    C, this round must publish a **stable exception taxonomy** — without it the two
    ABI rounds will map the same failure to different statuses. A `SubscribeCallback`
    **must not throw**: it runs on a provider thread, often inside a C callback in
    the transport, where an escaping exception terminates rather than unwinds.

11. **Guard-first, and cross-provider divergences are FIXED in-round.** The
    conformance suite (PDA-DEC-1) and the copy oracle (PDA-DEC-2) land **before** the
    vocabulary work. Divergences between the three providers are **expected** —
    they have never been mechanically compared — and are fixed so both ABI rounds
    build over consistent behaviour rather than a frozen inconsistency. A fix that
    would move **wire bytes** is a stop-and-ask.

12. **The conformance suite needs a cross-process subject for the DDS providers.**
    A single-process suite **cannot see transport behaviour**: Fast DDS serves
    same-process endpoints over intra-process delivery, bypassing data-sharing and
    much of the transport. This is not hypothetical — the Fast DDS modernization
    merge shipped a receive-side data-sharing defect that lost `TRANSIENT_LOCAL`
    samples to cross-process late joiners while all 70 provider tests stayed
    green. Certifying the seam on single-process evidence would be certifying it
    on evidence that does not cover it.

13. **The wire format does not change.** No change to encode→decode bytes for any
    input shipping today. This round moves *where* bytes are handed off and *who*
    owns them, never what they are. A wire-byte change is a stop-and-ask — this
    includes any fix landing under decision 11.

14. **Nothing ABI is built here.** No `extern "C"`, no C header, no `dlopen`, no
    version negotiation, no driver vtable, no host-callback struct. A registry of
    **built-ins** is in scope; a loader is not. Starting either ABI's function set
    inside this round is a stop-and-ask.
