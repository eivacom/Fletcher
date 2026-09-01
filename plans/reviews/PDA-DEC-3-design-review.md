# PDA-DEC-3 — architecture review (cycle 1 of 2)

Design: `plans/PDA-DEC-3-crossing-vocabulary.md` @ `e82fd8f` (300 lines).
Brief: `plans/PDA-DEC-3-brief.md` (60 lines).
Reviewed **as a specification**: two later rounds derive C types from this wording
without being able to consult each other.

**Verdict: NEEDS-REWORK — 4 BLOCKERs, 9 DEBT.**

The `Blob` half is right, including on the highest-value question (see "Rulings
asked of me", Q1). Every BLOCKER is on the **schema-arrival half** or on budget.

---

## BLOCKERs

### B1 — The schema arrival collapses three distinct outcomes into one null, and deletes a typed signal that ships today

Design §2 + rung-1 item 4: a resolver destroyed unresolved "resolves the arrival
with a **null** schema"; `SchemaArrival::Ready(nullptr)` is also how a schema-less
transport reports itself (§7 clause 1, and `in_process_provider.cpp:97` today).
`Wait` returns `bool` + `SharedSchema*`, so both surface identically: *ready,
schema null*.

These are not the same fact, and the tree already distinguishes them:

- `fastdds-pubsub-provider/src/internal/schema_channel.hpp:47` — `Break()` sets an
  **exception** on the promise.
- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:779` — `set_exception(...
  "XRCE: unsubscribed before schema arrived")`.
- `gateway/src/ws_session.cpp:230-243` **branches on it today**: "A ready future can
  still hold an exception — e.g. the provider breaks the promise when a
  subscription is dropped before the schema arrives."

So the design does not merely leave a distinction unmade; it **removes a typed
failure signal that exists**, in the same item that introduces `PubSubStatus`
precisely so no failure crosses the seam untyped. A binding built on this wording
cannot tell an application "this subscription will never get a schema" from "this
transport has no schemas — bring your own". On a schema-carrying transport those
demand opposite handling, and §7's own stated failure mode for guessing wrong is
silent wrong-slot decoding, not a crash. Note also that a provider-side schema
*failure* (`OwnedSchema::DeepCopy` throwing at `xrce_dds_pubsub_provider.cpp:231`)
has nowhere to go at all under the new type.

The same three lines carry the second half of this defect. The stated C form —

> `int wait(handle, int64_t timeout_ms, ArrowSchema** out)`

— does not say what the `int` means (boolean readiness? a `PubSubStatus`?), does
not say what a negative `timeout_ms` means (POSIX-`poll` infinity, or refusal), and
above all **does not say who owns `*out`**. §3.3 says `SharedSchema` needs §3.2's
treatment — a handle plus retain/release — yet the arrival hands back a bare
`ArrowSchema*`. Three competent readers get: (a) borrowed for the lifetime of the
arrival handle; (b) a new reference the caller must release; (c) an Arrow C Data
Interface object on which the caller calls `->release()`. Reader (c) is the natural
reading of a bare `ArrowSchema*`, it is what `arrow::ImportSchema` does, and it
**destroys a shared schema under every other holder**. That is the round's declared
worst outcome: one sentence, two rounds, incompatible and memory-unsafe results.

*Acceptable fix (one edit, ~4 lines of design):* make the arrival's outcome
three-valued and typed, and pin the schema's ownership. Concretely: `Wait` yields
`{PubSubStatus, SharedSchema}` (or a tri-state `kPending` / `kResolved` /
`kNoSchemaWillArrive`), with `kOk`+null reserved **only** for a schema-less
transport and a distinct value for abandoned-or-failed; state the C form as
`fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)` where `fl_schema` is
the §3.2 owner-handle pair, state that a boundary must **never** call the C Data
Interface `release` on a shared schema (only the owner handle's release), and
state the negative/`INT64_MAX` timeout convention. Add one named test for the
abandoned path — nothing in the current forcing-test mapping exercises
`SchemaArrival` beyond the happy path. Forbidding is not cheaper here: a resolver
destroyed during teardown is the legitimate path, so the case must be *handled and
distinguishable*, not refused.

### B2 — `SubscriberArrow::SubscribeResult::schema` is not a `SharedSchema`, and the design's replacement removes the only safe Arrow import in the tree

Design §2 and Files-to-delete say all three `shared_future` schema members become
`SchemaArrival`, and that the two `std::async(std::launch::deferred, …)` wrappers
in `subscriber_arrow.cpp` go with them. But the third member is
`std::shared_future<std::shared_ptr<arrow::Schema>>`
(`pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp:48`), not
`SharedSchema`. `SchemaArrival` as specified cannot carry it.

So the Arrow tier's callers — `integration-tests/fastdds-xrce-interop/tests/
test_interop.cpp:301,364,446`, `integration-tests/pubsub-arrow-fastdds/tests/
test_roundtrip.cpp:87,219,316`, `pubsub-arrow/tests/test_pubsub_arrow.cpp:125` —
stop receiving an `arrow::Schema` and must import one themselves. The wrapper being
deleted (`subscriber_arrow.cpp:24-34`) exists *because that import is unsafe done
naively*: `ImportFromNano` does `OwnedSchema::DeepCopy` first, since
`arrow::ImportSchema` **consumes** the schema it is given. It is `static` in an
anonymous namespace, so no caller can reuse it. The design therefore deletes the
one correct implementation and hands every Arrow-tier caller the same
release-a-shared-schema footgun as B1, silently.

The Stage Brief discloses the narrowing as "waiting forever silently is gone". It
does not disclose that the Arrow tier stops returning an `arrow::Schema`. The owner
is being asked to approve Decision 3 without that fact.

*Acceptable fix (~3 lines of design + 1 brief line):* name the replacement. Either
`SubscriberArrow::SubscribeResult::schema` keeps an Arrow-typed arrival (declare it
and count it in the surface), or it becomes a `SchemaArrival` **and**
`SubscriberArrow` exposes the deep-copying import as public API so no caller ever
calls `arrow::ImportSchema` on a shared schema. Add the type change to the brief's
Deleted section.

### B3 — `SchemaMode::kAbsent` is undefined against the behaviour the loopback ships, so rung-1 item 5's "mixing is unrepresentable" is unearned

Rung-1 item 5 claims §7 clause 1's "must never mix the two" becomes "a property of
the object, not of a code path", via a construction-time `SchemaMode`. The design
never says what `kAbsent` *does*, and the two available meanings contradict two
other statements in the same document.

Today's loopback is not "null throughout". `InProcessPubSubProvider::CreateTopic`
caches a declared schema (`in_process_provider.cpp:66-68`); `Publish` reads it per
sample (`:86`); `Subscribe` returns it (`:97`); and the gateway feeds it —
`ws_session.cpp:163-173` forwards a publisher-supplied JSON schema into
`CreateTopic`, then forwards it to WS clients as `schemaIpc`/`schema`
(`ws_session.cpp:236-239`). So:

- If `kAbsent` means "null throughout", the gateway's clients stop receiving
  `schemaIpc` — contradicting the design's own "`kAbsent` default — gateway
  unchanged" and the brief's Decision 1(a). That is a **STOP-AND-ASK**.
- If `kAbsent` means "today's behaviour", then mixing is *not* unrepresentable, and
  a live §7 clause-1 divergence survives: a subscriber on topic T receives
  null-schema samples, a publisher then declares T, and subsequent samples on the
  **same subscription** carry a non-null schema. That is exactly the mid-stream mix
  clause 1 forbids, and the 2026-08-31 divergence ruling forbids pinning it.

Either way the sixth conformance subject's meaning is undefined, because
`InProcessLocal` is registered `SchemaMode::kAbsent` today
(`integration-tests/pubsub-conformance/subjects/inprocess_main.cpp:24`) and
`InProcessCarrying` is defined only by contrast with it.

*Acceptable fix (~3 lines of design):* state normatively what `kAbsent` delivers
when `CreateTopic` supplied a schema, and reconcile it with §7 clause 1 in the same
edit — the item already touches the spec. The cheapest honest shape is to make the
axis per-instance and truthful ("`kAbsent` carries a schema only where a publisher
declared one on this instance, and once declared it is carried for that topic for
the life of the instance"), plus the one behaviour fix that removes the mid-stream
mix: a subscriber that has already been delivered null-schema samples on a topic
keeps seeing null for that subscription. Refusing `CreateTopic`-with-schema in
`kAbsent` is the cleaner forbid but breaks the gateway, so it is a stop-and-ask,
not a fix I can accept on my own authority.

### B4 — Budget: new public surface 6 against a budget of 3

`.claude/runbook.PDA-DEC.config.md:146` — `new_public_surface: 3`, with the comment
"PDA-DEC-4's registry is the one item expected to need more". The design declares
**6** (`Blob` reshaped, `SchemaArrival`, `SchemaResolver`, `SchemaMode`,
`PubSubStatus`, `PubSubError`) and nets it against 5 retirements. The budget key is
`new_public_surface`, not *net*; the retirements are real (I verified all five —
`MakeReadySchemaFuture` at `provider.hpp:48`, the three futures, the `Blob` alias)
but they do not make the new surface smaller. B2 may push it to 7.

This is disclosed honestly in both design and brief, and the overrun is structural,
not sloppy: five of the six are mandated by decision 5/6, §3.4 and decision 10.
Line count (+900/−330 = 1230) sits just under the design's own ~1300 split
threshold, which is a declared number, not a hidden one.

*Acceptable fix (PM decision, not a redesign):* either take the design's own
sanctioned split (PDA-DEC-3a: ownership + taxonomy + provider migration; PDA-DEC-3b:
schema arrival + ripple + sixth subject), or record an explicit budget waiver in
`.claude/runbook.PDA-DEC.config.md` beside the PDA-DEC-4 comment, surfaced to the
owner. **I do not recommend the split**: the design's argument against it is sound —
it would leave `shared_future` and `SchemaArrival` coexisting for a stage, i.e. the
coexistence bridge rung-1 item 9 forbids. A recorded waiver is the cheaper and more
honest resolution. The one real scope cut available is dropping `SchemaMode` and the
sixth subject to a later item (surface 5), which I also do not recommend, since
PDA-DEC-1's README already promises the subject lands here.

---

## Rulings asked of me

**Q1 — Is the vocabulary genuinely C-expressible without being an ABI? Yes, and the
design is right for a reason it does not quite state.** The worry — that
`shared_ptr<const void>` is a C++ type whose C form the two rounds will re-derive
differently — does not bite, because **the two C boundaries never exchange structs
with each other**. Each wraps the same C++ `Blob` from its own side: PDA-ABI builds
a `Blob` whose `owner` retains a driver's loan; BIND hands a C# caller a handle that
owns a `Blob` copy. Interop happens through the C++ value in the middle, so layout
divergence between the two C structs is harmless — which is exactly what decision 2
/ §1 already say ("structurally similar … a consequence, not a dependency"). The
normative sentence the design does write ("`owner` keeps `[data, data+size)` alive
for as long as any copy of this `Blob` lives"; retain/release thread-safe; release
never throws or re-enters; an argument blob is borrowed and a callee that keeps it
copies) is sufficient for each round to derive its own form without inventing.
Ownership and lifetime (pressure-test 3) are answered normatively, not left to
implementers. See DEBT-9 for the one wording guard I want added.

**Q2 — Does §3.3 require `SharedSchema` to be reshaped? No. P2 is correct; do not
stop-and-ask, do not split the item.** §3.2's "treatment" is the *five written
clauses*, which is what §3.3 imports. The clause that forces a **shape** change for
`Blob` is the separate "Consequence this round must deliver" paragraph, and its
stated ground is "Today's `Blob` cannot [carry memory it does not own]". That ground
does not exist for `SharedSchema`: `MakeSharedSchema`
(`pubsub/include/fletcher/pubsub/owned_schema.hpp:81-85`) already returns
`SharedSchema(owner, owner->get())` — the aliasing constructor, i.e. it is *already*
owner-plus-pointer, and can already alias memory Fletcher did not allocate. It needs
a length no more than an `ArrowSchema*` does. The written rule is all that is owed.
(B1's second half is not a contradiction of this: the gap there is the *arrival's*
C form omitting the owner handle, not `SharedSchema`'s shape.)

**Q3 — The Fast DDS loanable-read residue (design residue 14): an acceptable
in-scope reduction, not a violation being normalised. Do not escalate.** Decision 7
says a copy on the attachment path is a stop-and-ask, but spec §8 states receive-side
zero-copy is "*not* there" and §11 defers the loaned-sample path to PDA-ABI by name;
`out_of_scope` in the runbook repeats it ("Zero-copy RECEIVE delivery (enabled here,
delivered in PDA-ABI-7)"). The spec outranks the digest by the round's own
`spec_precedence` standing decision. One copy per *sample* down from one per
*attachment*, with a named owner and a raised tripwire, is the reduction this item
is scoped to deliver. The design raised it rather than designing around it, which is
the correct behaviour. DEBT-7 covers the one thing owed: the flipped pin must not be
allowed to read as a claim about a transport.

**Q4 — The PDA-DEC-2 pin flip is deliberate and visible; the tripwire's purpose is
served, not merely its letter.** I checked the tripwire's own text
(`integration-tests/pubsub-conformance/include/fletcher/conformance/copy_accounting.hpp:210-214`),
which names the *exact* residual it feared: "a PDA-DEC-3 that leaves `Blob`
untouched and adds a PARALLEL borrowed-blob type trips neither this nor the runtime
pin." The design does the opposite — `Blob` itself changes, with **no implicit
conversion** from the old alias — so the `static_assert` fires at build time before
any test runs, the ctest name changes (`…CostsExactlyOneCopy` → `…CostsNoCopies`, so
`ctest -N` shows it), and P5 forbids relaxing rather than replacing the assertion.
The three-valued leg survives: `copy_clauses.cpp:196-200` still scores the
deep-copying probe at 2, so the flipped 0 remains a measurement of the provider and
not an arithmetic constant.

**Q5 — Scope discipline: clean.** No `extern "C"`, no C header, no loader, no
vtable, no negotiation; every C form is prose in a C++ header (decision 14). No
crossing type defined in terms of a future ABI's types and no shared C header
(decision 2). Nothing above the seam branches on built-in vs loaded (decision 3;
DEBT-8 is a forward note, not a violation). `Files-to-delete` is real and I verified
each entry. No coexistence window, and the design explicitly refuses to build one —
correct, and worth saying so, because that refusal is what makes the +900 lines a
single landing rather than a bridge with a scheduled deletion.

---

## Tree claims checked

| Claim | Verdict |
|---|---|
| `MakeReadySchemaFuture` exists and is replaceable | ✓ `provider.hpp:48` |
| Three `shared_future` schema members | ✓ — but the third is `shared_future<shared_ptr<arrow::Schema>>`, see **B2** |
| `ProviderTraits` row `{kCarried, kDropsPreSubscribe}` + one `INSTANTIATE_TEST_SUITE_P` | ✓ `conformance/subject.hpp:47-53,108`; retention table `fixtures.cpp:34` keyed "inprocess" |
| Fast DDS internal headers named in Files-to-touch | ✓ all five exist |
| `DeserializeEnvelope` / `ParseEnvelopeBody` / `ReceivedData` | ✓ `core/include/fletcher/core/envelope.hpp:74`, `fletcher::internal::*` |
| P3, XRCE "can name a shared owner" | ✓ in substance — but the evidence cited is wrong, see DEBT-6 |
| P3, Fast DDS copying path / gateway WS publish own their bytes | ✓ |
| Copying probe scores 2 on the borrowed leg, so the pin is provider-dependent | ✓ `copy_clauses.cpp:196-200` |
| "every in-tree `Blob` site is in Files-to-touch" | ✗ — see DEBT-4 |

---

## DEBT register (9) — appended to `design-debt.md`, does not loop the design

- **DEBT-1** — `PubSubStatus::kOk` is representable inside `PubSubError`. A
  boundary translating `PubSubError{kOk, …}` returns success for a failed call: no
  typed signal, silently. Rung-1 item 6 claims "always carries a `PubSubStatus`", which
  is true and insufficient. *Proposed forbid:* `PubSubError`'s constructor refuses
  `kOk` (throw or `assert` + coerce to `kInternal`). One line.
- **DEBT-2** — "forever is `milliseconds::max()`" is an implementation trap:
  `condition_variable::wait_for(duration::max())` overflows the internal
  `time_point` on common implementations and can return immediately. Specify
  `wait_until(steady_clock::time_point::max())`, or clamp.
- **DEBT-3** — Decision 5 requires the rule "written in the header", but two of the
  five crossing types have no home in `Files-to-touch`:
  `core/include/fletcher/core/write_buffer.hpp` (design §4's `WriteBuffer` sentence)
  and `pubsub/include/fletcher/pubsub/owned_schema.hpp` (where `SharedSchema` is
  defined and where §3.3's rule must land). Two lines.
- **DEBT-4** — `fastdds-pubsub-provider/benchmarks/` builds `Blob`s
  (`bench_pub_sub_type.cpp:425`) and reads `result.schema` (`exp_zero_copy.cpp:98`)
  and is not in `Files-to-touch`. It is a standalone CMake project outside the
  conanfile and CI, so it rots without going red. Either migrate it or say in the
  design that it is knowingly left broken.
- **DEBT-5** — `SchemaMode` collides by name **and** by enumerator with the existing
  `fletcher::conformance::SchemaMode { kCarried, kAbsent }`
  (`conformance/subject.hpp:40`), which `inprocess_main.cpp` uses unqualified inside
  `namespace fletcher::conformance` — where the conformance one shadows the new one.
  Compile-visible, not silent, but this is the item whose deliverable is unambiguous
  vocabulary. Give the provider's axis a distinct name (e.g.
  `InProcessPubSubProvider::SchemaCarriage`).
- **DEBT-6** — P3's XRCE evidence points at the wrong artefact. `ts.pending`
  materialising an `Envelope` proves the opposite of what is wanted — an `Envelope`
  holds *copies* (`envelope.hpp:108` `make_shared<const vector<uint8_t>>`). The real
  shared-owner candidate is the per-sample local `std::vector<uint8_t> payload` at
  `xrce_dds_pubsub_provider.cpp:178`, which need only become a `shared_ptr`. The
  premise holds; fix the citation so the implementer does not chase `pending`.
- **DEBT-7** — `Envelope::row` stays a `std::vector` copy on every XRCE and gateway
  receive. When the pin flips to 0, `integration-tests/pubsub-conformance/README.md`
  must keep stating (2026-09-01 scoping ruling) that the number is about the *seam's
  capability*, never about any transport's receive path — otherwise "0 copies" reads
  as a claim the tree does not support.
- **DEBT-8** — Forward note for PDA-DEC-5: `SchemaMode` must be reachable through
  §4.1's typed-core-plus-opaque-document, not a second construction API, or decision
  3's "one creation signature" is breached by this item's ctor argument.
- **DEBT-9** — "The triple *is* the C form" (design §1) risks a layout reading.
  `std::shared_ptr<const void>` is two words with a control block; a C
  `{void*, const uint8_t*, size_t}` is not a memory image of it. Add one sentence:
  the C form is conceptual, no layout compatibility is implied or permitted, and a
  boundary *constructs* a `Blob` from the three fields. Cheap insurance against the
  one reading that is UB.
