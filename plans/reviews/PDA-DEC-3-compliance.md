# PDA-DEC-3 — compliance review (step 4a, independent/adversarial)

**Verdict: ISSUES.** 3 blocking. The vocabulary itself conforms — the five retirements are
simultaneous, PDA-DEC-2's tripwire is discharged honestly (verified by mutation), and all four
declared deviations survive scrutiny. What does not hold up is the **evidence**: the mandated
full-suite run was made in the configuration the design named as a false-green trap, with the
two XRCE conformance subjects absent — against the largest single rewrite in the diff.

Diff base `05d5a2c..60313db`. Working tree clean at review time.

---

## BLOCKING

### B1 — The full-suite run dropped the XRCE subjects. The design named this exact trap.

The design's Risks section: *"the full-suite run must pass `-DFLETCHER_CONFORMANCE_XRCE=ON`
**explicitly** (a cached `OFF` drops two subjects and still reports green)"*. PDA-DEC-2's log
records the same trap being hit and fixed. It was hit again here.

Verified in the implementer's own build tree (`integration-tests/pubsub-conformance/build`):

| Evidence | Value |
|---|---|
| `CMakeCache.txt` | `FLETCHER_CONFORMANCE_XRCE:BOOL=OFF` |
| `ctest -N -C Release` | **47** `ProviderConformance` entries — `InProcessLocal` 11, `InProcessCarrying` 12, `FastDdsLocal` 12, `FastDdsCrossProcess` 12. No `Xrce*` subject present. |
| `Release/conformance_inprocess.exe`, `…_carrying.exe`, `…_fastdds.exe` | built **20:01–20:02** |
| `Release/conformance_xrce.exe`, `…_xrce_peer.exe` | built **16:38** — 3½ hours before the change |
| commit `60313db` | 20:04:42 |

The commit message's **"Conformance 47/47"** is therefore precisely the XRCE-OFF number. With
`FLETCHER_CONFORMANCE_XRCE=ON` the suite is **71** (47 + 12×2 XRCE subjects). CMake says so
itself: *"FLETCHER_CONFORMANCE_XRCE=OFF — the XrceLocal and XrceCrossProcess subjects do NOT
exist in this build"*.

Why this blocks rather than being a record correction:

- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp` is the **largest single rewrite in
  the diff** (+401/−363): four `TranslateSeamFailure` wrappers, the `shared_future` →
  `SchemaArrival`/`SchemaResolver` replacement, and premise **P3**'s change — the per-sample
  `std::vector<uint8_t> payload` becoming a `shared_ptr` so parsed attachment `Blob`s **alias**
  it and can outlive the call in `ts.pending`.
- That aliasing is the item's central safety claim on the XRCE path, and **nothing else in the
  tree exercises it.** The XRCE package's 11 Agent-free unit tests did run and pass (20:00,
  `LastTest.log`) — but they are construction, id-encoding and wire-layout tests; none goes
  through `OnTopic`'s receive path.
- `integration-tests/fastdds-xrce-interop` was rebuilt (19:56) but also needs a live Agent; no
  run evidence was presented for it either.

**Owed before close:** re-run `pubsub-conformance` with `-DFLETCHER_CONFORMANCE_XRCE=ON` against
a live MicroXRCEAgent — expect **71/71**, with `conformance_xrce` *run*, not skipped — plus
`fastdds-xrce-interop`. If such a run already exists, B1 collapses to a RECORD correction of the
commit message's number and nothing more.

### B2 — Governed-number breach: the honest count is 10, not 7. Recommend ratifying 9.

The PM waived public surface at **7**. Landed in public headers, beyond the 7:

| Addition | Header | Product callers | Verdict |
|---|---|---|---|
| `EnvelopeAttachmentCount` | `core/…/envelope.hpp` | `gateway/src/ws_session.cpp:264` | **Genuinely forced.** `DeserializeEnvelope`'s new signature makes the caller decide whether it needs a shared owner *before* paying for one; the caller is a different component. It is the counterpart of a changed public function, sitting beside it. Ratify. |
| `TranslateSeamFailure` | `core/…/status.hpp` | 12 sites across three provider packages | **Required to exist and be shared.** It is the mechanism spec §5.1's "every seam entry point translates" bullet names, so publishing it is defensible. Ratify — or move to `core/…/internal/` (the `pubsub/…/internal/segments.hpp` cross-package precedent allows it) if the PM wants 8. |
| `PubSubStatusName` | `core/…/status.hpp` | **none** | **Avoidable, and the stated justification is false.** Every caller is a test or harness: `clauses.cpp:285,293`, `local_subject.cpp:29`, two integration tests, one `test_package` example. Translation needs no name function; the claim that it is "mechanically required by *every seam entry point translates*" does not hold. Move to `internal/`, or let the harness carry its own table. |

**Recommendation: ratify 9** (the 7 + `EnvelopeAttachmentCount` + `TranslateSeamFailure`), and
require `PubSubStatusName` out of the public header. 8 is available if `TranslateSeamFailure` goes
to `core/…/internal/` too; 10 is available if the PM prefers no churn, but the record must then
say the "mechanically required" justification for `PubSubStatusName` was not accepted.

`internal::RequireSegments` correctly does not count (`pubsub/…/internal/segments.hpp`).
`SchemaArrival::Message()` is a method on a counted type, not an eighth entry.

### B3 — The empty-segment refusal is normative, newly behaviour-changing, and untested.

Design rung 2, item 6: *"An empty topic-segment list ⇒ `kInvalidArgument`; one check, no default
topic, no recovery."* Spec §3.5 as amended now states it as a rule binding **every** method.
`internal::RequireSegments` is correctly on the path of all 12 provider entry points (all route
through `JoinSegments`/`JoinSegmentsInto`).

**No test anywhere in the tree asserts it.** Delete `RequireSegments` and nothing goes red — the
`SeamVocabulary` suite pins four of the five door refusals (unowned blob, null-data blob,
`Resolve(nullptr)`, negative timeout) and skips this one. It is also a *behaviour change*:
`JoinSegments({})` returned `""` — a legal topic key — before this commit and throws now, and it
is reachable from a WebSocket client (`WsSession::SplitTopic("")` yields an empty vector).

I confirmed the change is otherwise safe — both gateway handlers wrap in
`catch (const std::exception&)` and `PubSubError` derives from `std::runtime_error`, so P4 holds
and the client gets an error frame rather than a torn-down session. But a corner case the design
placed on rung 2 shipped with no guard. ~5 lines in `seam_vocabulary.cpp`.

---

## Specification quality — this item is read by two rounds that never talk to each other

Each of the following is a sentence where two competent readers build different things. None is
individually large; collectively they are the class of defect this item exists to prevent.

**S1 — `SchemaArrival::Wait`: the refusal *mechanism* is unstated.** The header says a negative
`timeout`, and a null `out`, are *"refused with `kInvalidArgument`"* — on a function whose return
type **is** `PubSubStatus`. The implementation **throws** `PubSubError`. The C form in the same
comment block reads `fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)` … *"`timeout_ms
< 0` is refused with `kInvalidArgument`"*, which a reader will implement as a **returned** status.
Per the 2026-09-01 ruling the bindings interface to the abstract C++ interface, so this is the
binding-facing path: BIND-Rust reading the sentence as written gets an uncaught C++ exception
across FFI. One sentence: *refusals at this entry point throw; the C form returns the same status.*

**S2 — `kOk` + null and the owner handle.** The header pins *"On kOk the handle is a NEW reference
the caller releases"* — unconditionally — but `kOk` + null is a documented `kOk` outcome in which
the owner is null. It never says that on that path `out->owner` is null and must **not** be
released. A boundary following the sentence literally releases a null handle on every schema-less
transport.

**S3 — `Blob` clause 5 versus `empty()`.** Clause 5 is normative: *"Empty is a null data pointer
and a zero size."* But `Blob(std::vector<uint8_t>{})` yields `{owner != null, data possibly
non-null, size == 0}`, and `empty()` is `size_ == 0` alone. A C view that asserts `data == NULL`
whenever `size == 0` — a reading clause 5 invites — is wrong against Fletcher's own constructor.
State that `size == 0` is the test and null `data` is permitted, not required.

**S4 — `SchemaResolver::Resolve(nullptr)` has an undocumented second outcome.** The header says
only *"Refuses null with `kInvalidArgument`"*. The implementation **also consumes the token and
settles the arrival at `kInternal`** with its own message. So the caller sees `kInvalidArgument`,
a concurrent waiter sees `kInternal`, and the resolver cannot be retried with a valid schema. All
three facts are normative for anyone implementing `resolve` at a boundary.

**S5 — "refuses" is coercion.** `PubSubError` does not refuse `kOk`/`kPending`/`kSubscriptionEnded`;
it silently coerces them to `kInternal`. The property rung-1 item 4 demanded (no success-coded
failure) *is* upheld, and `status.hpp` argues the choice well (throwing from inside a throw
expression is worse). But spec §5.1's bullet says "refuses" flat, and a boundary author will not
reproduce a coercion they were not told about. One word.

**S6 — the `std::overflow_error` mapping is in the code and not in the spec.** See deviation (d).
§5.1 as amended says *"anything else … becomes `kInternal`"*, which the landed
`TranslateSeamFailure` contradicts. Also: the catch is broad — a `std::overflow_error` thrown by a
**user** `RowEncoder` inside `Publish` is relabelled `kPayloadTooLarge`. Narrow it, or say so.

---

## The four declared deviations — judged

**(a) `kCarried` has no pre-schema buffer — ACCEPT. The property is preserved, by refusal.**

The design's §5 names a buffer; its own rung 2 item 6 makes the only publish that could fill one
illegal (`kTopicNotDeclared`). Verified in `in_process_provider.cpp`: `CreateTopic` refuses a null
schema in `kCarried`; `Publish` refuses `!declared`; `Subscribe` returns a pending arrival that
`CreateTopic` resolves *and* sets `subscription_schema` at the same instant, under one
non-recursive lock with synchronous dispatch. There is no reachable state in which a `kCarried`
instance delivers a null schema, so §7 clauses 1–2 hold. The header says "upheld by **refusal**
rather than by buffering" rather than leaving the design's sentence to rot. Clause 2
(`CallbackNeverSeesNullSchema`) passes on the new subject — I ran it: 12/12.

**(b) The sixth subject as its own binary — ACCEPT. Sound, and it does not fragment the suite.**

The rationale is checkable and correct: `INSTANTIATE_TEST_SUITE_P` registers every
`ProviderConformance` TEST_P *in the binary* against every subject in it, and clause 2's axis gate
is the link line (`conformance_clauses_carried` is linked only into carrying binaries). A second
INSTANTIATE beside `InProcessLocal` would put clause 2 on the schema-less subject — present and
failing, where the harness's design says absent. Confirmed in `ctest -N`: `InProcessLocal` 11
entries, `InProcessCarrying` 12. Not fragmentation: both link the same `conformance_clauses`
object library, one traits table, retention still keyed by provider (`{"inprocess",
kDropsPreSubscribe}`), so the two loopback subjects cannot disagree. Cost is one ctest target.
Recorded in the README and in both `main.cpp`s.

**(c) The copy residue narrowed further — ACCEPT. A genuine reduction, not a moved measurement.**

Verified in all three receive paths: `data_reader_listener.hpp` (loanable),
`fletcher_sample_pub_sub_type.hpp` (copying) and `gateway/src/ws_session.cpp` now take **one**
owning copy of the sample body, and only when `PeekAttachmentCount`/`EnvelopeAttachmentCount` is
non-zero — replacing one `make_shared<vector>` **per attachment**. An attachment-free sample takes
**none** and needs no owner. Not a measurement moved: the copy oracle scores the probe, not Fast
DDS, and the number it pins (0) is untouched by this. Spec §8 and the README state the reduction
*and* the surviving residue. The peeks are safe on malformed input (return 0; `ParseEnvelopeBody`
then refuses a non-empty blob with a null owner rather than producing an unowned one).

**(d) `std::overflow_error → kPayloadTooLarge` — ACCEPT in substance, one spec sentence owed.**

Confirmed: `write_buffer.hpp:200` is the only `std::overflow_error` thrower reachable inside the
translate scope, and this catch is the **only producer of `kPayloadTooLarge` in the tree** —
without it, taxonomy entry 4 is unreachable and the payload bound degrades to `kInternal`. That is
a real loss, so the mapping is right. It is not an unsanctioned taxonomy *entry* (the enumerator is
the design's own); it is an unsanctioned *mapping rule*, and a rule a C driver author must
reproduce or drift — decision 10's exact failure mode. Write it into §5.1, and narrow the catch (S6).

---

## What was verified and holds

**The five retirements are simultaneous.** Grepped the whole tree, not the diff: no `using Blob`,
no `MakeReadySchemaFuture`, no `shared_future` schema member, no `std::async(std::launch::deferred, …)`
wrapper, no `ImportFromNano` — in product code, tests, `test_package`s, benchmarks, integration
harnesses or commented-out code. The surviving `make_shared<const std::vector<uint8_t>>` sites are
all *owners* handed to `Blob(owner, data, size)`, which is the intended shape. Benchmarks were
migrated, discharging DEBT-4. **P5 honoured:** the `static_assert` was replaced by its inverse plus
two more (`is_constructible` on the owner-plus-span ctor; `!is_convertible` from the retired alias,
which closes the coexistence window) — strengthened, never relaxed.

**PDA-DEC-2's tripwire — discharged honestly. Verified by mutation, not by report.**
I rebuilt `conformance_copy_accounting` and `conformance_seam_vocabulary` with the loaned
attachment restored to a copying construction
(`Blob(std::vector<uint8_t>(loan_base_, loan_base_ + loan_len_))`). Both went red, with the right
diagnostics:

```
copy_clauses.cpp(189): the provider copied its own arena bytes (0x22742dcad50 -> 0x22742dd3720)
seam_vocabulary.cpp:  the seam copied borrowed transport memory: … 0x223e1539340 … 0x223e1541d10
seam_vocabulary.cpp:  a blob kept past the delivery no longer names the provider's bytes
```

Restored: `CopyAccounting` 7/7, `SeamVocabulary` 6/6, `conformance_inprocess` 11/11,
`conformance_inprocess_carrying` 12/12. **The pin at 0 is not an arithmetic constant**: the
negative control in the same test scores 2, and the new `retained_data`/`retained_content_ok` leg
reads the blob *after* the callback and *after* `Unsubscribe`. This is the recurrence PDA-DEC-2
was built to prevent, and it did not recur.

**The MSVC evaluation-order fix is correct and isolated.** `SchemaArrival::Create` names both
objects before returning. The only other braced returns carrying a `std::move` are
`schema_arrival.cpp:56` (two distinct objects) and `subscriber.cpp:167` (`{id, std::move(schema)}`,
no aliasing) — neither has the hazard. The vacuous-check shape: the two surviving `.valid()` calls
(`pubsub/test_package/src/example.cpp:53,58`) are `OwnedSchema::valid()`, a real check;
`SchemaArrival` deliberately has no `valid()` at all.

**Owner ruling 2026-09-01 on schema mode — implemented and genuinely exercised.** `Subscribe`
latches `subscription_schema`; `Publish` delivers *that*, not the topic's current schema;
`CreateTopic` sets it **only** in `kCarried`, with an explicit comment on why it deliberately does
not in `kAsDeclared`. Gateway `schemaIpc` is unchanged: `ws_session.cpp` still polls at zero
timeout and still emits `schemaIpc`+`schema` when the schema is in hand. C2-1's owed test
(`LaterDeclarationNeverReachesALiveSubscription`) is real work, not a placeholder — subscribe →
publish → declare → publish, both deliveries null, the arrival still answers `kOk`+null, then a
**new** subscription sees the declaration. It passes.

**Re-anchored tests: strengthened, not weakened.** `SubscribeNeverBlocksSchemaArrivesLater` gained
a typed status assertion and `RemainingBudget()`, and its schema-less leg now distinguishes
`kOk`+null from `kSubscriptionEnded` — which `future_status::ready` could not. The `EnvelopeTest`
family kept every case and gained a **new** assertion the old shape could not make (the restored
blob points *into* the parsed buffer). `test_fast_dds_pubsub_provider.cpp`'s oversized-row test
moved from `EXPECT_THROW(std::overflow_error)` to asserting `PubSubStatus::kPayloadTooLarge`. The
DDS subscriber-first test now asserts `kPending` explicitly, *"not kOk with a null schema, which
would mean this transport carries no schemas at all"*. The Arrow-tier waits became bounded (15 s)
where they used to block forever — the disclosed narrowing.

**Decisions.** 14 ✓ — no `extern "C"`, C header, `dlopen`/`LoadLibrary`, version negotiation,
driver vtable or host-callback struct anywhere in the diff. 4 ✓ — the same four methods, same
order, `Publish` still inverted over `RowEncoder`. 3 ✓ — nothing above the seam branches on
built-in vs loaded. 2 ✓ — every C form is marked conceptual, "the spelling is illustrative", no
shared header, no layout compatibility. 13 ✓ — the encode/decode byte layout is untouched; only
*who owns* the parsed bytes moved. `integration-tests/gateway-fastdds-ts` is not in the diff.
C2-9's forward note for PDA-DEC-5 is in `in_process_provider.hpp`.

**Cycle-2 debt C2-1…C2-9: all nine discharged.** (C2-4 and C2-5 are discharged but see S1/S2,
which are gaps in the *wording that discharged them*, not re-openings.)

**Budget — counted, not accepted.** Whitespace-insensitive the diff is **+2326/−536**; roughly 550
add/delete pairs are pure re-indentation from wrapping twelve provider bodies in
`TranslateSeamFailure`. Net substantive ≈ **+1790** against a declared net **+600** — about 3×.
**The implementer's account does not survive counting, in the same way PDA-DEC-2's did not:**
"323 lines of normative header prose in `status.hpp`+`schema_arrival.hpp`" is the *total* line
count of two whole files, which hold **150** comment lines and **140** code lines between them
(`status.hpp` 70/95, `schema_arrival.hpp` 80/45).

But the scope is real and in charter. Every new file is named in `Files-to-touch`. Each of
`seam_vocabulary.cpp`'s six tests pins a *named* item from the design's own corner-case ladder
(rung 1 items 1, 2, 3, 4; rung 2 item 6's blob and timeout halves; C2-1) — the design costed only
one of them. The 215 doc lines are the §7 clause 1, §3.2/§3.4/§5.1 and §8/§8.1 amendments the
rulings ordered **in this PR**. I found no padding. **Remedy: correct the account, not the scope.**
One consolidation is available and cheap: `EnvelopeAttachmentCount` (core) and
`PeekAttachmentCount` (fastdds) are the same 14-line parse of the same wire header, written twice.

**Spec amendments — in charter.** §7 clause 1's per-subscription restatement is verbatim what the
2026-09-01 ruling ordered ("lands in THIS PR"). §3.2/§3.4/§3.5/§5.1/§8/§8.1 record what landed
rather than bending the oracle to fit; §3.4's outcome table and §3.2's "conceptual, never a memory
image" paragraph are exactly the C2-6/DEBT-9 wording. The one place the spec now *lags* the code
is §5.1 versus the `overflow_error` mapping (S6).

---

## Should-fix (non-blocking)

1. `fastdds-pubsub-provider/test_package/src/example.cpp:50` ships a leftover debug line:
   `std::fprintf(stderr, "DBG status=%s msg=%s\n", PubSubStatusName(st), result.schema.Message().c_str());`
   in a package's consumer smoke test.
2. `envelope.hpp` now refuses adjacent failure modes with two different exception types — the
   null-owner case throws `PubSubError(kInvalidArgument)` (via `Blob`), every truncation case
   throws `std::invalid_argument`. The header documents only the first. Also, the claim *"a buffer
   that claims attachments with no owner … is refused"* is not quite true: a buffer whose
   attachments are all zero-length is accepted with a null owner (harmlessly).
3. XRCE keeps a `schema_resolved` bool shadowing `schema_resolver.has_value()`, where Fast DDS's
   `SchemaChannel` deliberately dropped its equivalent flag and says so. Harmless today; an
   unnecessary second source of truth in the one provider with no conformance coverage (B1).
4. XRCE resolves the arrival **while holding `impl_->mu`**, where `InProcessPubSubProvider` and
   Fast DDS's `SchemaChannel` both deliberately resolve outside their lock. No inversion (a waiter
   never takes the provider mutex), but the three providers now differ on a point two of them
   thought worth a comment.

## RECORD (fix in place; no fix cycle)

- `fastdds-pubsub-provider/README.md:256` still tells readers `Subscribe` *"returns a
  `std::shared_future<SharedSchema>`"* — the retired mechanism, in a component README.
- Commit message "Conformance 47/47" is the XRCE-OFF configuration (B1). The XRCE-ON number is 71.
- `Files-to-touch` names `gateway/src/main.cpp`, `fastdds-…/src/internal/ordered_delivery.hpp` and
  `integration-tests/pubsub-conformance/src/fixtures.cpp`; none is in the diff. All three are
  correct as they stand — the list over-reached, the work did not.
- `Files-to-touch` also names `.claude/runbook.PDA-DEC.config.md`, which is untracked, so no diff
  can show it.
