# BIND-4 — conformance against the acceptance

**Subject:** BIND-4's acceptance bullets in `BIND-csharp-bindings.md`, each checked against the tree
rather than against a commit message.
**Reviewed at:** `8cdb44b`. Code review: `BIND-4-codereview.md` (6 BLOCKERs, 1 question for a
ruling, 17 DEBT, 8 NITs).
**Why separate from the code review:** that pass asks what the code does; this one asks whether the
bullets are met. Where a bullet was *changed* during the item, this pass states what it says now and
who changed it.

**Outcome: 6 of 12 live bullets CONFORM; 2 PARTIAL; 2 NOT MET; 1 UNPROVEN; 1 awaits a ruling.
BIND-4 does not close.** The bullets that fail are the ones about the lifetime and refusal layer
and the ones that name specific tests — and in three of the four the test the bullet names does not
exist or cannot fail.

---

## The live bullets

| # | Bullet | Verdict |
|---|---|---|
| 1 | `ProviderSelector.Parse` (name-or-path, re-checked natively), `ProviderConfig {MaxPayloadBytes, DomainId, Document}`, `ProviderRegistry.Create` → opaque handle, `PayloadBound.IsValid/Min/Max`, no name query | ✅ all present; the selector rule matches `binding.h`'s NAME/PATH split and does not consult the registry; `Document` is `ReadOnlyMemory<byte>`, parsed by nothing; no query on `ProviderRegistry` |
| 2 | `Publisher`: `CreateTopic`, the two `BoundRows` publishes, `Publish(TopicPath, RowWriter)`, `PublishRaw`, `ListTopics`, `IDisposable`; `AttachmentsBuilder` owns and caches (D-BIND-44) | ✅ **with a stale bullet:** the `RowWriter` overload is `Publish(TopicPath, RowWriter, int minBytes, …)` since **D-BIND-45** (maintainer, 2026-09-22), which amended the public-surface document but not this bullet. The code is right; the bullet text is owed |
| 3 | `Subscriber`: `Subscribe → SubscribeResult {Subscription, SchemaArrival}`, `Unsubscribe`, `AbsorbedCallbackFailures`, `DispatchAfterDelivery`, `IDisposable`, **no finaliser**; `Subscription` a handle | ✅ shape and absences as stated. Its **behaviour** is graded under bullet 5 |
| 4 | The native half (D-BIND-31): subscriber entry points, schema-watch pair as `kNotSupported` per D-BIND-29, attachments builder and accessors, blob retain/release, `fl_schema_arrival_*`; plus `fl_blob_create` (D-BIND-42) and `fl_schema_retain` (D-BIND-43); "43 declared, 43 defined" | ⏸ **awaits a ruling (code review Q1).** Every entry point exists: **44 declared, 44 defined** (D-BIND-46's `fl_schema_copy` is the 44th; the bullet's "43" is stale). But the schema-watch pair answers `kNotSupported` with a message — "the seam does not carry a schema watch yet" — that the merge of `main` made false: `Subscriber::SubscribeSchema` now exists (#128). Conforms to the letter of D-BIND-29; its condition has fired |
| 5 | Thunk discipline per D-BIND-18: in-flight counter + `retired`, last one out frees the `GCHandle`; thread-static marker; managed refusal of `Dispose` from a handler; managed refusal of `Subscribe` to a new topic from a handler; every exception caught, counted, raised as `HandlerFaulted`; `ReadOnlySpan`/ref-struct signature | ❌ **NOT MET.** The mechanisms exist and the containment half holds. The two properties they exist for do not: the counter is **not** sufficient when a handler cancels a sibling mid-delivery (code review B1 — a freed `GCHandle` read on another thread), and the refusal is keyed on the **Subscriber**, where the seam's rule is per **provider** — so disposing another Subscriber on the same provider from a handler is not refused and terminates the process (B2) |
| 6 | Mapping per D-BIND-20: `Infinite` → `INT64_MAX`, other negatives throw; `TopicPath` in UTF-8 bytes, six rules, 246-byte cap; `AttachmentsView` positional, unsorted; `SchemaWaitResult` with `Ok` + null schema-less | 🟨 **PARTIAL.** `TopicPath`, `AttachmentsView` and `SchemaWaitResult` conform. The timeout mapping is implemented and negatives are refused, but "`Wait(Timeout.InfiniteTimeSpan)` **waits**" (bullet 11) is untested — see there |
| 7 | Provider contracts preserved, not re-implemented: one callback per topic, schema-before-data (schema-less exception), per-writer order, one callback at a time, no thread affinity, arguments borrowed | ✅ preserved by **wrapping** — the managed tier re-implements none of them. The row span and `AttachmentsView` are ref structs and cannot outlive the call. (The borrowed `SchemaHandle` is a class and can — code review D6.) Note: several tests offered as evidence for the drain pass on `inprocess`'s own mutex (B6); the contract holds, the evidence is weaker than it looks |
| 8 | Bounded-payload overflow surfaces as `FletcherException(PayloadTooLarge)` | ❔ **UNPROVEN.** `FletcherStatus.PayloadTooLarge` exists and `ThrowIfFailed` maps statuses generically, so it very likely holds. **No managed test publishes past a bound.** `inprocess` ignores `max_payload_bytes`; Fast DDS enforces it (`sample_writer.hpp`) and the transport suite can create one, so the proof is cheap |
| 9 | Fast DDS and XRCE reachable by selector with no per-transport C# code | ✅ no transport name appears in `dotnet/src`; both are selected by string. XRCE without an Agent answers a typed `TransportFailure` naming its endpoint, which is what the transport suite asserts |
| 10 | Bucket 3 over `inprocess`; Bucket 4 over `fastdds` and `xrce`; the C# arm of `CallerTier` with a **total** mapping onto the C++ cases; at least one case given back (seam §12.1) | 🟨 **PARTIAL.** Bucket 3: ✅ ported, two cases excluded by **D-BIND-47** (maintainer, 2026-09-24). Case given back: ✅ `AThrowingHandlerDoesNotAbortTheFanOut`. **Bucket 4: runs, but its lane does not gate the PR** (B3). **CallerTier totality: not true** — the checker counts strings and can be fooled (B4), one C++ property has no mirror because its "mirror" tests a different case, and eight more mirrors cannot fail for their named property (B5) |
| 11 | The two mapping traps tested: a key above U+E000 and a supplementary-plane key where UTF-16 and UTF-8 orders differ; `Wait(Timeout.InfiniteTimeSpan)` waits | ❌ **NOT MET.** No managed test uses either trap key: the ordering test (`TheSetIsOrderedByKeyBYTESWhateverOrderItWasBuiltIn`) uses ASCII keys, where UTF-16 and UTF-8 orders agree. And the only `InfiniteTimeSpan` test waits on a schema that already exists — its own comment says it "returns at once" — so it shows the value is **accepted**, not that it **waits**; a mapping of Infinite to "return immediately" would pass it |
| 12 | Per-row publish benchmark against the C++ generated publisher over `inprocess`, allocations per row; a D-BIND-1 STOP-AND-ASK if unacceptable | ✅ **D-BIND-48** (shape) and **D-BIND-49** (verdict: accepted, no STOP-AND-ASK), both maintainer rulings. Ten arms, both harnesses validated against one pinned payload. Code review D14: the batch arms' bytes are not read back |

---

## The bullets that changed, and who changed them

1. **The benchmark (bullet 12)** moved in from BIND-3 by **D-BIND-37** and gained its shape
   (**D-BIND-48**) and verdict (**D-BIND-49**), all maintainer rulings, all recorded in four places.
2. **Bucket 3's total (bullet 10)** moved 278 → 276 of 302 by **D-BIND-47**, a maintainer ruling.
3. **The `RowWriter` overload (bullet 2)** gained `minBytes` by **D-BIND-45**, a maintainer ruling —
   applied to the public-surface document and the code, **not to this bullet**.
4. **The native half (bullet 4)** gained `fl_blob_create` (D-BIND-42), `fl_schema_retain`
   (D-BIND-43) and `fl_schema_copy` (D-BIND-46) — each a missing declaration found by writing the
   caller, each an ABI minor bump. The bullet's count was not updated for the third.

## The public surface and the code disagree

`BIND-csharp-public-surface.md` is the frozen surface, and its §2.1/§2.3/§5.1 are BIND-4's tiers.
**Listed and not shipped:** `SchemaArrival.WaitAsync` (which D-BIND-22 and the development plan also
promise), `SchemaArrival.Message`, `SchemaWaitResult.IsSchemaless`, `TopicPath.Utf8ByteLength`,
`BlobHandle` (so a handler cannot keep an attachment blob past the callback, which `binding.h`
permits), `static Diagnostics.AbsorbedTotal`, `SchemaHandle : SafeHandle`, and
`AttachmentsBuilder.Set(ReadOnlySpan<byte>, ReadOnlyMemory<byte>)` (ships with a span value).
`ProviderSelector.IsName` is public in the diagram and internal in the code, deliberately.
**Shipped and not listed:** `HandlerFaulted` and its event args (required by D-BIND-18/19),
`SchemaWaitResult.HasSchema`, `AttachmentsBuilder.Set(string, …)` and `Dispose`,
`SchemaArrival.Dispose`.

D-BIND-45 already stated the rule this breaks — *"A frozen table that disagrees with the shipped API
is worse than no table: the next reader cannot tell which is current"* — and applied it to one row.
The rest was never reconciled. **Each difference is either code owed or a document amendment owed**,
and deciding which is not this review's to do.

---

## Carried forward

- **BIND-3's D1** (nested dictionary rewrite agreement) — **closed** at `828be6d`, both sides.
- **BIND-3's D3** (the integration C# project restores unlocked) — **still open, and now covers two
  projects**: BIND-4 added `PubSubTransportConformance` on the same terms.
- **BIND-3's N1/N2** — unchanged (`RefillFaultForTest` is still settable under an "immutable"
  sentence; `checked((int)…)` still throws outside the taxonomy).
- **To close BIND-4:** bullets 5, 10 and 11 must be met; bullet 8 proven; Q1 ruled; bullets 2 and 4's
  stale text corrected. The two design defects under bullet 5 each need a maintainer decision before
  code.

---

## Re-graded at close-out (`309c321`, 2026-09-25)

The verdicts above are left as found, at `8cdb44b`. This is what each unmet bullet reads now, and
what moved it.

| # | Was | Now | What moved it |
|---|---|---|---|
| 4 | awaits a ruling (Q1) | ✅ | **D-BIND-52** (maintainer): the shim implements the schema-watch pair, ABI 0.4 → 0.5, C# exposure owed to a later ruling (`1e4cc50`). The entry-point count corrected in `319ad02` |
| 5 | ❌ NOT MET | ✅ | B1 → **D-BIND-50**, B2 → **D-BIND-51** (`1fe40fa`); B7, found while fixing B5, in `319ad02`. The fixed tests fail, or crash the host, against the old code |
| 6 | 🟨 PARTIAL | ✅ | `SubscriberTests.AnInfiniteWaitActuallyWaitsUntilTheSchemaArrives` (`319ad02`) |
| 8 | ❔ UNPROVEN | ✅ | **D-BIND-53** and its amendment: `ErrorTests.ARowThatDoesNotFitAFixedWindowIsPayloadTooLarge` on a real native status, and `CrossTransportTests.ABoundedPayloadOverflowSurfacesAsPayloadTooLarge` over Fast DDS after the provider fix (`9255c19`, `c42763e`). **D-BIND-54** (`309c321`): both now pin the exact type, `FletcherException`, not the malformed-input subclass |
| 10 | 🟨 PARTIAL | ✅ **at `78ac363` — this row was wrong when first written (see below), and D-BIND-56 is what made it true** | B3: `pr_gate` reads the transport lane's result. B4: the totality checker counts live, named, asserting mirrors and proves ten fakes are refused. B5/B6: the mirrors fail for their named property, and those that cannot are declared structurally weaker with the reason (`319ad02`) |
| 11 | ❌ NOT MET | ✅ | `AttachmentsBuilderTests.KeysWhoseUtf16AndUtf8OrdersDisagreeAreOrderedByUtf8Bytes` (U+E000 against U+1F600, both build orders) and the infinite-wait test above (`319ad02`) |

Bullet 2's stale `RowWriter` signature was corrected in the tracker in `319ad02`.

**Outcome as first written: 12 of 12 live bullets CONFORM. BIND-4 closes.** **Corrected the same
day (D-BIND-56): 11 of 12, and BIND-4 does not close yet.** Bullet 10 says bucket 4 runs over
`fastdds` **and** `xrce`; its XRCE row asserted only a typed refusal with no Agent, which the code
review had filed as DEBT D15, and this re-grade marked the bullet met without answering it. The
`xrce` rows now run against a MicroXRCEAgent the suite starts and proves it owns. **Met at
`78ac363`** (`ci.pr` run 36144203527): the transport lane 21/21 on Linux and Windows, read by count. **12 of 12,
and BIND-4 closes — this time with the CI run as the basis rather than the fix list.**

**Not a bullet, and since ruled:** the public-surface disagreement listed above — every row
decided by **D-BIND-55** (six amended to the code, three members built, `WaitAsync` built above
`Wait`, `BlobHandle` deferred with a trigger). BIND-3's D3 and N1/N2 are carried
forward unchanged.

**Corrected again, the same day (D-BIND-57): bullet 10 is PARTIAL once more, and BIND-4 does not
close.** Its bucket-3 half names the FILE SET, and that file set grew from 23 cases to 49 when #128
was merged in - four hours after the port, and before either re-grade above. Neither re-grade
re-derived the matrix. The 26 new cases are mapped by D-BIND-57 (12 over `inprocess`, 11 over Fast
DDS, 3 excluded); the bullet is met when CI is green over them, by count, and
`scripts/check_test_matrix.py` now makes a stale file set fail a pull request rather than pass a
review.
