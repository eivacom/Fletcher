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

## 2026-09-01 — Buffer refill is not a zero-copy violation *(selection)*
> "Permit, publish the number — Refill is allowed and its cost is measured and reported; every OTHER byte movement fails the guard. Spec §3.1 clause 1 already sanctions movement inside a refill, and the review agrees this ratifies your zero-copy ruling rather than trading against it."

**Context:** PDA-DEC-2's design raised an oracle-wins tripwire — the 2026-08-31
ruling's *Applies-to* gloss said "a copy anywhere on either path is a violation,
not a trade", while spec §3.1 clause 4 says a growable buffer "refills". The
PDA-DEC-2 architecture review found this is **not** a genuine contradiction: §3.1
**clause 1** reads "must not move … *except inside a refill*", and §8 grounds the
row claim in `FixedWriteBuffer`. The rejected alternative banned growable send
buffers outright and made the in-process loopback non-conforming.
**Applies to:** PDA-DEC-2's `Judge()`; spec §3.1, §8. **Clarifies** (does not
supersede) the 2026-08-31 zero-copy ruling: the gloss "a copy anywhere" was
editorial, not the owner's prose. Movement inside a refill is permitted and
**published as a number**; every other byte movement is a violation.

## 2026-09-01 — The copy-accounting guard claims the interface, not the transport *(selection)*
> "Scope to the interface, say so plainly — Green means the interface performs no copies; the README states it proves nothing about a transport's internals. Honest about what is and isn't measured."

**Context:** how much PDA-DEC-2's oracle should claim about DDS. Rejected: running
the receive-side check against Fast DDS now (it would measure the currently-copying
path and call the number evidence), and enabling DDS shared-memory receive to test
it (which would reopen work the 2026-09-01 blind-spot ruling assigned to PDA-ABI-7).
**Applies to:** PDA-DEC-2's subjects are in-process; the limit is written into
`integration-tests/pubsub-conformance/README.md`, never implied. Consistent with
the 2026-09-01 blind-spot ruling — the receive-side data-sharing defect stays owned
by PDA-ABI-7.

## 2026-09-01 — The known receive-side copy is pinned at exactly one *(selection)*
> "Pin at one — Removing the copy turns this test red, forcing the next stage to come back and update the guard. Silence is how such a fix gets forgotten or half-landed."

**Context:** the one §3.2 receive-side copy that PDA-DEC-3 exists to remove.
Rejected: report-only, under which the fix could land silently or half-land.
**Applies to:** PDA-DEC-2's `Judge()` asserts the receive-side count is exactly 1;
PDA-DEC-3 must update the guard when it removes the copy. This is a
**red-on-fix tripwire**, not an accepted divergence — the 2026-08-31 divergence
ruling (which forbids pinning a divergence instead of fixing it) governs
cross-provider divergences; this is a uniform `Blob` limitation §8 already records
and decision 6 already assigns to PDA-DEC-3.

## 2026-09-01 — A live subscription's schema mode is held, not switched *(selection)*
> "Hold until resubscribe — That subscription keeps getting no shape until the client resubscribes; a later announcement reaches only new subscriptions. Under the alternative a client decodes one stream two ways with no signal — silent wrong data rather than an error."

**Context:** PDA-DEC-3. A gateway client that subscribes before any topic shape is
announced gets none; today the loopback caches a `CreateTopic` schema and hands it
to whatever subscription is live, so that subscription silently starts receiving a
shape mid-stream — which spec §7 clause 1 already forbids. The rejected alternative
was to permit the switch and loosen §7 clause 1.
**Applies to:** `SchemaCarriage::kAsDeclared` (defined as today's behaviour —
gateway `schemaIpc` is unchanged); the schema mode is **latched at first delivery**;
and the §7 clause 1 amendment restating "never mix" **per subscription**, which
lands in THIS PR. Review debt C2-1 (wording + the missing test) rides with it: no
conformance subject reaches this path today, only the gateway does.

## 2026-09-01 — One error type with a stable numbered cause *(selection)*
> "One error, numbered cause — A single error type carrying a stable numbered cause, identical across protocols. Messages stay as they are; branching on error type moves into the code."

**Context:** PDA-DEC-3's exception taxonomy (locked decision 10 requires it be
published). Rejected: today's assorted standard error types plus a prose map, which
two independent language bindings cannot mirror without drifting — the drift this
round exists to stop.
**Applies to:** `PubSubError` over `PubSubStatus` (fixed, append-only, pinned by
`static_assert`); every seam entry point translates; `kInternal` is the total
catch-all. This is what lets PDA-ABI and BIND proceed in parallel, since exceptions
cannot cross a C boundary.

## 2026-09-01 — One waiting mechanism, not two *(selection)*
> "One mechanism only — C++ callers use the same wait a C#/Rust app uses. About 10 call sites updated here; Arrow-facing callers convert the shape themselves via the new public conversion."

**Context:** PDA-DEC-3. Whether to keep the C++-only `shared_future` schema wait
beside the new `SchemaArrival`. Rejected: keeping both — two mechanisms drift, and
it leaves the binding path untested because no in-tree caller would exercise it.
**Applies to:** the three `shared_future` schema members are **retired, not
deprecated** (same shape as the 2026-08-31 `FastDDSProviderOptions` ruling);
`SubscriberArrow::SubscribeResult::schema` becomes a `SchemaArrival`; the tree's
only deep-copying import is made public as `ImportArrowSchema`.

## 2026-09-02 — An unloadable driver path is a valid selection that fails distinctly *(selection)*
> "Accept it, fail distinctly — The path is a valid selection and fails with a distinct \"this build cannot load drivers\" message, separate from an unknown name. This stage proves with a stand-in resolver that a real driver later arrives through the identical call."

**Context:** PDA-DEC-4. Configuration can name a driver file that nothing in this
build can load yet. Rejected: treating it as an unknown protocol name, under which
a path and a typo look identical to an operator and the round's central promise —
that PDA-ABI adds loading and changes nothing here — stays untested prose.
**Applies to:** `ProviderRegistry::Create` refuses a path selector with
`kNotSupported`, distinct from an unknown name's `kInvalidArgument`; the path
branch exists and is routed **in this item**, exercised by
`Registry.PathSelectorResolvesThroughTheSameCall` with a stand-in resolver and
`Registry.PathSelectorWithoutResolverIsRefusedAsUnsupported` as its live negative
control. This is what makes the no-signature-change claim executable rather than
prose.

## 2026-09-02 — The selector's shape decides name versus path *(selection)*
> "The shape decides — A plain word like `fastdds` is a name; anything else, like `/opt/x.so`, is a path. One setting, the same rule in every build."

**Context:** PDA-DEC-4. How one configuration string distinguishes a built-in
protocol from a driver file. Rejected: an explicit `file:` prefix (every existing
string and every operator must learn it, and a missing prefix silently means
something else), and two separate settings — **disqualified** because it lets an
application see which kind it has, the thing this round exists to prevent.
**Applies to:** `ProviderSelector::Parse` classifies totally and disjointly (a name
is `[A-Za-z0-9_-]+`; every other non-empty string is a path) and never consults the
registry, so a string means the same thing in every build. The design review
attacked the classification — drive letters, UNC paths, bare filenames, dotted
names — and found no realistic driver path misclassifies, and no misclassification
that would be repaired by widening the call rather than by an operator editing one
string.

## 2026-09-02 — Protocol-specific settings move into the protocol's own document *(selection)*
> "Into the document — Fletcher keeps exactly payload size and domain; everything protocol-specific lives in the document only that protocol reads. Visible cost: XRCE settings that are typed struct fields today become document lines, and Fast DDS QoS follows two stages later."

**Context:** PDA-DEC-4, whether the XRCE agent address stays a first-class Fletcher
setting. Rejected: keeping it typed at the seam, which would leave Fletcher knowing
one protocol's vocabulary — what the 2026-08-31 configuration ruling set out to
prevent.
**Applies to:** `ProviderConfig`'s typed core is exactly `{max_payload_bytes,
domain_id}`; everything else is the opaque document Fletcher copies and never
reads. Confirms the 2026-08-31 configuration ruling for PDA-DEC-7 (XRCE) and
PDA-DEC-6 (Fast DDS QoS). The typed-field loss is accepted deliberately.

## 2026-09-02 — The configuration setting holds the XML itself, not a filename *(selection)*
> "The XML text — The setting carries the profile content. The gateway grows a flag that reads a file for you, so operators keep the convenience. One setting, one meaning — and Fletcher never opens a file on a provider's behalf."

**Context:** PDA-DEC-6. Rejected: a filename Fletcher opens (which puts file access and
its failure modes into Fletcher for a provider-specific format), and accepting either by
guessing (which makes leading-`<`-versus-path a silent switch, so a malformed path is
read as XML or the reverse, with no error).
**Applies to:** the Fast DDS provider's document is the XML profile text. The
convenience of reading a file lives in the **gateway**, not in Fletcher or the seam.
Consistent with the 2026-08-31 configuration ruling and decision 8 — the document is
opaque to Fletcher and parsed only by the provider.

## 2026-09-02 — A supplied profile is that endpoint's WHOLE quality-of-service *(selection)*
> "Fast DDS's — the profile is the whole QoS. A supplied profile is that endpoint's complete quality-of-service; anything unmentioned takes the DDS default. We publish Fletcher's exact profile as a starting point, kept true setting-for-setting by a test."

**Context:** PDA-DEC-6, what happens to a policy an operator's profile omits. Rejected:
Fletcher's defaults staying underneath so a profile changes only what it mentions —
friendlier, but it requires a fact **the XML API never reports**, namely which policies a
document actually set, so it could be neither implemented reliably nor tested honestly.
**Applies to:** the no-merge rule (design §2). This is why **silence is load-bearing** in
this item, and why the omission guards exist. Fletcher's own defaults are published as a
starting profile, pinned setting-for-setting by
`DefaultProfileTranscriptionIsExact` — an in-process whole-struct equality, because
discovery cannot observe `history` or `resource_limits`.
**Note (review C2-1):** as designed, *nothing watched the policies a supplied profile
omits*, so a build implementing either answer would have passed every row. The
implementation must mandate the form (a fresh default-constructed QoS per call; the
built-in only on the not-found branch) and assert it, or this ruling is unfalsifiable.

## 2026-09-03 — The isolation claim is one machine, with its exclusions published *(selection)*
> "One application on one machine, with three exclusions in the docs — nothing about isolation between machines, vendor process-wide state, or the shared memory two *separate* processes on one machine use. Separate processes cannot share the in-memory state this stage disproves, so a wider harness adds maintenance and no evidence; it matches the scope you chose twice."

**Context:** PDA-DEC-8, how wide the published multi-instance isolation claim should be.
Rejected: buying a cross-machine harness — real added coverage, but new infrastructure
the round had not budgeted, and it would test DDS *transport* rather than the registry's
freedom from global state, which is what spec §4's third item actually requires.
**Applies to:** PDA-DEC-8's README wording and design §8. The claim is scoped to one
process on one host, within the window the same-domain positive control measured a real
crossing, with **three exclusions stated rather than implied**: cross-host isolation,
vendor process-wide state both instances would set identically, and the shared memory two
separate processes on one machine use. Consistent with the 2026-09-01 copy-accounting
ruling ("scope to the interface, say so plainly") and the 2026-09-01 blind-spot ruling —
the third time this round the owner has chosen a narrow claim stated honestly over a wide
one implied. **Review debt C2-1 rides with it:** §8 must stop publishing "exchange no
rows" for the different-bounds pair, which premise P1b makes an unearned claim.

## 2026-09-03 — The handoff states the platform evidence exactly *(selection)*
> "State the evidence exactly — all runs local on Windows plus one Linux compile — tell both later rounds to treat Linux as unverified, and make a Linux-only difference in seam behaviour a question for you rather than a local fix. Matches the three times you chose a narrow claim stated honestly."

**Context:** PDA-DEC-9, the round's last owner decision. No automated build has ever run on
`feature/protocol-driver-abi`: the CI lanes are `workflow_call` from a PR-triggered
workflow, so opening the pull request is the only trigger and that is the owner's step.
Eight items of Linux-side correctness therefore exist only as local Windows runs plus one
WSL compile of the single platform-forked file (PDA-DEC-1H's `/proc` ownership path).
Rejected: holding the handoff unsigned until the lanes pass — strongest evidence, but it
makes two downstream rounds wait on one manual step; and saying nothing about platforms,
which would let a reader assume the usual lanes backed the work.
**Applies to:** spec §12's evidence statement. The handoff names what was actually run,
tells PDA-ABI and BIND to treat Linux as **unverified**, and routes any Linux-only
difference in seam behaviour to the owner as a question rather than a local fix — because a
local fix by one round would silently change the seam both rounds share. **Fourth
consecutive time** the owner chose a narrow claim stated honestly over a wide one implied
(2026-09-01 copy-accounting scope, 2026-09-01 conformance blind spot, 2026-09-03 isolation
scope, and this). A design may now infer that preference rather than ask again.

## 2026-09-03 — The BIND-C# stop-and-ask is absorbed into PR #126, not deferred *(selection)*
> "Absorb into PR #126 now — Amend the spec and land all eight before merge, so the seam ships correct rather than shipping frozen-and-known-wrong. Strongest final artifact."

**Context:** a colleague raised a formal stop-and-ask against the frozen seam spec from the
BIND-C# implementer's frame (PR #126, 2026-09-03): nine amendments, seven touching `frozen`
text, one owner-allocated append, one correction to the sibling ABI spec. §1 and §12.1 route
exactly this to the owner — nobody acts alone on frozen text. An independent verification pass
**confirmed A1–A7 and A9**, found **A8 only partly** established, confirmed both header
corrections, and found four further defects the author missed. All eight require **product
code, not wording**, which is itself the evidence they are rulings rather than maintenance.
Rejected: opening PDA-ABI with them (which §1's mechanism arguably exists for, but would ship a
seam known to be wrong and slip the same-day handoff anyway), and pulling forward only A4 and
A5 (the two that are defects in shipped behaviour rather than gaps in wording).
**Applies to:** all eight amendments land before #126 merges. **Accepted cost, stated at the
time of the ruling:** the round reopens after its gate passed and all ten items were green and
CI-verified; the denominator grows from 10 to 18; every amendment carries a machine check its
author demanded, so each is a design-plus-implementation cycle, not an edit. Realistically this
doubles the round.

## 2026-09-03 — A re-entrancy refusal gets its own number *(selection)*
> "Allocate a new number — A distinct status lets a caller tell 'this provider can't do that at all' from 'you called back into me from my own callback', which are genuinely different operator problems."

**Context:** A8 asked the owner to allocate a `PubSubStatus` value for a re-entrancy refusal.
Verification established that the author never addressed `kNotSupported = 6` (*"This provider
does not implement the requested behaviour"*), so the premise "no existing status fits" was
**not** established — the owner ruled on the merits anyway, distinguishing the two operator
problems. Rejected: reusing `kNotSupported`, and deferring the allocation until A3's clause is
drafted.
**Applies to:** the append is **`kReentrantCall = 10`** — the next free value; `PubSubStatus` is
fixed and append-only (`core/include/fletcher/core/status.hpp`, values 0–9 today, pinned by
`static_assert`). Per §12.1 only the owner may authorise an append, and this is that
authorisation. The append moves as one change with `core/README.md`'s published table, the
`static_assert` set, and `Taxonomy.PublishedNumbersMatchTheEnum` — whose exhaustive `switch`
makes an un-published append **fail to compile**, so the guard enforces this ruling rather than
documenting it.

## 2026-09-04 — The idempotence amendment is authorised as a ninth, inside A4 *(selection)*
> "Authorise it, land it inside A4 — Cancelling something already cancelled becomes accepted and does nothing, instead of raising an error. Why it matters: a C#/Rust cleanup path calls cancel unconditionally during teardown, and a finaliser cannot let an error escape — you already ruled the bindings are full pub/sub clients, not read-only accessors. Cost: a mistyped identifier is silently ignored rather than reported. Deliberately no default-on-silence — frozen text needs your explicit word."

**Context:** PDA-DEC-A4 design review cycle 1 raised an **authorisation** tripwire. The
2026-09-03 absorption ruling authorised **eight** amendments; the Unsubscribe-idempotence
change is verification **finding #5**, not one of the eight, and reached A4 only through a
tracker line the PM wrote at round reopen (`plans/PDA-decouple-interface.md:109`). It writes
a new normative sentence into `frozen` §7, whose §12.1 *who may act* is "nobody alone" — so
it is a decision, not a repair, and the reviewer ruled a default-on-silence insufficient.
Rejected: giving it its own tracked item A9 (cleaner audit trail, costs an extra
design-plus-implementation cycle and moves the denominator to 19), and declining it (which
would ship the two-tier disagreement inside PR #126 and force the C# binding to wrap every
teardown cancel in a catch).
**Applies to:** `Unsubscribe` of an unknown or already-cancelled id is a **no-op at both
tiers**, not `kInvalidArgument`; the amendment lands **inside PDA-DEC-A4**, so the round's
denominator stays **18**. This is the ninth amendment authorised for PR #126 and the
authorisation the 2026-09-03 ruling did not cover. Consistent with the 2026-08-31 ruling that
BIND are **full pub/sub clients** — a finaliser cannot let an exception escape.

## 2026-09-04 — Cancelling waits for a delivery already in progress *(selection)*
> "Wait — Once cancel returns, that handler is not running and will not run again, so the application may immediately free whatever the handler was using. This is the memory-safety guarantee the item exists to deliver. Cost: shutdown can pause as long as the slowest handler takes. Carve-out you should see before agreeing: if a handler cancels its own subscription from inside itself, cancel cannot wait for the frame it is already in — so in that one shape the application must not free handler state on return, and that limit gets published rather than implied."

**Context:** PDA-DEC-A4, brief decision 2, carrying review debt A4-DEBT-3 so the carve-out was
visible before the owner answered rather than after. Rejected: returning immediately, under
which a message already in flight may still reach the handler afterwards and every application
must keep handler state alive indefinitely with no signal for how long — the crash class the
item was opened to remove.
**Applies to:** `Unsubscribe` blocks until any in-flight delivery for that subscription has
returned; on return the caller may free or unpin its callback state. **The self-unsubscribe
carve-out is authorised and must be PUBLISHED, not implied** — a handler cancelling its own
subscription does not wait for the frame it is in, and that is the one shape where a caller may
not free callback state on return. Fifth consecutive time the owner chose a narrow claim stated
honestly over a wide one implied.

## 2026-09-04 — The carve-out is one subscriber object, and the residual hang is published *(selection)*
> "One subscriber object — publish the hang — A handler that cancels any subscription on its own subscriber gets an immediate return instead of a wait. The published sentence becomes true as written. Cost, stated plainly: two handlers on DIFFERENT subscriber objects that cancel each other can still hang one another. Why I recommend it: a hang is loud, detectable and corrupts nothing, whereas a too-wide 'you may free' promise is a silent use-after-free — the exact defect class this item was opened to remove. Also matches the five prior times you chose a narrow claim stated honestly."

**Context:** PDA-DEC-A4 design review cycle 2 returned exactly one BLOCKER, and it was the P6
stop-and-ask. The 2026-09-04 carve-out ruling asserted uniqueness — "that is **the one shape**" —
but the design's mechanism was **two steps wider** than the sentence the owner was shown, and
wider in the unsafe direction: §1.3 published "issued from inside a delivery callback *on that
subscriber*" while §1.1 implemented it with a **file-local** `thread_local` shared by every
`Subscriber` in the process, so a handler on X cancelling on Y also skipped **Y**'s barrier —
Y's caller reads the frozen text, believes the wait happened, and frees handler state still in
use. The reviewer verified step one (within-subscriber) is genuinely necessary — narrowing it
back really does reinstate the deadlock — and step two (process-wide) is **not**, being an
artefact of where the counter was placed. Rejected: process-wide (nothing hangs, but the "you
may free" promise is off in a second, wider set of cases, including unrelated subscribers —
the case an application author is least likely to anticipate), and giving the cross-instance
hang its own tracked item (denominator 18 → 19).
**Applies to:** the delivery-depth counter is scoped to **`Subscriber::Impl`**, so the published
§7 sentence becomes true as written. The residual **cross-instance mutual cancel is handled
residue and must be PUBLISHED** in `integration-tests/pubsub-conformance/README.md` — two
handlers on different `Subscriber` objects cancelling each other can still hang. The owner's
stated reasoning is itself normative for this round: **a loud hang is preferred over a silent
use-after-free.** Sixth consecutive time the owner chose a narrow claim stated honestly over a
wide one implied. Note the 2026-09-03 licence to infer that preference permits **narrowing**
without asking, never **widening** — which is why this had to be carried.

## 2026-09-04 — A duplicate cancel waits too; the promise keeps exactly one exception *(selection)*
> "Make it wait too — A duplicate cancel waits for the same drain the first one is performing, so your promise holds with only the single carve-out you already approved. Cost: two threads cancelling the same subscription now both block until the handler finishes. Why I recommend it: this case arises precisely from the unconditional-cleanup shape your idempotence ruling exists to serve, so the C#/Rust teardown path is the most likely place to hit it — and an exception that only bites under a race is the silent use-after-free you already ruled against. The code is being restructured for the other fix anyway, so this is close to free now and expensive later."

**Context:** PDA-DEC-A4 compliance review, finding 2, established by probe (3/3 reproducible,
not by reading): a **concurrent duplicate `Unsubscribe(id)`** returned while that callback was
still running — a second, unpublished exception to the frozen "none is in progress when it
returns / you may free". It was created by the finaliser shape the 2026-09-04 idempotence ruling
itself asked for, and it contradicted the same day's carve-out ruling, which asserted uniqueness
("that is **the one shape**"). Rejected: publishing it as a second numbered limit in the header
and the harness README — smaller diff and no mechanism change, but it would leave the frozen
sentence with two exceptions, the second reachable only under a race, which is the hardest kind
for an application author to discover or test against.
**Applies to:** a duplicate `Unsubscribe` of an id already being retired by another thread
**blocks on the same drain** rather than returning as a no-op. The idempotence ruling still
holds for an id that is genuinely unknown or already fully cancelled — that stays a silent
no-op. After this, the "you may free on return" promise has **exactly one** exception, the
self-cancel carve-out of 2026-09-04, as that ruling claimed. Consistent with the owner's
standing preference, stated the same day, that **a loud hang is preferred over a silent
use-after-free** — and this trades a silent race for a bounded wait, which is the same trade.

## 2026-09-04 — Harden at the right scope rather than publish a weaker promise *(selection)*
> "One more cycle — fix at the right scope — ~3 lines plus one control. Why I changed my mind about stopping: the three previous fixes each scoped the guarantee to an approximation of the right thing, and this is the first one aimed at the object whose lifetime the promise is actually about. That is a different kind of change from another patch. Cost: one more implement-plus-review pass on an item already at 4.3x its declared size. Risk if I'm wrong: a fifth case, at which point the premise really is broken and I stop regardless."

**Context:** PDA-DEC-A4 had reached the runbook's non-convergence trigger — the same defect
CLASS ("some caller returns while a callback is still running") recurring across four review
cycles even as the instances differed, which the runbook routes to a premise escalation rather
than another cycle. The PM had also publicly committed to stopping rather than opening a third
fix cycle. What changed the reading: both reviewers, independently and from different angles,
named the same ROOT CAUSE — the deferral was scoped to the cancelling frame when the promise
needs it scoped to the gate — and proposed the identical three-line remedy. The scope sequence
had run process-wide → per-`Impl` → per-frame → per-gate, the last being the first scope whose
lifetime actually matches the guarantee. Rejected: closing A4 with the promise published more
weakly and its exceptions enumerated (cheapest, but it hands the binding authors the
growing-footnote contract A4 was opened to remove, and it is what the colleague's stop-and-ask
objected to in the first place), and closing A4 on what it had with the residual hardening
becoming a nineteenth item.
**Applies to:** PDA-DEC-A4 fix cycle 3, which landed and closed the class. **Forward-relevant
to the remaining amendments:** when a guarantee keeps leaking, the owner's preference is to fix
it at the scope the guarantee is actually about, not to publish the leak — provided the root
cause has been NAMED rather than another instance patched. The stop condition the owner set
stands for any future item: a further case after the root-cause fix means the premise is broken
and the item stops. Seventh consecutive time the owner chose the honest-and-narrow answer.

## 2026-09-04 — A topic part containing a separator is refused *(selection)*
> "Reject it outright — A loud refusal beats a silent wrong delivery. No remote client loses a working topic: Fletcher's own gateway can never produce such a part."

**Context:** PDA-DEC-A5 brief decision 1. Today the single part `"a/b"` and the two parts
`"a","b"` are the **same** topic in all three providers, so one can silently receive the other's
data — while §3.5's frozen sentence licenses a provider under which they are two. Rejected:
keeping both working by rewriting the name on the wire (preserves every name in use, but changes
wire bytes for names that work today, which decision 13 makes a **separate stop-and-ask**), and
declaring the collision intentional and documenting it (no code change, but it leaves the silent
wrong delivery the colleague's stop-and-ask was raised about).
**Applies to:** `internal::RequireSegments` refuses any segment containing `/`. Consistent with
the owner's standing preference that a **loud failure beats a silent wrong answer**, now stated
three times across A4 and A5.

## 2026-09-04 — An empty topic part is refused *(selection)*
> "Reject — Matches the existing rule that an empty topic names nothing — one rule at one level, which is what a language binding can reproduce. Neither Fletcher's gateway nor any code in the tree uses one."

**Context:** PDA-DEC-A5 brief decision 2. `{"a",""}` and `{""}` are accepted today and name
something degenerate; unlike decision 1 they are not a collision. Rejected: keeping them, which
leaves each language binding to decide independently what an empty part means — the drift this
round exists to stop. Consistent with PDA-DEC-3's in-round empty-**list** refusal, of which this
is the per-segment analogue.
**Applies to:** `internal::RequireSegments` refuses an empty segment.

## 2026-09-04 — The `__` prefix is reserved, and this is its authorisation *(selection)*
> "Reject any part starting with `__` — Closes the whole reserved namespace rather than the one name that collides today, so future companion channels cost nothing. Verified: no part starting with `__` exists anywhere in the tree. Because the text is frozen, the narrower literal fix would make every future companion name need its own ruling from you. Over-forbidding fails loudly in development; under-forbidding is the silent collision."

**Context:** PDA-DEC-A5, review debt A5-DEBT-1, found in **cycle-1 design review** — not by the
colleague's stop-and-ask. Both DDS providers derive a companion topic `name + "/__schema"`
(`fast_dds_pubsub_provider.cpp:331,494`; `xrce_dds_pubsub_provider.cpp:720,881`), so after A5's
other refusals the accepted list `{"a","__schema"}` still joins onto the schema channel of
`{"a"}`. The cycle-2 reviewer raised this as a **stop-and-ask** because `__schema` appears
**nowhere** in `PDA-DEC-9-bind-stopask-verification.md` — the document the 2026-09-03 absorption
ruling was given on — so it is a new normative refusal in frozen §3.5 that that authorisation did
not name, and §12.1's *who may act* is "nobody alone". The PM removed the brief's
default-on-silence accordingly, exactly as the 2026-09-04 idempotence ruling required
(*"Deliberately no default-on-silence — frozen text needs your explicit word"*). Rejected:
leaving the collision open (smallest change to frozen text, but a WebSocket client **can** send
such a part today, unlike decisions 1 and 2), and a safe-charset whitelist (strongest guarantee,
but it rejects dots and spaces that work today and are not wrong — it can break working setups).
**Applies to:** `internal::RequireSegments` refuses any segment beginning `__`. **This is the
tenth amendment authorised for PR #126**, after the eight of 2026-09-03 and the ninth of
2026-09-04. Note it also refuses the **prefix**, not the literal name, per the 2026-09-04
class-not-instance ruling — the whole provider-derived companion namespace is out of reach, so a
future companion name needs no further ruling.

## 2026-09-04 — A topic name is bounded at 246 bytes, leaving room for the companion *(selection)*
> "Refuse names longer than 246 bytes — Leaves 9 bytes of headroom so the hidden companion channel each DDS protocol derives (the name plus \"/__schema\") also stays under the limit — otherwise the collision just moves to the companion. Cost: a fifth refusal in frozen text, and topic names longer than 246 bytes stop working. This would be the eleventh amendment authorised for PR #126. Consistent with your standing preference: a loud refusal at development time beats a silent wrong delivery in production."

**Context:** PDA-DEC-A5 code review, the single blocking finding, **measured rather than
reasoned**: two accepted segment lists whose joined names agree on their first 255 bytes are
**one topic on Fast DDS** — a subscriber to A received all 5 rows published to B; 255 bytes
distinct, 256 aliased, controls green in every run. Cause: Fast DDS announces
`fastcdr::string_255 topic_name` and `fixed_string` truncates **silently**
(`fixed_size_string.hpp:83,331`, both `noexcept`). Both `segments.hpp` and the freshly amended
§3.5 asserted the opposite, so the item's central claim — that a topic name means exactly one
thing — was **false above the ceiling** at the moment the amendment landed. The remedy is a
fifth refusal in frozen §3.5 that the 2026-09-03 authorisation did not name, so it went to the
owner exactly as the `__` prefix did. Rejected: bounding at **255** (matches the protocol's own
limit and refuses the minimum, but a name near the ceiling still has a companion name that
overruns, so the same silent collision reappears on the hidden channel — harder to find, not
fixed), and **publishing the limit without enforcing it** (no code change, but the silent wrong
delivery stays in the shipped tree and a language binding has no way to enforce it).
**Applies to:** `internal::RequireSegments` refuses a segment list whose **joined** length
exceeds **246 bytes**. This is the **eleventh** amendment authorised for PR #126 — eight on
2026-09-03, the ninth (idempotence) and tenth (`__` prefix) on 2026-09-04, and this. Note the
headroom is what makes the rule close the class rather than move it, the same reasoning that
made the `__` refusal a prefix rather than a literal. Eighth consecutive time the owner chose the
honest-and-narrow answer, and the third time in two items that a **loud refusal was preferred to
a silent wrong answer**.

## 2026-09-04 — The seam permits an uncopied row end to end *(selection)*
> "Whole path — the seam permits an uncopied row end-to-end — Worded as PERMITS, not guarantees: a client that uses the new call can send without any copy; a client that ignores it still copies and the seam cannot stop it. Why: you ruled zero-copy a requirement rather than a trade, and the narrow promise is true but useless to the C#/Rust clients it exists to serve. This widens frozen §8, which your 2026-09-04 ruling says may not be inferred — hence no default. Review confirmed the amendment makes §8 achievable rather than weakening it."

**Context:** PDA-DEC-A1, brief decision 1, presented **with no default on purpose** because it edits
frozen §8 — the zero-copy property itself — and §12.1's *who may act* is "nobody alone". The
2026-09-03 absorption ruling authorises *amending* §8 for this amendment but **not the direction**,
and this option is a **widening**, which the 2026-09-04 narrow-claim ruling forbids inferring. Root
cause, verified verbatim by the design review: §8's promise and §8.1's measurement **both begin at
the window** — "the window base after the encoder's last append" (`spec:831`) — the harness README
excludes "the encode itself", §3.1 clause 5 gives the window a **readable** end only, and no public
member advances the write cursor without a source. The window's **write end was never part of the
contract**, which is why a binding handed a `WriteBuffer` cannot fill it without copying *and* why
`CopyAccounting` cannot see that copy. Rejected: keeping the narrow promise and publishing the gap
as a named limit — true, but it ships the seam telling language-binding authors in writing that the
cost it advertises is unavailable to them, which is the objection the colleague's stop-and-ask
raised; the review also noted it concedes a copy on the **row path**, which locked decision 7 makes
a stop-and-ask in its own right.
**Applies to:** §8's promise scope, amended to the whole send path. The wording is **permits, not
guarantees** — a client that ignores `AppendInPlace` still copies and the seam cannot stop it.
**Twelfth amendment authorised for PR #126.**

## 2026-09-04 — The guard claims what the interface permits, measured with a stand-in *(selection)*
> "That the interface PERMITS an uncopied send, said plainly — Measured with a stand-in client, and the README states exactly that limit rather than implying more. Why: no real C#/Rust binding exists to measure yet, so any wider claim would be unearned."

**Context:** PDA-DEC-A1, brief decision 2. Rejected: claiming that real C#/Rust clients send without
copying — a stronger headline, but nothing in the tree can measure it because the bindings do not
exist, so the claim would rest on a stand-in standing for something unbuilt.
**Applies to:** `integration-tests/pubsub-conformance/README.md`'s claim limit for the new
`encode_copies` measurement. Directly consistent with the 2026-09-01 copy-accounting ruling ("scope
to the interface, say so plainly"). **Ninth consecutive time the owner chose the narrow claim stated
honestly over a wide one implied** — and the second time in this item alone.

## 2026-09-05 — Every protocol driver is C++, so both sides of the protocol ABI are C++ *(scope)*
> "We decide that all protocol drivers will be C++, and so both sides of the protocol ABI will be pure C++. You only need to make aspects of the ABI fully language agnostic in cases where for instance data drills all the way up to the language ABI further up."

**Context:** Given 2026-09-05 during a re-plan of the remaining amendment items, in direct response
to the PM quoting `docs/protocol-driver-abi-spec.md:21-22` ("implementable by anyone in any
language, in a separate binary") and §0.2 ("A driver written in Rust or C# implements role 1 and is
entirely legitimate"). The owner was shown that text and ruled against it. It is therefore a
**deliberate supersession, not an oversight**.
**Supersedes:** `docs/protocol-driver-abi-spec.md` §0.2's multi-language driver licence and the
:21-22 claim. Both require amendment; until amended, the spec contradicts this ruling and **this
ruling wins** (`spec_precedence` standing decision applies to the seam spec; this ruling extends the
same precedence over the driver ABI spec).
**Applies to:** the language-agnosticism test for every remaining amendment item. The test is no
longer "could a non-C++ implementer construct this?" but **"does this data drill all the way up to
the language ABI?"** A concept that stops at the protocol ABI may be expressed in C++. A concept
that continues up to the language boundary must still be language-agnostic.
**Does NOT relax:** binary-stability constraints that hold between separately-built C++ binaries
(exceptions may not cross, layout may not be assumed) where those are separately established — the
driver ABI spec's own "Neither may throw or longjmp" (`:190-192`) stands on its own footing, as does
seam §5.3's framing around the transport's C frames.

## 2026-09-05 — Ceremony is expensive; group the remaining work into as few items as makes sense *(process)*
> "Ceremony in the round plan is expensive. Group the works into as few rounds as makes sense"

**Context:** Given 2026-09-05 after the PM measured the amendment round at ~5,000 lines of code
against ~12,400 lines of docs/plans (a 2.5:1 docs-to-code ratio, against 0.7:1 in the main round),
and showed that per-item process cost is fixed regardless of item size — eight small items were
each paying a ceremony calibrated for items roughly three times their size.
**Applies to:** the granularity of the remaining amendment items. Grouping is the PM's call under
the `naming` standing decision ("stage granularity"), and this ruling directs it toward **fewer,
larger items**. The cost being traded away is review independence per item: grouped items receive
one compliance pass and one code review between them rather than one each.
