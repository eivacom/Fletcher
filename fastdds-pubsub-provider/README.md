# FastDDSPubSubProvider

Implements `fletcher::PubSubProvider` using [eProsima Fast DDS](https://fast-dds.docs.eprosima.com/) (RTPS). Transports `EncodedRow` byte buffers over a DDS domain with reliability settings tuned to minimise message loss.

> **Targets Fast DDS 3.4.x** (`fast-dds/3.4.0` from Conan Center). The provider's public API names **no eProsima type at all**: it is configured by `fletcher::ProviderConfig` plus a Fast DDS XML profiles document, so the Fast DDS headers are *not* exposed transitively and a consumer never needs them (see [QoS configuration](#qos-configuration) and [Consuming the package](#consuming-the-package)). The XML you write is Fast DDS 3.4's own profiles grammar, and v3 consolidated the legacy `eprosima::fastrtps` namespace into `eprosima::fastdds`, which matters if you paste profiles out of a 2.x application.

## How it works

A single `FastDDSPubSubProvider` instance manages one DDS `DomainParticipant`, one `Publisher`, and one `Subscriber`. Topics are created on demand via `CreateTopic`, which is also where the topic's `DataWriter` is created. DataReaders are created when the topic's schema and payload bound arrive on `__schema`.

The binary payload sent over the DDS bus is a raw `EncodedRow` (the positional wire format produced by generated code or `Codec::EncodeRow`), wrapped in a minimal CDR-LE framing: a 4-byte encapsulation header followed by a 4-byte length prefix. A single custom `TopicDataType` handles the CDR serialisation without requiring IDL generation as a build step, named as `fastddsgen` would have named it: `FletcherSamplePubSubType` over the sample layout. The companion schema channel uses `SchemaBytesPubSubType`, a subclass that only changes the registered name and the bound — the schema rides as an ordinary Fletcher sample, a row (the IPC bytes) plus one attachment carrying the publisher's bound, through the same layout.

### Zero-copy: the plain sample

Fast DDS delivers zero-copy only for a **plain** type — one whose in-memory layout already *is* its CDR representation, so neither end serialises anything. The sample is therefore a fixed layout, spelled as an offset and a size in `internal/fletcher_sample.hpp`:

```
[0, 4)                     uint32 length — bytes of body in use
[4, 4 + payload_bytes)     body — row, then attachments
```

Both ends address those two fields — `ReadSampleLength`/`WriteSampleLength` and `SampleBody` — which is the whole difference from a hand-framed byte buffer. Nothing about the layout can be padded (one length, then bytes), and the sample stays free of *tail* padding exactly while its total is 4-aligned. That is the one property the bound has to have, and the whole of the rule:

```cpp
constexpr bool IsPayloadBound(uint32_t bytes) {
    return bytes >= kMinPayloadBytes && bytes <= kMaxPayloadBytes && bytes % 4 == 0;
}

template <uint32_t N>
concept PayloadBound = IsPayloadBound(N);   // the same rule, for a bound the compiler knows

options.max_payload_bytes = fletcher::kPayloadBytes<128 * 1024>;  // checked where it is written
options.max_payload_bytes = fletcher::kPayloadBytes<100'001>;     // does not compile
```

Written once and used twice, so a bound rejected at run time and one rejected at compile time are
rejected by the same expression. `IsPayloadBound()` is public, so a value read from configuration
can be checked without catching. `kMinPayloadBytes` is the smallest envelope that can exist and
`kMaxPayloadBytes` is where a sample's own size stops fitting the uint32 Fast DDS reports it in —
neither is a judgement about what a deployment can afford. What a bound *costs* is the bound
multiplied by resource limits that belong to the caller, and nothing here caps that product (see
[What this costs](#what-this-costs)).

Nothing is ever rounded: a bound that cannot frame a sample is a mistake, and one that only exists at run time — read from config, say — is refused by the constructor instead. A publisher registers `fletcher_<its bound>` (`fletcher_65536` at the default); a subscriber learns the bound from the `__schema` announcement and registers the same name, so bounds need no agreement out of band — it is the publisher's number that sizes both ends.

> An earlier revision made the sample a struct templated on its size, which forced the runtime option to be matched against a *compiled set* of bounds — powers of two from 4 KiB to 8 MiB, walked at compile time by doubling. Powers of two were never an alignment rule; they were what made the walk short enough to compile (stepping by 4 would need two million instantiations), and the floor and ceiling were where the walk started and stopped. Fast DDS never needed the compile-time size: `DataWriterImpl::loan_sample` reads the runtime `max_serialized_type_size`, and a loaned collection only casts the payload pointers it is handed. What the struct's `static_assert`s proved reduces to `bytes % 4 == 0`.

Each side splits into two flows, mirroring how DDS itself splits the calls — `WriteSample` (a free
function) and `LoanableSampleWriter` (a standalone class) on the publish side
(`internal/sample_writer.hpp`), `CopyingDataReaderListener` and `LoanedDataReaderListener`, sharing
`DataReaderListenerBase`, on the subscribe side (`internal/data_reader_listener.hpp`), each driven by
`Drain` calls from `on_data_available`, on the thread that delivered the sample (see
[Statuses](#statuses) for what else runs there):

| | Publish — `WriteSample` | Subscribe — `DataReaderListenerBase` |
|---|---|---|
| **Loanable** | `LoanableSampleWriter`: `loan_sample` → fill the length and the body → `write(sample)`. No `serialize()` runs at all; with shared memory the payload being filled *is* the one the reader reads. | `LoanedDataReaderListener`: `take(LoanableSequence&, SampleInfoSeq&)` → read the two fields in place → `return_loan`, invoked from `on_data_available`. |
| **Plain** | `WriteSample`: `write(&PublishData)` runs `serialize()`, writing the same layout **truncated after `length`** — so a small row stays small on the wire. | `CopyingDataReaderListener`: `take_next_sample(&ReceivedData, &SampleInfo)` — Fast DDS deserialises, which reads `length` out of the payload and needs nothing beyond the bytes that arrived; also invoked from `on_data_available`. |
| **Chosen by** | the provider always uses `WriteSample`; `LoanableSampleWriter` is kept, unit-tested, unselected — a loaned write ships the full bound on the wire and saves ~15 ns. | `Subscribe` always constructs `CopyingDataReaderListener` — the selection point is one line there. `LoanedDataReaderListener` stays in the tree, compiled and unit-tested against its precondition, `internal::CanLoanSamples(qos)` — reading a whole sample in place needs whole payload nodes, which only a `PREALLOCATED*` history memory policy guarantees; under `DYNAMIC_RESERVE`/`DYNAMIC_REUSABLE` the pool sizes each node to what arrived, so a truncated sample leaves a node shorter than a whole one and `length` would steer reads past its end — but it is not selected by the provider yet. |

Neither side is negotiated. `loan_sample` is gated by the *writer's* own type (`DataWriterImpl::loan_sample`) and the reader's loans by the *reader's* own type (`DataReaderImpl::enable`, where `is_plain` is computed from the type alone), so all four pairings interoperate — upstream regression-tests exactly that in `test/dds/communication/mix_zero_copy_communication.json`, and it branches on `zero_copy_` between exactly these two reader calls in `test/dds/communication/SubscriberModule.cpp`.

**Data-sharing** is `AUTOMATIC` at both ends — Fast DDS's own default — by owner decision 2026-09-14. Fast DDS engages shared memory when both endpoints are on one host and the type qualifies, and falls back to the transport when they are not. Upstream's own zero-copy test profile (`simple_reliable_zerocopy_profile.xml`) uses `AUTOMATIC` for the same reason.

The reader side of this was `off()` for a time — the one place this provider overrode the policy. With data-sharing on both ends, a reader that subscribes *after* rows were published could intermittently receive only part of the `TRANSIENT_LOCAL` backlog — frequently just the newest sample — and reported no error at either end. Measured cross-process on Windows against Fast DDS 3.4.0, three runs each:

| writer / reader | result |
|---|---|
| `AUTOMATIC` / `AUTOMATIC` | 4/4 pass, then 2/4, then 2/4 — one of three rows delivered, or none |
| `AUTOMATIC` / `off()` | 4/4 pass, 3 runs |
| `off()` / `off()` | 4/4 pass, 3 runs |
| `AUTOMATIC` / `AUTOMATIC`, `max_samples` 8 rather than 100 | 4/4 pass, 3 runs |

The provider was not the one dropping them: the pre-schema buffer in place when this was measured logged on every discard, and that log never fired. That a 0.5 MB pool is reliable where a 6.6 MB one is not points below the provider as well.

Both ends ship `AUTOMATIC` again as of 2026-09-14. The table above is kept as the historical measurement, and `integration-tests/gateway-fastdds-ts` is the guard going forward: it re-verifies the cross-process case, three runs at a time, because this defect is invisible to the provider's own test suite — that suite is single-process, so Fast DDS serves it over intra-process delivery, which bypasses data-sharing altogether. The `__schema` channel is the one place data-sharing still stays `off()` at both ends, and not for this reason — see the Fast DDS 3.4.0 teardown hang in [Known limits of the document](#known-limits-of-the-document).

What this costs:

- **Memory.** A bounded type puts payload pools in `PREALLOCATED`, so every history slot reserves the whole sample. The built-in document sets `resource_limits().allocated_samples` equal to `max_samples` (100) on both data endpoints, so the whole pool is reserved at endpoint creation, not grown into as samples arrive. A data-sharing writer allocates `(max_samples + extra_samples) * (bound + 8)` bytes of shared segment immediately. Size `max_payload_bytes` and the resource limits to the rows the topics actually carry; nothing in the provider caps their product, and if it does not fit a data-sharing segment (a 32-bit size) Fast DDS declines data-sharing and uses the transport. The defaults are 64 KiB against `max_samples = 100`, so **~6.5 MB per endpoint per topic**, reserved immediately — 300x a typical 214-byte row. Lower `max_payload_bytes` for a deployment whose rows are small: `max_payload_bytes` is the publisher's knob, and a subscriber's reader pool is sized by the bound the publisher announces, so lowering it at the publisher lowers it everywhere. Dropping the default to 8 KiB was measured and reverted — it costs the subscriber-first burst its accidental headroom (see below).
- **No pre-match headroom.** The data reader is not created until its schema and bound are known, so a subscriber that joins first buffers nothing of its own, and the built-in `VOLATILE` durability keeps nothing at the writer either: a burst published before the reader has matched is simply gone, not queued for later delivery. After the match, the WRITER's `resource_limits().max_samples` (100 by default) bounds the in-flight window between writer and reader, and a reader that lags past it for longer than `max_blocking_time` (100 ms) sees drops rather than a stalled publisher. Raise `resource_limits().max_samples` at the writer (and the reader) or pace the publisher to widen that window.
- **Wire size on the unselected loaned path.** Fast DDS stamps a loaned payload `length = max_serialized_type_size` and nothing recomputes it, so every sample would cross the wire at the full bound whatever the row weighs, were it selected. And it buys little: measured publish-side (`bench_dds_payload`, p50 of 2x4000 samples) it saved a **fixed ~0.1-0.2 us**, not a per-byte cost — 1.05 -> 0.95 us at a 198-byte row and 2.15 -> 2.00 us at 60 KB. Both paths write the row bytes exactly once, so loaning removes no copy: it removes the encapsulation and the length field, and the `PublishData` the serialising path hands to `write()`. Those are now **14.6 ns against 29.3 ns** at a 198-byte row (`bench_pub_sub_type`), so the publish-side case for it is weaker than those DDS-level numbers, which predate that work. Not selectable; kept in the tree.
- **Oversized rows throw** regardless of path: a row plus attachments past `max_payload_bytes` raises `std::overflow_error` out of `Publish`, or is reported inside `serialize()` on the regular path; either way the sample is dropped.

What the **read** side is worth, which is where the plain type actually pays: `bench_read_flow`
publishes flat out and meters the receive path, medians of 3 runs x 8000 samples.

| row bytes | `LoanedDataReaderListener` | `CopyingDataReaderListener` | throughput gain |
|---|---|---|---|
| 198 | 8.07 µs/sample, 24.5 MB/s | 8.26 µs/sample, 24.0 MB/s | +2% |
| 4 294 | 7.99 µs, 538 MB/s | 8.66 µs, 496 MB/s | +8% |
| 16 582 | 8.04 µs, 2 062 MB/s | 10.15 µs, 1 634 MB/s | +26% |
| 60 198 | 7.93 µs, 7 587 MB/s | 11.43 µs, 5 269 MB/s | **+44%** |

The loaned path's per-sample cost is **flat in payload size** — it never copies — while the
deserialising path grows linearly at roughly 58 ns/KiB, the cost of allocating a fresh `std::vector`
per sample and memcpy-ing `length` bytes into it. That is the case for turning the loaned flow on;
`Subscribe` does not make that choice automatically yet (see the flow table above) — by owner
decision the copying flow ships regardless of whether `internal::CanLoanSamples(qos)` holds.

The one deviation from a generated plain type: `serialize()` emits the used prefix rather than the whole sample, and `deserialize()` reads `length` bytes rather than memcpy-ing the lot. Fast DDS never reads past `payload.length`, so this is safe, and it is what keeps the non-loaned path from putting the whole bound on the wire per sample.

**Where fastcdr is used and where it is not.** The rule is that fastcdr does the work wherever it is
free, and the framing is hand-written only on the path where it is not:

- The representation id is `EncodingAlgorithmFlag | Cdr::LITTLE_ENDIANNESS` and the padding octet is
  `Cdr::alignment(length, 4)` — both fastcdr, both `constexpr`, both free.
- Everything else — the eight framing bytes of `serialize()`/`deserialize()` — is placed by hand, and
  a unit test pins them to what fastcdr produces (see the measured decisions table below).

The two channels are the **same type**: `SchemaBytesPubSubType` is a subclass of
`FletcherSamplePubSubType` that only sets the registered name and the bound, so the `__schema`
channel is serialised by the exact same `serialize()`/`deserialize()` as the data channel — no
separate `FastBuffer`/`Cdr` path remains. The schema rides as an ordinary Fletcher sample, a row
(the IPC bytes) plus one attachment carrying the publisher's bound.

That deviation has a limit worth stating plainly: **the plain claim holds between Fletcher endpoints, not against a `fastddsgen`-generated peer.** A generated type support for the equivalent IDL reads `length` and then a fixed `octet[N]`, so a serialised Fletcher sample — which stops after the bytes in use — is short by however much of the bound the row did not need, and its `Cdr` refuses it. Fletcher's own reader is unaffected because it bounds itself by `length` on the copying path and by `internal::CanLoanSamples` on the loaned one. A peer that has to read these samples has to read them the same way, which in practice means through this provider (or the XRCE one, which forwards the bytes opaquely).

On the loaned path, trusting a peer on this type name goes one step further than the wire shape: `LoanedDataReaderListener::Drain` (`internal/data_reader_listener.hpp`) checks a sample's declared `length` against `payload_bytes_` only, and Fast DDS's own loan hands back a bare pointer into a preallocated, fixed-size node with no length check of its own. A peer that stamps a `length` bigger than the row it actually wrote — while staying at or under `payload_bytes_` — is not caught by either check: the read stays in bounds, because every loaned node is `max_serialized_type_size` regardless of the row inside it, but the extra bytes the parser then reads are whatever an earlier sample left in that reused slot, not part of the current row. There is no API to bound a loan more tightly than that. A peer on this type name is therefore TRUSTED for `length`, not just for the CDR shape.

### Statuses

Endpoint and discovery status is **observed, not logged**: pass a `fletcher::FastDDSStatusListener*` to the provider's two-argument constructor and override the callbacks worth hearing about. Everything arrives in Fletcher's own vocabulary — no eProsima type appears on this seam, so a consumer needs no Fast DDS headers to use it.

```cpp
struct MatchWatcher : fletcher::FastDDSStatusListener {
    std::atomic<int32_t> matched_publishers{0};

    void OnMatched(Endpoint endpoint, int32_t current_count, int32_t /*change*/) noexcept override {
        if (endpoint.is_writer || endpoint.is_schema_channel) return;  // subscriber side, data rows
        matched_publishers.store(current_count);
    }

    // A writer on our topic whose type name carries another bound is a max_payload_bytes mismatch,
    // and this is the only place it is visible — see "Why discovery is exposed" below.
    void OnWriterDiscovered(std::string_view topic, std::string_view type_name,
                            bool alive) noexcept override {
        spdlog::debug("writer on '{}' type {} alive={}", topic, type_name, alive);
    }
};

MatchWatcher watcher;  // must outlive the provider
auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>(fletcher::ProviderConfig{},
                                                                 &watcher);
// watcher.matched_publishers is readable from any thread
```

`spdlog` above is illustrative, not a dependency this library takes.

`FastDDSStatusListener::Endpoint` is `{std::string_view topic; bool is_schema_channel; bool is_writer;}`. `topic` is the joined Fletcher topic; the companion `<topic>/__schema` channel reports under the **same** topic with `is_schema_channel == true`, so that flag — not a name comparison — is what tells a stuck schema handoff from a lost row. `is_writer` says which end of the topic the status is about, which is why one listener object can serve a publisher and a subscriber at once. The views alias names the transport owns and are valid for the duration of the call only; copy anything you keep.

**Silent by default, and that is a behaviour change.** With no listener — every provider built from a `ProviderConfig` alone, including every provider a `ProviderRegistry` builds — the provider says nothing about statuses: matches, QoS mismatches, deadline misses, lost and rejected samples all happen without a word. Earlier revisions logged them unconditionally through Fast DDS's own stdout/stderr consumer, so **an incompatible QoS used to print an error and now prints nothing.** That is the Fast DDS convention (no listener installed, no callbacks fired) and it is one line to undo: `fletcher::FastDDSLoggingStatusListener logger;` passed to the constructor restores every former line at its former level. This is also why the gateway (see [gateway/README.md](../gateway/README.md#providers)) observes no statuses: it builds its provider through `ProviderRegistry::Create`, and `RegisterFastDDSProvider`'s factory takes only a `ProviderConfig` — there is no listener parameter for a registry to plumb through. A caller who wants statuses constructs `FastDDSPubSubProvider` directly, with the two-argument constructor above, instead of going through the registry.

| Status | Level | Why it matters |
|---|---|---|
| `OnIncompatibleQos` | error | The endpoints never match, so the only symptom is a subscriber that stays unconnected with nothing logged. Reachable whenever writer and reader QoS are configured independently. |
| `OnMatched` | warning on loss, info on gain | An endpoint with no peers left keeps working and reaches nobody: a writer accepts publishes and delivers them nowhere, a reader receives nothing. |
| `OnDeadlineMissed` | warning | Only fires on an endpoint an operator gave a `DEADLINE` to — Fletcher sets none. |
| `OnLivelinessLost` | warning | Readers have marked the writer NOT_ALIVE. Fletcher leaves `LIVELINESS` at `AUTOMATIC` with an infinite lease, where it cannot fire, so this too reports a configured policy. |
| `OnLivelinessChanged` | warning while any writer is not alive, else info | Reader side of the same policy. |
| `OnSampleLost` / `OnSampleRejected` | warning; error for a rejected schema sample | On a data reader, resource limits are too tight. On the schema channel (`is_schema_channel`) either one leaves subscribers waiting on an arrival that never resolves, so the schema lines say so and name `kSchemaPayloadBytes`. |
| `OnUnacknowledgedSampleRemoved` | warning | Under `KEEP_ALL` + `RELIABLE`, history overflowed past `max_blocking_time` — loss rather than backpressure. No `StatusMask` bit; Fast DDS dispatches it whenever a listener is set at all. |

**What no test forces through a real Fast DDS condition.** `test_fast_dds_pubsub_provider.cpp` exercises every `FastDDSLoggingStatusListener` body directly (a probe that needs no DDS condition at all) and, through a live provider pair, forces `OnMatched`, `OnIncompatibleQos`, `OnReaderDiscovered`/`OnWriterDiscovered` and `OnParticipantDiscovered`. It does not force `OnDeadlineMissed`, `OnLivelinessChanged`, `OnLivelinessLost`, `OnSampleLost`, `OnSampleRejected` or `OnUnacknowledgedSampleRemoved` this way — each of those is checked only by calling the listener method directly, not by driving Fast DDS into the state that would call it. If you rely on one of these firing, verify it against your own QoS and workload rather than against this suite.

To keep a line *and* add behaviour, derive from `FastDDSLoggingStatusListener` instead and call the base from the override — `FastDDSLoggingStatusListener::OnMatched(endpoint, current_count, change);` — leaving alone whatever should just keep logging.

**`EPROSIMA_LOG_INFO` compiles to nothing** unless the build defines `FASTDDS_ENFORCE_LOG_INFO` (`Log.hpp`), so the routine half of `OnMatched` and `OnLivelinessChanged` needs that switch to appear. Warnings and errors are always compiled in. To route Fast DDS's own log stream (Fletcher's categories are `FLETCHER_SUBSCRIPTION`, `FLETCHER_PUBLICATION`, `FLETCHER_SCHEMA`, `FLETCHER_DELIVERY`) into an application logger, register a `LogConsumer`: `Log::RegisterConsumer` *adds* one next to the default stdout consumer, so call `Log::ClearConsumers()` first to replace it, and `Log::Flush()` before asserting on output — logging is asynchronous.

**What is installed where.** The DATA reader is a real Fast DDS listener again: `internal::DataReaderListenerBase` (`internal/data_reader_listener.hpp`) is installed with `StatusMask::all()`, so Fast DDS dispatches `on_data_available` (which calls `Drain`) and every reader status — `on_subscription_matched`, `on_requested_deadline_missed`, `on_liveliness_changed`, `on_requested_incompatible_qos`, `on_sample_lost`, `on_sample_rejected` — straight to it, the same dispatch the writer side already used. The `__schema` reader is different: it keeps the read-and-clear polling shape, driven now by the **schema thread** — one provider-owned thread that waits on a `WaitSet` over every subscribed topic's `__schema` reader (see [Delivery guarantees](#delivery-guarantees)) — which on each wake reads `get_status_changes()` and calls the matching `get_*_status()` getter and this listener for every changed bit, then drains the schema sample itself — the same pattern as eiva-ddsbus's `WaitsetDataReader`. WRITER statuses go through Fast DDS's own listener mechanism as before: Fletcher's `DataWriterListener` (`internal/data_writer_listener.hpp`) is installed with the status mask it implements, rather than the default `StatusMask::all()`, so Fast DDS dispatches only those. The participant listener (`internal/participant_listener.hpp`) is installed with **`StatusMask::none()`**, which is load-bearing: a participant listener holding the `data_on_readers` bit is handed every reader's data *instead of* the reader's own listener (`DataReaderImpl::set_read_communication_status` asks the subscriber/participant chain first). The three discovery callbacks are not mask-gated, so `none()` costs nothing.

**Why discovery is exposed.** `on_inconsistent_topic` is never invoked anywhere in Fast DDS 3.4.0. `OnWriterDiscovered` / `OnReaderDiscovered` fire from `PDP::addWriterProxyData` *before* EDP pairing runs, so the remote endpoint's `type_name` — the only place the bound is legible — is available there ahead of any match. What a differing bound in that `type_name` means has changed: a subscriber no longer has a bound of its own to disagree with, since its reader is created at whatever bound the publisher it follows announced, so a remote writer on another bound is no longer a subscriber-side mismatch. What it shows instead is either publisher-vs-publisher information (a second publisher announcing another bound for a topic already resolved is logged and never matches the first), or the sign that this instance already holds the topic, as a publisher, at another bound. `ASubscriberFollowsThePublishersPayloadBound` in the test suite pins that a subscriber ends up on the bound its publisher announced and that the data channel then matches (`OnMatched` fires on it, and the row arrives).

**Threading, in full.** A callback runs on a Fast DDS thread (data and writer statuses alike, and discovery), on an application thread already inside `CreateTopic`, `Publish` or `Subscribe` of *any* provider in this process — because intraprocess discovery and matching run synchronously inside `create_datareader` / `create_datawriter`, including between two participants in one process — or on the **schema thread**. The schema thread is where every `__schema` reader status arrives (`OnMatched`, `OnDeadlineMissed`, `OnLivelinessChanged`, `OnIncompatibleQos`, `OnSampleLost`, `OnSampleRejected`, on that endpoint only): it wakes on its `WaitSet` (an event-only wait: a status change on a registered schema reader, or the stop guard), reads `get_status_changes()`, and for each bit calls the matching `get_*_status()` getter and this listener before draining the schema sample — the same pattern as eiva-ddsbus's `WaitsetDataReader`. It is also where a data reader is created and enabled (`OpenDataReader`) once its schema has arrived (intraprocess matching for that reader completes there instead of on an application thread, under `schema_mu`); inside `Subscribe`, when this provider already knows the schema, that same call runs under both the provider mutex and `schema_mu` — as does `CreateTopic`'s data-writer creation. In the second case the calling provider's mutex (a non-recursive `std::shared_mutex`) is held for the duration; in the third, `schema_mu` is held instead. Either way an override **must not call into any provider** — re-entering deadlocks. It must not block, and must not wait on the thread destroying a provider: `~FastDDSPubSubProvider` waits for in-flight callbacks and calls can arrive until it returns, which is why the listener is a non-owning pointer that must outlive the provider. Every method is `noexcept`, so an override has to be spelled `noexcept override` too (the compiler refuses one that is not) and a throw inside it terminates: Fletcher does not catch here the way it does for a subscription callback, because one bad sample must not stop a stream but a throwing status callback is a defect worth stopping on. A status's `*_change` counter is the delta since the last delivery *or* the last `get_*_status()` poll, so polling the same status from another thread zeroes the delta the listener would otherwise see.

**Migrating from a `DataReaderListener` subclass.** Consumers that built on eiva-ddsbus's raw statuses were rebuilding three things: log lines, a reader-side matched count (`MatchedPublishers()` / `Connected()`), and a writer-side gate that blocked until a reader existed. The log lines are `FastDDSLoggingStatusListener`. The other two are `OnMatched`: store `current_count` for the count (`Connected()` is `current_count > 0`), and notify a condition variable for the gate — `is_writer` picks the side, and the identity a ddsbus subclass had to carry in a `topic_name_` member of its own arrives in `Endpoint::topic`.

### Topic name

The `std::vector<std::string>` topic segments from `PubSubProvider` are joined with `/` to form the DDS topic name. For example, segments `{"integration", "TelemetryFeed", "TelemetryStream"}` become the DDS topic `"integration/TelemetryFeed/TelemetryStream"`. The join is the **seam's**, not this provider's choice (spec §3.5): the joined name *is* the topic's identity.

**The joined name is capped at 246 bytes, and this provider is the reason.** Fast DDS announces a
topic in discovery as `fastcdr::string_255` (`PublicationBuiltinTopicData::topic_name`,
`SubscriptionBuiltinTopicData::topic_name`, `TopicDescription::topic_name`), and `fixed_string`
truncates **silently** — `fixed_size_string.hpp:83,331`, both `noexcept`, no error and no log. So
above that ceiling the name Fletcher computed is not the name this provider matches on: measured
on this box, two topics whose names agreed on their first 255 bytes were **one topic**, and a
subscriber to one received every row published to the other. The seam therefore refuses a joined
name longer than **246 bytes** at `internal::RequireSegments` (`kInvalidArgument`) — 255 less the
9 bytes of `"/__schema"`, so the schema companion this provider derives below stays under the
ceiling too. Bounded at 255 the data name would survive while its companion truncated back onto
it, which moves the collision to the hidden channel rather than closing it. Owner ruling
2026-09-04; pinned by `Segments.NamesThatWouldTruncateOnTheWireAreRefused` in `pubsub_tests`.

### QoS configuration

QoS is configured up-front, at construction, by **a Fast DDS XML profiles document handed to the
provider as text** in `fletcher::ProviderConfig::document`. There are no runtime setters and no
typed C++ QoS API — one way to do it. **An empty document is replaced by the provider's own default
document** (see [The published starting point](#the-published-starting-point)), so there is exactly
one path regardless of what the caller passes: the document — supplied or default — is loaded
**once per process** into Fast DDS's own profile registry
(`DomainParticipantFactory::load_XML_profiles_string`), the participant anchor
(`fletcher_participant`) is resolved from it, and each endpoint then resolves its QoS from the
document's `is_default_profile="true"` profile for its role, or a per-topic profile named after the
topic — see [Reserved profile names](#reserved-profile-names).

`ProviderConfig` carries exactly three things:

| Field | Meaning |
|---|---|
| `domain_id` | The DDS domain. Used exactly as given, and it always wins over the document. |
| `max_payload_bytes` | Governs this provider's PUBLISHERS only: the type name `CreateTopic` registers (`fletcher_<bound>`) and the row ceiling a publish has to fit. **0 means unset** and resolves to 65536. A subscriber takes its bound from the publisher it follows, announced on `__schema`, not from this field. |
| `document` | The XML profiles document, as **text**. Empty means the provider loads its own default document below (the fenced block), so every provider in the process ends up on the same document — an empty one and a different non-empty one in one process are refused like any two different documents. |

The setting holds the XML itself, never a filename: Fletcher never opens a file on a provider's
behalf. If you want the convenience of a file, `fletcher-gateway` has `--provider-config FILE`,
which reads the bytes and hands them over unexamined.

#### Reserved profile names

The only reserved name is the participant anchor. Everything else resolves through Fast DDS's own
default-profile mechanism, not through a second reserved name:

- **participant** — `fletcher_participant`, **mandatory**; a document with no `<profiles>` element
  parses fine and silently registers nothing, so the anchor is what turns that silent no-op into a
  construction-time refusal instead.
- **data writer on topic `T`** — the profile named `T` (the `/`-joined topic name) if the registry
  has one, else the document's `is_default_profile="true"` `<data_writer>` profile, else Fast DDS's
  own default.
- **data reader on topic `T`** — the mirror: `T`, else the document's `is_default_profile="true"`
  `<data_reader>` profile, else Fast DDS's own default.
- **the internal `__schema` channel** — *nothing, ever*: its own fixed QoS.

A **per-topic override** is therefore just a profile named after the topic.

#### A supplied profile is that endpoint's WHOLE quality-of-service

There is no merge and no floor. Anything a profile you supply leaves out takes **Fast DDS's**
default, not Fletcher's. This is not a convenience trade — the XML API returns a filled QoS and
cannot report which policies the document actually mentioned, so an overlay rule would rest on a
fact the substrate does not expose, and could be neither implemented reliably nor tested honestly.
A non-empty document that marks no `is_default_profile="true"` profile therefore gets Fast DDS's
own defaults, not Fletcher's — the honest reading of the same rule.

One rule instead: **supply a profile for a role, and you own that role's QoS.** Start from the
block below rather than from a bare profile.

#### Fletcher's default QoS profile

With an empty document, both the data DataWriter and the data DataReader get this profile:

| Policy | Setting | Reason |
|---|---|---|
| `reliability` | `RELIABLE_RELIABILITY_QOS` | The middleware retransmits unacknowledged samples until `max_blocking_time` (100 ms, Fast DDS's default) elapses; past that, a lagging reader costs a drop rather than blocking the writer forever. |
| `history` | `KEEP_ALL_HISTORY_QOS` | All samples are retained until every matched reader has acknowledged them. With `RELIABLE`, the writer blocks (rather than dropping) when the history is full. |
| `durability` | `VOLATILE_DURABILITY_QOS` | A stream's samples from before a reader matched are not worth keeping (owner, 2026-09-15); a topic that needs replay declares `TRANSIENT_LOCAL` in a per-topic profile. |
| `resource_limits` | `max_samples` 100, `max_instances` 1, `max_samples_per_instance` 100, `allocated_samples` 100 | The sample type is bounded and plain, so every endpoint reserves the whole payload bound per history slot. At Fast DDS's default 5000 that is gigabytes, which overflows the data-sharing segment's 32-bit size and silently drops the endpoint back to the transport. `allocated_samples` matches `max_samples`: no pool growth on the first samples, the cost is paid at creation. |
| `data_sharing` | `AUTOMATIC` at both ends (Fast DDS's own default) | Zero-copy receive on one host when the type qualifies. Owner decision 2026-09-14; the 2026-09 measured late-joiner backlog loss (see below) is re-verified cross-process by `integration-tests/gateway-fastdds-ts` — re-verified, see round record. |
| `reliable_writer_qos().times.heartbeat_period` | 20 ms (writer) | A writer waiting on a lagging reader re-syncs on the periodic heartbeat; 20 ms keeps the wait short. |

RELIABLE + KEEP_ALL give lossless delivery to a MATCHED reader; a reader that lags by more than
`max_samples` for longer than `max_blocking_time` (100 ms) costs drops, never a stalled publisher;
nothing is replayed to a reader that matches later.

#### The published starting point

This is the document the provider loads when yours is empty; the code holds the same text
(`FletcherDefaultProfilesDocument()`), and `DefaultProfileTranscriptionIsExact` asserts the two are
identical. Copy it, change what you need, and the policies you leave alone stay where Fletcher put
them. There is one copy of this XML in the repository and it is the one you are reading, so editing
either this block or the code alone turns that test red. `is_default_profile="true"` is what makes
these two the defaults — eProsima's own mechanism, not a Fletcher convention.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">
      <rtps><name>FletcherParticipant</name></rtps>
    </participant>
    <data_writer profile_name="default_writer" is_default_profile="true">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability>
          <kind>RELIABLE</kind>
        </reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
          <allocated_samples>100</allocated_samples>
        </resourceLimitsQos>
      </topic>
      <times>
        <heartbeat_period>
          <sec>0</sec>
          <nanosec>20000000</nanosec>
        </heartbeat_period>
      </times>
    </data_writer>
    <data_reader profile_name="default_reader" is_default_profile="true">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
          <allocated_samples>100</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_reader>
  </profiles>
</dds>
```

#### Refused at construction

All of these throw `PubSubError(kInvalidArgument)` **before** the DomainParticipant exists, so a
misconfigured provider never exists at all:

- a non-empty document Fast DDS cannot parse, or that does not define
  `<participant profile_name="fletcher_participant">`. The anchor is mandatory even when it carries
  no policies: a well-formed document can still define no profiles at all — Fast DDS accepts a
  `<dds>` with no `<profiles>` element as a silent no-op, `load_XML_profiles_string` registers
  nothing and returns OK — so without the anchor, a document that registered nothing would run on
  the built-in defaults unnoticed;
- a second, different document in the same process. Fast DDS profile names are process-wide, the
  first profile registered under a name wins, and nothing unloads — and Fast DDS alone would
  *accept* a partially colliding document (`XMLProfileManager::extractProfiles` downgrades a
  duplicate name to `XML_NOK` and carries on, which `load_XML_profiles_string` reports as OK). So
  the provider enforces the rule itself, before Fast DDS sees the bytes: every
  `FastDDSPubSubProvider` in one process is built from the same byte-identical document; a
  different one is refused, quoting the rule;
- a non-zero `<domainId>` in the anchor that disagrees with `ProviderConfig::domain_id`, quoting
  both numbers;
- a `max_payload_bytes` that cannot bound a payload (it must be a multiple of 4 within the
  supported range).

**The domain rule, stated positively:** the deployment's domain always wins. An anchor's
`<domainId>` must either match `ProviderConfig::domain_id` or be absent. An explicit
`<domainId>0</domainId>` cannot be told from absent — Fast DDS reports both as 0 — and is accepted
as absent.

#### Known limits of the document

- **A per-topic profile whose name matches no topic is inert.** The XML API resolves a profile *by
  name* and cannot enumerate what a document defines, so the provider cannot see, let alone
  complain about, a profile it never asks for. Check the spelling against the `/`-joined topic name.
- **A supplied reader profile can turn off receive-side data-sharing,** which Fletcher's built-in
  leaves at Fast DDS's own `AUTOMATIC` default. That is deliberate: a Fletcher floor underneath your
  profile would mean the document does not really configure QoS. Be aware that data-sharing on the
  receive side has a measured defect — part of the `TRANSIENT_LOCAL` backlog can be dropped to a
  late-joining reader with no error anywhere (see [Zero-copy: the plain
  sample](#zero-copy-the-plain-sample)); the built-in default carries it too, guarded by
  `integration-tests/gateway-fastdds-ts`.
- **A flat-out intraprocess publisher (publisher and subscriber in ONE process, RELIABLE +
  KEEP_ALL, no pacing) can lose a tail of samples.** Measured with `benchmarks/bench_e2e`'s 198 B
  throughput arm, 200 000 samples, under an infinite `max_blocking_time`: reader data-sharing
  `AUTOMATIC` — 3 of 3 one-core runs lost 13-981 samples, 0 of 3 two-core runs; `OFF` — 0 of 3
  one-core runs, 1 of 3 two-core runs (1 392 lost). Cross-process runs
  (`integration-tests/gateway-fastdds-ts`) never did. With the built-in's 100 ms
  `max_blocking_time` a lagging reader costs ordinary drops first. Pace or bound the publisher for
  a same-process loopback; the document can also set the reader's `<data_sharing><kind>OFF</kind>`
  for a one-core same-process setup.
- **Every reader with data-sharing on owns a Fast DDS `DataSharingListener` thread,** and Fast DDS
  3.4.0 can hang that reader's deletion forever: `StatefulReader::~StatefulReader` clears
  `is_alive_` before `DataSharingListener::stop()`, and `DataSharingListener::process_new_data`
  only advances its pool cursor when `process_data_msg` succeeds and never checks `is_running_`, so
  a payload still pending on the pool at teardown spins that thread forever and `stop()`'s join
  never returns. This is about teardown, not the backlog-loss defect above. The built-in data
  reader is AUTOMATIC (owner decision 2026-09-14), so it is exposed whenever data-sharing engages
  — same host, type qualifies — and a sample is in flight while the reader is deleted; not
  reproduced under the current design. `benchmarks/probe_teardown` is the stress for it; turning
  data-sharing off in a `<data_reader>` profile is the escape hatch.
- **A profile's `resource_limits` can oversize the data-sharing segment.** Fletcher does not know
  your memory budget, so it does not second-guess the number; see the 5000-sample note above.
- **One document per process.** Fast DDS profile names are process-wide (see
  [Refused at construction](#refused-at-construction)), so two differently configured providers in
  the same process express their differences as per-topic profiles inside **one** document rather
  than as two documents. Two corollaries. A document that loads and is *then* refused — anchor
  missing, a domain mismatch — has already been registered and is the process's document from then
  on; a later provider must bring the same bytes and is refused the same way. And an **empty**
  document is no exception: it is replaced by the provider's own default document (see [The
  published starting point](#the-published-starting-point)) before this rule ever sees it, so an
  empty document and a different non-empty document in one process collide and are refused exactly
  like any two different documents. A third corollary: the built-in document's
  `is_default_profile="true"` profiles are registered process-wide, so other Fast DDS entities
  created in the same process (outside Fletcher) inherit Fletcher's writer/reader defaults unless
  they pass explicit QoS.
- **The `__schema` channel bound is fixed, not configurable.** `kSchemaPayloadBytes`
  (`pubsub/include/fletcher/pubsub/payload_bound.hpp`) is 32 KiB — under Fast DDS's 65 500-byte
  default message size, so an announcement never fragments. A schema whose encoded size exceeds it
  is refused at `CreateTopic`, quoting both sizes. Usable size for the row is the bound minus 37
  bytes: the row length prefix, the attachment count, and the 29-byte `max_payload_bytes`
  attachment (was 8, before that attachment existed).
- **A subscription's reader pool is sized by its publisher's bound, not this provider's own.**
  `resource_limits().allocated_samples` times the announced bound is reserved when a topic's data
  reader is created; there is no subscriber-side ceiling, and an allocation failure there fails
  that topic's arrival with `kTransportFailure`. A provider that PUBLISHES a topic keeps its own
  bound on it: a remote publisher announcing a different bound for that topic is logged and never
  matches this provider's reader, and `CreateTopic` on a topic this provider already subscribes to
  at a different announced bound is refused `kInvalidArgument`; once that subscription is
  unsubscribed the topic is replaced at this instance's own bound. A second publisher announcing a
  different bound for a topic this provider has already resolved as a subscriber is logged the
  same way — like a differing schema — and never matches; the first announcement stands.
- **Fast DDS's own default profiles file always participates.**
  `DomainParticipantFactory::load_profiles` loads `FASTDDS_DEFAULT_PROFILES_FILE` /
  `DEFAULT_FASTDDS_PROFILES.xml` internally, at the top of every `create_participant*` call — this
  is eProsima's own mechanism, the one rmw_fastrtps relies on too, not a Fletcher extension — and
  its `fletcher_participant`, its `is_default_profile` writer/reader, and its per-topic profiles
  resolve exactly like the document's. Where both
  load, the default profiles file loads first, so a same-named profile there wins over the
  document's and Fast DDS logs `Error adding profile '<name>'` for the loser; the provider cannot
  tell the two apart.
- **`<library_settings>`, `<log>`, `<types>` and `<transport_descriptors>` apply once, process-wide,
  at the first provider's construction** — that is how eProsima designed `XMLParser::parseXML`, not
  a Fletcher restriction. Before this change they were re-applied on every endpoint creation, and a
  document with a `<transport_descriptor>` logged an "Error adding the transport" line per topic
  after the first.
- **Every document loses the name unless its anchor sets `<rtps><name>`, as the built-in one
  does**, because the anchor *is* the participant's QoS. This is universal rather than exotic,
  and it is diagnostic-only — nothing in the tree keys on that name. Set
  `<name>` in the anchor if you rely on it for tooling.
- **The `__schema` wire changed in this line:** the sample body now carries a row length and an
  attachment count (`[length][row_len][ipc bytes][attachment_count][attachments]`), where it used
  to be just `[length][ipc bytes]`. The announcement now also carries one attachment,
  `max_payload_bytes` — the publisher's payload bound, as a 4-byte little-endian `uint32_t` — so
  `attachment_count` is 1, not 0. Peers built before this change do not understand the new
  announcements — both in-tree providers changed together.

#### Two Fast DDS defaults worth knowing

"Anything unmentioned takes Fast DDS's default" means Fast DDS's, which is not always the DDS
specification's. Measured on `fast-dds/3.4.0`: a writer profile that omits `durability` resolves to
**TRANSIENT_LOCAL**, not the spec's VOLATILE — and it no longer coincides with Fletcher's built-in,
which is `VOLATILE` (owner decision 2026-09-15): a profile that leaves `durability` out gets a
*stronger* guarantee from the XML parser than Fletcher's own default gives. Reliability likewise
(`RELIABLE` for a writer). The policies where Fletcher and Fast DDS genuinely differ are now
`durability`, `history` and `resource_limits`, which is why the starting-point block spells all
three out.

The companion schema channel (`__schema` topic) always uses `RELIABLE` + `KEEP_LAST(depth=1)` +
`TRANSIENT_LOCAL`, data-sharing at Fast DDS's default. Its sample is carried as a Fletcher sample
(`SchemaBytesPubSubType`) — a row (the IPC bytes) plus the `max_payload_bytes` attachment — the
same plain, bounded type as the data channel. **No profile name is ever consulted for it**: it is
a Fletcher-internal implementation detail and not configurable. It is bounded at the fixed
`kSchemaPayloadBytes` (`pubsub/include/fletcher/pubsub/payload_bound.hpp`), not a property.

### Delivery guarantees

The provider upholds the `PubSubProvider::SubscribeCallback` contract:

- **Schema before data.** The subscription callback is never invoked with a null schema. Because `Subscribe` is non-blocking and may run before any publisher exists, a data sample can arrive before the topic schema does (the schema travels on the separate `__schema` channel). The data reader is created — and enabled — only once the schema and its bound are known, and the arrival resolved only after: by `Subscribe` itself, on the subscribing thread, when this provider already knows them, otherwise by the schema thread the moment the `__schema` sample arrives. So a resolved arrival means the reader already exists and is live; nothing published before it existed has reached it to hold, and the built-in `VOLATILE` durability keeps nothing from before either, so a burst published ahead of that point is simply not there to replay, and the callback only ever sees rows after the schema.
- **Per-writer order.** Samples from a single writer reach the callback in the order they were published. DDS delivers a single writer's samples to a `DataReader` in order under `RELIABLE` QoS; the provider preserves that order all the way to the callback — **including across the schema handoff**, where the writer's retained backlog is delivered before, and never interleaved with, samples that arrive live afterwards.

Both guarantees follow from when and how the data reader is created and enabled, rather than from an ordering layer in front of it. `Subscribe` creates the topic's `DataReader` — enabled — only once the schema and its bound are known: immediately, on the subscribing thread, if this provider already resolved them, otherwise from the schema thread the moment the `__schema` sample arrives. A reader that does not exist yet receives nothing, so there is no provider-side pre-schema buffer to keep in order. Once created, the reader is drained by Fast DDS's own listener dispatch (`on_data_available`, see [Constraints](#constraints)) — never by a thread this provider owns — so a writer's retained backlog and its live samples both reach the callback in the order Fast DDS handed them to that reader: DDS's own per-reader ordering, not a queue of Fletcher's.

**Schema handling.** One provider-owned thread waits on a `WaitSet` over every subscribed topic's `__schema` reader (an event-only wait: it wakes on a status change on a registered schema reader, or the stop guard); it decodes the schema and its bound, creates and enables the topic's data reader (`OpenDataReader`) — which `Subscribe` did not create while the schema was still unknown — and only then resolves the topic's `SchemaArrival`, so a resolved arrival means the reader already exists and is live (intraprocess: matched). Status callbacks for `__schema` endpoints run on that thread; a `SchemaArrival` resolves on it; a `FastDDSStatusListener` callback can run there too (intraprocess matching inside `create_datareader`), so its threading contract lists that thread as well (see [Statuses](#statuses)). One consequence: time-to-first-row for a remote late joiner grows by one discovery round — the round trip needed to notice the schema arrived.

**Why the listener and not a reader thread.** Verified in Fast DDS 3.4.0 (`rtps/writer/StatefulWriter.cpp`): a change a same-process reader refuses (`process_data_msg` returns false, history full) is marked `UNACKNOWLEDGED` in `deliver_sample_to_intraprocesses` and never retransmitted — `perform_nack_response` re-queues NACKed changes for `matched_remote_readers_` only, and the intraprocess path only ever sends `UNSENT` changes. So any asynchronous consumer that lags an in-process publisher by more than the reader's `max_samples` loses data; a reader thread is exactly such a consumer. A listener has no such lag: it consumes inside `process_data_msg` itself, so the reader's history cannot fill the way a lagging polling consumer's does — though a flat-out, no-pacing publisher can still leave a tail of samples undelivered (see [Known limits of the document](#known-limits-of-the-document)). Measured with `benchmarks/bench_e2e` (`after-e2e.txt` `## H`, 3 runs at each of two affinities): the listener path runs at 2.0-2.6 µs p50 / ~276-313k samples/s at 198 B; a per-topic reader thread (this round's rejected alternative, same file) ran only ~72-88k samples/s and stalled every two-core flat-out run at ~199k/200k samples.

## Usage

```cpp
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
using namespace fletcher;

// Defaults — Fletcher's profile on domain 0, 64 KiB payload bound for this provider's publishers.
auto provider = std::make_shared<FastDDSPubSubProvider>(ProviderConfig{});

// A DDS domain and nothing else: still Fletcher's built-in profile.
ProviderConfig config;
config.domain_id = 7;
auto custom = std::make_shared<FastDDSPubSubProvider>(config);

// A custom writer profile, with the history sized to the rows on the topic
// (see Zero-copy: the plain sample above). Note the profile restates
// durability and reliability: a supplied profile is the WHOLE QoS for its
// role, so anything it omits takes Fast DDS's default, not Fletcher's.
ProviderConfig custom_history;
custom_history.document = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant"/>
    <data_writer profile_name="default_writer" is_default_profile="true">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_LAST</kind><depth>10</depth></historyQos>
        <resourceLimitsQos>
          <max_samples>10</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>10</max_samples_per_instance>
          <allocated_samples>10</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_writer>
  </profiles>
</dds>)XML";
auto shared = std::make_shared<FastDDSPubSubProvider>(custom_history);
```

Or select it by name through the registry, which is how `fletcher-gateway` and any
configuration-driven application do it — the caller then names no provider type at all:

```cpp
fletcher::ProviderRegistry registry;
fletcher::RegisterFastDDSProvider(registry);   // registers "fastdds"

std::shared_ptr<fletcher::PubSubProvider> p =
    registry.Create(fletcher::ProviderSelector::Parse("fastdds"), config);
```

The provider is passed to `fletcher::Publisher` / `fletcher::Subscriber` or to generated `<Msg>Publisher` / `<Msg>Subscriber` classes:

```cpp
// Generated from a proto service definition (in fletcher_gen namespace):
using namespace fletcher_gen::integration;

TelemetryFeed_TelemetryStreamPublisher pub(provider);
pub.Publish(Telemetry().set_device_id(1).set_value(98.6));

TelemetryFeed_TelemetryStreamSubscriber sub(provider);
uint64_t sub_id = sub.Subscribe([](Telemetry msg, fletcher::Attachments att) {
    // Called on a Fast DDS internal listener thread.
});
```

Or used directly through the `PubSubProvider` interface:

```cpp
provider->CreateTopic({"my", "topic"}, schema);
provider->Publish({"my", "topic"}, encoded_row);
provider->Subscribe({"my", "topic"}, [](const uint8_t* data, size_t len,
                                          SharedSchema, Attachments) { ... });
provider->Unsubscribe({"my", "topic"});
```

### The schema without the data: a catalog

`SubscribeSchema` opens only the `__schema` side — one reader on `<topic>/__schema`, no data
reader, one retained sample — and returns the same `SchemaArrival` a `Subscribe` would. That is
what a catalog needs: a topic's shape without asking for one row of it.

```cpp
// On your own thread, never in the discovery callback (see below).
fletcher::SchemaArrival watch = provider->SubscribeSchema({"my", "topic"});

// On your tick. Zero timeout polls; kPending just means "not yet".
fletcher::SharedSchema schema;
switch (watch.Wait(std::chrono::milliseconds(0), &schema)) {
    case fletcher::PubSubStatus::kOk:
        // `fletcher::ImportArrowSchema(schema)` turns it into an `arrow::Schema` — that function
        // lives in the `fletcher-pubsub-arrow` package (`fletcher/pubsub_arrow/schema_import.hpp`),
        // not here: this provider carries no Arrow C++ dependency.
        break;
    case fletcher::PubSubStatus::kPending:
        break;  // no publisher has announced this topic yet
    case fletcher::PubSubStatus::kSubscriptionEnded:
        break;  // the watch was released; nothing will arrive on this arrival
    default:
        break;  // a transport fault — `watch.Message()` says what
}

provider->UnsubscribeSchema({"my", "topic"});
```

Topic names come from discovery: record the `topic` handed to
`FastDDSStatusListener::OnWriterDiscovered` and call `SubscribeSchema` **from your own thread**.
Never from the callback — an override must not call into any provider at all (the threading
contract on `FastDDSStatusListener`): the callback can be running on an application thread that is
already inside `Publish` or `Subscribe` with this provider's non-recursive mutex held, and
re-entering it deadlocks. Copy the name, return, and act on your next tick. Note what discovery
does and does not show: Fletcher's own `<topic>/__schema` endpoints are filtered out of
`OnWriterDiscovered`, and the data writer is created on a topic's first `Publish`, so a topic
that has been declared but never published to is not visible there.

`Unsubscribe` and `UnsubscribeSchema` release different things and neither substitutes for the
other. `Unsubscribe` releases the data subscription — its reader and listener — and leaves a watch
in place: the schema channel is kept, re-armed with a fresh `__schema` reader if the schema has
still not arrived, so the watcher's arrival keeps waiting rather than reporting
`kSubscriptionEnded`. `UnsubscribeSchema` releases the watch, and with no data subscription left
on the topic it takes the `__schema` reader down and ends a still-pending arrival with
`kSubscriptionEnded`; with a live data subscription sharing the channel it clears the watch only,
and those endpoints go with that subscription's `Unsubscribe`. Both are safe to call on a topic
that has neither.

### Per-topic QoS overrides

A per-topic override is a profile **named after the topic** — the `/`-joined topic string. It is
looked up first, and a topic with no profile of its own falls back to the document's
`is_default_profile="true"` writer/reader profile if one exists, else to Fast DDS's own default.

Remember the whole-QoS rule: each of these profiles is complete in itself, so each restates the
policies it wants rather than inheriting them from the default profile.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant"/>

    <!-- "telemetry/high-rate": shallow history, drop old samples. -->
    <data_writer profile_name="telemetry/high-rate">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_LAST</kind><depth>5</depth></historyQos>
      </topic>
    </data_writer>

    <!-- "config/snapshot": keep everything, durable for late subscribers — with the built-in
         default now VOLATILE, this profile is how a topic opts into replay at all. -->
    <data_writer profile_name="config/snapshot">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
      </topic>
    </data_writer>

    <!-- "ops/log": fire-and-forget, no reliability overhead. -->
    <data_writer profile_name="ops/log">
      <qos>
        <reliability><kind>BEST_EFFORT</kind></reliability>
      </qos>
    </data_writer>

    <!-- everything else on this instance -->
    <data_writer profile_name="default_writer" is_default_profile="true">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
    </data_writer>
  </profiles>
</dds>
```

`<data_reader>` profiles work the same way on the subscriber side. A profile whose name matches no
topic is silently inert — see [Known limits of the document](#known-limits-of-the-document).

### Constraints

- `CreateTopic` must be called before `Publish` on the publisher side. The conflict check is **per topic** (keyed by the topic name): re-declaring _the same topic_ with an identical schema is idempotent (so several publishers may share one topic), while re-declaring it with a _different_ schema throws (a conflict). Distinct topics are independent — two different topics may carry the **same** schema (identical schemas can describe different data); that is never a conflict. The topic's `DataWriter` is created inside `CreateTopic` itself, not on first `Publish` — first-publish latency moves to setup — so `Publish` on a topic this provider never `CreateTopic`ed, including one it only `Subscribe`d to, is refused `kTopicNotDeclared`. The writer's pool is reserved then and there: at the built-in defaults (`max_samples` 100, 64 KiB bound, `data_sharing` AUTOMATIC) that is roughly 6.6 MB of shared segment per declared topic, published to or not, so size `max_payload_bytes` and `resource_limits` for a many-topic deployment deliberately.
- On the subscriber side `Subscribe` can be called without a prior `CreateTopic` and is **non-blocking** — it never waits for a publisher. The schema arrives asynchronously over the `__schema` companion DDS topic; `Subscribe` returns a `SchemaArrival` whose `Wait` reports `kOk` with the schema once it is known (and `kSubscriptionEnded` if the subscription is torn down first); the data reader is not created until its schema and bound arrive, and is created (and enabled) before the arrival resolves, so nothing reaches it before it exists and the callback is never invoked with a null schema. (This is the subscriber-first contract — subscribe before any publisher exists.) Per-writer order is preserved across this handoff — see [Delivery guarantees](#delivery-guarantees).
- Only one subscription per topic per provider instance is supported (one `DataReader` per topic). Call `Unsubscribe` before re-subscribing. Multi-callback fan-out lives in `fletcher::Subscriber` one layer up.
- The subscription callback (`on_data_available`) runs on the thread that delivered the sample — a Fast DDS reception thread for a remote writer, the **publishing thread** for an intraprocess writer's live samples, or a Fast DDS flow-controller thread for a `TRANSIENT_LOCAL` replay — **while Fast DDS holds that reader's own RTPS mutex**: `StatefulReader::process_data_msg` takes it and still holds it through `change_received`, `NotifyChanges` and the listener call. Two consequences: shared state touched from the callback must be protected externally, and a slow or blocking callback stalls reception for that reader entirely, because nothing else can enter it. Hand work off to your own thread if it is not short.
- The callback can also run **on the thread that is still inside `Subscribe`, or on the schema thread**. `Subscribe` creates and enables the data reader (`OpenDataReader`) only once this provider knows the schema and its bound — immediately, on the subscribing thread, if it already does, otherwise later, on the schema thread, the moment the `__schema` sample arrives — and only then does the arrival resolve (see [Delivery guarantees](#delivery-guarantees)). Inside `Subscribe` that call runs under both the provider mutex and `schema_mu`; on the schema thread it runs under `schema_mu` alone. Creation pairs with an intraprocess writer synchronously, so a `TRANSIENT_LOCAL` topic that already has a publisher on this same participant replays its retained samples inline, on whichever thread created the reader — while that thread holds the locks above. A callback that re-enters the provider from either delivery would deadlock on the provider mutex — a `std::shared_mutex`, not recursive — but no longer reaches it: every seam method is refused at a door before any lock, from *any* delivery, first or later. The door prevents that deadlock; what it does not prevent is a stall — every other seam call on that instance waits for the whole chain to finish before the mutex is released. See the re-entrancy bullet below.
- **A callback must not throw, and if one does the exception goes nowhere** (spec §5.3, owner ruling 2026-09-05). It is absorbed at the dispatch site, inside `DeliveryChannel::Deliver`, which is `noexcept` — so it never unwinds into the Fast DDS thread (or the schema thread, for a replay driven from `create_datareader` there) holding the reader's RTPS mutex, and it is never reported to a publisher, which neither caused it nor can act on it. The count of absorbed failures is readable through `DeliveryChannel::AbsorbedCount()`; there is no log line, because containing an exception and losing it are otherwise indistinguishable and this layer has no logger of its own. The `Drain`-wide catch still stands, but for the provider's own failures (a bad allocation, a malformed envelope), not for the callback's.
- **From inside a delivery, EVERY SEAM METHOD is refused** (spec §6 clause 6). The four data-path methods `CreateTopic`, `Publish`, `Subscribe` and `Unsubscribe`, and the two schema-only ones `SubscribeSchema` and `UnsubscribeSchema`, issued on this same instance and this same thread, throw `PubSubError(kReentrantCall)` before any lock. This provider is why the rule is uniform: an earlier ruling would have kept the first three permitted on the claim that they already worked here, and **they do not**. A subscription callback runs with the reader's own RTPS mutex held; `Unsubscribe` is the clearest case — `delete_datareader` waits for any in-flight listener callback on that reader to return, so called from inside that reader's own callback it would be waiting on itself. Another **thread** calling during a delivery is not re-entrancy and is still served.
- A `FastDDSStatusListener` callback is bound by the same rules and one more: it can run on an application thread inside `CreateTopic`, `Publish` or `Subscribe` of *any* provider in this process, with that provider's mutex held, or **on the schema thread** — both when it opens a data reader on schema arrival (`OpenDataReader` runs intraprocess matching synchronously there, under `schema_mu`) and on every wake that carries a `__schema` reader status — so it must not call into a provider, must not block, and must not throw (every method is `noexcept`). See [Statuses](#statuses) for the full contract and for the lifetime rule that follows from it.
- `FastDDSPubSubProvider` is non-copyable and non-movable (DDS entities cannot be transferred).
- The callback's `data` pointer is a loaned DDS payload: it is valid for the duration of the callback only. Copy anything you keep. (This was always the contract — the pointer was never owned by the callback — but loans are where holding on to it actually breaks.)

### Measured decisions

Numbers that justify a shape in the code, kept here rather than in the comment that would otherwise
carry them — they date, and a stale figure in a source comment reads as current. Harnesses:
`benchmarks/bench_pub_sub_type.cpp` here, and `tools/fletcher_bench` in the consuming repo.

| Decision | Where | Measured |
|---|---|---|
| `Publish` takes the provider mutex **shared**, not exclusive | `src/fast_dds_pubsub_provider.cpp` | 16 threads on 16 topics: p99 **2.7 µs → 711 µs** when the exclusive lock spanned `DataWriter::write`. `DataWriter::write` is itself thread safe, so shared is enough to keep the topic and writer alive. |
| `PublishData` and `ReceivedData` are separate structs | `src/internal/transport_data.hpp` | Bundled, every serialised publish built and destroyed an `Attachments` the publish path never reads: **52 ns of the 137 ns** the publish spends outside Fast DDS. **That cost is gone as measured, and the split is kept for the reason rather than the number.** It was MSVC's `unordered_map` allocating a sentinel node in its default constructor; PDA-DEC-AG2 retired that alias for a sealed container over a `std::vector`, whose default constructor allocates nothing. `BM_AttachmentsConstruct` measured **50.2 ns** at the commit before this change and **0.616 ns** after, on the same machine in the same session (3 and 5 repetitions, cv 1.4 % and 3.0 %) — which is where the 52 ns above came from and where it went. |
| The sample is an offset and a size, not a struct templated on its bound | `src/internal/fletcher_sample.hpp` | The struct made the bound a compile-time constant, which forced a *compiled set* of bounds and gave the rule its floor, ceiling and power-of-two shape. It cost nothing to drop, because the bound never reached the encode loop as a constant either way: `EncodeEnvelopeBody` takes a `WriteBuffer&`, whose bound is `capacity_`, a runtime member. Measured against `BM_PublishFlow_LoanedStruct`, the baseline arm kept for exactly this (3 repetitions, mean wall): **14.7 ns against 16.2 ns** at 214 B, **31.9 against 32.4** at 4 KiB, **103 against 111** at 16 KiB, **971 against 976** at 60 KB — equal within a 2–9 % coefficient of variation, and not slower. The loaned read is unchanged at **2.05 ns**. |
| `serialize()` writes the 8 framing bytes directly instead of through a `Cdr` | `src/internal/fletcher_sample_pub_sub_type.hpp` | Byte-identical output for **13.5 ns** against **52.9 ns** (fastcdr placing the encapsulation with the length reserved and patched) and **49.5 ns** (fastcdr serialising the `sequence<octet>` outright), measured over 400 000 calls on a 222-byte envelope. Against a 14.6 ns loaned publish that is ~3.5×. Held by `FletcherSamplePubSubTypeTest.FastCdrReproducesTheBytesExactly` (in the unit suite, so CI runs it) and by `bench_pub_sub_type`'s fastcdr arm. |
| `OrderedDelivery` publishes queue emptiness in an atomic instead of locking to look | `src/internal/ordered_delivery.hpp` | The steady path has to know whether anything was queued while its callback ran — by the callback re-offering, or by another thread whose own drain bailed because this delivery was in progress. Taking `mu_` to find out cost **19.7 ns → 6.96 ns** per delivered sample once the answer became an acquire load (`BM_Deliver_OfferView`, 3 repetitions, cv < 1 %; `BM_Deliver_Offer` 19.6 → 7.17 ns), against a bare callback of 1.30 ns. `queue_` itself is still never touched without the lock; only the one bit is published, by `NoteQueuedLocked` under it. An earlier revision read `queue_.empty()` unlocked, which was the same speed and a data race. **Superseded 2026-09-14:** the layer is gone — data delivery is a Fast DDS listener again (no queue, a direct call); only schema readers are served off one provider-wide WaitSet thread, and each topic's data reader is created and enabled once its schema and bound arrive. Gate: `benchmarks/bench_e2e` BEFORE/AFTER, see below. |
| `Publisher::CreateTopic` encodes the schema **before** taking the lock | `../pubsub/src/publisher.cpp` | The locked section becomes a byte compare. First declaration **1.4 → 2.8 µs** (it now encodes where it used to deep-copy); re-declaration **4.2 → 2.5 µs**, and concurrent callers no longer queue behind ~3.5 µs of IPC work each. |
| `PublishData` holds the encoder and attachments by pointer | `src/internal/transport_data.hpp` | The provider layer costs **~80 ns** over a raw `DataWriter::write` of the same bytes, and the encoder and topic-name changes took **15–22 ns** off that (`tools/fletcher_bench/bench_publish` in the consuming repo, 16 interleaved A/B runs). |
| `SubscribeCallback` takes `schema` and `attachments` by **const reference** | `../pubsub/include/fletcher/pubsub/provider.hpp` | By value it was **~110 ns per delivered sample against 1.4 ns for the call itself** — an empty `Attachments` was an `unordered_map`, and MSVC allocates a sentinel node in its default constructor, so every delivery built and destroyed one whether the sample carried attachments or not. **The allocation is gone** (PDA-DEC-AG2 retired the alias; see the row above), so this number is historical. By-reference is kept regardless: by value still copies every `Blob` and its control block for a set that carries any. |
| The delivery layer latches into a lock-free path once the schema handoff is done | `src/internal/ordered_delivery.hpp` | `OrderedDelivery` exists for the subscriber-first startup window and used to charge for it forever. With that plus the listener reusing one `Attachments` — which after PDA-DEC-AG2 depends on `Attachments::Clear()`, the one member that empties the container while keeping its capacity — delivery went from **199 ns to 3.2 ns** per loaned sample and **398 ns to 3.5 ns** per copied one (`benchmarks/bench_pub_sub_type`, `BM_Deliver_*`). **Superseded 2026-09-14:** the layer is gone — data delivery is a Fast DDS listener again (no queue, a direct call); only schema readers are served off one provider-wide WaitSet thread, and each topic's data reader is created and enabled once its schema and bound arrive. Gate: `benchmarks/bench_e2e` BEFORE/AFTER, see below. |
| Schema readers on one provider WaitSet thread; data readers created and enabled on schema arrival; data delivery stays on the listener | `src/fast_dds_pubsub_provider.cpp`, `src/internal/data_reader_listener.hpp` | Measured 2026-09-15 (`bench_e2e`, `after-e2e.txt` `## H`, 3 runs each at 0x4 and 0xC on a clean machine): latency and every throughput arm that completed are within noise of BEFORE — 198 B p50 2.0-2.6 µs / ~276-313k samples/s, 60000 B p50 4.7-7.4 µs / ~140-165k samples/s, against BEFORE's ~303-342k samples/s at 198 B. Does **not** reproduce the per-topic-reader-thread round's regression (delivery frozen forever at exactly `max_samples` = 100, 0 writer failures). An occasional late-flat-out STALL (3 of 6 runs, 193k-199k/200k arrived) persists, but `after-e2e.txt`'s own `## affinity=0xC` header records the identical stall on the untouched pre-branch binary ("before_2 stalled 173+s") under the same harness before this modernization branch existed — a pre-existing characteristic of the no-pacing throughput arm under RELIABLE + KEEP_ALL + data-sharing AUTO on CPU-pinned cores, not introduced by this round. |

> **Read these as differences, not absolutes,** and take each from the same run as its control —
> `BM_Memcpy` for the publish rows, `BM_Deliver_CallbackOnly` for the delivery ones. The machine
> drifts 2–3% between two runs of identical code, which is larger than several of these numbers.
>
> The DDS-level harnesses cannot resolve any of it: `bench_e2e` reports `write_p50` quantised to
> 0.1 µs with ~0.2 µs of run-to-run drift, and `bench_contention` quantises p50 the same way. Three
> changes argued from "obviously less work" reasoning were measured afterwards and turned out to be
> regressions — what they cost and how they were caught is in the consuming repo's
> `modules/io/docs/fastdds-provider-review.md`.

## Building the package locally

### Windows (MSVC)

**Prerequisites:** Visual Studio 2022 with C++ workload, CMake, Python, Conan 2.

Conan profiles live in [`../.conan-profiles/`](../.conan-profiles/) in the repo and are referenced by relative path — no separate profile-install step is needed.

Build and package (Release, no tests):

```bat
conan create . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Build, run tests, and package:

```bat
conan create . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

The built package lands in the local Conan cache (`%USERPROFILE%\.conan2`).

To iterate without the full `conan create` cycle use `conan build` against the source tree:

```bat
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

To run the tests separately with CTest after a `conan build` (Visual Studio is a multi-config generator so the config must be specified):

```bat
ctest --test-dir build -C Debug --output-on-failure
```

Add `-V` for full GTest output:

```bat
ctest --test-dir build -C Debug --output-on-failure -V
```

### Benchmarks

[`benchmarks/`](benchmarks/) holds `bench_pub_sub_type`, which measures the DDS types against the one they replaced. It is outside this recipe's `exports_sources` and builds on its own — see that directory's README.

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory.

If the `build/` folder contains stale artifacts from a previous Windows build, remove it first — `DartConfiguration.tcl` bakes in absolute paths at configure time and will cause CTest to fail when those paths don't match the current platform:

```bash
rm -rf build/
```

Build and run tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Build, package, and run tests (equivalent to CI):

```bash
conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Run tests separately with CTest after a `conan build` (the Linux build lives under `build/<BuildType>`):

```bash
ctest --test-dir build/Debug --output-on-failure
```

Add `-V` for full GTest output:

```bash
ctest --test-dir build/Debug --output-on-failure -V
```

## Consuming the package

### 1. Add to your conanfile.py

```python
def requirements(self):
    self.requires("fletcher-fastdds-pubsub-provider/0.5.1-alpha")
```

Install dependencies:

```bash
conan install . --build=missing -pr:a=<your-profile>
```

### 2. Wire up CMake

```cmake
find_package(fletcher-fastdds-pubsub-provider REQUIRED)

# Fully qualified target name:
target_link_libraries(my_app PRIVATE
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)

# Or the convenience alias injected by the package's build module:
target_link_libraries(my_app PRIVATE fletcher::fastdds-pubsub-provider)
```

The `fastdds` dependency is **linked PRIVATE and its headers are not exported**: the public
header names no eProsima type, so nothing you write to configure this provider needs a Fast DDS
header. That is enforced rather than merely intended — `test_package` compiles with **no Fast DDS
include directories at all**, so any eProsima type creeping back into the installed header is a
compile error there.

The library is STATIC, so consumers still *link* the Fast DDS chain (`transitive_libs` is kept);
they just do not *see* it. If your own code uses Fast DDS directly for its own reasons, require
`fast-dds/3.4.0` explicitly — as `benchmarks/conanfile.py` does.

## CI pipeline

The build workflow is defined in `.github/workflows/ci.fastdds-pubsub-provider.yml`.
It is `workflow_call`-only — invoked from `ci.pr.yml` for pull requests
touching `fastdds-pubsub-provider/**` and from `cd.fastdds-pubsub-provider.yml`
on `fastdds-pubsub-provider-v*` tag pushes. The matching upload job
lives in `cd.fastdds-pubsub-provider.yml`, not here.

```
ci.pr.yml (PRs) / cd.fastdds-pubsub-provider.yml (tag push)
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
windows-2022                             ubuntu-latest
Native runner                            Docker container (.devcontainer)
Profile: Windows-msvc194-                Profile: Linux-gcc13-
         x86_64-Release                            x86_64-Release
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼ (only on tag push)
                        upload
              (cd.fastdds-pubsub-provider.yml job)
              Creates GitHub Release with
              fletcher-fastdds-pubsub-provider-{windows,linux}-conan-package.tgz
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-2022` | `.conan-profiles/Windows-msvc194-x86_64-Release` | Release |
| `build-linux` | `ubuntu-latest` (Docker) | `.conan-profiles/Linux-gcc13-x86_64-Release` | Release |

Both jobs build with `-o "&:run_tests=True"` so the full GTest suite runs as part of every CI build.

### Package handoff

Both platforms produce a separate binary package. Each build job saves
its package to a GitHub Actions workflow artifact; on a tag push the
`upload` job in `cd.fastdds-pubsub-provider.yml` downloads both and
attaches them as GitHub Release assets:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

The `upload` job only runs from `cd.fastdds-pubsub-provider.yml`
(tag push), and verifies that the tag version matches the version in
`conanfile.py` before creating the release.

## Runtime requirements

The Fast DDS runtime (discovery server or default multicast discovery) must be reachable at the configured domain ID. On a single machine with no network configuration, the default multicast discovery works out of the box. For multi-host deployments, configure Fast DDS via its XML profile mechanism or a discovery server — see the [Fast DDS documentation](https://fast-dds.docs.eprosima.com/).
