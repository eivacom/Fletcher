# FastDDSPubSubProvider

Implements `fletcher::PubSubProvider` using [eProsima Fast DDS](https://fast-dds.docs.eprosima.com/) (RTPS). Transports `EncodedRow` byte buffers over a DDS domain with reliability settings tuned to minimise message loss.

> **Targets Fast DDS 3.4.x** (`fast-dds/3.4.0` from Conan Center). The provider's public API names **no eProsima type at all**: it is configured by `fletcher::ProviderConfig` plus a Fast DDS XML profiles document, so the Fast DDS headers are *not* exposed transitively and a consumer never needs them (see [QoS configuration](#qos-configuration) and [Consuming the package](#consuming-the-package)). The XML you write is Fast DDS 3.4's own profiles grammar, and v3 consolidated the legacy `eprosima::fastrtps` namespace into `eprosima::fastdds`, which matters if you paste profiles out of a 2.x application.

## How it works

A single `FastDDSPubSubProvider` instance manages one DDS `DomainParticipant`, one `Publisher`, and one `Subscriber`. Topics are created on demand via `CreateTopic`. DataWriters and DataReaders are created lazily on the first call to `Publish` and `Subscribe` respectively.

The binary payload sent over the DDS bus is a raw `EncodedRow` (the positional wire format produced by generated code or `Codec::EncodeRow`), wrapped in a minimal CDR-LE framing: a 4-byte encapsulation header followed by a 4-byte length prefix. Custom `TopicDataType`s handle the CDR serialisation without requiring IDL generation as a build step, named as `fastddsgen` would have named them: `FletcherSamplePubSubType` over the sample layout, plus `RawBytesPubSubType` for the companion schema channel.

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

Nothing is ever rounded: a bound that cannot frame a sample is a mistake, and one that only exists at run time — read from config, say — is refused by the constructor instead. The bound rides in the registered type name (`fletcher_65536` at the default), so two providers on different bounds fail to match at discovery rather than exchanging samples one side cannot hold. That type name is also the *only* thing keeping two bounds apart, which is what makes the bound safe to be a runtime option at all.

> An earlier revision made the sample a struct templated on its size, which forced the runtime option to be matched against a *compiled set* of bounds — powers of two from 4 KiB to 8 MiB, walked at compile time by doubling. Powers of two were never an alignment rule; they were what made the walk short enough to compile (stepping by 4 would need two million instantiations), and the floor and ceiling were where the walk started and stopped. Fast DDS never needed the compile-time size: `DataWriterImpl::loan_sample` reads the runtime `max_serialized_type_size`, and a loaned collection only casts the payload pointers it is handed. What the struct's `static_assert`s proved reduces to `bytes % 4 == 0`.

Each flow is its own class over a shared base, mirroring how DDS itself splits the calls — `SampleWriter` /
`LoanableSampleWriter` on the publish side (`internal/sample_writer.hpp`), `DataReaderListener` /
`LoanableDataReaderListener` on the subscribe side (`internal/data_reader_listener.hpp`):

| | Publish — `SampleWriterBase` | Subscribe — `DataReaderListenerBase` |
|---|---|---|
| **Loanable** | `LoanableSampleWriter`: `loan_sample` → fill the length and the body → `write(sample)`. No `serialize()` runs at all; with shared memory the payload being filled *is* the one the reader reads. | `LoanableDataReaderListener`: `take(LoanableSequence&, SampleInfoSeq&)` → read the two fields in place → `return_loan`. |
| **Plain** | `SampleWriter`: `write(&PublishData)` runs `serialize()`, writing the same layout **truncated after `length`** — so a small row stays small on the wire. | `DataReaderListener`: `take_next_sample(&ReceivedData, &SampleInfo)` — Fast DDS deserialises, which reads `length` out of the payload and needs nothing beyond the bytes that arrived. |
| **Chosen by** | the `fletcher.loan_publish` document property — a preference. | `internal::CanLoanSamples(qos)` — a **precondition**, not a preference: reading a whole sample in place needs whole payload nodes, which only a `PREALLOCATED*` history memory policy guarantees. Under `DYNAMIC_RESERVE`/`DYNAMIC_REUSABLE` the pool sizes each node to what arrived, so a truncated sample leaves a node shorter than a whole one and `length` would steer reads past its end. |

Neither side is negotiated. `loan_sample` is gated by the *writer's* own type (`DataWriterImpl::loan_sample`) and the reader's loans by the *reader's* own type (`DataReaderImpl::enable`, where `is_plain` is computed from the type alone), so all four pairings interoperate — upstream regression-tests exactly that in `test/dds/communication/mix_zero_copy_communication.json`, and it branches on `zero_copy_` between exactly these two reader calls in `test/dds/communication/SubscriberModule.cpp`.

**Data-sharing** is left at `DataSharingQosPolicy`'s default `AUTOMATIC` on the **writer**. Fast DDS engages shared memory when both endpoints are on one host and the type qualifies, and falls back to the transport when they are not. Upstream's own zero-copy test profile (`simple_reliable_zerocopy_profile.xml`) uses `AUTOMATIC` for the same reason.

The default **reader** QoS sets `data_sharing().off()`, which is a deliberate exception and the one place this provider overrides the policy. With data-sharing on both ends, a reader that subscribes *after* rows were published intermittently receives only part of the `TRANSIENT_LOCAL` backlog — frequently just the newest sample — and reports no error at either end. Measured cross-process on Windows against Fast DDS 3.4.0, three runs each:

| writer / reader | result |
|---|---|
| `AUTOMATIC` / `AUTOMATIC` | 4/4 pass, then 2/4, then 2/4 — one of three rows delivered, or none |
| `AUTOMATIC` / `off()` | 4/4 pass, 3 runs (**what ships**) |
| `off()` / `off()` | 4/4 pass, 3 runs |
| `AUTOMATIC` / `AUTOMATIC`, `max_samples` 8 rather than 100 | 4/4 pass, 3 runs |

The provider is not the one dropping them: `OrderedDelivery`'s pre-schema trim warns when it discards and never fired. That a 0.5 MB pool is reliable where a 6.6 MB one is not points below the provider as well.

Two things follow. First, the cost is **zero-copy receive**, by default — the publish path, including `loan_publish`, is untouched, and the type stays bounded and plain, so a caller who wants receive-side data-sharing can set `data_sharing().automatic()` on their own reader QoS. Second, this is invisible to the provider's own test suite, which is single-process: Fast DDS serves those tests over intra-process delivery, which bypasses data-sharing altogether. `integration-tests/gateway-fastdds-ts` is the only cross-process coverage, and it is where this was found. Re-enabling the default wants an upstream answer first.

What this costs:

- **Memory.** A bounded type puts payload pools in `PREALLOCATED`, so every history slot reserves the whole sample — `resource_limits().allocated_samples` slots up front, growing to `max_samples`, per endpoint. A data-sharing writer allocates `(max_samples + extra_samples) * (bound + 8)` bytes of shared segment immediately. Size `max_payload_bytes` and the resource limits to the rows the topics actually carry; nothing in the provider caps their product, and if it does not fit a data-sharing segment (a 32-bit size) Fast DDS declines data-sharing and uses the transport. The defaults are 64 KiB against `max_samples = 100`, so **~6.5 MB per endpoint per topic** — 300x a typical 214-byte row. Lower `max_payload_bytes` for a deployment whose rows are small, but lower it on **every** endpoint that talks to those topics: the type name carries the bound, so a one-sided change stops discovery instead of saving memory. Dropping the default to 8 KiB was measured and reverted — it costs the subscriber-first burst its accidental headroom (see below).
- **Burst headroom is `max_samples`, not the bound.** A subscriber that joins first buffers at most its own `max_samples` samples until the schema arrives — `internal::OrderedDelivery` drops the oldest past that, as `KEEP_LAST` would have done to the same samples — and the writer's history is the same 100 deep. A 1000-sample burst published straight after `CreateTopic` therefore delivers on the order of 100 unless both ends are given room for it. Raise `resource_limits().max_samples` at both ends, or pace the publisher.
- **Wire size under `loan_publish`.** Fast DDS stamps a loaned payload `length = max_serialized_type_size` and nothing recomputes it, so every sample crosses the wire at the full bound whatever the row weighs. And it buys little: measured publish-side (`bench_dds_payload`, p50 of 2x4000 samples) it saved a **fixed ~0.1-0.2 us**, not a per-byte cost — 1.05 -> 0.95 us at a 198-byte row and 2.15 -> 2.00 us at 60 KB. Both paths write the row bytes exactly once, so loaning removes no copy: it removes the encapsulation and the length field, and the `PublishData` the serialising path hands to `write()`. Those are now **14.6 ns against 29.3 ns** at a 198-byte row (`bench_pub_sub_type`), so the publish-side case for it is weaker than those DDS-level numbers, which predate that work. Off by default.
- **Oversized rows throw** under `loan_publish`: a row plus attachments past `max_payload_bytes` raises `std::overflow_error` out of `Publish`. Without it the overflow is reported inside `serialize()` instead; either way the sample is dropped.

What the **read** side is worth, which is where the plain type actually pays: `bench_read_flow`
publishes flat out and meters the receive path, medians of 3 runs x 8000 samples.

| row bytes | `LoanableDataReaderListener` | `DataReaderListener` | throughput gain |
|---|---|---|---|
| 198 | 8.07 µs/sample, 24.5 MB/s | 8.26 µs/sample, 24.0 MB/s | +2% |
| 4 294 | 7.99 µs, 538 MB/s | 8.66 µs, 496 MB/s | +8% |
| 16 582 | 8.04 µs, 2 062 MB/s | 10.15 µs, 1 634 MB/s | +26% |
| 60 198 | 7.93 µs, 7 587 MB/s | 11.43 µs, 5 269 MB/s | **+44%** |

The loaned path's per-sample cost is **flat in payload size** — it never copies — while the
deserialising path grows linearly at roughly 58 ns/KiB, the cost of allocating a fresh `std::vector`
per sample and memcpy-ing `length` bytes into it. That is the whole reason the read flow is taken
whenever its precondition holds rather than offered as an option.

The one deviation from a generated plain type: `serialize()` emits the used prefix rather than the whole sample, and `deserialize()` reads `length` bytes rather than memcpy-ing the lot. Fast DDS never reads past `payload.length`, so this is safe, and it is what keeps the non-loaned path from putting the whole bound on the wire per sample.

**Where fastcdr is used and where it is not.** The rule is that fastcdr does the work wherever it is
free, and the framing is hand-written only on the path where it is not:

- The representation id is `EncodingAlgorithmFlag | Cdr::LITTLE_ENDIANNESS` and the padding octet is
  `Cdr::alignment(length, 4)` — both fastcdr, both `constexpr`, both free.
- The `__schema` channel builds a real `FastBuffer` and `Cdr` and serialises a `sequence<octet>`
  outright. It writes once per topic, so the ~36 ns costs nothing there.
- Only the data channel's eight framing bytes are placed by hand, and a unit test pins them to what
  fastcdr produces (see the measured decisions table below).

The two channels are therefore the **same shape on the wire** — a CDR `sequence<octet>`, because a
sample's length sits at exactly the offset and width of the sequence length field. They differ only
in memory: a length followed by raw bytes, which can be plain and loaned, against a `std::vector`,
which cannot. `TheSchemaChannelFramesTheSameShapeAsTheDataChannel` asserts the byte equality, since
the two are framed by different code.

That deviation has a limit worth stating plainly: **the plain claim holds between Fletcher endpoints, not against a `fastddsgen`-generated peer.** A generated type support for the equivalent IDL reads `length` and then a fixed `octet[N]`, so a serialised Fletcher sample — which stops after the bytes in use — is short by however much of the bound the row did not need, and its `Cdr` refuses it. Fletcher's own reader is unaffected because it bounds itself by `length` on the copying path and by `internal::CanLoanSamples` on the loaned one. A peer that has to read these samples has to read them the same way, which in practice means through this provider (or the XRCE one, which forwards the bytes opaquely).

### Statuses

Endpoints are created with the status mask their listener implements rather than the default `StatusMask::all()` (`internal/data_writer_listener.hpp`).

**`internal::DataWriterListener` overrides every callback `DataWriterListener` declares**, so nothing a DataWriter can report is left on the default no-op:

| Status | Level | Why it matters |
|---|---|---|
| `offered_incompatible_qos` | error | The endpoints never match, so the only symptom is a subscriber that stays unconnected with nothing logged. Reachable whenever writer and reader QoS are configured independently. |
| `publication_matched` | warning on loss, info on gain | A writer with no readers left keeps accepting publishes and delivers them nowhere. |
| `offered_deadline_missed` | warning | Only fires on a writer an operator gave a `DEADLINE` to — Fletcher sets none. |
| `liveliness_lost` | warning | Readers have marked the writer NOT_ALIVE. Fletcher leaves `LIVELINESS` at `AUTOMATIC` with an infinite lease, where it cannot fire, so this too reports a configured policy. |
| `on_unacknowledged_sample_removed` | warning | Under `KEEP_ALL` + `RELIABLE`, history overflowed past `max_blocking_time` — loss rather than backpressure. No `StatusMask` bit; Fast DDS dispatches it whenever a listener is set at all. |

The reader side, on `DataReaderListenerBase`, mirrors all of it: `requested_incompatible_qos` (error), `subscription_matched`, `requested_deadline_missed`, `liveliness_changed`, `sample_lost` and `sample_rejected`. `ReaderStatusMask()` subscribes to exactly those plus `data_available`.

**`EPROSIMA_LOG_INFO` compiles to nothing** unless the build defines `FASTDDS_ENFORCE_LOG_INFO` (`Log.hpp:355-359`), so the routine half of `publication_matched` needs that switch to appear. Warnings and errors are always compiled in.

### Topic name

The `std::vector<std::string>` topic segments from `PubSubProvider` are joined with `/` to form the DDS topic name. For example, segments `{"integration", "TelemetryFeed", "TelemetryStream"}` become the DDS topic `"integration/TelemetryFeed/TelemetryStream"`.

### QoS configuration

QoS is configured up-front, at construction, by **a Fast DDS XML profiles document handed to the
provider as text** in `fletcher::ProviderConfig::document`. There are no runtime setters and no
typed C++ QoS API — one way to do it. Fletcher itself gains no parser and this provider gains no
dependency: the document is passed straight to Fast DDS's own
`get_participant_extended_qos_from_xml` / `get_datawriter_qos_from_xml` /
`get_datareader_qos_from_xml`, which parse a *string* and return a QoS.

`ProviderConfig` carries exactly three things:

| Field | Meaning |
|---|---|
| `domain_id` | The DDS domain. Used exactly as given, and it always wins over the document. |
| `max_payload_bytes` | The row payload ceiling. **0 means unset** and resolves to 65536. The bound is part of the registered DDS type name, so two endpoints on different bounds do not discover each other *at all*. |
| `document` | The XML profiles document, as **text**. Empty means "Fletcher's built-in profile everywhere" — exactly what every caller got before this existed. |

The setting holds the XML itself, never a filename: Fletcher never opens a file on a provider's
behalf. If you want the convenience of a file, `fletcher-gateway` has `--provider-config FILE`,
which reads the bytes and hands them over unexamined.

#### Reserved profile names

| Role | Profile looked up, in order | Falls back to |
|---|---|---|
| participant | `fletcher_participant` — **mandatory in a non-empty document** | Fast DDS's default, named `FletcherParticipant` |
| data writer on topic `T` | `T` (the `/`-joined topic name), then `fletcher_writer` | Fletcher's built-in writer profile |
| data reader on topic `T` | `T`, then `fletcher_reader` | Fletcher's built-in reader profile |
| the internal `__schema` channel | *nothing, ever* | its own fixed QoS |

A **per-topic override** is therefore just a profile named after the topic.

#### A supplied profile is that endpoint's WHOLE quality-of-service

There is no merge and no floor. Anything a profile you supply leaves out takes **Fast DDS's**
default, not Fletcher's. This is not a convenience trade — the XML API returns a filled QoS and
cannot report which policies the document actually mentioned, so an overlay rule would rest on a
fact the substrate does not expose, and could be neither implemented reliably nor tested honestly.

One rule instead: **supply a profile for a role, and you own that role's QoS.** Start from the
block below rather than from a bare profile.

#### Fletcher's default QoS profile

With an empty document, both the data DataWriter and the data DataReader get this profile:

| Policy | Setting | Reason |
|---|---|---|
| `reliability` | `RELIABLE_RELIABILITY_QOS` | The middleware retransmits unacknowledged samples; no silent drops. |
| `history` | `KEEP_ALL_HISTORY_QOS` | All samples are retained until every matched reader has acknowledged them. With `RELIABLE`, the writer blocks (rather than dropping) when the history is full. |
| `durability` | `TRANSIENT_LOCAL_DURABILITY_QOS` | Samples published before a subscriber joins are replayed to that subscriber on discovery, so no data is lost during startup races. |
| `resource_limits` | `max_samples` 100, `max_instances` 1, `max_samples_per_instance` 100 | The sample type is bounded and plain, so every endpoint reserves the whole payload bound per history slot. At Fast DDS's default 5000 that is gigabytes, which overflows the data-sharing segment's 32-bit size and silently drops the endpoint back to the transport. |
| `data_sharing` (reader) | `OFF` | Receive-side data-sharing intermittently drops part of the `TRANSIENT_LOCAL` backlog to a late-joining reader, with no error anywhere. |

The first three together implement "at-least-once" delivery within a single DDS domain.

#### The published starting point

This is the **exact** XML transcription of the two profiles above. Copy it, change what you need,
and the policies you leave alone stay where Fletcher put them. It is kept true setting-for-setting
by `FastDdsConfig.DefaultProfileTranscriptionIsExact`, which compares the parsed result against
`MakeFletcherDefault{Writer,Reader}Qos()` **whole-struct** — so this block cannot drift from the
code without a test going red.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant"/>
    <data_writer profile_name="fletcher_writer">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
        </resourceLimitsQos>
      </topic>
    </data_writer>
    <data_reader profile_name="fletcher_reader">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
        <data_sharing><kind>OFF</kind></data_sharing>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
        </resourceLimitsQos>
      </topic>
    </data_reader>
  </profiles>
</dds>
```

#### The two settings a QoS profile cannot express

`loan_publish` (which publish path) and `max_schema_bytes` (the bound on the internal schema
channel) are Fletcher's, not DDS's. Neither is a QoS policy, and a second document format for them
would break "one way to do it", so they ride as **vendor properties inside the anchor's
`<rtps><propertiesPolicy>`** — native Fast DDS XML, parsed by Fast DDS:

```xml
<participant profile_name="fletcher_participant">
  <rtps>
    <propertiesPolicy>
      <properties>
        <property><name>fletcher.loan_publish</name><value>true</value></property>
        <property><name>fletcher.max_schema_bytes</name><value>131072</value></property>
      </properties>
    </propertiesPolicy>
  </rtps>
</participant>
```

Both are provider-wide switches, which is what a participant profile is for. A `fletcher.`-prefixed
property that is not one of these two, or whose value does not parse, is **refused** — a typo'd
`fletcher.loanpublish` must not be inert.

The two the provider consumes are **stripped before `create_participant`**, so a
`<propagate>true</propagate>` on one cannot put a Fletcher key into DDS participant discovery data.
Every other property reaches Fast DDS untouched: security plugins (`dds.sec.*`) need that.

#### Refused at construction

All of these throw `PubSubError(kInvalidArgument)` **before** the DomainParticipant exists, so a
misconfigured provider never exists at all:

- a non-empty document Fast DDS cannot parse, or that does not define
  `<participant profile_name="fletcher_participant">`. The anchor is mandatory even when it carries
  no policies: Fast DDS reports "malformed XML" and "no such profile" with the *same* return code,
  so without one profile a document must define, a broken document — or an XRCE `key=value`
  document pasted into the wrong field — would resolve to "no profiles found" and run happily on
  the defaults;
- an unknown or unparseable `fletcher.*` property, quoting it;
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
- **A supplied reader profile can re-enable receive-side data-sharing,** which Fletcher's built-in
  turns off. That is deliberate: a Fletcher floor underneath your profile would mean the document
  does not really configure QoS. Be aware that data-sharing on the receive side has a measured
  defect — part of the `TRANSIENT_LOCAL` backlog can be dropped to a late-joining reader with no
  error anywhere.
- **A profile's `resource_limits` can oversize the data-sharing segment.** Fletcher does not know
  your memory budget, so it does not second-guess the number; see the 5000-sample note above.
- **Every non-empty document loses the `FletcherParticipant` participant name** unless its anchor
  sets one, because the anchor *is* the participant's QoS. This is universal rather than exotic,
  and it is diagnostic-only — nothing in the tree keys on that name. Set
  `<name>` in the anchor if you rely on it for tooling.

#### Two Fast DDS defaults worth knowing

"Anything unmentioned takes Fast DDS's default" means Fast DDS's, which is not always the DDS
specification's. Measured on `fast-dds/3.4.0`: a writer profile that omits `durability` resolves to
**TRANSIENT_LOCAL**, not the spec's VOLATILE — so it coincides with Fletcher's built-in. Reliability
likewise (`RELIABLE` for a writer). The policies where Fletcher and Fast DDS genuinely differ are
`history` and `resource_limits`, which is why the starting-point block spells both out.

The companion schema channel (`__schema` topic) always uses `RELIABLE` + `KEEP_LAST(depth=1)` +
`TRANSIENT_LOCAL`. **No profile name is ever consulted for it**: it is a Fletcher-internal
implementation detail and not configurable. Only `fletcher.max_schema_bytes` bounds it.

### Delivery guarantees

The provider upholds the `PubSubProvider::SubscribeCallback` contract:

- **Schema before data.** The subscription callback is never invoked with a null schema. Because `Subscribe` is non-blocking and may run before any publisher exists, a data sample can arrive before the topic schema does (the schema travels on the separate `__schema` channel). The provider holds such samples until the schema is known, then delivers them.
- **Per-writer order.** Samples from a single writer reach the callback in the order they were published. DDS delivers a single writer's samples to a `DataReader` in order under `RELIABLE` QoS; the provider preserves that order all the way to the callback — **including across the schema handoff**, where the buffered pre-schema backlog is delivered before, and never interleaved with, samples that arrive live afterwards.

Both guarantees are enforced by routing every sample (buffered backlog and live) through a single ordered FIFO that is drained by one thread at a time (`internal::OrderedDelivery`). The schema handoff is the one moment two threads are active — the schema-listener thread that resolves the schema and flushes the backlog, and the data-reader thread delivering live samples — so a sample offered while a drain is in progress is appended behind the in-flight backlog rather than delivered inline, which is what keeps the two from interleaving.

## Usage

```cpp
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
using namespace fletcher;

// Defaults — Fletcher's profile on domain 0, 64 KiB max payload.
auto provider = std::make_shared<FastDDSPubSubProvider>(ProviderConfig{});

// A DDS domain and nothing else: still Fletcher's built-in profile.
ProviderConfig config;
config.domain_id = 7;
auto custom = std::make_shared<FastDDSPubSubProvider>(config);

// Publishing out of the transport's own buffer, with the history sized to the
// rows on the topic (see Zero-copy: the plain sample above). Note the writer
// profile restates durability and reliability: a supplied profile is the WHOLE
// QoS for its role, so anything it omits takes Fast DDS's default, not
// Fletcher's.
ProviderConfig loaned;
loaned.document = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">
      <rtps>
        <propertiesPolicy>
          <properties>
            <property><name>fletcher.loan_publish</name><value>true</value></property>
          </properties>
        </propertiesPolicy>
      </rtps>
    </participant>
    <data_writer profile_name="fletcher_writer">
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
auto shared = std::make_shared<FastDDSPubSubProvider>(loaned);
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

### Per-topic QoS overrides

A per-topic override is a profile **named after the topic** — the `/`-joined topic string. It is
looked up before `fletcher_writer` / `fletcher_reader`, and a topic with no profile of its own
falls back to those, then to Fletcher's built-in.

Remember the whole-QoS rule: each of these profiles is complete in itself, so each restates the
policies it wants rather than inheriting them from `fletcher_writer`.

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

    <!-- "config/snapshot": keep everything, durable for late subscribers. -->
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
    <data_writer profile_name="fletcher_writer">
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

- `CreateTopic` must be called before `Publish` on the publisher side. The conflict check is **per topic** (keyed by the topic name): re-declaring _the same topic_ with an identical schema is idempotent (so several publishers may share one topic), while re-declaring it with a _different_ schema throws (a conflict). Distinct topics are independent — two different topics may carry the **same** schema (identical schemas can describe different data); that is never a conflict.
- On the subscriber side `Subscribe` can be called without a prior `CreateTopic` and is **non-blocking** — it never waits for a publisher. The schema arrives asynchronously over the `__schema` companion DDS topic; `Subscribe` returns a `SchemaArrival` whose `Wait` reports `kOk` with the schema once it is known (and `kSubscriptionEnded` if the subscription is torn down first), and the provider buffers incoming data until then so the callback is never invoked with a null schema. (This is the subscriber-first contract — subscribe before any publisher exists.) Per-writer order is preserved across this handoff — see [Delivery guarantees](#delivery-guarantees).
- Only one subscription per topic per provider instance is supported (one `DataReader` per topic). Call `Unsubscribe` before re-subscribing. Multi-callback fan-out lives in `fletcher::Subscriber` one layer up.
- The subscription callback is invoked from a Fast DDS internal listener thread, **while Fast DDS holds that reader's own RTPS mutex**: `StatefulReader::process_data_msg` takes it and still holds it through `change_received`, `NotifyChanges` and the listener call, and the data-sharing thread reaches the same place through the same function. Two consequences. Shared state touched from the callback must be protected externally — and a slow or blocking callback stalls reception for that reader entirely, because nothing else can enter it. Hand work off to your own thread if it is not short.
- The callback can also run **on the thread that is still inside `Subscribe`**. `create_datareader` pairs with intraprocess writers synchronously, so a `TRANSIENT_LOCAL` topic that already has a publisher on this same participant replays its retained samples on the subscribing thread — while `Subscribe` is holding the provider mutex. A callback that re-enters the provider (`Publish`, `Subscribe`, `CreateTopic`) from that *first* delivery deadlocks: the mutex is a `std::shared_mutex` and is not recursive. Re-entering from any later delivery is fine.
- `FastDDSPubSubProvider` is non-copyable and non-movable (DDS entities cannot be transferred).
- The callback's `data` pointer is a loaned DDS payload: it is valid for the duration of the callback only. Copy anything you keep. (This was always the contract — the pointer was never owned by the callback — but loans are where holding on to it actually breaks.)

### Measured decisions

Numbers that justify a shape in the code, kept here rather than in the comment that would otherwise
carry them — they date, and a stale figure in a source comment reads as current. Harnesses:
`benchmarks/bench_pub_sub_type.cpp` here, and `tools/fletcher_bench` in the consuming repo.

| Decision | Where | Measured |
|---|---|---|
| `Publish` takes the provider mutex **shared**, not exclusive | `src/fast_dds_pubsub_provider.cpp` | 16 threads on 16 topics: p99 **2.7 µs → 711 µs** when the exclusive lock spanned `DataWriter::write`. `DataWriter::write` is itself thread safe, so shared is enough to keep the topic and writer alive. |
| `PublishData` and `ReceivedData` are separate structs | `src/internal/transport_data.hpp` | Bundled, every serialised publish built and destroyed an `Attachments` the publish path never reads — MSVC's `unordered_map` allocates a sentinel node in its default constructor. **52 ns of the 137 ns** the publish spends outside Fast DDS. |
| The sample is an offset and a size, not a struct templated on its bound | `src/internal/fletcher_sample.hpp` | The struct made the bound a compile-time constant, which forced a *compiled set* of bounds and gave the rule its floor, ceiling and power-of-two shape. It cost nothing to drop, because the bound never reached the encode loop as a constant either way: `EncodeEnvelopeBody` takes a `WriteBuffer&`, whose bound is `capacity_`, a runtime member. Measured against `BM_PublishFlow_LoanedStruct`, the baseline arm kept for exactly this (3 repetitions, mean wall): **14.7 ns against 16.2 ns** at 214 B, **31.9 against 32.4** at 4 KiB, **103 against 111** at 16 KiB, **971 against 976** at 60 KB — equal within a 2–9 % coefficient of variation, and not slower. The loaned read is unchanged at **2.05 ns**. |
| `serialize()` writes the 8 framing bytes directly instead of through a `Cdr` | `src/internal/fletcher_sample_pub_sub_type.hpp` | Byte-identical output for **13.5 ns** against **52.9 ns** (fastcdr placing the encapsulation with the length reserved and patched) and **49.5 ns** (fastcdr serialising the `sequence<octet>` outright), measured over 400 000 calls on a 222-byte envelope. Against a 14.6 ns loaned publish that is ~3.5×. Held by `FletcherSamplePubSubTypeTest.FastCdrReproducesTheBytesExactly` (in the unit suite, so CI runs it) and by `bench_pub_sub_type`'s fastcdr arm. |
| `OrderedDelivery` publishes queue emptiness in an atomic instead of locking to look | `src/internal/ordered_delivery.hpp` | The steady path has to know whether anything was queued while its callback ran — by the callback re-offering, or by another thread whose own drain bailed because this delivery was in progress. Taking `mu_` to find out cost **19.7 ns → 6.96 ns** per delivered sample once the answer became an acquire load (`BM_Deliver_OfferView`, 3 repetitions, cv < 1 %; `BM_Deliver_Offer` 19.6 → 7.17 ns), against a bare callback of 1.30 ns. `queue_` itself is still never touched without the lock; only the one bit is published, by `NoteQueuedLocked` under it. An earlier revision read `queue_.empty()` unlocked, which was the same speed and a data race. |
| `Publisher::CreateTopic` encodes the schema **before** taking the lock | `../pubsub/src/publisher.cpp` | The locked section becomes a byte compare. First declaration **1.4 → 2.8 µs** (it now encodes where it used to deep-copy); re-declaration **4.2 → 2.5 µs**, and concurrent callers no longer queue behind ~3.5 µs of IPC work each. |
| `PublishData` holds the encoder and attachments by pointer | `src/internal/transport_data.hpp` | The provider layer costs **~80 ns** over a raw `DataWriter::write` of the same bytes, and the encoder and topic-name changes took **15–22 ns** off that (`tools/fletcher_bench/bench_publish` in the consuming repo, 16 interleaved A/B runs). |
| `SubscribeCallback` takes `schema` and `attachments` by **const reference** | `../pubsub/include/fletcher/pubsub/provider.hpp` | By value it was **~110 ns per delivered sample against 1.4 ns for the call itself** — an empty `Attachments` is an `unordered_map`, and MSVC allocates a sentinel node in its default constructor, so every delivery built and destroyed one whether the sample carried attachments or not. |
| The delivery layer latches into a lock-free path once the schema handoff is done | `src/internal/ordered_delivery.hpp` | `OrderedDelivery` exists for the subscriber-first startup window and used to charge for it forever. With that plus the listener reusing one `Attachments`, delivery went from **199 ns to 3.2 ns** per loaned sample and **398 ns to 3.5 ns** per copied one (`benchmarks/bench_pub_sub_type`, `BM_Deliver_*`). |

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
    self.requires("fletcher-fastdds-pubsub-provider/0.5.0-alpha")
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
