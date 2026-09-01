# PDA-DEC-3 — The crossing vocabulary (reviewed as a specification)

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §3, §5, §7 cl.1, §8/§8.1,
§10 (§2, §11 constraining). Decisions 4, 5, 6, 7, 10, 13, 14. Rulings 2026-08-31, 2026-09-01. **Cycle-2 revision: B1/B2/B3.**

## Summary

Three pieces of vocabulary get a normative, **C-expressible** ownership model in the
headers, and the three providers are rewritten to it: `Blob` becomes an
owner-plus-span so the seam can carry memory Fletcher did not allocate; schema
arrival becomes a waitable handle with a single-use resolver and a **typed** outcome;
every failure becomes one error type with a stable numbered status. Plus the sixth
conformance subject and PDA-DEC-2's pin flipped 1 → 0. No ABI is written: every "C
form" is **prose in a C++ header** (decisions 14 and 5).

## Design

### 1. `Blob` — an owner plus a span (§3.2, decision 6)

`Blob` stops being `shared_ptr<const vector<uint8_t>>` and becomes a value type:

```cpp
// core/include/fletcher/core/types.hpp
class Blob {                              // Attachments = unordered_map<string, Blob>
 public:
  Blob() noexcept = default;              // empty: null data, zero size
  /// The ONE general form: `owner` keeps [data, data+size) alive for as long as any
  /// copy of this Blob lives. Refuses (kInvalidArgument) null `data` with non-zero
  /// `size`, and non-null `data` with a null `owner`.
  Blob(std::shared_ptr<const void> owner, const uint8_t* data, size_t size);
  explicit Blob(std::vector<uint8_t> bytes);       // bytes Fletcher allocated
  const uint8_t* data() const noexcept;  size_t size() const noexcept;  /* +empty() */
};
```

**The normative rule, in the header** (§3.2 clauses 1–5 as obligations a C view must
honour): a blob is `{owner, data, size}`; its C form is a struct of that shape plus
`retain(owner)`/`release(owner)`, safe from any thread concurrently, release never
throwing and never re-entering the seam; bytes are immutable once they cross; an
argument blob is borrowed for the call and a callee that keeps it copies the `Blob`
(which retains); empty is null-data/zero-size. **No implicit conversion from today's
`shared_ptr`** — that would be a coexistence window in which every call site keeps
compiling and the copy survives unnoticed.

The C form is **conceptual, never a memory image**: no layout compatibility is implied
or permitted, and a boundary *constructs* a `Blob` from the three fields rather than
reinterpreting one. That is why no shared C header is needed (decision 2) — the two
ABI rounds never exchange structs with **each other**; each wraps the same C++ `Blob`
from its own side, so their C shapes may differ freely.

### 2. Schema arrival — a waitable handle and a single-use resolver (§3.4)

```cpp
// pubsub/include/fletcher/pubsub/schema_arrival.hpp
class SchemaArrival {                     // copyable, thread-safe, shared state
 public:
  static SchemaArrival Ready(SharedSchema schema) noexcept;
  static std::pair<SchemaArrival, SchemaResolver> Create();
  /// Blocks up to `timeout` (0 == poll). The outcome is TYPED, never a bare bool:
  ///   kOk + non-null     the schema arrived
  ///   kOk + null         RESERVED: this transport carries no schemas (§7 cl.1)
  ///   kPending           not yet, within `timeout`; *out untouched
  ///   kSubscriptionEnded no schema will ever arrive (resolver destroyed)
  ///   anything else      the provider failed to produce one; *out untouched
  [[nodiscard]] PubSubStatus Wait(std::chrono::milliseconds timeout, SharedSchema* out) const;
};
class SchemaResolver {                    // move-only, exactly one terminal outcome
 public: void Resolve(SharedSchema) &&;   // consumes the token
         void Fail(PubSubStatus, std::string) &&;  // e.g. DeepCopy threw
         ~SchemaResolver();               // unresolved ⇒ kSubscriptionEnded
};
```

`PubSubStatus` gains `kPending = 8`, `kSubscriptionEnded = 9` (appended-only). **`kOk`
+ null is reserved for a schema-less transport and nothing else** — that is the
distinction today's broken promise carries (`schema_channel.hpp:47`,
`xrce_dds_pubsub_provider.cpp:779`, branched on at `ws_session.cpp:228-243`), and it is
preserved, not deleted: teardown before arrival is a **legitimate** path, so it is
distinguishable, never refused. A provider-side schema failure (`DeepCopy` throwing)
gets a home too — `Fail`.

**C form, pinned so two rounds cannot read it differently:**
`fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)`, where `fl_schema` is
§3.2's owner-handle pair `{owner, const ArrowSchema*}`, **not** a bare `ArrowSchema*`:
a boundary releases the *owner handle* and must **never** call the Arrow C Data
Interface `release` on a shared schema — that destroys it under every other holder.
`timeout_ms < 0` is refused (`kInvalidArgument`); `INT64_MAX` is the unbounded form,
in C++ `wait_until(time_point::max())` because `wait_for(duration::max())` overflows.

**The Arrow tier (§10 ripple).** `SubscriberArrow::SubscribeResult::schema` is a
`shared_future<shared_ptr<arrow::Schema>>`, which `SchemaArrival` cannot carry, so it
becomes a `SchemaArrival` and the deep-copying import hiding in an anonymous namespace
(`subscriber_arrow.cpp`'s `ImportFromNano`) is **promoted to public API** —
`fletcher::ImportArrowSchema(const SharedSchema&)`. It copies first because
`arrow::ImportSchema` **consumes** what it is given, so a caller doing it naively
destroys a schema others reference. One correct import, none left to reinvent.

### 3. Exception taxonomy — one type, one stable number (§5.1, decision 10)

```cpp
// core/include/fletcher/core/status.hpp
enum class PubSubStatus : int32_t {
  kOk = 0, kInvalidArgument = 1, kSchemaConflict = 2, kTopicNotDeclared = 3,
  kPayloadTooLarge = 4, kTransportFailure = 5, kNotSupported = 6, kInternal = 7,
  kPending = 8, kSubscriptionEnded = 9 };   // 8/9 are §2 outcomes, never thrown
class PubSubError : public std::runtime_error {
 public: PubSubError(PubSubStatus, std::string what);  // REFUSES kOk/kPending
         PubSubStatus status() const noexcept; };
```

Values are **fixed integers, appended-only, never reordered or reused** — one
`static_assert` per value. `kOk` exists because both C boundaries need a success value
in the same enum (§5.2), and `PubSubError` refusing it stops a boundary translating a
thrown error into success. **Every seam entry point translates:** the four methods in
all three providers wrap their bodies so the only exception leaving a provider is
`PubSubError`; anything else becomes `kInternal` with the original `what()` — a
taxonomy letting `std::bad_alloc` through is none.

### 4. The other three types (§3.1, §3.3, §3.5) — prose, no code

Rules land **in the header that defines each type** (decision 5): `write_buffer.hpp` —
the window is `{data, capacity, pos}`, borrowed for the encode call, never stored past
it, `Data()` its base; `owned_schema.hpp` — `SharedSchema` already satisfies §3.2's
five clauses, so §3.3 owes the written rule, not a reshape (P2); `provider.hpp` —
topic segments stay `vector<string>`, C form a pointer-and-count of pointer-and-length
pairs borrowed for the call, **empty list illegal**.

### 5. Providers, and the sixth subject

All three are rewritten to the vocabulary (owner ruling 2026-09-01). Beyond mechanical
migration:

- `DeserializeEnvelope` and Fast DDS's `ParseEnvelopeBody` take a
  `shared_ptr<const void> owner` and produce blobs that **alias** the parsed buffer
  instead of copying each attachment out of it. XRCE, the Fast DDS copying read path
  and the gateway's WS publish already own their bytes in shared storage, so their
  per-attachment copies disappear.
- `InProcessPubSubProvider` gains a construction-time
  `SchemaCarriage {kAsDeclared, kCarried}` — that name, not `SchemaMode`, which already
  exists in the conformance harness and would shadow. **Normative:** `kAsDeclared`
  (default, **what the loopback does today**, so the gateway keeps sending
  `schemaIpc`) carries the schema a publisher declared *on this instance* and delivers
  null for an undeclared topic; `kCarried` is schema-before-data — pending arrival,
  pre-schema samples buffered and flushed in order ahead of live ones, never a null
  schema (§7 clauses 1–2).
- Today `kAsDeclared` can flip a live subscription from null to non-null, which §7
  clause 1 forbids, so **one behaviour fix lands here: a subscription's schema mode is
  latched at its first delivery** — one delivered a null schema keeps seeing null, and
  a later declaration reaches only new subscriptions. **Spec amendment, same PR:** §7
  clause 1 calls the loopback a transport that "carries no schemas at all"; it is not,
  it carries declared ones, so the clause is corrected and "never mix" restated **per
  subscription** — stronger, and it binds the DDS providers too.
- The suite gains subject **`InProcessCarrying`**: one `INSTANTIATE_TEST_SUITE_P` line +
  one `ProviderTraits` row (`{kCarried, kDropsPreSubscribe}`), no new clause;
  `InProcessLocal` stays `kAsDeclared`.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *An unowned `Blob`.* No view-only constructor; non-null `data` without an `owner`
   is refused, so §3.2 clause 1's "a callee that keeps it takes its own reference"
   becomes exactly true and a C boundary implements "keep it" as `retain(owner)`.
   *Mutable or resizable bytes:* `data()` is `const uint8_t*`, no `resize`.
2. *Double or absent resolution of a schema arrival, and a wait that never ends.*
   `SchemaResolver` is move-only with two `&&`-qualified terminal calls; whichever runs
   consumes the token, and running neither is the third terminal outcome
   (`kSubscriptionEnded`) — which is what makes the unbounded `Wait` safe.
3. *Confusing "no schema will arrive" with "this transport has no schemas".* `kOk` +
   null is reserved for the second and refused for the first, so a binding cannot
   report one as the other — §7's failure mode for guessing wrong is silent wrong-slot
   decoding, not a crash. *A subscription whose schema mode changes mid-stream:*
   latched at first delivery, so §7 clause 1's "never mix" is a property of the
   subscription, not of a code path a test must police.
4. *An exception with no status, or a success-coded failure.* `PubSubError` is the
   only type leaving a provider, always carries a `PubSubStatus`, and **refuses
   `kOk`/`kPending`** at construction. *A silently renumbered status:* one
   `static_assert` per enumerator, so a reorder fails the build.
5. *A back-compatible `Blob`.* No implicit conversion from the old `shared_ptr`, so no
   site keeps the old copy by accident and no coexistence window exists.

**Rung 2 — refused typed at the door**

6. *An empty topic-segment list* ⇒ `kInvalidArgument`; one check, no default topic, no
   recovery. *A null-data/non-zero-size blob* ⇒ refused at construction, same status.
   *A negative `timeout_ms` at a C boundary* ⇒ same status, so "negative means forever"
   cannot be invented by one round and not the other. *Publishing to an undeclared
   topic on a carrying transport* ⇒ `kTopicNotDeclared`, never an implicit declaration.

**Handled residue** — each with why it could not be forbidden

7. *A `SubscribeCallback` that throws.* The provider catches at the delivery boundary,
   logs, drops the sample. **Why not forbidden:** C++ cannot stop a `std::function`
   from throwing, and `noexcept` on the alias would call `std::terminate` — the process
   death §5.3 exists to prevent.
8. *`kPending`, and `kSubscriptionEnded`.* **Why not forbidden:** §7 clause 5 makes
   asynchronous arrival the contract, and teardown before arrival is a **legitimate**
   path — both must be distinguishable, not refused.
9. *Fast DDS's **loanable** read path still materialises one owning copy per sample*
   (down from one per attachment). **Why not forbidden:** the loan is returned when
   `Take` returns and the pre-schema backlog can outlive it, so aliasing needs an owner
   holding the loan — §8/§11 assign the loaned-sample path to PDA-ABI by name and
   outrank the ledger digest. An in-scope reduction, restated in Risks.

## Premises and stop conditions

- **P1 — the method set survives**; only the *types* in the four signatures change.
  **STOP-AND-ASK** if anything needs a method added, removed, reordered, or `Publish`
  un-inverted (decision 4).
- **P2 — `SharedSchema` does not change shape** (settled, cycle-1 review):
  `MakeSharedSchema` already returns `SharedSchema(owner, owner->get())`, the aliasing
  constructor, and §3.3 imports §3.2's five *written clauses*, not `Blob`'s shape
  change. No split is requested.
- **P3 — every provider receive path can name a shared owner for the bytes it parses,
  without changing when a transport loan is returned.** For XRCE the candidate is the
  per-sample `std::vector<uint8_t> payload` (`xrce_dds_pubsub_provider.cpp:178`)
  becoming a `shared_ptr` — **not** `ts.pending`, whose `Envelope` already holds
  copies. Also verified: Fast DDS's copying path (`ReceivedData` owns its buffer) and
  the gateway's WS publish. **STOP-AND-ASK** if a path cannot.
- **P4/P5 — `PubSubError` deriving from `std::runtime_error` keeps existing catch
  sites working** (**STOP-AND-ASK** if a site outside the providers distinguishes seam
  failures by *specific* std type and cannot be migrated); PDA-DEC-2's `static_assert`
  on `Blob`'s exact type is **REPLACED, not relaxed** — its own text forbids relaxing.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` (new suite in `integration-tests/pubsub-conformance`) | §1's `Blob(owner, data, size)`: the probe hands over arena bytes **where they lie**, and the ledger records `delivered_data == published_data`; the blob outliving the callback proves the owner is real | It cannot compile today — no such constructor exists. Its standing red is the negative control in the same suite: the identical leg against a probe that copies scores 1, so an inert instrument fails |
| `CopyAccounting.BorrowedAttachmentCostsNoCopies` — PDA-DEC-2's pin, **renamed** from `…CostsExactlyOneCopy` and flipped 1 → 0 | the same constructor, in the same probe | Deliberate: the guard is red between the `Blob` change and this update. The build stops first, at P5's `static_assert`, so the update cannot be skipped |
| `ProviderConformance.*` on the sixth subject `InProcessCarrying` | the loopback's `kCarried` mode: pending arrival, pre-schema buffer, ordered flush | The subject does not exist today; on landing, clause 2 (`CallbackNeverSeesNullSchema`) and clause 1 are red until the buffering lands. Clause 3 is red if any code path can mix the two modes |
| `SeamVocabulary.AbandonedSubscriptionReportsNoSchemaWillArrive` — the §2 outcome no happy path reaches | `SchemaResolver`'s destructor resolving to `kSubscriptionEnded`, distinct from `kOk`+null | Red today: `shared_future`'s broken promise surfaces as a *thrown* `get()`, which the new type cannot express, and a `bool`-returning `Wait` cannot distinguish it at all |

The forcing condition is **both** the new test and the flipped pin; the shape the pin
cannot see (leaving `Blob` alone, adding a parallel type) is not what this does. Inner
loop `-R 'SeamVocabulary\.|CopyAccounting\.'` — the whole oracle, since a broken
instrument must not green the forcing test; forcing test alone `-R
'SeamVocabulary\.BorrowedTransportMemoryCrossesWithoutCopy'`; then
`-R 'ProviderConformance\.'`. **A full-suite run is mandated here.** *Machine checks,
so nothing graph-wide is hand-composed:* the compiler (every `Blob`, `shared_future` and
exception site), the replaced and per-enumerator `static_assert`s, `ctest -N`, and
`ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` (decision 13).

## Risks / Unknowns

- **The flipped pin claims the seam, never a transport.** Residue 9 and
  `Envelope::row`'s vector copy on every XRCE and gateway receive both survive, so
  `pubsub-conformance/README.md` must keep saying — per the 2026-09-01 scoping ruling —
  that "0 copies" is the *seam's capability*, never a claim about a transport.
- **Budget.** Public surface **7** against `new_public_surface: 3`, under the PM waiver
  of 2026-09-01 (the split was rejected: it would stand up the coexistence bridge
  rung-1 item 5 forbids). The 7th is B2's Arrow import and needs the waiver extended.
  The five retirements are **simultaneous with the additions**, not scheduled later —
  no bridge, no shim, nothing awaiting a later deletion.
- **False-green traps already hit here:** the full-suite run must pass
  `-DFLETCHER_CONFORMANCE_XRCE=ON` **explicitly** (a cached `OFF` drops two subjects and
  still reports green), and `-o run_tests=True` is a **no-op on an already-cached
  package**. New files need SPDX + copyright in their first 10 lines; clang-format is
  18.1.3. **Assumed, unverified:** no out-of-tree consumer holds `Blob` as a `shared_ptr`.

## Files-to-touch

**New:** `core/…/status.hpp`; `pubsub/…/schema_arrival.{hpp,cpp}`;
`pubsub-arrow/…/schema_import.hpp` + src;
`integration-tests/pubsub-conformance/src/seam_vocabulary.cpp`.

**Changed** — decision-5 rules land in the header that *defines* each type, so
`write_buffer.hpp` and `owned_schema.hpp` are in scope, not only `provider.hpp`:
`core/…/{types,envelope,write_buffer}.hpp` · `pubsub/…/{provider,owned_schema,
subscriber,in_process_provider}.{hpp,cpp}` · `pubsub-arrow/subscriber_arrow.*` ·
`fastdds-pubsub-provider/src/{fast_dds_pubsub_provider.cpp,internal/{envelope_codec,
data_reader_listener,ordered_delivery,schema_channel,transport_data}.hpp}` ·
`xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp` ·
`gateway/src/{ws_session,main}.cpp` · each of those five components' `tests/` and
`test_package/` **plus `fastdds-pubsub-provider/benchmarks/`**, which builds `Blob`s and
reads `result.schema` but sits outside the conanfile and CI, so it rots silently if
skipped. Also `integration-tests/pubsub-conformance/{…/copy_accounting.hpp,
src/{copy_accounting,copy_clauses,clauses,fixtures}.cpp, subjects/inprocess_main.cpp,
CMakeLists.txt, README.md}` + the four other harnesses holding `Blob`/schema-future
sites · `docs/pubsub-interface-spec.md` (§3.2/§3.4/§5.1 **and §7 cl.1**) +
`wire-format-specification.md` · `.claude/runbook.PDA-DEC.config.md`.

## Files-to-delete

- `MakeReadySchemaFuture` → `SchemaArrival::Ready`; the three `shared_future<…> schema`
  members + the two `std::async(deferred, …)` wrappers in `subscriber_arrow.cpp` →
  `SchemaArrival`. **Narrowed, disclosed:** blocking indefinitely without saying so is
  gone, and the Arrow tier's member stops being Arrow-typed — replaced by
  `SchemaArrival` + the public `ImportArrowSchema`, which also replaces the anonymous
  `ImportFromNano`.
- `using Blob = std::shared_ptr<const std::vector<uint8_t>>` → the class, and every
  `blob->size()` / `*blob` / `make_shared<const vector<uint8_t>>` with it;
  `CopyAccounting.BorrowedAttachmentCostsExactlyOneCopy` → `…CostsNoCopies` (pin 0); the
  `static_assert` on `Blob`'s exact type → its inverse, never relaxed.

## Numbers

Declared net lines **+950 / −350**. New public surface **7** (`Blob` reshaped, `SchemaArrival`,
`SchemaResolver`, `SchemaCarriage`, `PubSubStatus`, `PubSubError`, `ImportArrowSchema`) against
**5 simultaneous** retirements — **net +2**, under the PM's 2026-09-01 waiver; the 7th is B2's fix.
