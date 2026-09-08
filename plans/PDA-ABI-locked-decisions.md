# PDA-ABI — Locked Decisions

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

4. **Public, versioned PURE C forms at the boundary.** No C++ types crossing, no
   exceptions crossing, explicit version negotiation, append-only structs within a
   major version. A third party must be able to ship a driver built with a
   different compiler and standard library, with no Fletcher rebuild. Downgrading
   to "internal decoupling with a C++ interface" would make this runtime
   *selection* — which the seam already delivers — rather than a driver contract.
   No compatibility promise before 1.0, but that exemption plus a deprecation
   policy must be stated **in the header**.

   **Reconciled against ruling 46 (2026-09-05), which supersedes this decision in
   part.** Every protocol driver is C++, so both sides of this ABI are C++, and the
   words *"third-party-implementable"* that this decision carried until 2026-09-08
   meant *in any language*. That reading is **withdrawn, not relocated**: it was put
   to the owner verbatim and ruled against (`docs/protocol-driver-abi-spec.md` §0.2;
   seam §9 since ruling 60, which authorised this correction). What survives
   untouched is (i) the C **forms** at the boundary — ABI spec §3's `fletcher_blob`,
   `fletcher_write_buffer`, `fletcher_status`, which ruling 46 explicitly left
   standing — and (ii) the binary-stability constraints that hold *between
   separately built C++ binaries*: no exception may cross, no `longjmp`, no assumed
   layout (ruling 46's own *"Does NOT relax"* clause; ABI spec `:190-192`).
   "Third party" survives as *a different team with a different toolchain*, not as
   *a different language*. The ruling also replaced the language-agnosticism test:
   not *"could a non-C++ implementer construct this?"* but **"does this data drill
   all the way up to the language ABI?"** — a concept that stops at the protocol ABI
   may be expressed in C++; one that continues up to the language boundary must
   still be language-agnostic. Note `.claude/runbook.PDA-ABI.config.md:95` still
   lists *"making the ABI a C++ interface"* as a scope violation under this
   decision: read it as the C forms and the crossing constraints above, not as a bar
   on a C++ driver.

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

---

## Inherited from PDA-DEC — owner rulings that bind this round

Migrated at PDA-DEC's close (2026-09-08) from `plans/PDA-DEC-rulings.md`, which is
archived with that round's execution corpus. This round's own ledger is created at
kickoff by **copying** that file (`.claude/runbook.PDA-ABI.config.md:130-132`), and
every design-phase ruling in it governs this round too; the six below are the ones
whose forward half is not otherwise written down in a kept document. Numbering
continues the list above — a proposed deviation from any of them is a
**stop-and-ask**, and the ruling text, not this digest, is the authority. Ruling
numbers are positions in that ledger (ruling *n* = its *n*-th `## 2026` entry),
which is how the ledger's own cross-references read.

Every change to the seam spec goes through **seam §12.1**, whose *who may act* is
"nobody alone", and whose `append-only` class makes an append itself a stop-and-ask
with the owner allocating the number or the field. Authorisations to write into
frozen text have been given before; each named its own section and its own single
change, each states its own limit inside the section it amended, and **none is
precedent for any other**. Read the ledger entry, not this line.

16. **Re-permitting re-entry from a delivery callback is this round's obligation,
    and it is NOT pre-authorised.** Ruling 52 (2026-09-05, *"refuse everywhere; hand
    the capability to PDA-ABI"*) refused re-entry on all four `PubSubProvider`
    methods, on every provider — seam §6 clause 6 — and made re-permitting *"a
    forward obligation on PDA-ABI, to be registered as debt"*. Seam §6 clause 6
    points at that register by name (*"a registered obligation on PDA-ABI"*); the
    register is **AG1-DEBT-19**, carried in full below. Refused→permitted is
    source-compatible for callers, but the contract text is frozen, so it needs a
    **fresh owner ruling** — ruling 52 explicitly does not pre-authorise it. The
    conformance clause `EveryProviderMethodIsRefusedFromInsideADelivery` must be
    **re-aimed, not deleted**, if that ruling ever comes: the charter constraint
    depends on clause 6 remaining a control neither mechanism can green.
    **Ruling 52 SUPERSEDES ruling 48** (*"refuse only what's unsafe"*) as to
    direction — 48's premise was falsified by probe (Fast DDS hangs, its listener
    callback holding the RTPS reader mutex; the capability worked on XRCE only, one
    protocol of three). **Any statement about re-entry cites 52, never 48.**

17. **The registry gets no typed name query.** Ruling 55 (2026-09-06) was put to the
    owner *against the architect's recommendation*, and the owner chose *"No — only
    Fletcher's sentence. The application can only display the refusal message
    Fletcher composed. No new public surface on the registry."*
    `ProviderRegistry::RegisteredNames()` was **rejected in any form**. The
    substitute is already normative: §4's refusal message is a deterministic
    function of the registry's contents (not of `unordered_map` hash order) and is
    reconstructible from its number and message alone. **The question has been put
    and answered** — proposing the query again is a stop-and-ask on a settled
    decision, not a fresh discovery.

18. **When a guarantee keeps leaking, fix it at the scope the guarantee is actually
    about — do not publish the leak.** Ruling 39 (2026-09-04), given after
    PDA-DEC-A4 had scoped one promise four ways (process-wide → per-`Impl` →
    per-frame → per-gate, the last being the first scope whose lifetime matched the
    guarantee). It names itself *"forward-relevant to the remaining amendments"*,
    and the preference is conditional: it applies **provided the root cause has been
    NAMED** rather than another instance patched. **Its stop condition stands for
    any future item:** *a further case after the root-cause fix means the premise is
    broken and the item stops.*

19. **Narrowing a published claim may be inferred; widening it never may.** Ruling
    32 (2026-09-03) granted a licence to infer the owner's standing preference for a
    narrow claim stated honestly over a wide one implied, so a design need not ask
    again. Ruling 37 (2026-09-04) bounds it: that licence *"permits **narrowing**
    without asking, never **widening**"*. Seam §12.4 records the preference but not
    the asymmetry, and the asymmetry is the half that decides whether an item must
    stop and ask. Widening any published promise or claim is a stop-and-ask.

20. **The owner's charter is this family's acceptance standard, and its requirement
    (a) is THIS round's Definition of Done.** Ruling 1 (2026-08-31): *"a) I set up
    Fletcher with the protocol \"driver\" at run-time, and b) there is a way for me
    to configure the driver with protocol-specific setup details at runtime."*
    PDA-DEC delivered runtime **selection** and **configuration**; runtime
    **loading** is this round's, and (a) is not satisfied by selection by name. The
    Gate above states this round's entry condition; this states its exit condition.

21. **Fewer, larger items — per-item ceremony is the cost being managed.** Ruling 47
    (2026-09-05): *"Ceremony in the round plan is expensive. Group the works into as
    few rounds as makes sense."* Given after PDA-DEC's amendment phase measured
    ~5,000 lines of code against ~12,400 lines of docs and plans, because per-item
    process cost is fixed regardless of item size. Granularity remains the PM's call
    under the `naming` standing decision; this ruling directs it toward fewer,
    larger items, and names the cost being traded: grouped items receive one
    compliance pass and one code review **between** them rather than one each. This
    round's plan currently declares 8 items with per-item ceremony.

---

## Inherited design debt from PDA-DEC

Migrated at PDA-DEC's close from `plans/reviews/design-debt.md`, which is archived
with that round's per-item reviews. These are the entries **addressed forward to
this round**, carried with their evidence and their *addressed-to* lines intact.
Debt is not a stop-and-ask: it is a hazard someone already measured, and the item
that meets it owns the fix.

### AG1-DEBT-19 — re-permitting re-entry from a delivery callback (PDA-ABI)

*The register decision 16 and seam §6 clause 6 both point at. Carried in full: the
probe evidence exists nowhere else in the kept corpus.*

**Raised by:** implementation of PDA-DEC-AG1, under owner ruling 2026-09-05 *"refuse
everywhere; hand the capability to PDA-ABI"*, which supersedes the earlier *"refuse only
what's unsafe"*.

**What was given up, and what it cost.** All four `PubSubProvider` methods now throw
`kReentrantCall` when issued from inside a delivery callback on the same instance and
thread, on all three providers — spec §6 clause 6. Transform-and-republish (read a row,
publish a derived one) must therefore cross a queue of the caller's own. **XRCE genuinely
loses a working capability**: it was the one protocol of three that served these calls. The
loopback answered `kInternal` from re-locking the mutex it holds across dispatch, and Fast
DDS *hung* — a listener callback holds the reader's RTPS mutex, measured by probing each
call on its own. No in-tree caller re-enters the seam from a handler; the gateway already
copies and posts to its own executor.

**Why PDA-ABI and not sooner.** A delivered row crosses as a bare pointer and length with
**no owner handle** — retain exists for attachments and schemas, not for rows — so
deferring a re-entrant publish forces a whole-row copy today regardless. That copy is
already mandatory until the loaned-sample receive path exists, which is PDA-ABI's. The
decision should be made with the real mechanism in hand rather than against a copy nobody
can avoid.

**Acceptable fix:** once loaned-sample receive lands, re-open whether re-entry can be
permitted — most likely by deferring the re-entrant call to the end of the delivery frame,
holding the loan rather than a copy.

**This is NOT pre-authorised.** Refused→permitted is source-compatible for callers, but the
spec text is frozen (§12.1) and the ruling above explicitly withholds authority: it needs a
fresh owner ruling. The conformance clause
`EveryProviderMethodIsRefusedFromInsideADelivery` asserts the refusal on all six subjects
and must be re-aimed, not deleted, if that ruling ever comes.

**Addressed to:** PDA-ABI, at the item that lands the loaned-sample receive path.

### A1-DEBT-6 — the overflow guard lives at one door, not at the arithmetic (PDA-ABI)

**Raised:** PDA-DEC-A1 step-4b re-review, 2026-09-05, as should-fix R1 (non-blocking).
**Finding:** `AppendZeros(SIZE_MAX - 20)` still silently yields a 19-byte row. Unreachable
from anything in-tree today — every in-tree caller passes a `WriteLengthPlaceholder()`
result — so it was correctly not treated as a gate for A1.
**Why it is written down anyway:** PDA-ABI will give `Append` and `AppendZeros` the same C
form that A1 gave the window. Each will then need its own copy of A1's door check, and the
first one that forgets reproduces B1 — the defect this item existed to close — at a new
entry point.
**Acceptable fix:** hoist the `min_bytes > SIZE_MAX - pos_` refusal into `Refill` /
`ReserveStorage`, where the addition actually lives, so every present and future entry
point inherits it instead of re-implementing it.
**Addressed to:** PDA-ABI, at the item that gives the append path its C form.

### AG1-DEBT-20 — the re-entrancy identity is typed at one end only

**Raised by:** PDA-DEC-AG1 cycle-3 code review (should-fix S1), 2026-09-05.

Cycle 3 gave `DeliveryChannel` a typed constructor, `DeliveryChannel(const PubSubProvider*,
SubscribeCallback)`, so the four real push sites can no longer get the provider identity
wrong. The *ask* side did not follow: `internal::RefuseIfInsideDeliveryOn` and
`internal::InsideDeliveryOn` still take `const void*`, because they live in `core` and
`core` must not depend on `pubsub`. All thirteen ask sites therefore hand-write
`static_cast<const PubSubProvider*>(this)`, and a door added later can omit the cast
silently — it compiles, and it compares a derived address against a base address that
happens to be equal today only because the single non-virtual base sits at offset 0.

**Why it was not fixed in-round.** The fix is a `pubsub`-tier internal header wrapping the
two `core` entry points at `const PubSubProvider*`, which is new surface in the tier PDA-ABI
is about to re-cut. Doing it now means doing it twice.

**Acceptable fix:** an inline `pubsub`-tier wrapper taking `const PubSubProvider*`, with the
`core` `const void*` forms becoming the implementation detail no provider calls directly.
**Addressed to:** PDA-ABI, at the item that fixes the driver-side identity token.

### AG2-DEBT-11 — the per-envelope attachment count is unbounded on both decode paths

**Raised by:** PDA-DEC-AG2 fix cycle 1, 2026-09-06 (PM decision, recorded).

Neither `ParseEnvelopeBody` nor `core::DeserializeEnvelope` bounds the number of attachments a
sample may declare. `ParseEnvelopeBody` bounds it only by `(total - pos) / 8`, so a 1 MiB sample may
claim ~131k attachments; `DeserializeEnvelope` has no count bound at all and the gateway calls it on
a frame up to 16 MiB. A hostile peer can therefore make a decoder allocate proportionally to the
frame size it was already willing to receive.

**Why it is not a regression, and why it was not fixed in-round.** Nothing was lost: the retired
`std::unordered_map` allocated *k* nodes for the same *k*, so the memory exposure is exactly what
shipped before. AG2's blocking defect was the *quadratic time*, and that is fixed
(`AttachmentsWireBuilder`, O(k log k), measured 58,745 ms → 38 ms at k=200,000). Adding a count
ceiling would invent a published limit with **no measured basis** — the design explicitly declined
exactly that for key length (*"A5's 246-byte bound was measured, not reasoned … inventing either is
over-forbidding without evidence"*) — and a new refusal would need new normative text in frozen
§3.2, which is a fresh owner ruling. The time defect is closed on its own terms; the memory question
is **deferred, not answered**.

**Acceptable fix:** measure what a real deployment's attachment count actually is, then state a
per-envelope ceiling in the wire format and refuse past it in both decoders (`return false` /
`std::invalid_argument`). Measure first — the bound must be evidence, not a guess.
**Addressed to:** PDA-ABI, at the item that gives the decode path its C form, or any round that
first has a measured deployment figure.

### Two more, addressed elsewhere but reachable from here

- **AG1-DEBT-21 — a refused `Subscribe` still walks the rollback path.** Addressed to
  *"the next item to touch `subscriber.cpp`'s subscription bookkeeping"*, which is the
  caller tier BIND wraps rather than the driver tier this round re-cuts. Carried in
  [BIND-csharp-inherited-constraints.md](BIND-csharp-inherited-constraints.md); read
  it there before touching that bookkeeping from here.
- **A5-DEBT-6 — the gateway's binary-`publish` path is unwatched.** The gateway refusal
  case watches the two **text**-frame paths into `SplitTopic` but not the binary
  `publish` path (`gateway/src/ws_session.cpp:271`). Non-blocking and **owned by
  nobody**: the seam refuses the name either way, so no wrong delivery is reachable —
  the gap is in what the *gateway* case observes, not in the product. Closing it means
  driving a binary publish frame with an empty part through `gateway-end-to-end`.
  Raised by PDA-DEC-A5 code review's final check, 2026-09-04. Recorded here so it does
  not vanish with the register; it is not this round's obligation.
