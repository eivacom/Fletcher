# PDA-DEC-3 — The crossing vocabulary (reviewed as a specification)

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §3, §5,
§7 clause 1, §8/§8.1, §10 (§2, §11 constraining). Locked decisions 4, 5, 6, 7, 10,
13, 14. Rulings: 2026-08-31 ("also for attachments", ABI audience), 2026-09-01
(pin at one; what DEC does to provider code). Cited, not restated.

## Summary

Three pieces of vocabulary get a normative, **C-expressible** ownership model in
the headers, and the three providers are rewritten to it: `Blob` becomes an
owner-plus-span so the seam can carry memory Fletcher did not allocate; schema
arrival becomes a waitable handle with a single-use resolver, retiring the
`shared_future` as the contract; every failure crossing the seam becomes one error
type carrying a stable numbered status. Plus the sixth conformance subject — a
schema-**carrying** in-process loopback — and PDA-DEC-2's borrowed-attachment pin
flipped from 1 to 0. No ABI is written: every "C form" below is **prose in a C++
header**, which is what decision 14 permits and decision 5 requires.

## Design

### 1. `Blob` — an owner plus a span (§3.2, decision 6)

`Blob` stops being `shared_ptr<const vector<uint8_t>>` and becomes a value type in
`core/include/fletcher/core/types.hpp`:

```cpp
class Blob {
 public:
  Blob() noexcept = default;                       // empty: null data, zero size
  /// The ONE general form. `owner` keeps [data, data+size) alive for as long as
  /// any copy of this Blob lives. Refuses (kInvalidArgument) null `data` with a
  /// non-zero `size`, and non-null `data` with a null `owner`.
  Blob(std::shared_ptr<const void> owner, const uint8_t* data, size_t size);
  explicit Blob(std::vector<uint8_t> bytes);       // bytes Fletcher allocated
  const uint8_t* data() const noexcept;  size_t size() const noexcept;
  bool empty() const noexcept;  explicit operator bool() const noexcept;
};
using Attachments = std::unordered_map<std::string, Blob>;
```

**The normative rule, in the header** (§3.2 clauses 1–5 as obligations a C view
must honour): a blob is `{owner, data, size}`; its C form is a struct of that shape
plus `retain(owner)`/`release(owner)`; retain/release are safe from any thread,
concurrently; release never throws and never re-enters the seam; bytes are
immutable once they cross; an argument blob is borrowed for the call and a callee
that keeps it copies the `Blob` (which retains); empty is null-data/zero-size.

**Why this and not a custom-deleter `shared_ptr`** (§3.2 offers both): a custom
deleter still names a `vector`, so the bytes must live in one;
`shared_ptr<const uint8_t[]>` carries no length. The triple *is* the C form. **No
implicit conversion from today's `shared_ptr`** — that would be a coexistence
window in which every call site keeps compiling and the copy survives unnoticed.

### 2. Schema arrival — a waitable handle and a single-use resolver (§3.4)

`pubsub/include/fletcher/pubsub/schema_arrival.hpp`:

```cpp
class SchemaArrival {                     // copyable, thread-safe, shared state
 public:
  static SchemaArrival Ready(SharedSchema schema) noexcept;
  static std::pair<SchemaArrival, SchemaResolver> Create();
  /// Blocks up to `timeout` (0 == poll); false while still pending. There is no
  /// second, unbounded entry point — "forever" is `milliseconds::max()`.
  [[nodiscard]] bool Wait(std::chrono::milliseconds timeout, SharedSchema* out) const;
};
class SchemaResolver {                    // move-only, single use
 public:
  void Resolve(SharedSchema schema) &&;   // consumes the token
  ~SchemaResolver();                      // unresolved ⇒ resolves with null
};
```

`SubscriptionResult::schema` becomes a `SchemaArrival`; the `shared_future` is
**deleted, not kept as a convenience** — §3.4 permits keeping it, the lean default
does not, and two ways to learn a schema is the divergence §1 exists to prevent.
`Subscriber::SubscribeResult::schema` and `SubscriberArrow::SubscribeResult::schema`
follow, so one waiting idiom runs from provider to application and BIND binds it
rather than a `std::future`. C form (prose): an opaque arrival handle with
retain/release plus `int wait(handle, int64_t timeout_ms, ArrowSchema** out)`, and
a one-shot resolver handle whose resolve call consumes it.

### 3. Exception taxonomy — one type, one stable number (§5.1, decision 10)

`core/include/fletcher/core/status.hpp`:

```cpp
enum class PubSubStatus : int32_t {
  kOk = 0, kInvalidArgument = 1, kSchemaConflict = 2, kTopicNotDeclared = 3,
  kPayloadTooLarge = 4, kTransportFailure = 5, kNotSupported = 6, kInternal = 7 };
class PubSubError : public std::runtime_error {
 public: PubSubError(PubSubStatus, std::string what);
         PubSubStatus status() const noexcept; };
```

Values are **fixed integers, appended-only, never reordered or reused** — pinned by
one `static_assert` per value, so a renumbering fails the build rather than
silently giving the two ABI rounds different codes. Deriving from
`std::runtime_error` keeps every existing `catch (const std::exception&)` and
PDA-DEC-1 clause 8 (which asserts *that* a call failed, not which type) green;
`kOk` exists because both C boundaries need a success value in the same enum
(§5.2). **Every seam entry point translates:** the four methods in all three
providers wrap their bodies so the only exception type leaving a provider is
`PubSubError`; anything else becomes `kInternal` with the original `what()`
preserved — the honest catch-all, not a hole, because a taxonomy that lets
`std::bad_alloc` or an SDK exception through is not a taxonomy.

### 4. The other three types (§3.1, §3.3, §3.5) — prose, no code

- **`WriteBuffer`** keeps its shape and gains the normative sentence that the
  window is `{data, capacity, pos}`, borrowed for the encode call and never stored
  past it, with `Data()` (PDA-DEC-2) the window base.
- **`SharedSchema`** already satisfies §3.2's five clauses. §3.3's "§3.2's
  treatment" is read as *writing the rule*, not reshaping the type; the schema's
  own bytes cross free as the Arrow C Data Interface. See premise P2.
- **Topic segments** stay `vector<string>`; the header states the C form
  (pointer-and-count of pointer-and-length pairs, borrowed for the call) and that
  an **empty segment list is illegal** — refused at every provider's door.

### 5. Providers, and the sixth subject

All three are rewritten to the vocabulary (owner ruling 2026-09-01). Beyond
mechanical migration:

- `DeserializeEnvelope` and Fast DDS's `ParseEnvelopeBody` take a
  `shared_ptr<const void> owner` and produce blobs that **alias** the parsed
  buffer instead of copying each attachment out of it. XRCE, the Fast DDS copying
  read path and the gateway's WS publish path already own their bytes in shared
  storage, so their per-attachment copies disappear.
- `InProcessPubSubProvider` gains a construction-time `SchemaMode` (`kAbsent`
  default — gateway unchanged; `kCarried` for the new subject). In `kCarried` it
  holds a real pending arrival, buffers pre-schema samples and flushes them in
  order ahead of live ones (§7 clauses 1–2).
- The suite gains subject **`InProcessCarrying`**: one `INSTANTIATE_TEST_SUITE_P`
  line and one `ProviderTraits` row (`{kCarried, kDropsPreSubscribe}`), no new
  clause — as PDA-DEC-1 specified.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *An unowned `Blob`.* No view-only constructor; a non-null `data` without an
   `owner` is refused. §3.2 clause 1's "a callee that keeps it takes its own
   reference" becomes exactly true, and a C boundary implements "keep it" as
   `retain(owner)` with nothing else to know.
2. *A mutable or resizable blob.* `data()` is `const uint8_t*`; no non-const
   accessor, no `resize`, no `operator*` onto a `vector`.
3. *Double resolution of a schema arrival.* `SchemaResolver` is move-only and
   `Resolve` is `&&`-qualified — resolving consumes the token.
4. *A schema wait that can never end.* A resolver destroyed unresolved resolves
   the arrival with a **null** schema ("no schema will arrive"), so a torn-down
   subscription cannot hang a waiter — which is what makes an unbounded `Wait`
   safe to allow at all.
5. *A transport mixing schema-carrying and schema-less delivery.* Schema mode is
   fixed at construction; the DDS providers are `kCarried` by type. §7 clause 1's
   "must never mix the two" becomes a property of the object, not of a code path.
6. *An exception with no status.* `PubSubError` is the only type that leaves a
   provider and it always carries a `PubSubStatus` — no untyped escape.
7. *A silently renumbered status.* One `static_assert` per enumerator; a reorder
   fails the build, which is the machine check that both ABI rounds map the same
   cause to the same number.
8. *Any ABI surface* (decision 14): no `extern "C"`, C header, `dlopen`, vtable,
   host-callback struct or negotiation; C forms are prose in C++ headers. No
   crossing type is defined in terms of a future ABI's types (decision 2).
9. *A back-compatible `Blob`.* No implicit conversion from the old `shared_ptr`,
   so no site can keep the old copy by accident and no coexistence window exists.

**Rung 2 — refused typed at the door**

10. *An empty topic-segment list* ⇒ `PubSubError{kInvalidArgument}`; one check,
    one refusal, no default topic, no recovery. *A null-data/non-zero-size blob*
    ⇒ refused at construction, same status.
11. *Publishing to an undeclared topic on a carrying transport* ⇒
    `kTopicNotDeclared`. No implicit declaration, no null-schema delivery.

**Handled residue** — each with why it could not be forbidden

12. *A `SubscribeCallback` that throws.* The provider catches at the delivery
    boundary, logs, drops the sample. **Why not forbidden:** C++ cannot stop a
    `std::function` from throwing, and `noexcept` on the alias would call
    `std::terminate` — the process death §5.3 exists to prevent.
13. *A `SchemaArrival` still pending when the caller must proceed* ⇒ `Wait`
    returns false. **Why not forbidden:** §7 clause 5 makes asynchronous arrival
    the contract; refusing "not yet" would forbid late joiners.
14. *Fast DDS's **loanable** read path still materialises one owning copy per
    sample* (down from one per attachment). **Why not forbidden:** the loan is
    returned when `Take` returns and the pre-schema backlog can outlive it, so
    aliasing needs an owner that holds the loan — the loaned-sample path §11
    assigns explicitly to PDA-ABI. The **seam** copies nothing; this is a
    provider-local residue with a named owner. Raised again in Risks.

## Premises and stop conditions

- **P1 — the method set survives.** Only the *types* in the four signatures change
  (`SubscriptionResult::schema`, `Attachments`' element type). **STOP-AND-ASK** if
  anything here needs a method added, removed, reordered, or `Publish`
  un-inverted (decision 4) — do not design around it.
- **P2 — `SharedSchema` does not have to change shape.** §3.3 says it "needs
  §3.2's treatment"; this design supplies the written rule, not a new type.
  **STOP-AND-ASK** if review reads §3.3 as requiring `SharedSchema` to become an
  owner-plus-span struct — that roughly doubles the item; ask for a stage split.
- **P3 — every provider receive path can name a shared owner for the bytes it
  parses, without changing when a transport loan is returned.** Verified for XRCE
  (`ts.pending` already materialises the `Envelope`), the Fast DDS copying path
  (`ReceivedData` owns its buffer) and the gateway's WS publish. **STOP-AND-ASK**
  if a path cannot — never ship an owner-less `Blob` to make it fit.
- **P4 — `PubSubError` deriving from `std::runtime_error` keeps existing catch
  sites working. STOP-AND-ASK** if a site outside the providers distinguishes seam
  failures by *specific* std exception type and cannot be migrated.
- **P5 — PDA-DEC-2's `static_assert` on `Blob`'s exact type is REPLACED, not
  relaxed** (its own text forbids relaxing it); the replacement asserts the new
  capability, so the tripwire keeps pointing forward.

## Forcing-test mapping

| Test | Turned green by | Red for the right reason before |
|---|---|---|
| `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` (new suite in `integration-tests/pubsub-conformance`) | §1's `Blob(owner, data, size)`: the probe hands over arena bytes **where they lie**, and the ledger records `delivered_data == published_data`; the blob outliving the callback proves the owner is real | It cannot compile today — no such constructor exists. Its standing red is the negative control in the same suite: the identical leg against a probe that copies scores 1, so an inert instrument fails |
| `CopyAccounting.BorrowedAttachmentCostsNoCopies` — PDA-DEC-2's pin, **renamed** from `…CostsExactlyOneCopy` and flipped 1 → 0 | the same constructor, in the same probe | Deliberate: the guard is red between the `Blob` change and this update. The build stops first, at P5's `static_assert`, so the update cannot be skipped |
| `ProviderConformance.*` on the sixth subject `InProcessCarrying` | the loopback's `kCarried` mode: pending arrival, pre-schema buffer, ordered flush | The subject does not exist today; on landing, clause 2 (`CallbackNeverSeesNullSchema`) and clause 1 are red until the buffering lands. Clause 3 is red if any code path can mix the two modes |

The forcing condition is **both** the new test and the flipped pin. The shape the
pin cannot see — leaving `Blob` alone and adding a parallel borrowed type — **is
not what this design does**: `Blob` itself changes, so P5's `static_assert` fires
at build time and the update is forced, not remembered.

Inner loop `-R`: `'SeamVocabulary\.|CopyAccounting\.'` (whole oracle — a broken
instrument must not be able to green the forcing test); forcing test alone
`ctest -R 'SeamVocabulary\.BorrowedTransportMemoryCrossesWithoutCopy'`; then
`-R 'ProviderConformance\.'` for the sixth subject. **A full-suite run is mandated
at this item.** *Machine checks, so nothing graph-wide is hand-composed:* the
compiler (every `Blob`, `shared_future` and exception site), the replaced
`static_assert`, the per-enumerator `static_assert`s, `ctest -N`, and
`ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` (decision 13 — wire untouched).

## Risks / Unknowns

- **Oracle-wins note, raised not designed around.** Decision 7 says "accepting a
  copy anywhere on the row or attachment path is a stop-and-ask"; §8 says
  receive-side zero-copy is "*not* there" and §11 defers the loaned-sample path to
  PDA-ABI. Residue 14 sits in that gap. The seam itself copies nothing.
- **Size.** The largest non-guard item in the round. Past ~1300 changed lines the
  sanctioned split is **PDA-DEC-3a** (ownership + taxonomy + provider migration;
  `SeamVocabulary.…` + the flipped pin) and **PDA-DEC-3b** (schema arrival + the
  above-seam ripple + the sixth subject). Recommend *not* splitting up front: the
  sixth subject needs the arrival type, and a split leaves `shared_future` and
  `SchemaArrival` coexisting for a stage — the bridge the lean default forbids.
  **No coexistence window, bridge, shim or re-export is introduced** (rung-1 item
  9), and nothing here is scheduled for deletion later.
- **False-green traps already hit in this round:** the full-suite run must pass
  `-DFLETCHER_CONFORMANCE_XRCE=ON` **explicitly** (a cached `OFF` drops two
  subjects and still reports green), and `-o run_tests=True` is a **no-op on an
  already-cached package** — a provider change not `conan create`d is invisible to
  the harness. CI: `core/**`/`pubsub/**` are already in the lane's path filter;
  new files need SPDX + copyright in their first 10 lines; clang-format is 18.1.3.
- **Assumed, unverified:** no consumer outside the tree holds `Blob` as a
  `shared_ptr`; every in-tree one is in Files-to-touch.

## Files-to-touch

**New:** `core/include/fletcher/core/status.hpp`;
`pubsub/include/fletcher/pubsub/schema_arrival.hpp` + `pubsub/src/schema_arrival.cpp`;
`integration-tests/pubsub-conformance/src/seam_vocabulary.cpp`.

**Changed:** `core/include/fletcher/core/{types,envelope}.hpp` + `core/{tests,
test_package,README.md}` · `pubsub/include/fletcher/pubsub/{provider,subscriber,
in_process_provider}.hpp` + `pubsub/src/{subscriber,in_process_provider}.cpp` +
`pubsub/tests/` · `pubsub-arrow/` (`subscriber_arrow` header + src, tests,
test_package) · `fastdds-pubsub-provider/src/{fast_dds_pubsub_provider.cpp,
internal/{envelope_codec,data_reader_listener,ordered_delivery,schema_channel,
transport_data}.hpp}` + tests/test_package ·
`xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp` + tests/test_package ·
`gateway/src/{ws_session,main}.cpp` · `integration-tests/pubsub-conformance/
{include/fletcher/conformance/copy_accounting.hpp, src/{copy_accounting,
copy_clauses,clauses,fixtures}.cpp, subjects/inprocess_main.cpp, CMakeLists.txt,
README.md}` · `integration-tests/{fastdds-xrce-interop,pubsub-arrow-fastdds,
gateway-fastdds-ts,protoc-arrow-bridge}/` (the `Blob`/schema-future sites) ·
`docs/{pubsub-interface-spec.md (§3.2/§3.4/§5.1),wire-format-specification.md}` ·
`.claude/runbook.PDA-DEC.config.md`.

## Files-to-delete

- `MakeReadySchemaFuture` (`provider.hpp`) — replaced by `SchemaArrival::Ready`.
- The three `std::shared_future<…> schema` members (`SubscriptionResult`,
  `Subscriber::SubscribeResult`, `SubscriberArrow::SubscribeResult`) + the two
  `std::async(std::launch::deferred, …)` wrappers in `subscriber_arrow.cpp` →
  `SchemaArrival`. **Behaviour narrowed, disclosed:** blocking on a `std::future`
  is gone; callers state a timeout.
- `using Blob = std::shared_ptr<const std::vector<uint8_t>>` → the class; every
  `blob->size()` / `*blob` / `make_shared<const vector<uint8_t>>` goes with it.
- `CopyAccounting.BorrowedAttachmentCostsExactlyOneCopy` → `…CostsNoCopies` (same
  leg, pin at 0); the `static_assert` on `Blob`'s exact type in
  `copy_accounting.hpp` → its forward-pointing inverse (P5), never relaxed.

## Numbers

Declared net lines **+900 / −330**. New public surface **6** — `Blob` (reshaped),
`SchemaArrival`, `SchemaResolver`, `SchemaMode`, `PubSubStatus`, `PubSubError` —
paid for by retiring **5**: the `Blob` alias, `MakeReadySchemaFuture`, the three `shared_future` schema members. **Net +1.**
