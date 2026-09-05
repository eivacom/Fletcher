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

---
---

# Re-check after fix cycle 1 - 2026-09-01

**Range:** `60313db..a24dd8e` (27 files, +470/-155). Whole item vs `05d5a2c`: +2722/-565.
**Method:** built and mutated, not read. The conformance harness was rebuilt from source at
`a24dd8e` against the `a24dd8e` core/pubsub Conan packages; `pubsub-arrow` rebuilt; plus two
standalone MSVC 14.44 probes (one over `envelope.hpp`/`Blob`, one compiling
`pubsub/src/schema_arrival.cpp` directly so the `Wait` clamp could be mutated).

**Verified independently:** `pubsub-conformance` **62/62** (XRCE ON, Agent up),
`pubsub-arrow` **16/16**.

**Verdict: all seven should-fix findings closed. 0 blocking / 2 should-fix / 3 nits new.**
Both new should-fix items are about a *guard* claiming teeth it does not have; neither is a
defect in shipped behaviour.

## Status of my seven

| # | finding | status |
|---|---|---|
| 1 | `discard_probe.cpp` stale; gate greens on neighbours | **CLOSED - mutation-verified** |
| 2 | `retained_content_ok` vacuous | **CLOSED - mutation-verified; the fix is stronger than described** |
| 3 | `Publisher` tier reports conflict untyped | **CLOSED** |
| 4 | encoder failure: two statuses by QoS option | **CLOSED; the "pre-existing" claim checks out this time** |
| 5 | `EnvelopeAttachmentCount` public / owner rule callable wrongly | **CLOSED - and the bad state is now unrepresentable** |
| 6 | `Resolve(nullptr)` terminal settle undocumented | **CLOSED** |
| 7 | huge-finite timeout | **product side CLOSED (defensively); the test that claims to pin it is vacuous - see N1** |

---

## S1 - closed, and the fix is load-bearing (measured)

Three mutations against the rebuilt harness, 20 runs each.

| configuration | `SeamVocabulary` forcing test |
|---|---|
| clean | **20/20 PASS** |
| **MUT-A** - provider hands `Blob(std::make_shared<int>(0), loan_base_, loan_len_)` (bystander owner) | **20/20 FAIL** |
| **MUT-A2** - bystander owner **and** the `0xDD` fill in `~Arena` removed | **20/20 PASS - undetected** |
| **MUT-B** - a stray `static shared_ptr` keep-alive on the probe | **FAIL at the `weak_ptr` assert** |

Answering the three questions put to me:

**(a) A bystander owner fails only the ownership assertion.** Under MUT-A the sole failure is
`seam_vocabulary.cpp:97` (`retained_content_ok`). `subject_released` (line 92), provenance
`retained_data == published_data` (line 95), `attachment_copies == 0` and `row_copies == 0` all
still pass, and the whole `CopyAccounting` suite stays **7/7 green**. The ownership claim is now
isolated to the one assertion that means it.

**(b) A re-introduced keep-alive trips the `weak_ptr` assert.** MUT-B fails at line 92 with the
intended message, and because it is an `ASSERT_TRUE` it aborts before the now-vacuous assertions
below it can report a misleading green.

**(c) I could not find a third vacuity route.** Every other path fails closed: `retain_expected`
empty gives `retained_content_ok == false`; nothing retained gives `retained.data() == nullptr`,
so both fields stay at their defaults and both assertions fail; a zero-size blob gives a null
`data()` by clause 5, likewise; `release_subject` absent leaves `subject_released` false and the
`ASSERT` fires. `retain_expected` is a genuinely independent allocation (vector copy-assign from
the harness `loaned`, then moved with the ledger), so the `memcmp(p, p, n)` route the implementer
found is gone.

**The poisoning cannot fake a pass, and it is not decorative.** It can only produce a mismatch,
never a match - `retain_expected[0] == 0x07` and the compare is full-length. And MUT-A2 shows it
is doing real work: with the fill removed, the freed arena bytes survive intact in the MSVC
Release heap and the bystander-owner mutation goes **completely undetected, 20/20**. Without that
destructor this leg would be luck rather than measurement.

Instrumenting the clean build confirms the mechanism directly: at the moment the retained blob is
read, `arena_deaths == 0` and `subject_released == 1` - the probe is gone and the arena is alive
on the `Blob`'s reference alone.

**Residual (nit N3).** The teeth come entirely from `~Arena` running. Any future change that keeps
the `Arena` alive independently - a function-local `static`, a `shared_ptr` with a no-op deleter -
restores MUT-A2's silence without touching a line of `seam_vocabulary.cpp`. `subject_released`
guards the *provider's* death, not the arena's.

## S2 - closed, and the new regex catches exactly the rot I found

Verified by running the gate. The three `DeserializeEnvelope` discards sit at
`discard_probe.cpp` lines 50, 51 and 52, and the build output carries `error C4834` at **50, 51
and 52** - the probe now matches its own lines. No `error C2xxx`/`C3xxx` appears anywhere in the
output. Test passes.

Re-introducing the exact rot (`DeserializeEnvelope(bytes)` with the retired `const vector&`):

```
16: ...discard_probe.cpp(50,15): error C2665: 'fletcher::DeserializeEnvelope': no overloaded
    function could convert all the argument types
1/1 Test #16: NodiscardTest.CompileFailsOnDiscard ...***Failed
    Error regular expression found in output. Regex=[error C2[0-9][0-9][0-9]|error C3[0-9][0-9][0-9]]
```

Red - despite the neighbouring lines still emitting `C4834`, which is precisely the failure mode
that hid it last time. Restored gives green. `pubsub-arrow` 16/16.

## S3 - closed, the re-anchoring is a strict strengthening

`RefusedWith(status, call)` fails three ways the old `EXPECT_THROW(std::...)` did not: the call
not being refused at all, a refusal carrying the wrong number, and a refusal carrying an
*untyped* exception (with a message naming that as the thing the taxonomy replaces). Four tests
migrated; none lost an assertion. `Publisher publisher(nullptr)` is spelled as a declaration, so
there is no most-vexing-parse.

## S4 - closed, and this time "pre-existing" holds up

Checked rather than taken, since it was wrong once already:

- `FastDDSPubSubProviderTest.DataSharingOversizedRowDoesNotThrow` exists at base
  `05d5a2c:.../test_fast_dds_pubsub_provider.cpp:459` and today at line 472 - genuinely
  test-pinned, not newly asserted.
- The drop-and-log `catch (const std::overflow_error&)` in `serialize()` exists at base
  (`05d5a2c:.../fletcher_sample_pub_sub_type.hpp:105`) - genuinely pre-existing.
- At base, `sample_writer.hpp` threw a bare `std::runtime_error`, so no *taxonomy* asymmetry
  existed to be pre-existing. What survives is a behavioural difference (throw vs drop-and-log),
  which is exactly what the new comment claims.

Encoder failure is now `kInternal` on both flows, so the QoS-selected split is gone.

## S5 - closed, and the bad state is now unrepresentable

`EnvelopeAttachmentCount` is in `namespace fletcher::internal`, the gateway's conditional dance is
gone, and the two-argument `DeserializeEnvelope(const uint8_t*, size_t)` is a **restoration** -
`git show 05d5a2c:core/include/fletcher/core/envelope.hpp` has that exact signature - so the
surface did not grow. Every `PubSubStatusName` reference in the tree is now `internal::`-qualified.

I verified the lifetime and aliasing of the new overload empirically (standalone MSVC probe
against the packaged headers, 11/11 checks):

- an **attachment-free** frame: the peek returns 0, the parse takes the null-owner branch, and
  nothing beyond `Envelope::row` is copied - the claim holds;
- a frame **with** attachments: the blob does **not** alias the caller's buffer, and its bytes still
  read back correctly after that buffer is `memset` to `0x5A` **and freed** - the hazard this
  overload exists to remove;
- an empty attachment comes back `size()==0, data()==nullptr` (clause 5);
- a corrupt attachment count is refused, not amplified into an allocation.

I also tried to construct a frame where the peek and the parse disagree - peek says "no
attachments", the parse then builds a non-empty blob with a null owner. **It is impossible by
construction:** the peek returns 0 only when the buffer is too short (the parse then throws) or
when the count field is literally 0 (the parse loops zero times). The two read the same field with
the same arithmetic.

## S6 - closed

The header now states all three parts of the terminal refusal (throws `kInvalidArgument`, consumes
the token, settles the arrival at `kInternal`), and
`ResolverRefusesNullAndWaitRefusesNegativeTimeout` asserts the status of the throw, the resulting
`kInternal`, and a non-empty `Message()`. Non-vacuous: it would fail if the arrival were left
pending or settled `kOk`.

## S7 - product side closed defensively; see N1 for the test

---

## New findings

### N1 (should-fix) - the huge-finite-timeout test is vacuous, and cannot be made non-vacuous on MSVC

*Confidence: high - measured two ways.*

```cpp
EXPECT_EQ(abandoned.Wait(std::chrono::milliseconds(std::numeric_limits<int64_t>::max()), &out),
          PubSubStatus::kSubscriptionEnded)
    << "a huge finite timeout must wait, not overflow into an immediate kPending";
```

`abandoned` is **already settled** (its resolver was dropped), and `Wait` reads `timeout` only
inside `if (!state_->settled)`. The call therefore returns before the clamp is ever consulted.
Two proofs:

1. Instrumented, it returns in **0 microseconds** - it never entered the wait at all.
2. Compiling `pubsub/src/schema_arrival.cpp` standalone and mutating the clamp back to
   `timeout == milliseconds::max()`, this exact assertion **still passes**.

Worse for the claim: I also wrote the *non*-vacuous version (a genuinely pending arrival settled
by another thread after 300 ms) and it passes with the clamp removed too - MSVC 14.44's `wait_for`
clamps the deadline internally, so it blocked 308 ms either way. On this toolchain the clamp is
**unfalsifiable**.

The clamp itself is fine and worth keeping (other standard libraries have not always been so
careful, which was the case I raised), but the assertion that claims to guard it guards nothing,
and its message asserts a failure mode that does not occur here. Either drop the pretence and keep
the clamp as documented defence, or assert it somewhere it can bite. Flagged rather than waved
through because this is the fourth guard this round that passed for a reason other than the one it
stated.

### N2 (should-fix) - `RunCaptured` holds a dangling `CopyRunner&` after `release_subject()`

*Confidence: high.*

`RunBorrowedAttachmentRoundTrip` passes `*runner` by reference and then has `release_subject` do
`runner.reset()`. From that point the `CopyRunner& runner` parameter inside `RunCaptured` is a
dangling reference for the remainder of the function. Nothing touches it today -
`runner.Unsubscribe(topic)` happens before the release - so this is not a live defect, but it is a
use-after-free that any line added below the release acquires silently, in the one function whose
job is to be trustworthy. Take the runner by `unique_ptr&`, or move the release to the caller
after `RunCaptured` hands the retained blob back.

### Nits

- **N3.** The S1 teeth depend on `~Arena` running: a future `Arena` that outlives the provider (a
  function-local `static`, a no-op deleter) silently restores MUT-A2's 20/20 silence, and
  `subject_released` would not notice - it watches the provider, not the arena.
- **N4.** Under a failing mutation the leg reads the freed arena (that is how it detects the
  fault). Deterministic here because the destructor poisons first, but it would surface as a crash
  rather than an assertion under ASan.
- **N5.** `Blob(owner, ptr, 0)` now normalises `data` to null but still **retains `owner`**, so a
  zero-length attachment can pin a whole transport body. No caller does this today (both parsers
  use `Blob()` for empty). My earlier clause-5 nit is otherwise closed:
  `Blob(std::vector<uint8_t>{})` and `Blob(nullptr, ptr, 0)` both come back `{nullptr, 0}`,
  verified.

My earlier nits are otherwise resolved: the `DBG` fprintf is gone, `std::forward` added,
`body.reset()` moved after the parse (with the reason recorded), `RemainingBudget` uses
`std::chrono::ceil`, double-subscribe is `kInvalidArgument` in both DDS providers, the XRCE
`schema_resolved` shadow flag is gone (the optional is the flag), and the `SchemaArrival`
thread-safety claim is now stated precisely.

## The declared disagreement - I withdraw my nit; keep the overlap

I asked for the `SeamVocabulary` / `CopyAccounting` leg-3 overlap to be collapsed. The measurement
settles it against me: under **MUT-A**, `CopyAccounting` stays **7/7 green** while the
`SeamVocabulary` forcing test goes red. Provenance and ownership are separately falsifiable, the
two suites catch different defects, and collapsing them would have deleted the only assertion that
survives a bystander owner. The duplicated staging control is the price of the forcing test
standing on its own instrument, which is the right trade. Accepted as argued.

## Note on the working tree

Mid-review, `git status` briefly showed an uncommitted
`if (false && segs.empty())  // MUTATION C: refusal removed` in
`pubsub/include/fletcher/pubsub/internal/segments.hpp` - a live mutation marker in a shipped
header. It was reverted while I worked and the tree is clean now (the section 3.5 test in
`conformance_seam_vocabulary` was green throughout, so no packaged build ever carried it).
Recorded only so nobody is surprised to meet it in a stash.
