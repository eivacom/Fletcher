# PDA-DEC-3 — independent code review (step 4b)

**Diff base:** `05d5a2c..60313db` (`feature/protocol-driver-abi`), 57 files, +2876/−1086.
**Reviewer:** independent; did not write this code.

**Counts: 0 blocking / 7 should-fix / 10 nits.**

The ownership rewrite is sound. I traced every `Blob` producer in the tree
(in-process, FastDDS loanable + copying + `deserialize`, XRCE `OnTopic`, the
gateway, the conformance probe) and every consumer that keeps one past its
borrow window (`OrderedDelivery`'s pre-schema backlog, XRCE `ts.pending`,
`SubscriberArrow`'s batcher, the copy-oracle's `retained`). In every case the
owner handed to `Blob` covers the bytes and outlives the span, and the
partial-parse failure path is safe *because* the `Blob` copies the owner — a
`ParseEnvelopeBody` that returns `false` halfway leaves already-built blobs
holding the body alive rather than dangling. The `SchemaArrival` state machine
has no missed wakeup, no double-resolve and no resolve-after-destruction path I
could construct; the unbounded `Wait` is woken by provider destruction because
the resolver lives in the topic map. The MSVC evaluation-order fix is correct and
I found no second instance of the shape. The `PubSubStatus` static_asserts are
pinned one value at a time, so a swap or a mid-insertion fails the build, not
just an append.

---

## should-fix

### 1. `pubsub-arrow/tests/discard_probe.cpp` was not migrated; `NodiscardTest` now greens for the wrong reason
*Confidence: high.*

```cpp
// pubsub-arrow/tests/discard_probe.cpp:37-38
fletcher::DeserializeEnvelope(bytes);                      // bytes is const std::vector<uint8_t>&
fletcher::DeserializeEnvelope(bytes.data(), bytes.size()); // no 2-arg overload exists any more
```

Both overloads are gone: `envelope.hpp` now offers only
`(shared_ptr<const void>, const uint8_t*, size_t)` and
`(const shared_ptr<const vector<uint8_t>>&)`, and a `const vector&` does not
convert to the latter. These are hard compile errors, not `nodiscard`
diagnostics.

The test is `PASS_REGULAR_EXPRESSION "C4834|ignoring return value"` over the
build output of a TU that is *expected* to fail to build, so it still passes — on
the strength of the `nodiscard` diagnostics emitted by the *other* lines in the
same TU. Net effect: the probe silently stopped guarding `DeserializeEnvelope`'s
`[[nodiscard]]`, and the gate cannot tell the difference. This file was not
touched by the diff.

### 2. `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy`'s ownership half is vacuous
*Confidence: high.*

The test's second claim is *"Ownership is real: a callee that keeps the blob past
the borrow window still reads those same bytes"*, asserted by
`EXPECT_TRUE(trip.ledger.retained_content_ok)` with the message *"its owner does
not keep [data, data+size) alive"*.

But the arena is kept alive independently by `provider`, a local `shared_ptr` in
`RunBorrowedAttachmentRoundTrip` that outlives the `RunCaptured` call in which
`retained_content_ok` is computed. The assertion passes whether or not the `Blob`
owns anything — a span with no owner would satisfy it too, which is exactly the
case the comment says it distinguishes. To have teeth, the run must drop the
provider (and the runner) before reading `retained`. The *provenance* half
(`retained_data == loaned.published_data`) is genuine and unaffected.

### 3. The `Publisher` tier still reports a schema conflict as an untyped `std::runtime_error`
*Confidence: high.*

`pubsub/src/publisher.cpp:51` throws `std::runtime_error` for §7 clause 3, and it
short-circuits *before* reaching the provider — so an application using
`Publisher` (the gateway's own path, and the tier `PublisherArrow` sits on) never
observes `kSchemaConflict` for a re-declaration. The same logical failure carries
a stable number from a provider and no number at all one layer up. Same family:
`Subscriber::Unsubscribe`'s unknown-id `std::runtime_error`, and the
null-provider `std::invalid_argument`s. Nothing in the tree notices, because the
conformance local subject holds a `PubSubProvider` directly.

### 4. The same encoder failure gets two different statuses depending on a QoS option
*Confidence: high.*

- Loaned publish path (`LoanableSampleWriter::Write`): the encoder throws, it
  propagates as thrown, `TranslateSeamFailure` maps it to `kInternal` (or
  `kPayloadTooLarge` for the overflow case).
- Copying publish path: `FletcherSamplePubSubType::serialize` catches the
  encoder's exception, records `serialize_error`, and `SampleWriter::Write`
  rethrows it as `PubSubError(kTransportFailure, ...)`
  (`internal/sample_writer.hpp:71`).

So a deterministic bug in the caller's encoder is reported as a *transport*
failure on one flow and an *internal* failure on the other, selected by
`loan_publish`. A binding that retries `kTransportFailure` will retry forever.
(The oversized-row asymmetry — `kPayloadTooLarge` on the loaned path, silently
dropped and logged on the copying path — is pre-existing, but the taxonomy is
what makes it a statable inconsistency now.)

### 5. `EnvelopeAttachmentCount` exists to let a caller get the owner rule wrong
*Confidence: high. This is the simplification in the forbidding direction, and it
answers the public-surface question.*

`core/envelope.hpp` now ships a public peek function whose whole purpose is to
let a caller decide whether `DeserializeEnvelope`'s `owner` argument may be null.
It has exactly one caller:

```cpp
// gateway/src/ws_session.cpp:264-269
if (EnvelopeAttachmentCount(parts.envelope_data, parts.envelope_size) > 0) { owner = ...copy...; }
auto envelope = DeserializeEnvelope(owner, owner ? owner->data() : parts.envelope_data, ...);
```

The precondition it exists to serve ("`owner` may be null ONLY for a buffer
carrying no attachments") is not even checked by `DeserializeEnvelope`; it is
enforced incidentally, mid-parse, by `Blob`'s constructor throwing. And it buys
nothing here that the function could not do for itself: `Envelope::row` is
already an owning `EncodedRow` copy, so a `DeserializeEnvelope(const uint8_t*,
size_t)` that takes its own shared copy of the buffer *only when the frame
carries attachments* is byte-for-byte equivalent for this caller, deletes the
peek function, deletes the null-owner rule, and makes the bad state
unrepresentable rather than documented.

Keep the 3-argument form: XRCE's `OnTopic` has a legitimate use for it (it
already allocated the payload and must not pay a second copy). But
`EnvelopeAttachmentCount` should not be public — it should not exist.

On the other two free functions: `PubSubStatusName` is the naming half of a
public enum's contract and belongs where it is. `TranslateSeamFailure` must be
public because FastDDS and XRCE are separate Conan packages that consume core's
public headers; it is provider-authoring API rather than caller API, so a
`fletcher/core/provider_support.hpp` (or a doc line) would say so without hiding
it. Neither can move to `internal/`.

### 6. `SchemaResolver::Resolve(nullptr)` poisons the arrival, and the normative header does not say so
*Confidence: high.*

The header promises only *"**Refuses null** with kInvalidArgument"*. The
implementation additionally settles the shared state to `kInternal` before
throwing (`schema_arrival.cpp`), so the token is consumed and the arrival is
terminally `kInternal` — a waiter is answered a failure the header never
mentions, and a binding author reading only the header would implement this
differently. Either document the settle or leave the token unconsumed on
refusal. (The `.cpp` comment explains the choice; the normative header is the one
bindings read.)

### 7. A large-but-not-`max()` timeout silently returns `kPending` instead of waiting
*Confidence: medium.*

`Wait` routes only the exact `milliseconds::max()` to the unbounded
`cv.wait(...)`; everything else goes to `cv.wait_for(lock, timeout, pred)`, which
computes `now + timeout` and overflows the implementation's `time_point` for
values near `INT64_MAX`. The documented C form is `timeout_ms < 0` refused /
`INT64_MAX` unbounded, which invites a binding to pass "a very large number" for
"effectively forever" — and it will get an immediate `kPending` with no error.
Clamp anything above a safe threshold onto the unbounded path.

---

## nits

- `fastdds-pubsub-provider/test_package/src/example.cpp:49` ships a leftover
  `fprintf(stderr, "DBG status=%s msg=%s\n", ...)` from the evaluation-order hunt.
- `SeamVocabulary.AbandonedSubscriptionReportsNoSchemaWillArrive`:
  `EXPECT_EQ(out, nullptr) << "*out is untouched for every outcome except kOk"` —
  `out` is already null, so it passes whether `Wait` leaves it alone or writes
  null into it. Pre-set `out` to a non-null schema to make the claim real.
- `SeamVocabulary.ResolverRefusesNullAndWaitRefusesNegativeTimeout` checks
  `EXPECT_THROW(..., PubSubError)` for the null resolve without checking the
  status, and never checks what the arrival reports afterwards.
- `SeamVocabulary.BorrowedTransportMemoryCrossesWithoutCopy` re-asserts
  `CopyAccounting.BorrowedAttachmentCostsNoCopies` almost line for line
  (including its own staging control); only the `retained_*` legs are new.
- `Blob(std::vector<uint8_t>{})` leaves `data()` at whatever `vector::data()`
  returns for an empty vector, which is not guaranteed null — the class's own
  clause 5 ("Empty is a null data pointer and a zero size") is unenforced.
- `Subscribe` on an already-subscribed topic reports `kNotSupported` in both DDS
  providers; that is a caller error, not an unimplemented behaviour —
  `kInvalidArgument` fits the enum's own comments better.
- `TranslateSeamFailure` calls `fn()` rather than `std::forward<Fn>(fn)()`.
- A user `RowEncoder`/`SubscribeCallback` that throws `std::overflow_error` for
  its own reasons is misclassified as `kPayloadTooLarge`; one that throws a
  non-`std::exception` type loses its identity entirely at the `catch(...)`
  (previously it propagated unchanged out of `InProcessPubSubProvider::Publish`,
  which now dispatches the subscriber callback inside the translation wrapper).
- `SchemaArrival` is documented "Copyable, thread-safe"; concurrent *assignment*
  to one `SchemaArrival` object is still a data race on the `shared_ptr` itself.
  "Every copy observes the same arrival, and distinct copies may be waited on
  concurrently" is what the code actually gives.
- `ProviderConformance::RemainingBudget()` truncates sub-millisecond remainders
  to zero, turning the tail of a budget into a poll. Harmless here; noted because
  `wait_until(Deadline())` did not.
- `FletcherSamplePubSubType::deserialize` calls `d->body.reset()` before
  `ParseEnvelopeBody` clears `d->decoded_attachments`, so for an instant the map
  holds blobs whose owner has been released. Safe today (nothing dereferences in
  between, and the blobs hold their own owner reference), but the ordering is
  fragile; reset after the parse.

---

## Line-count assessment (counted, not taken on trust)

Actual **+2876/−1086** against a declared +950/−350. I counted it:

| bucket | added lines |
|---|---|
| `.hpp`/`.cpp` total | 2622 |
| …of which comment or blank | **1180** (45%) |
| …of which byte-identical to a removed line modulo indentation (the `TranslateSeamFailure` re-indent) | **371** |
| `docs/` + `README.md` | 215 |
| **genuinely new non-comment code** | **≈1071** |

The implementer's account holds this time. If anything it understates the prose:
I count 398 added comment lines in the eight vocabulary headers alone
(`types.hpp` 58, `status.hpp` 82, `write_buffer.hpp` 17, `owned_schema.hpp` 28,
`schema_arrival.hpp` 101, `provider.hpp` 40, `schema_import.hpp` 33,
`in_process_provider.hpp` 39) against the claimed 323, and overstates docs
(215 actual vs 275 claimed). ~1071 new code lines against a declared 950 is
close; the 3× is prose and re-indentation, not undeclared machinery. Judged
proportionate. The two places volume is not carrying weight are finding 5
(`EnvelopeAttachmentCount` + the null-owner rule + the gateway's conditional
dance, ~25 lines a 2-argument overload would delete) and the `SeamVocabulary`
duplication of the copy oracle's leg 3.

---

## Things I checked and found clean

- **MSVC evaluation order.** The `SchemaArrival::Create()` fix is correct (two
  named locals, then a two-`std::move` return of independent objects). I swept
  every added line containing `std::move` inside a multi-argument call or braced
  init-list: 11 candidates, all with disjoint moved-from and read-from operands.
  `SchemaChannel`'s constructor and `InProcessPubSubProvider::Subscribe` use the
  same two-statement pattern.
- **Re-anchored tests.** `EnvelopeTest` (all 9), `SubscriberTest`,
  `pubsub-arrow`, `FletcherSamplePubSubTypeTest.RoundTripsAttachments`,
  `ProviderConformance` clause 10, `test_interop`, `test_roundtrip`,
  `test_pubsub`, and the four `test_package` examples: every one gained
  assertions rather than losing them. The strongest additions are the aliasing
  checks (`EXPECT_GE(at, base); EXPECT_LT(at, base + size)`) that the old
  copy-out parser could not have satisfied, and
  `LoanedOversizedRowThrowsWithoutLeakingLoans` now pinning `kPayloadTooLarge`
  rather than a bare exception type.
  `CopyAccounting.BorrowedAttachmentCostsNoCopies` is a deliberate, argued 1→0
  flip with the three-valued control intact. `copy_accounting.hpp`'s replacement
  `static_assert`s (not-the-old-alias, has-the-owner-span-ctor,
  not-convertible-from-the-alias) are the inverse of the old tripwire and
  genuinely load-bearing — the third closes the coexistence window by itself.
  The only test that lost coverage is finding 1 (`discard_probe.cpp`).
- **`LaterDeclarationNeverReachesALiveSubscription`** genuinely exercises the
  kAsDeclared latch: two publishes straddling a `CreateTopic`, both asserted to
  carry null, plus the positive control that a *new* subscription does see the
  declaration. No conformance subject reaches it, as claimed. The behaviour
  change is contained for the gateway, which ignores the per-delivery schema and
  only forwards `schemaIpc` synchronously from the subscribe response.
- **Concurrency.** `SchemaArrivalState::Settle` notifies outside the lock,
  predicates guard every wait, `Break`/`Resolve` in `SchemaChannel` move the
  token out under the channel mutex and consume it outside — no inversion with
  the provider mutex, which was the documented hazard. The in-process provider
  resolves outside its own lock. `SchemaResolver`'s destructor as the third
  terminal outcome is reached on provider destruction, so an unbounded `Wait`
  cannot outlive its provider. `optional<SchemaResolver>` move-then-`reset()` is
  used consistently and correctly (a moved-from resolver settles nothing).
- **Exception translation.** All twelve provider entry points (4 × 3 providers)
  are wrapped. `TranslateSeamFailure`'s catch order is correct
  (`PubSubError` → `overflow_error` → `exception` → `...`; the first two are
  siblings under `runtime_error`, so their relative order is immaterial). The
  `std::overflow_error → kPayloadTooLarge` mapping is justified:
  `FixedWriteBuffer` is its only thrower in non-test code. Findings 3 and 4 are
  the two holes.
- **`SchemaListener` / XRCE `OnTopic`.** Both still guard every path that can
  throw across a C frame; the new `Resolve` calls are inside those guards.

---

RECORD: `pubsub/src/in_process_provider.cpp` `Publish` comment says "mu_ is held
across the callback" / "re-enters Unsubscribe() would null the std::function" but
the member is `impl_->mu`, not `mu_`.
RECORD: `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp` `Subscribe`
still says "A fresh promise/future is wired up" and "hand back the schema
future"; `Unsubscribe` still says "break the promise ... the promise is not
destroyed unsatisfied".
RECORD: `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp` `CreateTopic`
still says "leaves every subscriber of this topic waiting forever on a future
that never resolves"; `Subscribe` still says "resolve the future immediately".
`schema_channel.hpp`'s `SchemaListener` class comment still says "resolves the
subscription's schema future".
RECORD: `copy_clauses.cpp` leg-3 trailing comment still reads "Standing proof
that the \"1\" above is a measurement" after the pin became 0.
