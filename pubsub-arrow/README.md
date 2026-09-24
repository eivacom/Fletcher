# fletcher-pubsub-arrow

Server-side Arrow C++ wrappers around `fletcher-pubsub`'s `Publisher` and
`Subscriber`. Bridges the gap between the nanoarrow-based pub/sub core (raw
bytes + `ArrowSchema` C structs) and Apache Arrow C++ types
(`arrow::Schema`, `ArrowRow`).

```cpp
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>

fletcher::PublisherArrow pub(provider);
pub.CreateTopic({"orders", "v1"}, arrow_schema);
pub.Publish({"orders", "v1"}, arrow_row);

fletcher::SubscriberArrow sub(provider);
sub.Subscribe({"orders", "v1"}, [](fletcher::ArrowRow row,
                                   fletcher::Attachments att) { ... });
```

`PublisherArrow` internally owns a `Publisher` and a per-topic `Codec` (from
`fletcher-arrow-bridge`); `SubscriberArrow` owns a `Subscriber` and lazily
creates a `Codec` from the schema received from the publisher. The wire
format remains byte-identical to what edge code produces via the raw
`Publisher` / `Subscriber`.

A consumer that wants a topic's shape without any of its data uses
`SubscriberArrow::SubscribeSchema` — it forwards `Subscriber::SubscribeSchema` as is and opens no
data path at all, returning the same `SchemaArrival` a `Subscribe` on that topic would. Convert with
`fletcher::ImportArrowSchema(const SharedSchema&)` once the arrival reports `kOk`; release the watch
with `UnsubscribeSchema`. A later `Subscribe` on the topic reuses what `SubscribeSchema` opened but
does not take it over — the watch outlives the subscription, and watches are counted per topic.

### TopicOptions

`fletcher::TopicOptions` (`fletcher/pubsub/provider.hpp`) is a per-topic, OPTIONAL options struct —
a `profile` name the provider interprets and a publisher-side `max_payload_bytes` bound — with a
default-constructed value meaning "the provider's defaults". `PublisherArrow::CreateTopic` and both
`SubscriberArrow::Subscribe` overloads (per-row and batched) take `options` as a defaulted trailing
argument (`= {}`) that forwards straight to the underlying `Publisher` / `Subscriber` method of the
same name, which is what reaches the provider's `CreateTopicWithOptions` / `SubscribeWithOptions` —
always, on both tiers (see [pubsub/README.md](../pubsub/README.md)):

```cpp
fletcher::TopicOptions options{.profile = "reliable_large"};
pub.CreateTopic({"orders", "v1"}, arrow_schema, options);
sub.Subscribe({"orders", "v1"}, [](fletcher::ArrowRow, fletcher::Attachments) { ... }, options);
```

A provider with no notion of profiles or per-topic bounds refuses a non-empty `TopicOptions` with
`kNotSupported`; an empty one is never refused. The conflict check is field-wise, not whole-struct:
re-declaring a topic, or joining a live subscription, may repeat or omit a field already in force
but never change one, and naming a field that was never set before is a conflict too, against the
stored EMPTY value — a conflicting field is `kInvalidArgument`. The batched `Subscribe` overload
takes `BatchOptions` positionally and `TopicOptions` as a defaulted fourth argument (`= {}`);
`BatchOptions` itself still needs a separate convenience overload rather than a default value,
because a nested aggregate's member initializers aren't usable in a default argument of the same
class — `TopicOptions` is not nested in `SubscriberArrow`, so it defaults fine.

---

## Batched RecordBatch subscribe

Rows arrive one at a time, but analytics wants columns. `SubscriberArrow`
provides a batched `Subscribe` overload that accumulates incoming rows and
delivers an `arrow::RecordBatch` when **either** a row-count limit is reached
**or** a timeout elapses since the batch started filling — whichever comes
first (defaults: 8000 rows, 1 minute).

```cpp
fletcher::SubscriberArrow sub(provider);

sub.Subscribe(
    {"orders", "v1"},
    [](std::shared_ptr<arrow::RecordBatch> batch,
       std::vector<fletcher::Attachments> attachments,  // attachments[i] -> row i
       fletcher::SubscriberArrow::BatchStatus status) {
        // status.reason: kRowLimit | kTimeout | kClosing
        // status.rows_dropped: rows lost since the last flush (0 == all good)
        process(batch);
    },
    {.max_rows = 4096, .timeout = std::chrono::seconds(5)});  // or omit for defaults
```

The callback's third argument, `BatchStatus`, says **why** the batch was
delivered (`reason`) and whether any samples were lost (`rows_dropped`). A row
that fails to decode is counted in `rows_dropped` and contributes neither a row
nor an attachment, so a window with only dropped rows still delivers a zero-row
batch to report the loss. The partial batch is flushed with reason `kClosing`
on `Unsubscribe`. The callback target must outlive the subscription.

Rows are decoded straight into Arrow builders (`fletcher::BatchDecoder`, from
`fletcher-arrow-bridge`) rather than through per-row scalars, so the batched
path never pays for the ArrowRow round trip. A topic whose schema
`BatchDecoder`'s constructor rejects delivers a null `batch` with **every**
row counted in `rows_dropped` instead, for the entire life of the
subscription — there is no later recovery once the schema is known, and no
partial decoding.

That refusal has two different causes, and only one of them has a fallback.
Null, extension, decimal32/64, run-end-encoded, and list-view types are not
decodable through either `SubscriberArrow::Subscribe` overload: `Codec`'s
scalar codec has no case for any of them, so the per-row overload throws on
the identical schema too, and there is nothing to switch to. A dictionary
nested below the top level, an ordered dictionary, and a dictionary whose
value type is nested or `float16` are different — `Codec::DecodeRow` (the
per-row overload) decodes all three fine. A caller whose topic uses one of
those three dictionary shapes and wants the data at all uses the per-row
`Subscribe` overload for that topic instead of the batched one; there is no
way to get a `RecordBatch` for it.

A window that would overflow a 32-bit Arrow offset (a utf8/binary/list
column growing past 2 GiB) is flushed early with reason `kRowLimit`, same as
hitting `max_rows`.

Measured (`arrow-bridge/benchmarks`, per row at 8000 rows per batch): a 10-scalar
row 1.23 → 0.16 µs, a pose row with two nested `list<double>` 9.5 → 0.13 µs, a 2 × 2667-float cloud
row 566 → 4.4 µs, a 1000-point `list<struct>` row 5.46 ms → 57 µs, with allocations per row falling
from tens or thousands to under 0.2. The planner bridge, which subscribes this way, went from 79 % to
4 % of a core at 60 000 points per frame. The table and the method are in
[`arrow-bridge/README.md`](../arrow-bridge/README.md#measured-decisions-2026-09-02) and
[`arrow-bridge/benchmarks/README.md`](../arrow-bridge/benchmarks/README.md).

### Dictionary columns

A dictionary is a columnar optimisation that means nothing for a single row, so
a `dictionary(index, value)` field is transferred as its **value type**, one
value per row. The per-row `Subscribe` overload hands you a plain value scalar;
the batched overload re-folds the accumulated values into a real
`DictionaryArray` of the field's declared type when it assembles each batch:

```cpp
auto schema = arrow::schema({arrow::field(
    "category", arrow::dictionary(arrow::int32(), arrow::utf8()))});

fletcher::PublisherArrow pub(provider);
pub.CreateTopic({"events", "v1"}, schema);

// Publish plain values; no need to dictionary-encode on the sending side.
pub.Publish({"events", "v1"}, {std::make_shared<arrow::StringScalar>("click")});

// In the batched subscriber, the "category" column of each RecordBatch comes
// out as a DictionaryArray (values deduplicated, nulls preserved).
```

The dictionary **value type must be a primitive/scalar type**; nested value
types (struct/list/map/union) are rejected with a clear error.

---

## Building locally

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

### Windows

```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory:

```bash
conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

---

## Consuming the package

```python
def requirements(self):
    self.requires("fletcher-pubsub-arrow/0.5.1-alpha")
```

```cmake
find_package(fletcher-pubsub-arrow CONFIG REQUIRED)
target_link_libraries(my-target PRIVATE fletcher::pubsub-arrow)
```

```cpp
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
```

`pubsub-arrow` re-exports its dependencies (`fletcher-pubsub`,
`fletcher-arrow-bridge`, `arrow::arrow`) transitively, so
consumers don't need to declare them separately.

---

## CI pipeline

`.github/workflows/ci.pubsub-arrow.yml` is `workflow_call`-only;
it runs `build-windows` + `build-linux` and is invoked from `ci.pr.yml`
on PRs touching `pubsub-arrow/**` and from `cd.pubsub-arrow.yml`
on `pubsub-arrow-v*` tag pushes. The `upload` job that creates the
GitHub Release lives in `cd.pubsub-arrow.yml`.
