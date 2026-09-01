# PDA-DEC — owner rulings

*Verbatim. Appended the moment a ruling is given. Never summarised, never deleted.
A ruling is superseded only by a later ruling recorded beside it.*

**Seeded at kickoff** from the design phase (2026-08-31 → 2026-09-01), which ran
before the round split and produced most of these. Entries marked *(selection)*
are the owner's choice among options presented to them; the quoted text is the
option as it was worded to them. Everything else is the owner's own prose.

---

## 2026-08-31 — The round's charter
> "The next round I would like to create an ABI layer that decouples the choice of protocol (DDS/MQTT/Zenoh/etc) from Fletcher even more. Basically, the generic interface that decouples the protocol from Fletcher today should be turned into an ABI such the protocol becomes a driver I set Fletcher up to use runtime rather than something I choose compile-time. This also means that protocol-specific configuration can be done at the driver level. So the technical requirement is that an ABI for the protocols is defiend such that they become drivers, and the end-user requirement is that a) I set up Fletcher with the protocol \"driver\" at run-time, and b) there is a way for me to configure the driver with protocol-specific setup details at runtime."

**Context:** opening the round. **Applies to:** the whole PDA family. The two
end-user requirements (a) and (b) are the acceptance standard; PDA-DEC delivers
runtime *selection* and *configuration*, PDA-ABI delivers runtime *loading*.

## 2026-08-31 — Linkage *(selection)*
> "Dynamic + static registration — One C entry-point contract. Desktop/server discovers .so/.dll at runtime; MCU targets register the same driver statically at link time."

**Context:** how a driver is packaged and brought in. **Applies to:** PDA-ABI
§2. Governs PDA-DEC only negatively: the registry must not assume a loader.

## 2026-08-31 — Configuration shape *(selection)*
> "Small common core + opaque per-driver document — Fletcher understands a tiny generic core (e.g. max payload, domain/endpoint identity). Everything else is an opaque JSON/YAML/XML blob only the driver parses — for Fast DDS that can be its native XML QoS profile, which it already supports. Fletcher never learns DDS vocabulary."

**Context:** how protocol-specific config is expressed at runtime — the Fast DDS
QoS problem. **Applies to:** seam §4.1, §4.2; PDA-DEC-4, PDA-DEC-6, PDA-DEC-7.
"Fletcher never learns DDS vocabulary" is the acceptance test for PDA-DEC-6.

## 2026-08-31 — ABI audience *(selection)*
> "Public versioned pure-C ABI, third-party implementable — Anyone can ship a driver, built with a different compiler/stdlib, no Fletcher rebuild. Forces a genuine C boundary: no C++ types, no exceptions crossing, explicit version negotiation."

**Context:** who implements drivers and what stability is owed. **Applies to:**
PDA-ABI. For PDA-DEC it is why the seam's vocabulary must be C-expressible at all.

## 2026-08-31 — Zero-copy, attachments, bridge, discovery
> "On 1) I'm afraid it is necessary yes, and also for attachments. 2) They're full pub/sub clients. 3) Yes, ultimately; it is not the plan for this round to build a bridge component, but the intent is to make the framework now such that this becomes trivially possible. 4) Explicit path from config for now, and perhaps that's all we'll ever do"

**Context:** answering four questions — (1) is zero-copy across the ABI a hard
requirement? (2) what are BIND-C#/BIND-Rust meant to do? (3) must one process load
multiple drivers at once? (4) how should drivers be discovered?
**Applies to:** (1) seam §8 and PDA-DEC-2/PDA-DEC-3 — zero-copy is required for
rows **and attachments**, so a copy anywhere on either path is a violation, not a
trade. (2) BIND are **full pub/sub clients**, not read-only accessors. (3) seam §4
clause 3 and PDA-DEC-8 — multi-instance must be trivial, **no bridge is built**,
and nothing may foreclose one. (4) PDA-ABI §2.1 — explicit path only, and the
owner considers that possibly permanent.

## 2026-08-31 — The question that reshaped the ABI relationship
> "This is one of the important points of the ABI - and one area I want to make sure the design gets right. The actual drivers should of course just be implemented in whatever language the implementer chooses, often C++. Fletcher itself then calls them via the ABI. But the C# and Rust bindings - will they need to call the ABI directly, or are their bindings at a higher level? Are there any functions in the protocol ABI that needs to be called directly by the Rust or C# bindings?"

**Context:** asked instead of choosing from the offered roadmap options.
**Applies to:** seam §1, §9; PDA-ABI §0.2. The answer that survived review: **no**
— no driver data-plane function is callable by a binding, and provider *selection*
is seam surface, not driver surface. See the 2026-09-01 ruling that settled it.

## 2026-08-31 — Port scope *(selection)*
> "All three"

**Context:** which providers become drivers in the round. **Applies to:** all
three providers (InProcess, Fast DDS, XRCE) are migrated — in PDA-DEC to the
registry and document config, in PDA-ABI to drivers.

## 2026-08-31 — Conformance suite *(selection)*
> "Yes, in this round (Recommended) — A public ABI whose delivery contract is only prose will be violated silently by third-party drivers. Building the suite alongside the ABI also pressure-tests the ABI itself — anything the suite cannot express is probably underspecified."

**Context:** whether the conformance suite is built in-round. **Applies to:**
PDA-DEC-1. After the split the suite lands in PDA-DEC, because the contract it
encodes is the seam's and both ABI rounds inherit it.

## 2026-08-31 — C++ Fast DDS configuration *(selection)*
> "XML profile config only — one way to do it"

**Context:** how a C++ application configures Fast DDS QoS after the round; the
options included keeping or deprecating the typed API. **Applies to:** PDA-DEC-6.
`FastDDSProviderOptions` is **retired, not deprecated** — one path, so one path to
test. This is a deliberate breaking change.

## 2026-08-31 — Cross-provider divergences *(selection)*
> "Fix in-round, before the ABI — Any divergence found is fixed as part of PDA-1/PDA-2 so all three providers agree before the ABI wraps them. Means the ABI is defined over consistent behaviour rather than freezing an inconsistency."

**Context:** what happens to divergences the conformance suite finds.
**Applies to:** PDA-DEC-1. Accepted cost: the item's size is not knowable up
front. Pinning a divergence as known-divergent instead of fixing it is a
violation.

## 2026-08-31 — The spec comes first *(selection)*
> "Yes — PDA-0 spec doc first — Write docs/protocol-driver-abi-spec.md before any item, following the recordbatch-accessor-spec.md precedent. It pins fat-handle ownership rules and the delivery contract precisely — the wording most expensive to get wrong later."

**Context:** whether the round gets an oracle spec up front. **Applies to:** both
rounds' specs, written before any item.

## 2026-08-31 — Zero-copy receive stays in scope *(selection)*
> "Keep it in-round"

**Context:** whether zero-copy receive via DDS loaned samples is carved out.
**Applies to:** PDA-ABI-7. After the split it sits in PDA-ABI; PDA-DEC only makes
it *possible* (seam §3.2). Note this ruling predates the discovery of the
receive-side data-sharing defect.

## 2026-08-31 — Bring in the Fast DDS modernization first
> "There is a branch, \"feature/fastdds_modernization/19645\", that contains some improvements to the DDS implementation. These were done over an older version of the repo. I would like to bring these changes into the abi branch before the work kicks off."

**Context:** before any round item. **Applies to:** merged as `d77b9c4`. A
PDA-DEC prerequisite: it reshaped `WriteBuffer` into the window-plus-refill form
the seam needs (seam §3.1).

## 2026-08-31 — Investigate the data-sharing regression rather than patch around it *(selection)*
> "Investigate further before landing"

**Context:** offered a policy workaround for the receive-side data-sharing defect
found during that merge; chose deeper investigation instead. **Applies to:** the
standing expectation for this family — root-cause before landing a policy change.
Carried into PDA-ABI decision 10: the defect must be **answered, not stepped
around**.

## 2026-09-01 — The bindings do not depend on the driver ABI
> "1) You yourself came up the the excellent idea that instead of replacing the current interface, the ABI should simply be implemented below it. This also means that it principle, it should be possible to have a mix of built-in and run-time-loaded protocol plugins. 2) In order for the language bindings to use all protocols, they need to interface to the abstract interface, not the underlying ABI. 3) The abstract interface may need to be updated to properly support the underlyding ABI, but it should completely abstract away whether a particular protocol driver is built-in or run-time loaded - and thus make this invisible to the language binding ABI"

**Context:** rejecting the original design's claim that the driver ABI and the
binding ABI must share a literal buffer struct and "cannot be designed
independently". **Applies to:** seam §0.1(2), §1, §1.1, §4, §9; PDA-DEC decisions
2, 3, 4. Three consequences, all normative: a **mix** of built-in and
runtime-loaded providers must be possible; bindings interface to the **abstract
interface**, never the driver ABI; and the interface **may be updated** to support
the ABI, but must make built-in-versus-loaded invisible above it.
**Supersedes:** the original round's L5 (shared buffer struct), which is void.

## 2026-09-01 — Split the round
> "I would like you to split the PDA round into two separate rounds: PDA-decouple and PDA-ABI. PDA-decouple should alter existing the protocol driver interface such that it is fully prepared for the language ABI to be developed above it and the protocol driver ABI below it. It should not do any development on any of the ABIs, only prepare for them. Once PDA-decoupe is done, the interface should be amended such that protocol implementations underneath it can be either ABI-driven or built in, and this is essentially invisible to anyone above the interface. PDA-ABI is then the implementation of the actual protocol driver ABI. It is important that the decoupling is done is such a way that the protocol driver ABI and language binding ABI can then take place completely in parallel and only \"meet\" and the interface defined in PDA-decouple"

**Context:** the round split. **Applies to:** everything. **"It should not do any
development on any of the ABIs, only prepare for them"** is the scope test for
every PDA-DEC item. **"completely in parallel and only meet at the interface
defined in PDA-decouple"** is the round's success criterion, and the reason
neither ABI round may change the seam.

## 2026-09-01 — What PDA-DEC does to the existing protocol code
> "DEC will update the interface and also the existing protocol code - so that after DEC has executed, the interface will have the correct shape, and the built-in protocol code will have been updated to follow the updated internal interface, so that it is properly decoupled (but of course still not behind an ABI and still not run-time loadable)"

**Context:** confirming PDA-DEC's scope. **Applies to:** PDA-DEC-1, PDA-DEC-3,
PDA-DEC-5, PDA-DEC-6, PDA-DEC-7. The existing provider implementations **are
rewritten in this round** to follow the updated seam. Explicitly still: no ABI, not
runtime-loadable. (Runtime *selection by name* is delivered; runtime *loading* is
not.)

## 2026-09-01 — Conflicting topic re-declaration is refused *(selection)*
> "Refused, every protocol — Every protocol rejects the conflicting declaration with an error. Spec §7 clause 3 tightens from \"may be rejected\" to \"must be rejected\" in this PR. Consequence: the loopback stops silently overwriting and XRCE gains conflict handling — both are divergence fixes your 2026-08-31 ruling already put in this round. This is also the default if you don't answer."

**Context:** PDA-DEC-1 design review cycle 1 raised an oracle-wins tripwire — the
suite's clause 8 asserted rejection uniformly while spec §7 clause 3 said
rejection "may" happen, and the three providers diverge today (Fast DDS throws;
the gateway loopback silently overwrites; XRCE has no conflict handling). The
offered alternative was uniform *acceptance* with the topic adopting the new
shape. **Applies to:** `docs/pubsub-interface-spec.md` §7 clause 3 — the "may" →
"must" amendment lands in **this** PR, not deferred to PDA-DEC-9; PDA-DEC-1
clause 8; and the loopback + XRCE conflict-handling divergence fixes, which are
in-round work under the 2026-08-31 divergence ruling.

## 2026-09-01 — The conformance suite ships with a documented blind spot *(selection)*
> "Ship the guard, hunt elsewhere — Close this stage: the suite guards the delivery contract, with the blind spot and the evidence written into its README. The row-loss defect keeps its existing owner — the stage that turns on zero-copy receive, which is the feature it actually blocks — starting from the evidence table we now have. Cost: the conformance suite ships with a known blind spot. This is also the default if you don't answer."

**Context:** PDA-DEC-1's design made the falsification a close gate — clause 6 had
to go red against a provider with reader-side data-sharing re-enabled, or "the
item is not done". It did not go red, twice: the harness-shape hypothesis was
refuted by the `gateway-fastdds-ts` control (same shape, does reproduce) and the
sentinel hypothesis by measurement after the sentinel was removed. The remaining
untested candidate — several data-sharing endpoints coexisting in one participant
— is unreachable from this harness because rung-1 item 5 gives the peer protocol
no `subscribe` verb. The alternatives offered were extending the peer to a full
participant inside this item, or a separate bounded hunt item.
**Applies to:** PDA-DEC-1 closes with the blind spot recorded in
`integration-tests/pubsub-conformance/README.md`; the design's falsification gate
is **relieved by this ruling** and by nothing else. The receive-side data-sharing
defect remains owned by **PDA-ABI-7 / PDA-ABI decision 10** ("answered, not
stepped around", 2026-08-31), which starts from PDA-DEC-1's evidence table.
`gateway-fastdds-ts` remains the only harness that reproduces the defect, on both
platforms in CI — do not weaken or delete it.
