# PDA-DEC-2 — architecture review (cycle 1 of 2)

Design: `plans/PDA-DEC-2-copy-accounting-oracle.md` @ `ec0897a`. Brief:
`plans/PDA-DEC-2-brief.md`.
Oracles: `docs/pubsub-interface-spec.md` §8/§8.1/§3.1/§3.2/§7.2;
`plans/PDA-decouple-locked-decisions.md` 2, 3, 6, 7, 11, 12, 13, 14; the PDA-DEC
rulings ledger.

**Verdict: APPROVE-WITH-DEBT(7).** No BLOCKERs. The forcing test can be made to go
red by a control that ships with it, the mechanism is sound for both registered
subjects, and the scope boundary against the transports is declared rather than
implied. Seven survivable items go to the register.

---

## Findings

### DEBT-1 — the measured span is narrower than the design's own definition of a copy

The design defines a copy as payload bytes coming to exist at a second address
"by code **at or above the seam**, between the encoder's first write and the
subscriber callback's return". But both subjects hand the harness a
`shared_ptr<PubSubProvider>` and the legs call `Publish` / the raw
`SubscribeCallback` directly, so the measured span is *provider entry → callback
return*. Everything above the seam — `Publisher::Publish`, `Subscriber`'s fan-out
lambda — is outside every measured path, and this is nowhere declared. The Risks
section declares only the *transport* blind spot.

This matters because spec §8's own zero-copy claim is worded across those layers:
"Attachments: already there **publisher → provider → subscriber**, via
`shared_ptr`." §8.1 is what makes *that* claim falsifiable.

Verified in the tree, so nothing is red today:
- `pubsub/src/publisher.cpp:70-73` — `Publish` is a straight forward of `encoder`
  and `attachments` by reference. No copy.
- `pubsub/src/subscriber.cpp:81-88` — the provider callback forwards `data`, `len`
  and `att` by reference to every entry in the snapshot. No copy.

So the gap is a *guard* gap, not a live defect: a copy added to `Subscriber`'s
fan-out (the obvious place someone would materialise a `std::vector` "for
safety") would be silent, and PDA-DEC-3 is scheduled to ripple through exactly
these files (§10). Also fold in here: the README's "what it does not prove"
should name the **Fast DDS / XRCE publish-side loan path** explicitly — the
existing `LoanedRoundTrip`, `LoanedDeliversAttachments` etc. in
`fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp:346,573` assert
*delivery over* the loan path, never *absence of copies*, so a reader could
easily believe that ground is already covered.

*Owed:* either register a third in-process subject that publishes through
`Publisher` and receives through `Subscriber` (~25 lines, closes the gap
outright), **or** narrow the copy definition to "by the provider, between
`Publish` entry and callback return" and put the above-seam boundary in the
README beside the transport one. Either is fine; leaving the definition and the
measurement disagreeing is not.

### DEBT-2 — the row rule should be equality, not containment

`row_copies = 0 if [delivered_data, +delivered_len) lies inside
[encode_base, +encode_len)`. When the lengths are equal, containment degenerates
to identity, so this is only weaker than identity for a delivered sub-range — and
in that one shape it admits a **copy that preserves the observed address**: a
provider that `memmove`s the payload down to the window base (stripping a
prefix it framed itself) delivers `encode_base` with a shorter length, every
payload byte has moved, `memcmp` against the written bytes still passes for the
delivered range, and `Judge()` scores 0. That is the only false-pass shape I could
construct that does not require undefined behaviour, and it costs one line to
close.

Both registered subjects satisfy strict equality today: `SeamProbe` delivers
`slot.base` with the encoder's `Position()`; `InProcessPubSubProvider::Publish`
delivers `buf.data()`/`buf.size()` where `buf` came from
`VectorWriteBuffer::Finish()` — whose `buf_.resize(pos_)` is a shrink (no
reallocation) followed by a move, so the block address is the one `Data()`
reported.

*Owed:* `row_copies = 0` iff `delivered_data == encode_base && delivered_len ==
encode_len`; a subject that legitimately delivers a sub-range must say why in the
subject table before the rule is relaxed for it.

### DEBT-3 — the premise that makes provenance immune to allocator reuse is unstated

The mechanism argument's load-bearing word is *live*: "a copy into recycled
storage still lands at a different address than the **live** encode window". That
is true only while the encode window is still allocated at delivery time. For
both registered subjects it is (SeamProbe's arena outlives the call; the
loopback's `buf` is alive across `cb(...)` at `in_process_provider.cpp:76-89`),
and any subject for which it is false is delivering a dangling pointer, which is
a worse bug. But it is a premise, it is not in the premise list, and PDA-ABI is
explicitly expected to register a third, driver-backed subject into this same
shape.

*Owed:* a **P5** — "the bytes `encode_base` names remain allocated, unfreed, until
the subscriber callback returns; a subject that frees or recycles the encode
window before delivery makes address provenance unsound and must not be
registered" — with the same STOP-AND-ASK shape as P2.

### DEBT-4 — P3's stop condition does not follow from the design's own rules

P3 reads: "`Attachments` crosses the seam by const-ref, with no map copy. Verified
in `provider.hpp`. **If false, leg 2 is red on landing**". It would not be. Leg 2
compares each blob's `data()` pointer, and a by-value `Attachments` map copy
copies `shared_ptr`s, leaving every `data()` identical — `attachment_copies == 0`,
green. Under the design's own definition (payload bytes = the bytes of each
`Blob`) that is the *correct* answer; only the stop condition is wrong.

The premise itself is true: `pubsub/include/fletcher/pubsub/provider.hpp:93-94`
and `:123-125` both take `const Attachments&`, so nothing is at risk today.

*Owed:* restate P3's consequence — a map copy is a per-delivery allocation, not a
payload copy, and the oracle will not and should not see it; if the const-ref
disappears, that is a §3.2 finding raised in prose, not a red leg.

### DEBT-5 — the 4 KiB leg only exercises relocation under an append pattern the design does not pin

The stated reason for the second size is that "a large row is what forces a
growable provider buffer to relocate". Whether it does depends entirely on the
harness encoder's append granularity, which the design leaves open:

- One `Append(data, 4096)` takes `VectorWriteBuffer::AppendSlow`'s `len >= kChunk`
  bulk path (`write_buffer.hpp:119-125`) on an empty buffer: one `reserve`, one
  `insert`, **nothing already-written relocates**, `refill_bytes == 0`.
- Sub-`kChunk` appends go through `Refill` → `Reserve`
  (`write_buffer.hpp:138-149`) and reallocate at 512, 1536, 3584 …, relocating
  512/1536/… bytes.

Only the second shape produces the evidence Brief Decision 1 asks the owner to
rule on, and only it distinguishes the 4 KiB leg from the 64 B one (which never
relocates: its single refill happens at `Position() == 0` and is correctly not
counted).

*Owed:* pin the 4 KiB row to many sub-`kChunk` appends in the subject table or the
leg description, and say that `refill_bytes > 0` on that leg is the expected
observation.

### DEBT-6 — `Data()`'s documented lifetime is incomplete

"valid until the next append that refills" misses two things a caller can trip
over: `VectorWriteBuffer::Finish()` also invalidates it (it nulls `data_`,
`write_buffer.hpp:106-113`), and only `[Data(), Data() + Position())` is defined —
bytes past `Position()` are whatever `resize` last left there.

*Owed:* both clauses in the header doc line and in the §3.1 sentence.

### DEBT-7 — Brief Decision 1 cites the weaker of the two applicable spec clauses

The brief tells the owner: "spec §3.1 clause 4 says 'a growable one refills'".
Clause 4 says a growable buffer refills; it does not say the already-written bytes
may **move**. The clause that does is §3.1 **clause 1**: bytes already written
"must not move or be flushed **except inside a refill**, which must preserve them
verbatim" — an explicit, normative sanction for exactly the movement the brief is
asking about. §8 reinforces it by grounding the row claim in "`Publish`'s
inversion and `FixedWriteBuffer`", i.e. the non-growable path.

*Owed:* swap the citation (same line count, the brief is at 60/60). See "Brief
Decision 1" below for why this changes what the owner is being asked.

---

## The seven questions the PM put

**1 — Is the control genuine?** Yes, in both degenerate directions. An instrument
stubbed to always report `row_copies == 0` fails `StagingIsCaught` (which demands
1 and 2). An instrument that is inert — `Data()` returning null, the ledger never
captured, `encode_base` cached before the first append — leaves the *real*
subjects red, not green, because a null or stale base fails the containment test.
The dangerous middle case (control green, real subject green, real copy present)
would need the instrument to score `StagingProbe`'s scratch-vector copy correctly
while missing a copy on the `VectorWriteBuffer` path; both go through one capture
path and one pure `Judge()`, so there is no per-path branch for that to hide in.
The plausible regressions on the real path — `Finish()` returning a trimmed copy,
`Publish` staging into a second vector, `Attachments` copied by value into a new
map with fresh vectors — all land at a different address and all go red. The
remaining hole is DEBT-2 (identity-preserving in-place move) and it is one line
to close.

One structural note in the control's favour: `StagingProbe` is deliberately *not*
in the parameterised subject list, so rung-1 item 2 ("every registered subject
faces the same numbers") stays true while the control still runs the same
`Judge()`. That is coherent, not a contradiction.

**2 — Is address provenance sound?** Yes, with two named boundaries.
- *Pool reuse returning the same address* cannot produce a false pass while the
  encode window is live, because a live allocation cannot be handed out twice.
  Getting a same-address copy requires free-then-allocate, and a freed encode
  window delivered to a callback is a use-after-free, not a copy. This is exactly
  why DEBT-3 asks for the liveness premise to be written down rather than left
  implicit in the word "live".
- *A copy into the same buffer* is the real hole, and it is DEBT-2. Content
  `memcmp` does not close it — a memmove preserves content by construction.
- *Compiler/allocator coincidence* is not a hazard here: nothing is optimised away
  because the addresses are stored in a ledger the test reads, and the comparison
  is between two pointers that must both be dereferenceable.

Alternatives rejected for correct reasons. The Windows point against
`operator new` replacement is the decisive one and it is right: a provider DLL
with its own CRT is exactly where a loaded driver will live, which is where the
counting mechanism is blind. Nothing about the rejection is over-stated.

**3 — Coverage vs the ruling.** The ruling requires zero-copy for rows and
attachments on publish and receive. This oracle measures two in-process subjects
and declares, in the Summary, the Risks section and the README, that it says
nothing about any transport. That is honest and it is spec-consistent: §8 scopes
zero-copy to "a property *of this seam*", and decision 12 / §7.2 already record
that a single-process harness cannot see transport behaviour. It is also the
shape the owner's 2026-09-01 ruling blessed for PDA-DEC-1 — ship the guard, write
the blind spot down.

Cross-checked against the data-sharing defect: receive-side data-sharing is off by
default and owned by PDA-ABI-7; P4 says so and forbids adding a DDS subject here
if that changes mid-item. Correct — registering a DDS subject would measure the
serialize-and-copy path and report it as evidence, which is the failure mode
§7.2's last paragraph exists to prevent.

The one thing the declaration should sharpen is the *publish-side* loan path,
which is in-process observable in principle and is not measured — folded into
DEBT-1.

**4 — Is pinning the receive-side copy at exactly one acceptable?** Yes, and the
2026-08-31 divergence ruling does not apply. That ruling forbids pinning a
**cross-provider divergence** as known-divergent instead of fixing it. The
receive-side copy is not a divergence — it is a uniform property of
`Blob = shared_ptr<const vector<uint8_t>>` that every provider shares, the oracle
(§8) already records it as "Receive: *not* there", and locked decision 6 assigns
the fix to PDA-DEC-3 by name ("that it is fixed is not optional"). Nor is it
"accepting a copy" under decision 7: the design does not grant it a pass, it
measures it at a constant and arms a test that goes red the moment PDA-DEC-3
removes it. That is the opposite of pinning-instead-of-fixing, and it is what
makes PDA-DEC-3's own forcing test expressible.

**5 — The Brief Decision 1 tripwire.** My read: **it is not a genuine
spec-vs-ruling contradiction**, and the design's framing overstates it — though
raising it is still the right call and the recommended default is right.

- The spec is the oracle and it wins over the digest and over the ledger. §3.1
  clause 1 says bytes already written "must not move or be flushed **except inside
  a refill**, which must preserve them verbatim". That is an explicit permission
  for in-refill relocation, not a silence to be read against the ruling.
- §8 grounds the row zero-copy claim in "`Publish`'s inversion and
  `FixedWriteBuffer`" — the non-growable path. The growable buffer's refill was
  never inside the property §8 asserts.
- The phrase "a copy anywhere on either path is a violation, not a trade" is the
  ledger's **Applies-to annotation**, not the owner's prose (the owner's words are
  "I'm afraid it is necessary yes, and also for attachments"). Locked decision 7's
  blanket wording is the digest, and the digest loses to the spec by decision 1's
  own terms.

So option (b) is not a concession — it is the status quo of the oracle. Raising it
is still correct, because decision 7 says "accepting a copy anywhere on the row or
attachment path is a stop-and-ask" and a reviewer should not read that down on the
owner's behalf. But the owner should be told that (b) *is what §3.1 already says*,
so the decision is a ratification rather than a trade-off. Hence DEBT-7. If the
owner still answers (a), note the consequence the brief already flags: no growable
buffer reaches zero relocations for an arbitrary row size, so (a) effectively bans
`VectorWriteBuffer` from the conforming set, which is a much larger change than
"~40 lines of loopback rework".

**6 — Scope: is `WriteBuffer::Data()` PDA-DEC-2's to add?** Yes. I am answering
premise **P1** so the implementer does not stall on it:
- It introduces no type, changes no ownership rule, and does not touch the method
  set — so it is not the §3 vocabulary work decision 4 and PDA-DEC-3 own.
- §3.1 already declares the window to be `{data, capacity, pos}` and that a C view
  must honour it, so the state is already normatively visible; this only makes it
  readable from C++.
- Decision 11 orders the guards **before** the vocabulary work. Requiring
  PDA-DEC-3 to add the accessor first inverts the round's own ordering.
- Verified: `core/include/fletcher/core/write_buffer.hpp` has no such accessor
  today (`Position()` and the patchers only), and the oracle genuinely cannot work
  without it — `SeamProbe` knows its own arena base, but nothing outside
  `VectorWriteBuffer` can see `buf_.data()`.

New public surface **= 1** is accurate: the harness header lives under
`integration-tests/pubsub-conformance/`, a standalone non-installed CMake project
(`project(pubsub-conformance-integration)`), following `subject.hpp`'s precedent.
No ABI surface (decision 14) — rung-1 item 6 forbids each named construct
explicitly. Nothing above the seam branches on built-in vs loaded (decision 3):
the oracle sees only `PubSubProvider`. No wire-format assertion (decision 13).
No construct scheduled for deletion: the `Files-to-delete: none` claim is real
(spot-checked — the `Loaned*` and `DataSharing*` tests in the Fast DDS provider
assert delivery over those paths, never absence of copies, so nothing is
superseded).

**7 — Budgets.** Design doc is **267** lines, not the 245 the tracker carries —
still comfortably ≤300, so no breach, but the tracker number is stale and worth
correcting. Brief is 60/60. `+560 / −5` is proportionate for what it buys: roughly
120 lines of it is `SeamProbe`, and that earns its keep twice over — it is the base
for the negative control and the only vehicle for leg 3's arena-backed stand-in
for a loaned sample, which the loopback cannot express. No scope cut warranted, no
stage split.

---

## Tree claims spot-checked

| Claim | Result |
|---|---|
| `WriteBuffer` has no window-base accessor today | Confirmed — `write_buffer.hpp:52-79` |
| P2: loopback delivers synchronously, callback gets the encode buffer's bytes | Confirmed — `in_process_provider.cpp:74-89`; `Finish()`'s shrink cannot reallocate, the move preserves the block |
| P3: `Attachments` crosses by const-ref both directions | Confirmed — `provider.hpp:93-94`, `:123-125` |
| No existing test asserts zero-copy | Confirmed — `Loaned*`/`DataSharing*` assert round-trip delivery only |
| `core/**` missing from the conformance CI filter | **False** — it is present at `.github/workflows/ci.pr.yml:251`. Corrected in the design as a NIT; the `ci.pr.yml` Files-to-touch entry is removed and nothing is owed |
| Harness shape supports a second target + `gtest_discover_tests` | Confirmed — `integration-tests/pubsub-conformance/CMakeLists.txt:104-213` |
| PDA-DEC-1's blind spot is recorded in the harness README | Confirmed — `integration-tests/pubsub-conformance/README.md:91-145` |

## NITs fixed silently

- The CI-path-filter risk bullet and its `Files-to-touch` entry, replaced with the
  verified fact.
