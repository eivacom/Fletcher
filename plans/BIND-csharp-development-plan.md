# BIND-C# — Development Plan (proposal for review, 2026-09-08)

**Status: RECORD of the 2026-09-08 re-plan.** All nineteen questions were ruled on
2026-09-11 and folded into [BIND-csharp-bindings.md](BIND-csharp-bindings.md) (the
tracker, again the single source) and [BIND-locked-decisions.md](BIND-locked-decisions.md)
(D-BIND-1 … D-BIND-27). This file is kept for §1 (the re-verification against `main`),
§3.2 (the codec surface), §3.5 (error handling), §6 (the full risk register) and the
files-to-touch note under BIND-2, all of which the tracker cites rather than repeats.
Originally: *proposal, not a tracker.* Written at `main` = `6c541e9` (PR #126 merged,
round PDA-DEC closed; re-checked at `0a56829` on 2026-09-10, where #127 is
packaging-only and moves no premise, §1 row 7). It re-verifies the premises of the five BIND documents of
2026-08-31 against the tree as it now stands, folds in
[BIND-csharp-inherited-constraints.md](BIND-csharp-inherited-constraints.md)
(2026-09-07), and proposes the item breakdown, sequencing, decisions and risk
register for the round. Nothing here is locked until the maintainer says so; where
this document disagrees with an existing BIND file, the disagreement is stated as a
**proposed change**, not silently applied.

Read in this order: §1 (what changed and why the plan must move), §2 (the proposed
decision deltas), §4 (the work breakdown), §6 (potential issues), §7 (what needs
your yes/no before kickoff).

Oracles that win on any conflict, unchanged:
[docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) (the seam,
**frozen**), [GIR-locked-decisions.md](GIR-locked-decisions.md),
[docs/wire-format-specification.md](../docs/wire-format-specification.md),
[docs/recordbatch-accessor-spec.md](../docs/recordbatch-accessor-spec.md).
Existing round documents this proposal amends:
[BIND-csharp-bindings.md](BIND-csharp-bindings.md),
[BIND-locked-decisions.md](BIND-locked-decisions.md),
[BIND-native-transport.md](BIND-native-transport.md),
[BIND-architecture-diagrams.md](BIND-architecture-diagrams.md).

---

## §1 — What changed between 2026-08-31 and now

The plan was written against an unmerged `feature/protocol-driver-abi` branch and a
round called "PDA" that has since been split in two. Six facts moved, and four of
them change the plan's shape.

| # | Then (plan, 2026-08-31) | Now (`main` 6c541e9, 2026-09-08) | Effect on BIND |
|---|---|---|---|
| 1 | Stage D (pub/sub) **gated on PDA merging**; `FastDDSProviderOptions` still present | **PDA-DEC merged as #126.** `ProviderRegistry` (`Create`/`Register`/`SetPathResolver`), `ProviderConfig {max_payload_bytes, domain_id, document}`, InProcess promoted to a built-in in `pubsub/`, Fast DDS on an XML document, XRCE on `key=value`, `FastDDSProviderOptions` **retired** | **The Stage D gate is discharged.** Everything BIND's pub/sub item needed is on `main`. PDA-ABI (loadable drivers) has **not started** and BIND does not need it: a path selector returns `kNotSupported` until PDA-ABI installs a resolver, which is additive (§4 clause 2). See D-BIND-3′. |
| 2 | "PDA-L5: share the buffer struct, blob retain/release and error conventions **verbatim** with the driver ABI; bindings use driver-ABI **role 3**" | Seam §1: **no shared C header between the two ABIs**; PDA-ABI decision 2 says the same from its side. The driver ABI has **two** roles, not three, and selection is seam surface. Owner ruling 2026-09-05 (ruling 46): **both sides of the driver ABI are C++**. | D-BIND-2 is stale in wording. BIND derives its C types from the **seam spec**, not from PDA. The binding ABI stays pure C for a different reason: .NET P/Invoke cannot consume a C++ ABI. See D-BIND-2′. |
| 3 | The delivery callback contract "changed three times in three days" | **Frozen** (§12.1). Re-entrancy is refused on all four methods (`kReentrantCall = 10`), callbacks are contained and counted, `Unsubscribe` blocks with one carve-out, `Subscriber` destruction from a handler terminates by design. | The contract stopped moving, which was the real gate. The constraints file lists what that contract obliges a binding to do; §2 and §6 here turn each into a mechanism. |
| 4 | `AppendInPlace` did not exist; a binding cost one whole-row copy | PDA-DEC-A1 added `WriteBuffer::AppendInPlace`; the C form is `size_t (*writer)(void* ctx, uint8_t* dst, size_t room)`; **no production caller yet** | Under D-BIND-1 the **native codec** writes into the window with ordinary appends, so the common C# publish path never needs `AppendInPlace`. It is needed only for a raw-bytes publish entry point. See §3.3. |
| 5 | Test matrix: 177/178 non-generator cases | Counts moved substantially (Fast DDS 20 → 87 cases across three files; new `pubsub-conformance` with 80 cases incl. `CallerTier`) | Re-baselined in §5. The 18786 reading still needs the Feature owner. |
| 6 | Version series `0.5.x` "confirm at kickoff" | All components at `0.5.0-alpha`; `xrcedds-pubsub-provider` at `0.5.1-alpha` | Confirmed: .NET packages join **0.5.x**. |
| 7 | *(added 2026-09-10)* Release archives carried only the executable; the LGPL notice for the NuGet was an open "how" under P-1 | **#127** (`0a56829`): the gateway archives now ship `LICENSE` + `THIRD-PARTY-LICENSES.txt`, the latter **derived from the resolved Conan graph** by `gateway/deployers/third_party_licenses.py`, staged on every CI run and verified inside the finished archive | Touches no header, seam, codec or generator: no premise or decision moves. **BIND-9 reuses the deployer** for the native shim, whose graph is the same stack (Fast DDS, Fast CDR, foonathan_memory, tinyxml2, Micro XRCE-DDS, microcdr, nanoarrow), so the "notice" half of P-1 has a mechanism. The gateway is also an in-tree precedent for **statically linking Fast DDS and shipping it** (B-3, Q3). |

One more fact worth stating because the plan assumes the opposite: **there is no
descriptor-driven, nanoarrow-only codec in the tree to wrap.** The two encoders
that exist are `arrow-bridge`'s `Codec` (Arrow C++ scalars, `<arrow/api.h>`) and the
generated C++ `WriteTo`/`ReadFrom` over `positional_io.hpp`. GIR-8 left the
descriptor-driven codec as BIND-2 *scope*, not as a deliverable. So D-BIND-1 ("wrap
the codec, never reimplement") is implementable only as **"build one schema-driven
driver of `positional_io` in C++ over the Arrow C Data Interface, and wrap that."**
It is the round's largest C++ item and its critical path (§4, BIND-2).

---

## §2 — Proposed changes to the locked decisions

Marked ′ where an existing decision is amended; new numbers continue from D-BIND-16.

- **D-BIND-1 — unchanged.** One codec. Sharpened: the one codec is `positional_io`,
  and BIND-2 adds a **schema-driven C++ driver of it** over nanoarrow's
  `ArrowArrayView` / array builders. No wire byte is written by managed code
  except in `Eiva.Fletcher.GatewayClient` (the standing exception).

- **D-BIND-1a′ — value transfer is the Arrow C Data Interface. LOCKED 2026-09-11 (Q1), together with D-BIND-23.**
  Rider (i) accepted: `Apache.Arrow` is a base dependency of every native-backed
  managed package; with that, collapse `Core`+`Arrow` (see D-BIND-14′). Rider (ii)
  accepted: the native artifact links **nanoarrow only**; a CI packed-size budget
  guards it. Descriptors are opened from an **exported `ArrowSchema`** (C Data
  Interface), not from IPC bytes, because nanoarrow IPC rejects dictionary types
  (the DICT halt) and the C Data path does not have that ceiling.

- **D-BIND-2′ — the binding ABI is derived from the seam spec, and from nothing
  else.** Replaces the "share PDA vocabulary verbatim / role 3" wording. The C
  spellings of `Blob` (owner handle + retain/release), `Attachments` (positional
  `size`/`KeyAt`/`ValueAt`), `SharedSchema` ({owner, `const ArrowSchema*`}),
  `SchemaArrival` (`wait(arrival, timeout_ms, out)`), the write-buffer window and
  writer, topic segments (pointer+length pairs), selector and document
  (pointer+length, length authoritative) and the status enum (numbers from
  `core/README.md`'s published table) are each **constructed from the C++ value**
  on BIND's side, per §3.2's "conceptual, never a memory image". No header is shared
  with PDA-ABI, and PDA-ABI's `fletcher_status` is never seen by C#. The binding ABI
  is pure C because P/Invoke requires it, not because the driver ABI is.

- **D-BIND-3′ — the pub/sub stage is gated on nothing.** BIND wraps
  `ProviderRegistry::Create(selector, config)`; built-ins are registered inside the
  native shim; path selectors are forwarded and today refuse with `kNotSupported`
  exactly as the C++ registry does. When PDA-ABI lands, the shim calls
  `SetPathResolver` and nothing above it changes. **Sub-decision, LOCKED 2026-09-11
  (Q3): the shim registers all three.** Recommended and ruled: **all three** (`inprocess`,
  `fastdds`, `xrce`), statically linked, because 18689 requires the native
  protocols and PDA-ABI-5/6 have no start date. Cost: the native asset carries
  Fast DDS and Micro XRCE-DDS per RID. Alternative: `inprocess` only, with Fast
  DDS and XRCE reachable from C# only once PDA-ABI-5/6 ship drivers; smallest
  package, but C# has no real transport until a round BIND does not control.

- **D-BIND-5..9, D-BIND-11, D-BIND-13, D-BIND-15, D-BIND-16 — unchanged.**

- **D-BIND-12′ — test matrix re-baselined (§5); adds a second exclusion class. LOCKED 2026-09-11 (Q10).**
  Besides "no managed analogue" (`test_owned_schema`), a case that tests a
  provider's *internal* C++ type (`test_fletcher_sample_pub_sub_type`, the Fast DDS
  `TypeSupport`) is "internal to the provider implementation" and is proposed for
  exclusion, documented. Both readings still need the Feature owner.

- **D-BIND-14′ — three managed packages, not six. LOCKED 2026-09-11 (Q2).** With `Apache.Arrow`
  a base dependency there is no dependency-free tier to protect, and the owner's
  2026-09-05 process ruling ("ceremony is expensive; fewer, larger items") applies
  to packages as much as to items. Proposed:
  `Eiva.Fletcher.Interop` (native assets + raw P/Invoke, the **only** package with
  `runtimes/`), `Eiva.Fletcher` (rows, codec wrapper, Arrow tier, pub/sub,
  PubSubArrow), `Eiva.Fletcher.GatewayClient` (managed, no native). Namespaces
  inside `Eiva.Fletcher` keep the C++ component names (`Eiva.Fletcher.PubSub`,
  `Eiva.Fletcher.Arrow`, …) so the split can be re-introduced later without
  renaming types. Alternative: keep the six-package layout of the plan.

- **D-BIND-17 (new, LOCKED 2026-09-11 with Q11) — exactly one copy of Fletcher per process, and a check.**
  From constraint 1 / `delivery_frame.hpp` P1. The native shim is **one shared
  library** that statically links every Fletcher component it uses; no Fletcher
  code is linked into any other binary the package ships. At load the shim
  enumerates loaded modules (`EnumProcessModules` / `dl_iterate_phdr`) for a second
  export of its marker symbol and **refuses to initialise** if found, naming both
  modules. **Stated limit:** a C++ host that statically linked Fletcher without
  the marker is undetectable; that deployment is a **STOP-AND-ASK** under P1
  before it is attempted, and the .NET README says so. Two shims (two versions of
  `Eiva.Fletcher.Interop` in one process) is the detectable case and the likely
  one.

- **D-BIND-18 (new, LOCKED 2026-09-11 with Q4) — the delivery thunk owns the lifetime bookkeeping.** From
  constraints 2, 3, 4, 5, 8. Each managed subscription carries an atomic in-flight
  counter maintained by the thunk (increment on entry, decrement on exit) and a
  `retired` flag set by `Unsubscribe`. Because the seam guarantees no invocation
  *begins* after `Unsubscribe` returns, **whoever brings the counter to zero after
  retirement frees the `GCHandle`**, on either path; the two paths differ only in
  whether `Unsubscribe` itself is that party. A thread-static "inside my own thunk"
  marker is still needed for: refusing `Dispose` of a `Subscriber` from a handler
  with a managed exception (constraint 3); refusing a synchronous `Subscribe` to
  a new topic from a handler with a managed exception and pointing at the async
  helper (constraint 4, see §6 S-4 for why not silent queueing); and the thunk
  catching every managed exception, counting it in a per-instance
  `AbsorbedCallbackFailures` and raising an optional `HandlerFaulted` event
  (constraint 8), since native's count cannot see a managed catch.

- **D-BIND-19 (new, LOCKED 2026-09-11, widened) — error handling across the boundary:
  five rules.** Detail and code in §3.5. (1) One numbered error type per side, and
  the number **and** the message always cross: `PubSubError(status, message)` ↔
  `FletcherException { Status, Message }`, carried by a caller-owned `fl_error
  {status, origin, message, message_len}` out-parameter, never a global slot;
  `origin` (seam · codec · callback) records which C++ catch arm fired and is
  BIND's own field, not a change to the append-only numbers. (2) No exception
  crosses in either direction, and each direction has exactly one containment
  site: `Translate()` in the shim, the thunk prologue in the wrapper. (3) A managed
  exception that caused a native failure is rethrown **as itself**
  (`ExceptionDispatchInfo`), never wrapped. (4) Expected outcomes are values
  (`kPending`, `kSubscriptionEnded`, `Ok` + null); refusals the caller can fix are
  .NET argument exceptions thrown in managed code before the call, with native
  re-checking the same rules. (5) Absorbed handler failures are counted **and**
  raised as a `HandlerFaulted` event, never silent and never escaping. The
  original writer-frame rule from constraints 6 and 7 is rule 3's first instance:
  `AppendInPlace` is reachable from C# **only** through `PublishRaw` and the
  `RowWriter` overload; the thunk captures, returns 0, the adapter throws
  `PubSubError(kInternal, …)` so nothing commits, and the wrapper rethrows the
  original. The `SIZE_MAX` route is not used. A second containment site, a wrapped
  user exception, or a per-thread error slot → STOP-AND-ASK.

- **D-BIND-20 (new, LOCKED 2026-09-11) — the two mapping details are code, not comments.**
  `Timeout.Infinite` / `Timeout.InfiniteTimeSpan` (−1) map to `INT64_MAX` before
  the call; any other negative is an `ArgumentOutOfRangeException` raised in
  managed code. Every bound is **bytes**: topic segments are UTF-8-encoded and
  measured before crossing (246-byte joined cap), attachment keys are never sorted
  in C# and are read in the sequence Fletcher publishes; `string.Length` is never
  compared to a seam bound.

- **D-BIND-21 (new, LOCKED 2026-09-11 with Q7) — toolchain.** .NET **10 LTS** SDK pinned in the devcontainer
  and `global.json` (the plan said 8.0, whose support ends 2026-11); library TFMs
  `net8.0;net10.0` unless the Feature owner names the consumers' TFM. `Apache.Arrow`
  pinned to a version whose `Apache.Arrow.C` namespace exports and imports
  `CArrowArray`/`CArrowSchema` (verify at BIND-0; do not assume).

- **D-BIND-22 (new, LOCKED 2026-09-11) — nothing crossing the binding ABI is a `std::shared_future`.**
  A future has no C form, which is why `SchemaArrival` exists (§3.4, ruling
  2026-09-01). A future re-entering the seam is a stop-and-ask against the spec,
  raised by BIND and never bridged by it.

- **D-BIND-23 (new, LOCKED 2026-09-11 with Q1) — the encoder behind the ABI takes a bound
  array, a row index and a destination; arrays are borrowed, never consumed; no
  encode entry point returns bytes.** The three-step surface of §3.2 (`open` once per
  schema, `bind` once per batch, `publish_row`/`publish_rows`/`encode_row` per row or
  range). A returned buffer is the staging copy §8.1's negative control exists to
  catch; a consumed array forces one export per row; per-value C arguments are the
  row-builder ABI D-BIND-1a rejected. Drifting into any of the three → STOP-AND-ASK.

---

## §3 — Architecture as proposed

### 3.1 Layers

```
 C# application
   │  generated <stem>.fletcher.cs (POCO, Schema, ToArrow/FromArrow, typed Publisher/Subscriber)
   ▼
 Eiva.Fletcher            (managed)  Descriptor · RowCodec · Publisher · Subscriber · SubscriberArrow
   │                                 ProviderRegistry · ProviderConfig · SchemaArrival · Attachments
   ▼                                 typed FletcherException(status, message)
 Eiva.Fletcher.Interop    (managed)  P/Invoke, SafeHandles, [UnmanagedCallersOnly] thunks,
   │                                 GCHandle contexts, C Data Interface export/import
   ▼  ── pure C, versioned: fletcher/abi/binding.h ─────────────────────────────────────
 fletcher_binding.{dll,so}  (native shim, C++ inside, C outside; the ONLY Fletcher copy)
   ├─ codec surface     fl_codec_open(ArrowSchema*) · fl_rows_bind(codec, ArrowArray*)  [array BORROWED]
   │                    fl_publisher_publish_row(s)(rows, i) · fl_encode_row(rows, i, window)
   │                    fl_decode_rows(codec, bytes, n) → ArrowArray   [nanoarrow-only, on positional_io]
   ├─ pub/sub surface   registry_create(selector, config) · publisher_* · subscriber_*
   │                    schema_arrival_wait · blob retain/release · attachments positional
   └─ statically links  fletcher-core · fletcher-pubsub (+InProcess) · fastdds-provider · xrce-provider
                        · nanoarrow    (no Arrow C++)
   ▼
 the seam (PubSubProvider) ── built-ins today ── PDA-ABI drivers later via SetPathResolver
```

`Eiva.Fletcher.GatewayClient` sits beside this stack with no native dependency,
exactly as the plan has it.

### 3.2 The codec surface (BIND-2), stated precisely (refined 2026-09-11)

**The principle.** Today's `Codec::EncodeRow(const ArrowRow&) -> EncodedRow` has two
properties that rule it out as the shape behind the ABI, and only one is about C#.
Its **return type is the copy**: a function that returns a vector has written the row
somewhere other than the transport window, and the caller then `Append`s it, which is
exactly the whole-row copy the oracle's `StagingProducerIsCaught` control catches.
Its **parameter is unbindable and slow**: a vector of `shared_ptr<arrow::Scalar>` is
one heap object per field per row, cannot cross P/Invoke, and is re-validated on
every call. So the encoder behind the ABI takes a **destination** it does not own, a
**bound array view** plus a **row index** instead of scalars, and hoists validation
out of the per-row call. Three steps, because each has a different cost and lifetime:

```c
/* fletcher/abi/binding.h — encode side; fl_ prefix as in schema_arrival.hpp's sketch */

typedef struct fl_codec fl_codec;   /* a schema-bound field plan; opened once per topic */
typedef struct fl_rows  fl_rows;    /* one ArrowArray bound to a codec; validated once, BORROWED */

/* 1. OPEN — once per schema. Validates against the wire mapping (wire-format spec
   §"Proto to Arrow Type Mapping"), refuses unsupported types (dictionary, D-BIND-8)
   naming the field, precomputes the field plan. `schema` is BORROWED and deep-copied
   (OwnedSchema::DeepCopy): the caller may release its export as soon as this returns. */
fl_status fl_codec_open(const struct ArrowSchema* schema, fl_codec** out, fl_error* err);
void      fl_codec_close(fl_codec*);

/* 2. BIND — once per batch. Checks the array is a struct of this codec's schema and
   builds the ArrowArrayView (nanoarrow validates every buffer HERE, not per row).
   `array` is BORROWED: the codec never calls array->release. The caller keeps the
   export alive until fl_rows_unbind and releases it afterwards. */
fl_status fl_rows_bind(const fl_codec*, const struct ArrowArray* array, fl_rows** out, fl_error* err);
void      fl_rows_unbind(fl_rows*);

/* 3a. PUBLISH row i — the zero-copy path. The codec runs INSIDE Publish's RowEncoder
   and writes straight into the provider's window; no intermediate bytes exist. */
fl_status fl_publisher_publish_row(fl_publisher*, fl_topic topic, const fl_rows* rows,
                                   int64_t i, const fl_attachments* atts, fl_error* err);

/* 3b. PUBLISH rows [first, first+count) — N samples, one crossing.
   atts_per_row is NULL (no attachments) or one entry per row. */
fl_status fl_publisher_publish_rows(fl_publisher*, fl_topic topic, const fl_rows* rows,
                                    int64_t first, int64_t count,
                                    const fl_attachments* const* atts_per_row, fl_error* err);

/* 3c. ENCODE row i into a caller-supplied window — bytes in hand (WAL, tests, relay).
   `sink` is the seam's own C form of WriteBuffer, {ctx, data, capacity, pos, grow}:
   one crossing per REFILL, never per append. */
fl_status fl_encode_row(const fl_rows* rows, int64_t i, fl_write_window* sink, fl_error* err);

/* DECODE — the mirror. Produces a fresh struct array the caller imports and OWNS; its
   release frees the nanoarrow buffers. Row bytes are borrowed and never copied.
   Touches no provider, so it is callable from inside the delivery thunk. */
fl_status fl_decode_rows(const fl_codec*, const uint8_t* bytes, size_t len, int64_t count,
                         struct ArrowArray* out, fl_error* err);
```

Errors follow §5.1: a returned status plus an `fl_error` out-parameter carrying number
and message bytes, never a global slot.

**The C++ behind it**, which is what BIND-2 writes:

```cpp
class NanoarrowCodec {
 public:
  explicit NanoarrowCodec(const ArrowSchema& schema);        // deep copy + FieldPlan, once
  BoundRows Bind(const ArrowArray& array) const;             // ArrowArrayViewSetArray + type check, once per batch
  void EncodeRow(const BoundRows& rows, int64_t i, WriteBuffer& out) const;   // the one encoder
  void DecodeRows(const uint8_t* bytes, size_t len, int64_t count, ArrowArray* out) const;
};

// the shim's publish fusion — this is where zero-copy is decided
fl_status fl_publisher_publish_row(fl_publisher* p, fl_topic t, const fl_rows* rows,
                                   int64_t i, const fl_attachments* atts, fl_error* err) {
  return Translate(err, [&] {
    p->publisher.Publish(Segments(t),
        [&](fletcher::WriteBuffer& window) { rows->codec->EncodeRow(rows->view, i, window); },
        ToAttachments(atts));
  });
}
```

`EncodeRow` drives `PositionalWriter` as it stands (`SetNull`, the scalar writers,
`WriteFixedArray<T>` for contiguous list runs, `WriteString`/`WriteBinary` from the
Arrow data buffer, `BeginStruct`/`BeginList`/`BeginMap` for nesting) and `DecodeRows`
drives `PositionalReader` and nanoarrow's builders (`ArrowArrayStartAppending`,
`ArrowArrayAppend*`, `ArrowArrayFinishBuildingDefault`). Every HARD-1..7 malformed-input
check already lives in `PositionalReader`; the surface adds none and removes none.

**The C# on top:**

```csharp
using var codec = new FletcherCodec(SensorReading.Schema);   // fl_codec_open, once per topic
using var rows  = codec.Bind(batch);                         // ONE export + ONE validation for N rows
for (int i = 0; i < batch.Length; i++)
    publisher.Publish(topic, rows, i);                       // one P/Invoke, zero intermediate bytes
publisher.Publish(topic, rows);                              // or all N in one crossing
```

`BoundRows.Dispose()` unbinds and **then** releases the export, so the borrow rule is
enforced by the type rather than remembered by the caller.

**What each choice buys.**
- Destination as a parameter, fused into `Publish`: the row's bytes come to exist inside
  the transport window, so `encode_copies == 0` by the oracle's own definition. This is
  the only choice that is about zero-copy; the rest are about cost.
- Borrowed array, never consumed: one export serves N publishes. This is the B-2 batch
  mitigation made structural.
- Bind separated from encode: nanoarrow's buffer validation and the schema-equality
  check run once per batch; per row the native side allocates nothing and validates
  nothing, it reads offsets and memcpys.
- Strings cost nothing extra at encode: `Apache.Arrow` stores UTF-8, so a string field
  is a memcpy from the Arrow data buffer. The UTF-16→UTF-8 transcoding happens once,
  when C# builds the array, which is where D-BIND-1b says the guarantee ends.
- Columnar layout makes list runs cheap: a `list<double>` element run is contiguous in
  the child values buffer, one `WriteFixedArray` of `n` elements.
- `fl_codec` and `fl_rows` are immutable after construction, so N threads may publish
  different rows of one bound batch concurrently without a lock.

**What not to do.** Return `EncodedRow` or any owned buffer from encode (the staging
copy). Consume the `ArrowArray` per call (one export per row; most of B-2). Take an
`ArrowSchema` per encode call (validation is the expensive part). Take values as
individual C arguments (the row-builder ABI D-BIND-1a rejected). Re-run
`ArrowArrayViewSetArray` per row (it walks every buffer of every child).

**Forcing test.** Byte identity with `arrow-bridge`'s `Codec` across the whole
`emit_vectors` scenario corpus (`NanoarrowCodec.ByteIdenticalToArrowBridge`), the old
two-way parity harness repurposed exactly as D-BIND-11 says. The same three changes
would improve the server-tier `arrow-bridge` codec, and the modernization branch has
already taken the first (`EncodeRow(values, WriteBuffer&)`); that file is that branch's
to change (P-7), not BIND's.

### 3.3 Where `AppendInPlace` does and does not appear

| C# publish path | Bytes produced by | Buffer route | Copies |
|---|---|---|---|
| `Publisher.Publish(row)` / `Publish(batch, i)` (generated + Arrow) | native codec, inside `RowEncoder` | ordinary `Append*` into the window | 0 (oracle-measurable) |
| `Publisher.PublishRaw(ReadOnlySpan<byte>)` (relay, gateway bridge, tests) | managed | `AppendInPlace` + managed writer that `memcpy`s from pinned managed memory | 1 (from managed memory; stated) |

So constraints 6 and 7 bind one entry point, and the "generated encoder returns its
own cursor delta" clause has no C# instance: generated C# emits no wire bytes.

### 3.4 Delivery path

`subscriber_subscribe(handle, segments, ctx, thunk)` → native `Subscriber` →
`[UnmanagedCallersOnly] static void Thunk(void* ctx, ulong id, byte* data, nuint len, fl_schema* schema, fl_attachments* atts)`.
The thunk: (1) resolves `ctx` to the managed subscription, (2) increments in-flight,
(3) sets the thread-static "inside" marker, (4) invokes the user delegate with
non-escaping `ReadOnlySpan<byte>` and ref-struct views over schema and attachments,
(5) catches everything and counts, (6) clears the marker, (7) decrements in-flight
and frees the handle if retired and last. The codec's `decode_rows` **is callable
from inside the thunk** (it touches no provider), so a typed subscriber decodes in
place; a batching subscriber copies the borrowed bytes (a copy that is required by
the borrow rule, not a zero-copy violation) and decodes N rows in one call later.

### 3.5 Error handling across the boundary (D-BIND-19, locked 2026-09-11)

Five rules; each has a seam clause or a .NET convention behind it. BIND-1's header
and BIND-3's wrapper implement one design.

**Rule 1 — one numbered error type per side; number and message always cross.**
Seam §5.1 already makes this the C++ rule (`PubSubError(status, message)`, message as
bytes plus length, never a global slot). The ABI carries both in a caller-owned
out-parameter, plus one field of BIND's own:

```c
typedef struct fl_error {
    int32_t   status;       /* PubSubStatus number, or FL_OK */
    int32_t   origin;       /* which catch arm fired: FL_ORIGIN_SEAM | FL_ORIGIN_CODEC | FL_ORIGIN_CALLBACK */
    uint8_t*  message;      /* UTF-8 bytes, no NUL reliance; NULL when status == FL_OK */
    size_t    message_len;
} fl_error;
void fl_error_dispose(fl_error*);   /* frees message; safe on a zeroed struct */
```

`origin` is not a change to the seam's append-only numbers. It exists because the
number alone cannot separate two things a C# caller must: a `kInvalidArgument` from
the seam ("no such provider name") and a `kInvalidArgument` from `PositionalReader` on
a truncated buffer. D-BIND-15 promises a typed `FletcherFormatException` for the HARD
cases; `origin` is how the wrapper knows to throw it. Heap-allocated message, freed
through the ABI: registry refusals list every registered provider and will not fit a
fixed buffer.

**Rule 2 — no exception crosses in either direction; one containment site each way.**

```cpp
// the shim: every extern "C" entry point is a call through here
template <class Fn>
fl_status Translate(fl_error* err, Fn&& fn) noexcept {
    try { fn(); return FL_OK; }
    catch (const fletcher::PubSubError& e)  { return Fill(err, e.status(),        FL_ORIGIN_SEAM,  e.what()); }
    catch (const std::invalid_argument& e)  { return Fill(err, kInvalidArgument,  FL_ORIGIN_CODEC, e.what()); } // PositionalReader, DeserializeEnvelope
    catch (const std::out_of_range& e)      { return Fill(err, kInvalidArgument,  FL_ORIGIN_CODEC, e.what()); }
    catch (const std::overflow_error& e)    { return Fill(err, kPayloadTooLarge,  FL_ORIGIN_SEAM,  e.what()); } // §5.1's one normative by-type rule
    catch (const std::exception& e)         { return Fill(err, kInternal,         FL_ORIGIN_SEAM,  e.what()); }
    catch (...)                             { return Fill(err, kInternal,         FL_ORIGIN_SEAM,  "non-std exception"); }
}
```

Managed to native is the thunk prologue: every `[UnmanagedCallersOnly]` method is a
try/catch whose body does nothing else, because an exception leaving one is a
fail-fast. A reflection test fails on any such method not marked with the wrapper
attribute (N-1).

**Rule 3 — a managed exception that caused a native failure is rethrown as itself.**
A user's own `InvalidDataException` thrown from a `RowWriter` comes back as
`InvalidDataException` with its own stack, not as `FletcherException(kInternal)`
wrapping it. Native still sees a real `PubSubError(kInternal, message)`, so C++
logging is consistent; the managed caller's contract is untouched.

```csharp
[UnmanagedCallersOnly]
static nuint WriterThunk(void* ctx, byte* dst, nuint room)
{
    var state = (WriterState)GCHandle.FromIntPtr((IntPtr)ctx).Target!;
    try   { return (nuint)state.Writer(new Span<byte>(dst, (int)room)); }   // returns BYTES written
    catch (Exception ex)
    {
        state.Captured = ExceptionDispatchInfo.Capture(ex);
        state.Message  = ex.ToString();      // what the adapter puts in PubSubError(kInternal, …)
        return 0;                            // adapter sees Captured and THROWS: nothing commits
    }
}

// the ONE throw site in the wrapper
static void ThrowIfFailed(int status, ref FlError err, WriterState? ctx = null)
{
    if (status == 0) return;
    try
    {
        if (ctx?.Captured is { } edi) edi.Throw();
        var message = Encoding.UTF8.GetString(err.Message, err.Length);
        throw err.Origin switch
        {
            FlOrigin.Codec => new FletcherFormatException((FletcherStatus)status, message),
            _              => new FletcherException((FletcherStatus)status, message),
        };
    }
    finally { NativeMethods.fl_error_dispose(ref err); }
}
```

This is the writer-frame rule of constraints 6 and 7, generalised. `AppendInPlace` is
reachable from C# only through `PublishRaw` (which does the `memcpy` itself and
returns the byte count) and the advanced `RowWriter` overload, whose contract says
bytes. The `SIZE_MAX` route is not used: it also commits nothing, but its diagnostic
("reported more than it was lent") is misleading for a managed exception.

**Rule 4 — expected outcomes are values; refusals the caller can fix are .NET
argument exceptions thrown before the call.** The seam says so itself: `kPending` and
`kSubscriptionEnded` are outcomes and `PubSubError` refuses to carry them, so
`SchemaArrival.Wait` returns a `SchemaWaitResult` and `Ok` + null is a value.
Managed pre-checks mirror native refusals for the stack trace, and native re-checks:
`ArgumentOutOfRangeException` for a negative timeout other than `-1` (D-BIND-20),
`ArgumentException` for a topic segment failing one of the six rules,
`InvalidOperationException` for `Dispose` or a synchronous `Subscribe` from inside a
handler (D-BIND-18). One table-driven test proves both sides refuse the same inputs
(Q17). `OperationCanceledException` is managed-only and never crosses.

**Rule 5 — absorbed handler failures are counted and raised, never silent, never
escaping.** Seam §5.3 absorbs and counts a throwing delivery callback; the thunk does
the same for managed handlers and adds an event, because a counter alone is easy to
never read.

```csharp
[UnmanagedCallersOnly]
static void DeliveryThunk(void* ctx, ulong id, byte* data, nuint len, void* schema, void* atts)
{
    var sub = (Subscription)GCHandle.FromIntPtr((IntPtr)ctx).Target!;
    sub.EnterDelivery();                                   // in-flight++ and the thread-static marker
    try   { sub.Handler(new ReadOnlySpan<byte>(data, (int)len), new SchemaHandle(schema), new AttachmentsView(atts)); }
    catch (Exception ex) { sub.Owner.RecordAbsorbed(sub, ex); }   // counter++ and HandlerFaulted; nothing escapes
    finally { sub.ExitDelivery(); }                        // in-flight--; frees the GCHandle if retired and last
}
```

**Not used, deliberately:** `SetLastError`/`GetLastError` or any per-thread slot (the
seam forbids it; several provider instances share a process); `HRESULT`-style codes
that lose the message; a fixed-size inline message buffer; wrapping user exceptions
in `FletcherException`; exceptions for `kPending`.

---

## §4 — Work breakdown and sequencing

Following the 2026-09-05 process ruling, items are fewer and larger than in the
2026-08-31 plan. Four tracks; only track A has a strict internal order.

```
Track A (native + managed core)   BIND-0 → BIND-1 → BIND-2 → BIND-3 → BIND-4 → BIND-5
Track B (generator, protoc/)      BIND-6 → BIND-7        (BIND-7 needs BIND-3's Arrow import)
Track C (managed-only)            BIND-T · BIND-8         (independent; can start day one)
Track D (pipelines, docs)         BIND-9 (starts with BIND-0, closes last) · BIND-10
```

Critical path: **BIND-0 → BIND-1 → BIND-2 → BIND-3 → BIND-4**. BIND-2 is the long
pole.

| ID | Item | Track | Kind | Depends on | Forcing test |
|---|---|---|---|---|---|
| BIND-0 | Kickoff: decisions locked, skeleton `c-abi/` + `dotnet/` green in CI, matrix re-baselined | A/D | 🟪 | — | `ci.dotnet.yml` + `ci.c-abi.yml` green on both platforms with an empty ABI |
| BIND-1 | The binding ABI header, reviewed as a specification (no implementation) | A | 🟪 | BIND-0 | `BindingAbi.CompilesAsC99AndIsSelfContained` |
| BIND-2 | Nanoarrow schema-driven codec + copy oracle producer | A | 🟦 | BIND-1 | `NanoarrowCodec.ByteIdenticalToArrowBridge` + `CopyAccounting.BindingProducerWritesInPlace` |
| BIND-3 | `Eiva.Fletcher.Interop` + codec/Arrow tier in `Eiva.Fletcher` | A | 🟦 | BIND-2 | Bucket 1 green in C#; `Errors.EveryHardCaseKeepsItsMessage` |
| BIND-4 | Pub/sub in `Eiva.Fletcher`: registry, Publisher, Subscriber, SchemaArrival, thunk discipline | A | 🟦 | BIND-3 | Bucket 3 over `inprocess`; Bucket 4 over `fastdds`/`xrce` by selector; C# arm of `CallerTier` |
| BIND-5 | `SubscriberArrow` batching + the oracle run end-to-end from C# | A | 🔬 | BIND-4 | `pubsub-arrow` cases; copy oracle green with the **C#** producer |
| BIND-6 | C# backend on the IR: type table + visitor → `<stem>.fletcher.cs` | B | 🟦 | — (GIR) | `CsharpVisitor.*` in `protoc/tests`; no-drift test unchanged |
| BIND-7 | Arrow view + accessor emitters (`csharp_accessor`) + capstone third arm | B | 🟦 | BIND-6, BIND-3 | `accessor-capstone` C# arm `observed == expected`; StructArray windowing fixture at non-zero offset |
| BIND-T | TS `Publisher`/`Subscriber` emitter | C | 🟦 | — | `TsVisitor.DescriptorByteIdentical` still green + new emitter cases |
| BIND-8 | `Eiva.Fletcher.GatewayClient` (managed port; the codec exception) | C | 🟦 | — | Bucket 2 (56) green; `Package.GatewayClientHasNoRuntimesFolder` |
| BIND-9 | CI/CD: RID matrix, NuGet Trusted Publishing, size budget, LGPL notice | D | ⚙ | BIND-0 (skeleton), all for release | `cd.dotnet.yml` dry run; packed-size check; asset-isolation check |
| BIND-10 | Docs, TD-009, archive to `docs/archive/BIND/` | D | 📓 | all | docs review |

### Item notes (what each must settle, beyond the plan's acceptance text)

**BIND-0 — Kickoff.** Owner decisions from §7 recorded in
`BIND-locked-decisions.md`. Branch `feature/csharp-bindings` from `main`.
Devcontainer gains the .NET SDK (D-BIND-21). The two new component lanes run on
an **empty** ABI (`fletcher_binding_abi_version()` only) so that the first
automated run happens before any real code exists: §12.4's lesson is that the
first lane run found seven defects local green could not. Re-baseline §5's matrix
with the command shown there and commit it. ADO 18689 updated to point at the
locked plan.

**BIND-1 — The header.** `c-abi/include/fletcher/abi/binding.h`, C99, versioned,
append-only structs, pre-1.0 exemption and deprecation policy **in the header**.
Contents: status (numbers from `core/README.md`, `kOk`..`kReentrantCall`), error
object with number + message bytes + length (never a global slot, §5.1 rule 1),
registry (`create` with selector+config; `register_builtin` by name for the
three the shim links; `set_path_resolver` reserved), publisher and subscriber
handles, `schema_arrival_wait`, blob and attachments views, write-buffer window
and writer, delivery callback signature, codec descriptor/encode/decode, and the
**single-copy marker**. Reviewed as a spec, like PDA-ABI-1: the wording of who
owns what and for how long is the expensive thing to get wrong.

**BIND-2 — The codec.** See §3.2. Also: the C writer adapter for `publish_raw`
(D-BIND-19); a **real** producer for the copy oracle that goes through the ABI
window from C (replacing the "stand-in" caveat in
`integration-tests/pubsub-conformance/README.md` for the client half); a
packed-size budget for the shim in CI; the single-copy check. Lives in `c-abi/`
(Conan component `fletcher-c-abi`), links `fletcher-core`, `fletcher-pubsub`,
nanoarrow; **never** `arrow-bridge`. **Prior art to read first:** commit `0050365` on
`feature/fastdds_modernization/19645` (not on `main`) adds
`Codec::EncodeRow(const ArrowRow&, WriteBuffer&)`, writing through `PositionalWriter`
straight into a caller-supplied buffer, and a public `arrow-bridge/…/batch_decoder.hpp`
that decodes rows directly into Arrow builders. Structurally that is this item over
Arrow C++ instead of nanoarrow, still per-`Scalar` on the encode side; a design
reference, not something to link. The byte-identity oracle targets whichever
`arrow-bridge` `Codec` `main` carries when it first runs.

**Files to touch for §3.2 (BIND-1 + BIND-2), as of 2026-09-11.** Nothing in the
existing C++ tree changes: every API the codec needs is public today and is driven as
is. The modifications are new files, two existing test/CI files that gain a case or a
job, and the plan documents.

*Existing code — relied on, not modified:*

| File | Methods relied on | Change |
|---|---|---|
| `core/include/fletcher/core/positional_io.hpp` | `PositionalWriter::SetNull`, `WriteInt8..WriteDouble`, `WriteBool`, `WriteTimestamp`, `WriteDuration`, `WriteFixedArray<T>`, `WriteString`, `WriteBinary`, `BeginStruct`, `BeginList` + `ListContext::SetElementNull`, `BeginValues`, `BeginMap`; `PositionalReader::Read<T>`, `ReadString`, `ReadFixedArray`, `ReadStruct`, `ReadListHeader`, `IsElementNull`, `VerifyFullyConsumed` | **None.** One optional addition: `PositionalWriter` has no fixed-size-list primitive; compose it from `BeginValues()` plus the element bitfield (as the modernization branch's codec does), or add a small `BeginFixedSizeList(count)` helper. Additive, wire bytes unchanged |
| `core/include/fletcher/core/write_buffer.hpp` | the window, `Append*`, `Position`, `PatchByte`/`PatchU32` | None |
| `pubsub/include/fletcher/pubsub/publisher.hpp` | `Publish(segments, const RowEncoder&, const Attachments&)`: the fusion is a lambda passed here | None |
| `pubsub/include/fletcher/pubsub/owned_schema.hpp` | `OwnedSchema::DeepCopy` for `fl_codec_open`'s borrowed schema | None |
| `pubsub/conanfile.py` | already exports nanoarrow headers and lib (`cpp_info.libs = ["fletcher-pubsub", "nanoarrow"]`) | None |
| `arrow-bridge/…/codec.hpp`, `Codec::EncodeRow` | byte-identity oracle target in tests only | **None**, by D-BIND-1 and P-7 |
| `pubsub/include/fletcher/pubsub/provider.hpp` | frozen | None |

*New files:*

| File | Contents |
|---|---|
| `c-abi/conanfile.py`, `c-abi/CMakeLists.txt` | Conan component `fletcher-c-abi`; requires `fletcher-core`, `fletcher-pubsub` (brings nanoarrow), the provider packages per Q3; **never** `arrow-bridge` |
| `c-abi/include/fletcher/abi/binding.h` | `fl_codec_open`/`fl_codec_close`, `fl_rows_bind`/`fl_rows_unbind`, `fl_publisher_publish_row`, `fl_publisher_publish_rows`, `fl_encode_row`, `fl_decode_rows`, `fl_write_window`, `fl_error`, beside the registry, subscriber and schema-arrival entry points BIND-1 already lists |
| `c-abi/src/nanoarrow_codec.hpp`, `nanoarrow_codec.cpp` | `NanoarrowCodec(const ArrowSchema&)`, `FieldPlan`, `Bind(const ArrowArray&) -> BoundRows`, `EncodeRow(const BoundRows&, int64_t, WriteBuffer&)`, `DecodeRows(bytes, len, count, ArrowArray*)`; one `switch` per mapping row, recursive for list, struct and map |
| `c-abi/src/codec_abi.cpp` | the C entry points above plus `Translate(err, fn)`, the `PubSubError` → status-and-message adapter |
| `c-abi/src/publisher_abi.cpp` | the publish fusion shown in §3.2 |
| `c-abi/src/write_window_adapter.hpp` | `fl_write_window` → `WriteBuffer` adapter for `fl_encode_row` (grow hook = one crossing per refill) |
| `c-abi/tests/test_nanoarrow_codec.cpp` | `NanoarrowCodec.ByteIdenticalToArrowBridge` over the `emit_vectors` corpus; malformed-input parity with HARD-1..7; the borrow rule (the array's `release` count stays zero across N publishes) |
| `c-abi/tests/binding_abi_c99.c` | `BindingAbi.CompilesAsC99AndIsSelfContained` |
| `dotnet/src/Fletcher.Interop/NativeMethods.Codec.cs` | P/Invoke declarations for the eight functions |
| `dotnet/src/Fletcher/FletcherCodec.cs` | `FletcherCodec(Schema)`, `Bind(RecordBatch) -> BoundRows`, `Decode`, `DecodeBatch`, `Dispose` |
| `dotnet/src/Fletcher/BoundRows.cs` | holds the `CArrowArray` export; `Dispose` = `fl_rows_unbind` **then** release |
| `dotnet/src/Fletcher/Publisher.cs` | `Publish(TopicPath, BoundRows, int)`, `Publish(TopicPath, BoundRows)`, `Publish(TopicPath, RowWriter)`, `PublishRaw` |

*Existing files that gain something:*

| File | Change |
|---|---|
| `integration-tests/pubsub-conformance/src/copy_accounting.cpp` | **add** `CopyAccounting.BindingProducerWritesInPlace`: a producer through `fl_rows_bind` + `fl_publisher_publish_row` scores `encode_copies == 0`, retiring the README's "stand-in" caveat for the client half (§12.1 expects both rounds to add cases) |
| `integration-tests/pubsub-conformance/CMakeLists.txt`, `conanfile.py` | link `fletcher-c-abi` for that one subject |
| `.github/workflows/ci.pr.yml` | filter + caller job + `pr_gate` `needs:`/`results:` for the new `ci.c-abi.yml` (D-BIND-16) |
| `protoc/` (BIND-6, new backend files only) | the generated `<Svc>_<Method>Publisher.Publish(Msg)` body: build a one-row batch, `Bind`, `Publish(rows, 0)`; `Publish(IEnumerable<Msg>)` binds once and publishes N. No existing emitter is edited (D-BIND-6) |

*Plan documents:* this file (§3.1 diagram, §3.2, D-BIND-23, this note);
`BIND-csharp-public-surface.md` §2.3 `Publish` overloads and §2.4 `FletcherCodec` gain
`BoundRows`, diagrams 5.2/5.3 gain the class (applied 2026-09-11);
`BIND-locked-decisions.md` gains D-BIND-23 once ruled.

Two methods are deliberately absent because the design removes the need for them: a
native `EncodeRow` that returns bytes, and any per-value setter on the ABI.

**BIND-3 — Interop + Arrow tier.** `SafeHandle` per native handle;
`NativeLibrary.SetDllImportResolver` with a diagnostic that names the missing RID;
ABI version check at load; C Data Interface export/import via `Apache.Arrow.C`;
`FletcherException : Exception { PubSubStatus Status; }` with the native message
verbatim; `FletcherFormatException` for HARD malformed-input cases; the
UTF-16↔UTF-8 boundary stated in the XML docs and in the oracle's scope note
(D-BIND-1b). Ports Bucket 1.

**BIND-4 — Pub/sub.** `ProviderRegistry.Create(selector, config)`; `ProviderConfig
{ uint MaxPayloadBytes; uint DomainId; ReadOnlyMemory<byte> Document; }` (bytes,
length authoritative, C# parses nothing); `Publisher`, `Subscriber` with
D-BIND-18's thunk; `SchemaArrival.Wait(TimeSpan)` with D-BIND-20's mapping and a
typed outcome (`Ok(schema)`, `Ok(null)` for schema-less transports, `Pending`,
`SubscriptionEnded`, failure); `Attachments` as a positional read-only view on
delivery and a builder on publish; `DispatchAfterDelivery` helper (constraint 5)
that hands work to the thread pool and returns a `Task`. Ports Buckets 3 and 4;
writes the C# arm of `CallerTier` (the tier BIND actually wraps, per §9) and adds
at least one case to the C++ suite as §12.1 expects.

**BIND-5 — Arrow subscriber + oracle end to end.** Managed batching: copy borrowed
rows, decode N per native call, deliver `RecordBatch`. Dictionary re-folding
deferred to DICT (D-BIND-8). The copy oracle run with the C# producer is this
item's acceptance, not BIND-2's stand-in.

**BIND-6 — C# row emitter.** `csharp_backend_type_table` + `csharp_backend_visitor`
on the IR, reusing `cpp_backend::BuildFlattenedFieldList`. Emitted per message:
POCO with nullable annotations, a real `enum` (D-BIND-8), a static
`Apache.Arrow.Schema` built in code (metadata preserved) and exported to the
descriptor at first use, `ToArrow(IEnumerable<T>)`/`FromArrow(StructArray, int)`,
and per service method a typed `<Service>_<Method>Publisher`/`Subscriber` pair
mirroring the C++ names. **No `WriteTo`/`ReadFrom`** (that was the pre-D-BIND-1
draft). Temporal mapping decided in the type table (see §6, G-2). NativeAOT
publish of a consumer with zero trim warnings is the reflection-free proof.

**BIND-7 — Views + accessors + capstone.** Two emitters under one token; the
`StructArray.Fields` windowing question settled empirically first and recorded in
`BIND-locked-decisions.md`; accessor depth cap decided (match 2/3 recommended,
see §6 G-4); C# joins `accessor-capstone` against the shared fixture and oracle.

**BIND-T, BIND-8, BIND-9, BIND-10** — as in the 2026-08-31 plan, with BIND-9
carrying the LGPL notice text and the packaging statement (§6 P-1), and BIND-10
adding **TD-009** (TD-008 is now taken by the seam).

**BIND-9, licence files (added 2026-09-10, after #127).** The native shim statically
links the same permissively licensed stack the gateway does, plus Fletcher's own
LGPL components, so `Eiva.Fletcher.Interop` owes the same two files the gateway
archive now carries: the repository `LICENSE` and a `THIRD-PARTY-LICENSES.txt`.
**Reuse `gateway/deployers/third_party_licenses.py` as is** (D-BIND-16: existing
machinery, not a second implementation), run against the shim's Conan package
with `-c tools.graph:skip_binaries=False`, which is mandatory: a static
library's dependencies carry nothing needed at run time, so Conan otherwise
marks them "Skip" and they have no package folder to read the texts from. Use
`dependencies` over the whole host graph, not `dependencies.host`, which drops
skipped header-only packages that are nonetheless in the binary; asio was the
case that found this. Stage both files into the package on **every** CI run and
have the pack step fail if either is missing, mirroring `ci.gateway.yml`'s
archive check; the asset-isolation check for `GatewayClient` already opens the
`.nupkg`, so the licence check rides in the same step. If the deployer wants a
shared home rather than living under `gateway/`, that is a small move and the
gateway's owner's call, not BIND's; do it before BIND-9 or import by path.

---

## §5 — Test matrix, re-baselined at `6c541e9`

Counts are `grep -c "^TEST\(_F\|_P\)\?("` per file, so they are **cases, not
parameterised instances**, and they are reproducible from the tree:

```bash
for f in core/tests/*.cpp pubsub/tests/*.cpp arrow-bridge/tests/*.cpp pubsub-arrow/tests/*.cpp fastdds-pubsub-provider/tests/*.cpp xrcedds-pubsub-provider/tests/*.cpp gateway/tests/*.cpp protoc/tests/*.cpp; do echo "$(grep -c '^TEST\(_F\|_P\)\?(' "$f") $f"; done
```

| Bucket | Files (cases) | Total | Treatment |
|---|---|---|---|
| 1 codec / envelope / buffer / status | `test_positional_io` 19, `test_envelope` 11, `test_write_buffer` 11, `test_status_taxonomy` 3, `test_codec` 35, `test_codec_edge` 23, `test_codec_property_fuzz` 2 | **104** | Port as binding conformance over the ABI (BIND-3). `test_status_taxonomy` ports naturally: the C# enum is compared to `core/README.md`'s table. |
| 2 gateway client | `test_schema_codec` 11, `test_publish_frame` 9, TS 36 | **56** | Port (BIND-8), true parity: managed code both sides. |
| 3 pub/sub semantics | `test_publisher_subscriber` 18, `test_segments` 5, `test_pubsub_arrow` 15 | **38** | Port over `inprocess` (BIND-4/5). |
| 4 native providers | Fast DDS: `test_fast_dds_pubsub_provider` 40, `test_profile_document` 23, `test_fletcher_sample_pub_sub_type` 24; XRCE: `test_xrce_provider` 6, `test_xrce_document` 9 | **102** | Port as driver-selection tests where the assertion is seam-visible (78); **propose excluding** `test_fletcher_sample_pub_sub_type` (24) as internal to the provider. Needs the Feature owner. |
| 5 generator | `test_type_mapper` 36, `test_option_metadata` 33, `test_schema_builder` 9, `test_schema_visitor` 9, `test_ir` 8, `test_schema_codec_lockstep` 2, `test_ts_visitor` 1 | **98** | Stay C++; extended with `test_csharp_*`. Needs the Feature owner. |
| 6 no managed analogue | `test_owned_schema` 1 | **1** | Excluded, documented. |
| Conformance (new since plan) | `integration-tests/pubsub-conformance` 80 cases; `CallerTier` 20 of them | — | Inherited **oracle**, not a port target. BIND writes a C# arm of `CallerTier` and adds cases to the C++ suite. |

Non-generator total is **300** (was 178). The two exclusion classes were **ruled on
2026-09-11 (Q10)**: 275 of 300 non-generator cases port (300 less the 24
provider-internal cases and `test_owned_schema`); the 98 generator cases stay in C++ and
are extended. The tracker's "177 of 178" sentence is rewritten at the fold.

---

## §6 — Potential issues for the implementation

Grouped by where they bite. Each names the item that must close it. **B** =
blocking or architectural, **S** = inherited from the seam, **N** = .NET interop,
**G** = generator, **P** = packaging/process.

### Blocking / architectural

- **B-1 — There is no codec to wrap.** D-BIND-1 assumes a descriptor-driven codec
  exists; only its *scope* does (GIR-8). BIND-2 must build a nanoarrow-only
  schema-driven driver of `positional_io`, covering every type in the mapping,
  nesting to arbitrary depth, and both directions. It is a third *driver* of the
  one wire format, not a second wire format, and the byte-identity test against
  `arrow-bridge` is what keeps that sentence true. Size: **L**, on the critical
  path. *Close in BIND-2.*

- **B-2 — Per-row publish pays for an Arrow array.** A single-row `Publish(row)`
  builds a one-row `StructArray` in managed code, exports it, crosses, and encodes.
  The C++ generated path writes bytes into the window directly. The copy oracle
  will still report zero copies (the wire bytes are written once, into the
  window), but allocation and latency per row are higher than C++. **Mitigation:**
  measure in BIND-3 with a per-row benchmark against the C++ generated publisher;
  offer `Publish(batch)` as the fast path; document. §3.2's bind step makes that
  path structural: one export and one validation per batch, N publishes. **If the owner judges the
  per-row cost unacceptable, the only faster route is generated C# writing wire
  bytes, which is a D-BIND-1 STOP-AND-ASK**, not a local fix. *Measure in BIND-3;
  decide in BIND-4.*

- **B-3 — Which built-ins the shim links decides the package.** Linking Fast DDS
  and XRCE statically into one shim gives C# real transports now, but the native
  asset per RID grows (Fast DDS + fastcdr + foonathan + tinyxml2 + Micro XRCE +
  microcdr), Fast DDS's shared-memory transport needs writable
  `eprosima/fastdds_interprocess` directories on the consumer's machine, and the
  LGPL notice must list what the shim contains. `inprocess`-only defers all of
  that to PDA-ABI, which has not started. **Precedent (2026-09-10):** the gateway
  already statically links Fast DDS and its whole dependency chain into one
  executable and ships it as a release archive, with the licence notice #127
  added; the archive's size on the latest `gateway-v*` release is a usable proxy
  for the shim's per-RID native asset before BIND-0 builds one. *Decide in BIND-0
  (§7 Q3).*

- **B-4 — A second copy of Fletcher is silent.** `inline thread_local` per binary
  (P1). The detectable case is two shims; the undetectable case is a C++ host that
  statically links Fletcher and also hosts the CLR. Feature 20061 ("Flight:
  refactor services onto Fletcher") is the consumer: **if any Flight service is a
  C++ process hosting .NET, or hosts two bindings, this is a STOP-AND-ASK before
  design, per P1.** *Answered 2026-09-11 (Q11): no such consumer exists; the check
  is implemented in BIND-2 for the detectable case and the README states the rule.*

- **B-5 — PDA-ABI and BIND now share no gate but will share code paths.** When
  PDA-ABI installs a resolver, the shim calls `SetPathResolver` and driver
  binaries appear beside the shim. Constraint 1 then applies to drivers too: a
  driver must not carry a Fletcher copy (PDA-ABI keeps the adapter host-side, so
  it does not). Nothing to do now except keep the shim's registry seat open and
  say so in the header. *BIND-1.*

### Inherited from the seam (constraints 1–8 and the two details)

- **S-1 — `Unsubscribe` blocks for the duration of a handler.** Correct and
  needed (it is what lets the `GCHandle` be freed), but a handler that never
  returns blocks `Unsubscribe` forever, and a `Dispose` from a finaliser thread
  would block the finaliser thread. **Recommend no finaliser on `Subscriber`**; an
  undisposed `Subscriber` at GC is a leak reported by a debug-only
  `~Subscriber` that only logs. *BIND-4.*

- **S-2 — The re-entrant `Unsubscribe` carve-out.** Solved by the in-flight
  counter (D-BIND-18): on the re-entrant path `Unsubscribe` returns without
  freeing; the last thunk out frees. The subtle case is a *sibling* subscription
  on the same `Subscriber` running on another thread; the counter covers it
  because native guarantees no new invocation begins. Needs a test that cancels a
  sibling from inside a handler while the sibling is mid-delivery, mirroring
  `CallerTier.CancellingASiblingRunningOnAnotherThreadKeepsItPublished`. *BIND-4.*

- **S-3 — `Dispose` of a `Subscriber` from inside a handler terminates the
  process.** Managed refusal (`InvalidOperationException`) before native, keyed on
  the thread-static marker. Also reachable via `using` scope exit inside a handler
  and via `await using`; the marker must be set for the synchronous extent of the
  thunk only, and the docs must say an `async` handler continuation runs outside
  the marker (see N-3). *BIND-4.*

- **S-4 — `Subscribe` from a handler is data-dependent in C++.** The constraints
  file says "always queue". **Recommendation differs:** a synchronous
  `Subscribe` from a handler throws a managed exception naming
  `DispatchAfterDelivery`, and the deferred spelling is that helper returning a
  `Task<Subscription>` whose failure surfaces on the `Task`. Silent queueing would
  make a refused topic name (a `kInvalidArgument`) surface only through a failed
  `SchemaArrival` later, which is a worse error surface than a throw. Both are
  deterministic, which is the property the constraint asks for. *Owner's call;
  BIND-4.*

- **S-5 — The absorbed-failure count.** The thunk's catch increments the managed
  per-instance counter; native's `AbsorbedTotal` stays at 0 for managed faults.
  Expose both, document which counts what, and test that a throwing handler
  raises the managed delta by exactly one. *BIND-4.*

- **S-6 — `AppendInPlace` has never met production code.** BIND is its first
  consumer (via `publish_raw`). The residue "bytes in `[written, used)` are
  published having been written by nobody" is real on `FixedWriteBuffer`; the
  managed writer must return the byte count it copied, never a character count.
  Cover with a test that publishes a non-ASCII row through `publish_raw` and
  checks the decoded row. *BIND-2 / BIND-3.*

- **S-7 — `Timeout.Infinite` is refused by the seam.** Mapped in the wrapper
  (D-BIND-20). The unbounded-wait clause is pinned by no test on Windows, by
  ruling; BIND inherits that blind spot and lists it here rather than in a bug.
  *BIND-4.*

- **S-8 — Strings are bytes at the seam.** Topic segment and joined-name caps in
  UTF-8 bytes; attachment key order is `memcmp`, not `string.CompareOrdinal`;
  `Attachments` in C# must not expose a sorting API. Test with a key above U+E000
  and a supplementary-plane key, where UTF-16 and UTF-8 orders differ. *BIND-4.*

- **S-9 — Null schema is a mode, not an error.** `SchemaArrival.Wait` returning
  `Ok(null)` on a schema-less transport; the generated typed subscriber must
  handle it by using its own descriptor, and `Ok(null)` must **not** release an
  owner handle (§3.4). *BIND-4 / BIND-6.*

### .NET interop hazards

- **N-1 — No exception may cross in either direction.** `[UnmanagedCallersOnly]`
  thunks catch everything; native entry points translate everything to a status.
  A managed exception escaping an `UnmanagedCallersOnly` method is a fail-fast.
  Enforce with a Roslyn analyzer or a reflection test that every such method's
  body is wrapped. *BIND-3.*

- **N-2 — Transport threads call managed code.** Fast DDS listener threads and the
  XRCE session pump enter the CLR via the thunk; that is legal, but a handler that
  blocks starves the transport (Fast DDS holds the reader mutex across the call).
  Document; provide `DispatchAfterDelivery`; consider a debug watchdog that logs
  handlers over a threshold. *BIND-4.*

- **N-3 — `async void` handlers and `SynchronizationContext`.** An `async`
  delegate passed as a delivery callback returns at its first `await`, the thunk
  frees nothing wrongly (the counter is per synchronous extent), but the
  continuation runs **after** the borrowed `data`/`schema`/`attachments` are gone.
  The delegate type must not be awaitable; give the span/ref-struct signature so
  an `async` lambda cannot type-check, and document the trap. *BIND-4.*

- **N-4 — `ReadOnlySpan<byte>` cannot be captured, ref structs cannot cross
  `await`.** This is the protection, and it is also the ergonomics cost: a handler
  that wants the row later copies explicitly (`ToArray()`), which is the borrow
  rule made visible. Same for the schema: retaining it is `Retain()` on the owner
  handle, never the C Data Interface `release`. *BIND-4.*

- **N-5 — Two `release` protocols coexist.** The C Data Interface `release` on an
  `ArrowArray`/`ArrowSchema` that C# *exported* is called by native when done; the
  `ArrowSchema` a subscriber *receives* is shared and must **never** be released by
  C# (only its owner handle). `Apache.Arrow.C.CArrowSchemaImporter` consumes what
  it imports, so a received schema must be **deep-copied on the native side**
  (`OwnedSchema::DeepCopy`, public for exactly this reason) before import. Misuse
  is a use-after-free under every other holder. *BIND-3; test with a schema
  received twice.*

- **N-6 — `GCHandle` and pinning.** Context handles are `GCHandle.Alloc(obj)` (not
  pinned; they are opaque tokens). Buffers handed to native for the duration of a
  call are pinned with `fixed`; nothing pinned outlives its call. The
  `publish_raw` writer copies from a `fixed` region inside the writer frame.
  *BIND-3.*

- **N-7 — Native library loading.** `DllNotFoundException` at runtime, not build
  time, when the RID asset is missing or the wrong architecture; on Linux, a shim
  built on Ubuntu 24.04 (glibc 2.39) does not load on older distributions.
  Mitigate with `SetDllImportResolver` naming the RID and the search path, an ABI
  version handshake, and a **decision** on the glibc baseline for `linux-x64`
  (build on an older image, or state the floor). MSVC: the shim links the CRT
  **statically** (`/MT`) so consumers need no redistributable. *BIND-3 / BIND-9.*

- **N-8 — NativeAOT and static native linking change the LGPL analysis.** Default
  P/Invoke to a shared shim keeps the shim replaceable (see P-1). A consumer that
  statically links the shim into a NativeAOT binary creates a combined work with
  different obligations; the README must say so. *BIND-9 / BIND-10.*

- **N-9 — `Apache.Arrow` gaps.** C Data Interface support must be verified for
  every type in the mapping (nested lists, maps, struct-of-list, timestamps with
  timezone metadata, large binary); `StructArray.Fields` windowing is an open
  empirical question (D-BIND-10); dictionary arrays are deferred anyway. Pin the
  version and write a probe test per type at BIND-3, before generated code depends
  on it. *BIND-3 / BIND-7.*

### Generator

- **G-1 — Generated C# does not encode.** The plan's BIND-3 acceptance
  (`WriteTo`/`ReadFrom`) predates D-BIND-1 and must be rewritten as
  `ToArrow`/`FromArrow` plus a descriptor. If a reviewer asks for direct wire
  writing in generated C#, that is the D-BIND-1 STOP-AND-ASK. *BIND-6.*

- **G-2 — Temporal precision.** `DateTime`/`DateTimeOffset` ticks are 100 ns;
  Arrow nanosecond timestamps lose two digits on the round trip. The type table
  must choose: expose `long` + unit (lossless, less ergonomic), a Fletcher
  `Timestamp` struct, or `DateTimeOffset` with a documented truncation for `ns`.
  **Recommend lossless by default** (`long`-backed struct with `ToDateTimeOffset()`),
  since a silent precision change is a wire-visible difference in the capstone.
  Same question for `Duration` vs `TimeSpan`. *BIND-6, recorded in the type table.*

- **G-3 — Maps.** Arrow `Map` permits duplicate keys and preserves order; proto
  maps do not. `Dictionary<K,V>` drops duplicates silently on `FromArrow`.
  Recommend `IReadOnlyList<KeyValuePair<K,V>>` on the row and a documented
  conversion helper, or `Dictionary` with a refusal on duplicates. *BIND-6.*

- **G-4 — Accessor depth cap.** The IR has no cap; RBA's emitter caps nested lists
  at depth 2/3. Recommend **matching the cap** for capstone parity and recording
  the asymmetry as RIR's to remove; exceeding it deliberately needs a fixture C++
  cannot consume, which breaks D-BIND-11's shared-fixture rule. *BIND-7.*

- **G-5 — Nullability and `optional`.** Proto3 `optional`, message fields and
  wrapper WKTs map to nullable C#; scalars do not. Nullable reference type
  annotations must be emitted so consumers get warnings, and the descriptor's
  nullability must match the Arrow schema's, or the capstone fails on the
  nullable-flag-tolerant read. *BIND-6.*

- **G-6 — Multi-file and namespace.** Reuse `CollectCrossFileIncludes` read-only
  for namespace qualification; shared helper emitted exactly once from
  `GenerateAll()` (the RBA-1 failure mode). *BIND-6.*

### Packaging and process

- **P-1 — LGPL relinking.** Fletcher is LGPL-3.0-or-later; today consumers build
  from source. The argument to put to the owner: the shim is a **shared library**
  the .NET assembly loads at runtime, so the application is a combined work "using
  a shared library mechanism" and §4(d)(1) is met if the user can replace the shim
  with a rebuilt one; the corresponding source is the public repository at the
  tagged commit, and the NuGet carries the notice, the tag, and the build
  instructions. Static Fast DDS/XRCE inside the shim are permissive and do not
  change the analysis. **This is an engineering argument, not a legal conclusion;
  it needs an owner and a decision before BIND-9 designs packaging.** The
  *notice* half is no longer open: #127 (2026-09-10) ships `LICENSE` and a
  graph-derived `THIRD-PARTY-LICENSES.txt` with the gateway, and BIND-9 reuses
  that deployer for the shim (§4, BIND-9 licence files). What still needs the
  owner is the *relinking* half: whether a replaceable shared shim plus the
  public source at the tagged commit satisfies §4(d)(1), and what the NuGet
  README must say. *Owner assigned 2026-09-11 (Q9): the maintainer; BIND-9 hands
  over the brief and waits for the answer on packaging only.*

- **P-2 — RID matrix.** `win-x64` and `linux-x64` are the ones the tree already
  builds. `linux-arm64` needs cross-compilation or an ARM runner; `osx-arm64` needs
  a macOS runner and a Fast DDS build the tree has never done. Recommend
  **`win-x64`, `linux-x64` at first release**, `linux-arm64` as the first addition,
  macOS on demand. *BIND-9.*

- **P-3 — Conformance suite duplication.** The C++ suite cannot exercise C# code;
  the C# arm re-states `CallerTier` and the `ProviderConformance` clauses over
  `inprocess`. Two suites over one contract will drift unless each C# case names
  the C++ case it mirrors in an attribute and a script checks the mapping is
  total. *BIND-4.*

- **P-4 — NuGet Trusted Publishing** needs an org-side policy and a reserved
  `Eiva.Fletcher.*` prefix, both user actions outside a PR. Start at BIND-0 so
  the first `cd.dotnet.yml` run is not blocked. *BIND-0 / BIND-9.*

- **P-5 — ADO 18786 "all tests".** Two exclusion classes now (generator; provider
  internals) instead of one. Do not close 18786 before Bucket 4 is green over
  `fastdds` and `xrce`. *BIND-0 (ask), BIND-4 (deliver).*

- **P-6 — First lane run beats local green (§12.4).** Windows is the primary dev
  platform; three of PR #126's seven CI-found defects were Linux-only. BIND-0's
  empty-ABI lanes exist to make the Linux signal standing from day one.

- **P-7 — `protoc/` and `arrow-bridge/` are contended.**
  `feature/fastdds_modernization/19645` carries four commits not on `main`
  (2–3 September) that rewrite 19 files in `arrow-bridge` and change `protoc/` by
  +2334/−4460 against the IR-based generator GIR landed; BIND-6/7 add a third
  backend to that same `protoc/` directory and BIND-2's oracle targets
  `arrow-bridge`'s `Codec`. Whoever lands second pays the rebase, and the branch's
  protoc part cannot rebase at all without being re-done on the IR.
  `TsVisitor.DescriptorByteIdentical`, the no-drift test and BIND-2's byte-identity
  oracle are what prove neither party moved wire bytes. *Agree the landing order at
  BIND-0.*

---

## §7 — Decisions needed before kickoff

Answer these and BIND-0 can close in a day. Q13–Q18 are interface-shape
questions and live in [BIND-csharp-public-surface.md](BIND-csharp-public-surface.md) §7;
the numbering is shared so a ruling can cite one number.

| # | Question | Recommendation | Blocks |
|---|---|---|---|
| Q1 | Lock the value transfer as the Arrow C Data Interface (D-BIND-1a′) with both riders, **together with the encode surface in its three layers (D-BIND-23, §3.2)**: the C ABI (`fl_codec_open` once per schema, `fl_rows_bind` once per batch with the array borrowed, `fl_publisher_publish_row(s)` fused into `Publish`, `fl_encode_row` into the seam's window form, `fl_decode_rows`), the C++ behind it (`NanoarrowCodec{Bind, EncodeRow(BoundRows, i, WriteBuffer&), DecodeRows}`), and the C# on top (`FletcherCodec.Bind → BoundRows`, `Publisher.Publish(topic, rows, i)`, no method returning bytes) | ✅ **LOCKED 2026-09-11**: all three layers as one lock; B-2 stays an open measurement in BIND-3 with the D-BIND-1 STOP-AND-ASK as the escape hatch | BIND-1, BIND-2 |
| Q2 | Three managed packages instead of six (D-BIND-14′) | ✅ **LOCKED 2026-09-11**: three | BIND-0 |
| Q3 | Shim links and registers `inprocess` + `fastdds` + `xrce` (D-BIND-3′) | ✅ **LOCKED 2026-09-11**: all three | BIND-2, BIND-9 |
| Q4 | Constraint 4 spelling: managed refusal + explicit async helper, or silent queueing (S-4) | ✅ **LOCKED 2026-09-11**: refusal + helper (D-BIND-18) | BIND-4 |
| Q5 | Temporal mapping: lossless `long`-backed struct vs `DateTimeOffset` (G-2) | ✅ **LOCKED 2026-09-11**: lossless (D-BIND-26) | BIND-6 |
| Q6 | Accessor depth cap: match RBA's 2/3 (G-4) | ✅ **LOCKED 2026-09-11**: match (D-BIND-9 amended) | BIND-7 |
| Q7 | Toolchain: .NET 10 LTS SDK; TFMs `net8.0;net10.0` (D-BIND-21) | ✅ **LOCKED 2026-09-11** | BIND-0 |
| Q8 | RID matrix at first release (P-2) | ✅ **LOCKED 2026-09-11**: `win-x64`, `linux-x64` (D-BIND-27) | BIND-9 |
| Q9 | Owner for the LGPL relinking review (P-1) | ✅ **Assigned 2026-09-11: the maintainer.** Decision due before BIND-9 designs packaging; BIND supplies the engineering brief (P-1's argument, the #127 notice mechanism, the NativeAOT caveat N-8) | BIND-9 |
| Q10 | 18786 exclusion classes: generator tests and provider-internal tests stay in C++ (P-5) | ✅ **LOCKED 2026-09-11**: both classes, documented | closing 18786 |
| Q11 | Are any Flight (20061) consumers C++ processes hosting .NET, or hosting two bindings? (B-4) | ✅ **Answered 2026-09-11: no** — consumers are .NET processes with this binding only. D-BIND-17's check covers the detectable case; a future C++ host is a STOP-AND-ASK under P1 | BIND-2 design |
| Q12 | Is BIND-Rust in this round? (plan open decision 5) | ✅ **Ruled 2026-09-11: next round** | scope |
| Q19 | Confirm the reading of 16353's "make C# emit Rust-native accessor classes" as *C# accessor classes equivalent to the Rust-native ones* (plan open decision 4; the literal phrasing would mean C# emitting Rust and looks like a copy-paste from the RBA feature) | ✅ **CONFIRMED 2026-09-11**: equivalence reading; ADO 16353 text to be corrected | BIND-7 |

---

## §8 — Definition of done (round), updated

- BIND-0..10 and BIND-T green, reviewed, logged in `BIND-progress-log.md`.
- **One wire format**: byte identity between the nanoarrow codec and `arrow-bridge`
  across the full scenario corpus; no managed code writes wire bytes except
  `GatewayClient`.
- **Zero-copy proven from C#**: the copy oracle green with the C# producer for rows
  and attachments, the UTF-16↔UTF-8 boundary stated in its scope note.
- **Generic transport proven**: `fastdds` and `xrce` reachable from C# by selector
  with no per-transport C# code; a path selector refuses with `kNotSupported`
  until PDA-ABI installs a resolver.
- **Single copy enforced**: the shim refuses to load beside a second marker.
- Every seam-inherited obligation (constraints 1–8, the two details) has a named
  test in the C# suite, and the C# `CallerTier` arm maps totally onto the C++ one.
- Test matrix of §5 green per the owner's ruling on exclusions; 18786 not closed
  before Bucket 4.
- Generator: every plugin output has a C# counterpart; TS gains
  `Publisher`/`Subscriber`; no existing output byte changed.
- `GatewayClient` carries no `runtimes/`; the shim is within its size budget.
- Docs incl. TD-009; round archived to `docs/archive/BIND/`.
