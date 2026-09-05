# Verification of the BIND-C# stop-and-ask against the frozen seam spec

Read-only. Branch `feature/protocol-driver-abi`, HEAD `cf31767`. Nothing changed.
Verdict per claim is about **whether the stated defect is true of the tree**, not about the
proposed replacement wording.

Summary: **A1 A2 A3 A4 A5 A7 A9 CONFIRMED. A6 CONFIRMED on substance, one citation overstated.
A8 PARTLY — the premise "no existing status fits" is not established.** Both header
corrections are true. The closing premise about no automated build is stale and load-bearing
for nothing.

---

## A1 — §3.1: the C form is not constructible from `WriteBuffer`'s public API — CONFIRMED (all three sub-claims)

1. **`Data()` returns `const uint8_t*`** — `core/include/fletcher/core/write_buffer.hpp:83`:
   `const uint8_t* Data() const { return data_; }`. The window pointer is `uint8_t* data_` but
   it is `protected` (`write_buffer.hpp:120`), reachable only by a subclass.
2. **There is no `Capacity()`** — CONFIRMED. `grep -rn "Capacity()" --include=*.hpp --include=*.cpp
   core pubsub pubsub-arrow arrow-bridge gateway` returns **zero hits**. The only capacity-ish
   member is `VectorWriteBuffer::Reserve(size_t)` at `write_buffer.hpp:178`, which is **private**
   and is a `std::vector::reserve` helper, not a window reserve — a name collision worth noting,
   because A1's proposed `Reserve` would shadow it in the derived class.
3. **Every `Append*` copies from a source** — CONFIRMED. `Append` `memcpy`s
   (`write_buffer.hpp:47`), `AppendByte` writes one byte (`:56`), `AppendZeros` `memset`s (`:65`),
   `AppendFixed` delegates to `Append` (`:102`). There is no public way to advance `pos_` without
   supplying bytes.

The asymmetry the claim rests on is real and documented: the ABI direction gets
`uint8_t* data; /* the window; Fletcher writes into it directly */` plus `size_t capacity`
(`docs/protocol-driver-abi-spec.md:183-185`), because the **driver** supplies the window; the
binding direction is handed a `WriteBuffer&` and has neither.

**Worse than claimed.** Three points the author did not make:

- **§8 is the frozen clause this breaks, not just §3.1's prose.** `docs/pubsub-interface-spec.md:726-730`
  says zero-copy for rows is *required* and *"a property of this seam — both ABI rounds inherit it
  and neither can restore it if the seam loses it"*, with rows *"already there, via `Publish`'s
  inversion and `FixedWriteBuffer`"*. A binding that must assemble a row in a scratch buffer and
  `Append` it once **cannot inherit that property**. §8's zero-copy property is named in the
  `frozen` list (`docs/pubsub-interface-spec.md:899-902`), so this is a frozen-property gap, not a
  crossing-count inconvenience.
- **`CopyAccounting` cannot see the loss.** §8.1 (spec:745-752) measures *the window base after the
  encoder's last append* against what the subscriber receives. A binding doing scratch-then-`Append`
  still passes: the provider's window is still the address delivered. So the existing oracle is
  blind to exactly the copy a binding is forced into — which is why A1's demand for a
  `Reserve`/`Commit` case with a live negative control is not decoration.
- **The workaround a binding will actually find may be worse than one copy per row.**
  `AppendZeros(n)` then `PatchU32`/`PatchByte` below `pos` does give a back-patch path
  (`write_buffer.hpp:86-96`) — one crossing per **4 bytes**, and `PatchByte` only ORs, so a
  single byte cannot be *cleared*. A binding author who finds this before finding the
  scratch-buffer route ships something much worse than the author's estimate.

## A2 — §5.3 frozen as an undischarged obligation, three divergent behaviours — CONFIRMED, and worse

**The obligation.** `docs/pubsub-interface-spec.md:609-615`, verbatim at 613-615: *"The seam must
state that a callback **must not throw**, and say what a provider does if one does anyway."*
§12.1 names *"§5.3's callback rule"* in the `frozen` list (`docs/pubsub-interface-spec.md:901`).
So a task sentence is frozen as contract text.

**No header states it.** `pubsub/include/fletcher/pubsub/provider.hpp:103-135` documents
`SubscribeCallback`'s delivery contract in three bullets (schema-before-data, per-writer order,
one-callback-at-a-time) and says nothing about throwing. Whole-header search —
`grep -rn "must not throw|not throw|throwing callback|escaping exception" --include=*.hpp
pubsub/include core/include fastdds-pubsub-provider/include xrcedds-pubsub-provider/include` —
returns **zero hits**.

*Correction to the author's attribution:* he pins this on §12.2 condition 1 (*"a normative rule in
the header"*). Condition 1 is scoped to **§3 ownership rules** (`docs/pubsub-interface-spec.md:955`),
not to §5.3. The real position is **worse**: §5.3's rule is in the `frozen` list and is covered by
**none** of the six handoff conditions. Nobody signed it off wrongly; nobody signed it off at all.

**The three-way divergence — CONFIRMED, all three:**

- **Fast DDS catches, drops, continues.** `fastdds-pubsub-provider/src/internal/data_reader_listener.hpp:46-56`
  — `on_data_available` wraps `Take(reader)` in `catch (const std::exception&)` / `catch (...)` and
  logs. Pinned by `LoanedThrowingCallbackNeitherEscapesNorLeaksLoans`
  (`fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp:664`) and by
  `CopyingThrowingCallbackDoesNotEscape` (`:688`).
- **The loopback has no catch.** `pubsub/src/in_process_provider.cpp:279` — `cb(buf.data(), buf.size(),
  schema, attachments)` sits inside the lambda passed to `TranslateSeamFailure` at `:251` (the body
  of `Publish`). There is no `catch` between them.
- **XRCE adds no blanket try/catch at the delivery site.** `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:306-308`
  (schema-flush path) and `:357-359` (steady path) invoke `callback(...)` **outside** the
  `try`/`catch (...)` blocks at `:254-295` and `:337-350`, which cover only deserialization and
  schema resolution.

**The sharpest sub-claim — a subscriber bug reported to a publisher as `kPayloadTooLarge` — CONFIRMED.**
`core/include/fletcher/core/status.hpp:137-140`: `catch (const std::overflow_error& e) { throw
PubSubError(PubSubStatus::kPayloadTooLarge, e.what()); }`. The mapping is by type and
unconditional; there is no discrimination of *whose* frame threw. In the loopback, a subscribe
callback throwing `std::overflow_error` (a marshalling overflow in a binding thunk, a decoder
bound check) propagates from `:279` to `TranslateSeamFailure` at `:251` and leaves an unrelated
publisher's `Publish` as `kPayloadTooLarge`.

**Worse than claimed, on XRCE.** The author files XRCE as "no blanket catch". The tree is sharper
than that in two ways:

- XRCE's own comments declare this path UB: `xrce_dds_pubsub_provider.cpp:259-263` — *"NOTHING in
  the schema-resolution sequence below may throw out of the XRCE session callback thread. OnTopic
  is invoked from inside `uxr_run_session_time()`, so unwinding would cross C frames from the XRCE
  client library: UB on MSVC and process termination in practice (H-INV-3 / HARD locked decision #3)."*
  The guard the comment describes stops short of the `callback(...)` calls that follow it.
- **The wrong-party mapping exists in XRCE too, and through a `CreateTopic`.**
  `xrce_dds_pubsub_provider.cpp:758` calls `uxr_run_session_until_confirm_delivery` **inside**
  `CreateTopic`'s `TranslateSeamFailure` (opened at `:633`). That pump dispatches `OnTopic`, so a
  subscriber's throwing callback can reach a *publisher's `CreateTopic`* — after crossing the XRCE
  client's C frames, which the code itself says is UB. So the failure is not "a mislabelled
  status", it is "undefined behaviour, and if it survives, a mislabelled status charged to the
  wrong party."

## A3 — §6 has no re-entrancy clause; three providers, three answers — CONFIRMED, and worse

**No clause.** `docs/pubsub-interface-spec.md:619-635` — §6 has exactly five clauses (delivery
serialized per subscription; concurrent across subscriptions; any thread; reference ops
thread-safe; destruction requires quiescence). None is about re-entry from a delivery callback.
§6 is in the `frozen` list (`docs/pubsub-interface-spec.md:901`).

**Three behaviours:**

- **Loopback: hangs.** `pubsub/src/in_process_provider.cpp:166` — `std::mutex mu` (non-recursive),
  held across dispatch by the `std::lock_guard` at `:258` through the `cb(...)` at `:279`. The
  comment at `:272-275` says so outright: *"mu_ is non-recursive, so a re-entering callback
  deadlocks rather than corrupting."* Function-level comment at `:248`: *"a callback must not
  re-enter."*
- **XRCE: guaranteed safe.** `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:168` —
  `std::recursive_mutex mu`, with `:161` explaining the choice. Re-entrant `Unsubscribe` is pinned
  by `XrceProviderTest.ReentrantUnsubscribeNoUseAfterFree`
  (`xrcedds-pubsub-provider/tests/test_xrce_provider.cpp:36`).
- **Fast DDS: partly served, partly a hang, nowhere stated.**
  `fastdds-pubsub-provider/src/internal/ordered_delivery.hpp:152-155` — `DeliverSteady` returns
  false when re-entered, so a re-entrant *offer* is queued rather than lost, and `:93-95` claims the
  drain slot for the same reason. But re-entrant `Unsubscribe` is a self-wait:
  `fast_dds_pubsub_provider.cpp:570-574` deletes the readers with the comment *"Deleting the schema
  reader first **waits for any in-flight schema delivery to finish**"* — called from inside that
  delivery, that is a hang. The only quiescence statement in the header is about **destruction**
  (`fast_dds_pubsub_provider.hpp:107`).

**Worse than claimed** in the small: the author says Fast DDS *"takes no position on the rest"*.
It takes a deliberate position on re-entrant publish (queue it, with the ordering argument written
out) and has an **undocumented** hang on re-entrant `Unsubscribe`. So the spread is not
"safe / hang / silent", it is **safe (XRCE) / hang-by-design-and-documented (loopback) /
mixed-and-undocumented (Fast DDS)** — three answers with three different *epistemic* statuses,
which is worse for a blind binding round than three answers plainly stated.

## A4 — §7 clause 6 contradicted by the tier §9 assigns BIND — CONFIRMED. They genuinely conflict.

**The two texts.** `docs/pubsub-interface-spec.md:681` — *"6. **After `Unsubscribe` returns**, no
further callback for that topic."* `docs/pubsub-interface-spec.md:777` (§9's *Implements* row) —
BIND implements *"the caller side of `Publisher`/`Subscriber` + §4 selection"*; §9's
*Inherits as its oracle* row (`:781`) names the four conformance suites for both rounds.

**The tier BIND wraps states the opposite, and calls it intentional.**
`pubsub/include/fletcher/pubsub/subscriber.hpp:64-71`:

> Unsubscribing does NOT guarantee that no further callbacks fire. A subscriber may still receive
> one final in-flight message after Unsubscribe returns. … This is intentional and by design
> (copy-then-release-then-call fan-out), not a bug.

**Confirmed in code.** `pubsub/src/subscriber.cpp:84` — `EntryList entries = fanout->entries.load();`
snapshots, then `:86-88` invokes every entry in the snapshot outside the lock.
`pubsub/src/subscriber.cpp:189-196` — `Unsubscribe` calls `Impl::RewriteEntries` to publish a new
COW list; a delivery already holding the previous snapshot still invokes the removed entry.

**The suite measures the tier below.** `integration-tests/pubsub-conformance/src/clauses.cpp:301`
— `TEST_P(ProviderConformance, NoDeliveryAfterUnsubscribeReturns)`, whose body drives
`Subject().Unsubscribe(topic)` (`:313`), a `ProviderSubject`
(`integration-tests/pubsub-conformance/include/fletcher/conformance/subject.hpp:120`).
`clauses.cpp:7` states the design: *"A clause body sees only a ProviderSubject: no
PubSubProvider&, no Publish."*

*One refinement:* the author writes that §9 names *"a suite that … tested only below it"*. Not
quite — `integration-tests/pubsub-conformance/src/copy_accounting.cpp:265-266` holds
`Publisher publisher_; Subscriber subscriber_;`, so `CopyAccounting` **does** run at BIND's tier.
The precise fact is narrower and still fatal: **`ProviderConformance` — the suite that encodes §7's
clauses, including clause 6 — never touches `Publisher`/`Subscriber`.** BIND's tier has copy
coverage and zero delivery-contract coverage.

So: **yes, they genuinely conflict**, and the conflict is a use-after-free in the one direction a
binding cannot avoid. A binding frees or unpins its callback state on `Unsubscribe` because that
is the only thing the call can mean; a late delivery then calls freed foreign state. For a managed
runtime this is not a dropped sample, it is a corrupted handle or an access violation.

## A5 — §3.5: an embedded NUL truncates a topic name on the wire — CONFIRMED, and the surface is wider than claimed

This was the one to test hardest. It holds.

**The check is empty-only.** `pubsub/include/fletcher/pubsub/internal/segments.hpp:20-25` —
`RequireSegments` tests `segs.empty()` and nothing else. §3.5 mandates the length-authoritative C
form at `docs/pubsub-interface-spec.md:264-266` and refuses only the empty list at `:267-269`.

**The NUL prohibitions that landed are elsewhere, and none of them covers a segment:**

- Loopback **config document** — `pubsub/src/in_process_provider.cpp:75-79` (PDA-DEC-5).
- XRCE **config document** — `xrcedds-pubsub-provider/src/internal/xrce_document.cpp:71-75` (PDA-DEC-7).
- **Provider selector** — `pubsub/src/provider_registry.cpp:107-113`, with the reason stated
  verbatim: *"a selector is bytes-plus-length and a NUL would silently truncate it on the wire"*.

Three NUL refusals exist, on the three strings that are *not* the topic name.

**The truncation site.** `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:679-681` —
`uxr_buffer_create_topic_bin(&impl_->session, impl_->reliable_out, ts.topic_id, ts.participant_id,
name.c_str(), impl_->type_name.c_str(), UXR_REPLACE)`. `name` is `internal::JoinSegments(...)`
output, so `{"a\0b"}` and `{"a\0c"}` are two distinct `impl_->topics` keys (`std::string`, length
authoritative) that both reach the Agent as topic **`a`**. Silent wrong-slot delivery, reached
through the very form §3.5 requires.

**Two exposures the author missed:**

- **The participant name truncates the same way** — `xrce_dds_pubsub_provider.cpp:673-675`,
  `uxr_buffer_create_participant_bin(..., name.c_str(), ...)`. The collapse is at two entity
  levels, not one.
- **Fast DDS does not truncate.** `grep -rn "c_str()" --include=*.cpp --include=*.hpp
  fastdds-pubsub-provider/src` returns zero hits, so the exposure is XRCE-specific exactly as
  claimed. Worth recording so a fix is not scoped wider than it needs to be.

**Empty individual segment — CONFIRMED unvalidated.** `RequireSegments` never looks at individual
segments; `JoinSegments` (`segments.hpp:43-51`) happily produces `""` from `{""}` and `"a/"` from
`{"a", ""}`.

**A third case the author missed, and it may be sharper than the empty-segment one: an embedded
`/`.** All five call sites share `internal::JoinSegments` (`pubsub/src/in_process_provider.cpp`,
`pubsub/src/publisher.cpp`, `pubsub/src/subscriber.cpp`,
`fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp`,
`xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`), which joins with a bare `'/'` and
escapes nothing (`segments.hpp:36-40`). So `{"a/b"}` and `{"a","b"}` are **one topic** today —
while §3.5's frozen first sentence (`docs/pubsub-interface-spec.md:263`) says
*"`std::vector<std::string>`, so the provider may join with any separator"*, which licenses a
future provider under which they are **two**. Two segment lists whose identity depends on the
provider is §0.1(2) failing in exactly the way A5 describes, and a length-carrying binding reaches
it by accident far more easily than it reaches a NUL.

## A6 — `Blob` has a C form, `Attachments` does not — CONFIRMED on substance; one citation overstated; worse than claimed

**Confirmed.** `docs/pubsub-interface-spec.md:147-190` gives `Blob` five numbered clauses
(`:169-174`) and names `Attachments = unordered_map<string, Blob>` at `:150`. Nothing in §3.2 or
anywhere else in the spec says how the **container** crosses:
`grep -n -i "attachment" docs/pubsub-interface-spec.md` yields only `:86`, `:98`, `:147`, `:150`,
`:726`, `:731`, `:737`, `:760` — every one a mention, none a form. `unordered_map` has no
positional accessor, so a boundary must invent one; two rounds will invent two.

**Overstated citation.** The author says §3.2 gives `Blob` *"a complete C form"*. §3.2 explicitly
declines: `:166-168` — *"This round does not write that C form — it **states the contract it must
satisfy**"* — and `:186-190` calls the C form *"conceptual, never a memory image"*. The
**complete** form is in the sibling doc: `docs/protocol-driver-abi-spec.md:151-158`
(`fletcher_blob`). Substance unaffected — `Blob` has five normative clauses and `Attachments` has
none — but the owner should not be told §3.2 contains something it deliberately does not.

**Worse than claimed.** The gap is not seam-only. The ABI spec has **no attachments type either**:
its §3 publishes exactly two structs, `fletcher_blob` and `fletcher_schema`
(`docs/protocol-driver-abi-spec.md:151-166`), while its role table names `publish` as a driver
vtable entry (`:69`) and its §6 claims *"the seam preserves zero-copy on publish and on attachment
sharing"* (`:267`). A driver's `publish` must receive attachments and **neither document gives it
a type**. So A6 is a gap in both rounds' vocabulary, not a BIND-only one, and PDA-ABI hits it on
day one just as hard.

## A7 — §5.1 has no C form for the message, which for §4 refusals is the whole answer — CONFIRMED

**§5.1 pins the number and stops.** `docs/pubsub-interface-spec.md:543-600` — the bullets cover
numbering, publication, refusal of `kOk`/`kPending`/`kSubscriptionEnded`, entry-point translation,
the `overflow_error` mapping, and the base class. On the message, the only sentence is `:598-600`:
*"Messages are unchanged; what moved is branching on the error type."* No C form, no retrieval
rule, no obligation on a boundary to convey it.

**For §4 the message is the answer.** `docs/pubsub-interface-spec.md:317-321` — an unregistered
name is *"`kInvalidArgument` listing what is registered"*; a path in a build with no resolver is
`kNotSupported`. The list exists only in a string: `pubsub/src/provider_registry.cpp:166-179`
builds `"no built-in provider named " + Quoted(...) + "; available: " + available.str()`, where
`available` is composed from `factories_`. Same for the path classification —
`provider_registry.cpp:194-207` composes *"was read as a driver path, because the character at
offset N (0x..) is outside a provider name's [A-Za-z0-9_-]"*. Both statuses are shared with other
refusals, so a boundary that forwards the number and drops the message hands an operator a bare
"invalid argument" for a typo'd protocol name.

**The sibling round already solved this.** `docs/protocol-driver-abi-spec.md:229-232` —
*"retrievable from the **instance** that produced it — never a global or thread-local
`errno`-style slot, which would not survive multiple instances per process."* The seam has no
counterpart. Confirmed.

## A8 — an append is needed for a re-entrancy refusal — PARTLY

**The mechanics are all CONFIRMED:**

- **Append-only, machine-pinned.** `core/include/fletcher/core/status.hpp:66-83` — ten
  per-enumerator `static_assert`s, one per value, with the comment *"Not a single assert on the
  last value: that would let two values swap places and still pass."*
- **The owner allocates.** `docs/pubsub-interface-spec.md:908-914` — *"making an append is itself a
  stop-and-ask, and the owner allocates the number or the field"*, and the append *"carries its
  `core/README.md` row in the same change"*. `core/README.md:41-44` restates it.
- **`PubSubError` would accept it.** `core/include/fletcher/core/status.hpp:105-115` — `Sanitize`
  coerces only `kOk`, `kPending`, `kSubscriptionEnded`; any appended value passes through.
- **Red until the row lands.** `core/tests/test_status_taxonomy.cpp:222-226` — part 3 asserts that
  one past the last published row is not a status.

**What is NOT established: that no existing status fits.** The author argues only against
`kInvalidArgument`. He does not address **`kNotSupported = 6`**, whose published meaning is
*"This provider does not implement the requested behaviour"* (`core/README.md:37`;
`core/include/fletcher/core/status.hpp:49-50`) — a defensible reading of "this provider cannot
serve this call re-entrantly". Since an append costs an owner allocation, a README row, a guard and
a compile break, the premise matters.

There **is** a good argument for an append, and the author did not make it: `kNotSupported` is used
in the tree for **permanent capability facts** — a build with no path resolver
(`pubsub/src/provider_registry.cpp:207`), a serial transport that is not implemented
(`XrceConfig.SerialIsRefusedAsUnsupported`) — whereas a re-entrancy refusal is **contextual**: the
identical call succeeds outside a callback. Collapsing the two would tell a caller "never" where
the truth is "not from here", which is the same class of harm §4 clause 1 cites for refusing to
collapse a typo and a driver path. Recommend the owner rule on the append with **that** argument in
front of him rather than on A8 as written.

## A9 — the ABI spec publishes a second, differently-numbered status enum — CONFIRMED. Exact divergence below.

`core/README.md:20` — *"**This table is the only enumeration of them**"*.
`docs/pubsub-interface-spec.md:565-566` — *"A second enumeration anywhere, including in this
document, would be the drift the guard exists to stop."*
`docs/protocol-driver-abi-spec.md:216-227` is a second enumeration.

| # | seam (`status.hpp:32-63`, `core/README.md:31-40`) | ABI (`protocol-driver-abi-spec.md:216-227`) | agree? |
|---|---|---|---|
| 0 | `kOk` | `FLETCHER_OK` | yes |
| 1 | `kInvalidArgument` | `FLETCHER_ERR_INVALID_ARGUMENT` | yes |
| 2 | `kSchemaConflict` | `FLETCHER_ERR_UNSUPPORTED` | **no** |
| 3 | `kTopicNotDeclared` | `FLETCHER_ERR_CONFIG` | **no** (and no seam counterpart at all) |
| 4 | `kPayloadTooLarge` | `FLETCHER_ERR_SCHEMA_CONFLICT` | **no** |
| 5 | `kTransportFailure` | `FLETCHER_ERR_NO_SUCH_TOPIC` | **no** |
| 6 | `kNotSupported` | `FLETCHER_ERR_TRANSPORT` | **no** |
| 7 | `kInternal` | `FLETCHER_ERR_OVERFLOW` | **no** |
| 8 | `kPending` | `FLETCHER_ERR_INTERNAL` | **no** |
| 9 | `kSubscriptionEnded` | — | **no** |

*"Every number after 1 differs"* is exactly right, and the shift is not a constant offset — it is
an interleave, because `FLETCHER_ERR_UNSUPPORTED` sits at 2 and `FLETCHER_ERR_CONFIG` at 3 with no
seam counterpart. Five names also differ for the same concept (`UNSUPPORTED`/`kNotSupported`,
`NO_SUCH_TOPIC`/`kTopicNotDeclared`, `OVERFLOW`/`kPayloadTooLarge`,
`TRANSPORT`/`kTransportFailure`, `CONFIG`/nothing).

**Can both be right?** No. `docs/protocol-driver-abi-spec.md:211-213` asserts the seam→ABI mapping
is *"normative and must be exhaustive — an unmapped exception type is a defect"* and then states
no mapping anywhere in the document. With `FLETCHER_ERR_CONFIG` having no seam number and
`kPending`/`kSubscriptionEnded` having no ABI number, an exhaustive mapping is not merely absent,
it is **not constructible from the two tables as published**.

**The guard is blind to it.** `core/tests/test_status_taxonomy.cpp:83-86` reads exactly one file,
`FLETCHER_CORE_README_PATH`. It cannot see `docs/protocol-driver-abi-spec.md`.

**Context that reframes A9 usefully for the owner.** The ABI enum is not a rival taxonomy
published in defiance of the rule — it is **older than the rule and older than the seam enum**:

- `git log -S "FLETCHER_ERR_CONFIG" -- docs/protocol-driver-abi-spec.md` → `4430a49`
  *"docs(PDA): open round PDA — protocol driver ABI design"*.
- `status.hpp` was added by `60313db` *"feat(PDA-DEC-3): the crossing vocabulary …"*.
- `core/README.md`'s *"only enumeration"* sentence by `5d06812` *"feat(PDA-DEC-9): the seam spec,
  taxonomy guard and parallelism handoff"*.

Sequence: sketch the ABI codes at round opening → land the real taxonomy → land the
only-enumeration rule → never reconcile the sketch. The author's *"landed in this same PR"* is
**true** (`4430a49` is not an ancestor of the merge base `c61bfbe`, so it is on this branch and in
PR #126) but it invites the wrong inference. Same fix, and cheaper than it looks: the ABI spec is
**not `frozen`** — §12.1's freeze list (`docs/pubsub-interface-spec.md:899-906`) covers
`docs/pubsub-interface-spec.md` only — so this is PDA-ABI's own document to correct, not an
amendment to frozen text.

---

## The two header corrections — both TRUE

1. **`pubsub/include/fletcher/pubsub/publisher.hpp:33`** — *"Throws if the topic already exists."*
   Stale. `pubsub/src/publisher.cpp:48-61`: a re-declaration throws
   `PubSubError(PubSubStatus::kSchemaConflict, ...)` only `if (incoming.ConflictsWith(it->second))`,
   and `publisher.cpp:61` is `return;  // identical (or non-comparable) re-declaration — no-op`. A
   binding author working from the header alone implements the wrong contract, exactly as claimed.
2. **`pubsub/include/fletcher/pubsub/owned_schema.hpp:60`** — TRUE. `DeepCopy` is public and static
   (`:56`) and throws `std::runtime_error(...)` at `:60-62`, not `PubSubError`. One nuance in the
   tree's favour: the seam's own call site is inside a translating entry point
   (`pubsub/src/in_process_provider.cpp:197`, within `CreateTopic`'s `TranslateSeamFailure`), so
   there it arrives at a seam caller as `kInternal`. The defect bites exactly the caller the author
   names — a binding calling `DeepCopy` **directly** to consume a borrowed `SharedSchema`, which
   §3.3 (`docs/pubsub-interface-spec.md:194-201`) makes the sanctioned move. An untyped exception
   out of a header §5.1 promises is typed. Confirmed.

*One premise correction on both:* the author cites §12.1 as permitting these as maintenance.
§12.1's record permission is bounded to **spec §10 and §11**
(`docs/pubsub-interface-spec.md:919-927`) — a permission over two sections of *that document*, not
a general licence over headers. The conclusion is nevertheless right, for a simpler reason: §12.1
freezes the *spec*, and a doc comment in a header is neither §10 nor §11 nor spec text. Correcting
a header comment to match the code it sits on needs no ruling because nothing frozen says
otherwise.

## The stale closing premise

**Confirmed stale, and already corrected in the tree.** The document's last paragraph reads
*"§12.4's instruction stands over all of it: no automated build has ever run on this branch."*
`docs/pubsub-interface-spec.md:1002-1024` now carries the amendment, landed in `cf31767`
*"docs(PDA-DEC-9): amend §12.4 — the evidence changed"*: PR #126 ran the lanes, green on **46
checks** at `08d1b81`, and it found **seven** defects local running could not reach.

**Which of its arguments depended on it: none of the nine defect claims.** Every claim confirmed
above is established by reading the tree and reads identically before or after the lane run. What
the staleness touches is only the document's **epistemic framing** of its own cost estimates
(*"every cost estimate above is from reading"*) — a caveat in the author's favour, not a premise
of any claim. Two second-order notes:

- The stop-and-ask is *more* credible now, not less: §12.4's amended clause 2 (*"Prefer the first
  lane run to any amount of local green"*) and its evidence that six of seven defects were
  invisible on Windows both **strengthen** A1/A2/A3's guard demands, since those defects live in
  precisely the class §12.4 says local running cannot reach.
- The one place it should be edited before the owner rules: A1's *"Cost, named"* and A3's *"That is
  a real change to `in_process_provider.cpp`"* remain read-only estimates and should say so on
  their own authority rather than by citing a §12.4 sentence that no longer exists in that form.

---

## Ranking, on the evidence

**I agree with the author's three, with one reordering, and I would add A5.**

1. **A4** — the only claim that is a **memory-safety** defect rather than a specification gap.
   Frozen clause 6 promises the strong form; the tier BIND is assigned states the opposite and
   calls it deliberate; and the suite named as BIND's oracle proves clause 6 one tier below the one
   BIND wraps. A binding cannot write a correct `Unsubscribe` under any reading. I rank this above
   A1.
2. **A1** — a binding cannot implement the encode path at the seam's stated cost, and per §8 it
   *cannot recover the property later*. Blocking.
3. **A2** — a binding's callbacks are foreign code; a callback that throws is UB in XRCE by the
   tree's own comments and a wrong-party status in the loopback. Blocking, and worse than the
   write-up.
4. **A5** — I rank this above A3 and would call it blocking too. It is the only confirmed defect
   that is a **silent wrong answer already in the tree** rather than a missing statement: two
   distinct Fletcher topics deliver into one slot on XRCE today. The author's own §7 framing
   (*silent wrong-slot decoding, not a crash*) applies verbatim, and with the embedded-`/` case
   added it is reachable without any hostility at all.
5. **A3** — real divergence, real §0.1(2) failure, but the outcomes are a hang or a guarantee:
   both loud.
6. **A9** — agree it gets monotonically more expensive; agree it is cheap now (one code block, and
   the document is not frozen). Not blocking for BIND, which never sees `fletcher_status`.
7. **A6**, **A7** — genuine gaps that will produce divergence between BIND-C# and BIND-Rust, but a
   binding can proceed by choosing and documenting a form.
8. **A8** — parked behind A3's ruling, and its own premise needs the `kNotSupported` argument
   settled first.

**Defects worse than the author claims:** A1 (frozen §8, plus the oracle is blind to the loss, plus
the `PatchU32` pseudo-workaround), A2 (XRCE's own comments make it UB, and `CreateTopic` is a
second wrong-party path), A3 (Fast DDS is a third *undocumented* answer, not silence), A5
(participant name truncates too; embedded `/` aliases segment lists in **all three** providers),
A6 (the ABI spec has no attachments type either, so it blocks PDA-ABI as well).

**Defects less severe than framed:** A6's *"complete C form"* citation, A8's *"no existing status
fits"*.

## Contract gap vs record gap

**Contract gaps — need the owner.** §12.1's freeze list (`docs/pubsub-interface-spec.md:899-906`)
names §3 entire, §5.1's mapping and refusal rules, §5.3's callback rule, §6, §7's clauses and §8's
zero-copy property, and makes §9 frozen by default:

- **A1** (§3.1 clauses + §8's property), **A2** (§5.3 + §5.1's by-type carve-out), **A3** (§6),
  **A4** (§7 clauses + §9's oracle row), **A5** (§3.5, named explicitly in the freeze list as
  *"§3.5 including the empty-segment refusal"*), **A6** (§3.2), **A7** (§5.1), **A8** (an
  `append-only` allocation, which §12.1 makes a stop-and-ask in its own right).

All eight also require **product code**, not only text: A1 four members on `WriteBuffer`; A2 a
catch at the loopback's and XRCE's delivery sites; A3 the loopback's dispatch moved out from under
`mu_`; A4 an in-flight count in `Fanout`; A5 two or three checks in `RequireSegments`. None is a
wording-only fix — which is itself the strongest evidence that these are contract rulings and not
record corrections.

**Record gaps — maintenance, no ruling:**

- **A9**. `docs/protocol-driver-abi-spec.md` is not in §12.1's freeze list; it is PDA-ABI's own
  document. The enum is a *contract* statement inside that document but a *stale record* relative
  to the seam, so PDA-ABI may correct it under its own authority — recorded as inheriting §5.1's
  only-enumeration rule rather than as a fresh decision.
- **Both header corrections.** Doc comments made to match the code beside them.

## What the author missed that a BIND implementer would hit

1. **The embedded `/` in a segment** (see A5). `{"a/b"}` and `{"a","b"}` are one topic in all three
   providers today (`pubsub/include/fletcher/pubsub/internal/segments.hpp:33-51`), while §3.5's
   frozen first sentence licenses a provider under which they are two. Sharper than the
   empty-segment case, and it belongs in whatever refusal A5 wins.
2. **`Attachments` blocks PDA-ABI as hard as it blocks BIND** (see A6). Neither document has a type
   for the container, and a driver's `publish` needs one.
3. **`CopyAccounting` cannot see a binding-side copy** (see A1). §8.1's oracle compares the window
   base after the *encoder's last append* to what the subscriber received; a scratch-buffer binding
   passes. Whatever A1 lands must extend the oracle, or the seam keeps a green guard over a lost
   property.
4. **`VectorWriteBuffer` already has a private `Reserve`**
   (`core/include/fletcher/core/write_buffer.hpp:178`) with different semantics
   (`std::vector::reserve`). A public `WriteBuffer::Reserve` as A1 proposes would be shadowed in
   the derived class. A naming decision the owner should make *with* the ruling, not after it.
5. **The two tiers disagree about whether `Unsubscribe` is idempotent.**
   `pubsub/src/subscriber.cpp:177-181` throws `PubSubError(PubSubStatus::kInvalidArgument,
   "Subscriber: unknown subscription ID")` for an unknown id, while
   `pubsub/include/fletcher/pubsub/provider.hpp:156-160` documents an unknown topic as *"a no-op,
   not an error, so it is safe to call unconditionally on teardown"*. Opposite idempotence at the
   two tiers, on the one call a binding's finalizer / `Dispose` path will make unconditionally —
   and a finalizer cannot let an exception escape. Neither §7 nor §9 says which tier is right. Not
   among the nine, and it compounds A4: the same call is both non-idempotent *and* not a
   release-point for callback state.
6. **§3.4's unbounded-wait clause is pinned by no test, by ruling**
   (`docs/pubsub-interface-spec.md:250-256`, and §12.3's blind-spot list at `:978-983`). A C#
   binding spelling `Timeout.Infinite` as a large finite `long` is the exact case that clause
   exists for, and BIND inherits an unmeasured guard on the first API a `Task`-based wrapper
   touches. Already a recorded blind spot, so not a new defect — but it belongs on BIND's own risk
   list and the stop-and-ask does not mention it.
