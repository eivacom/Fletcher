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
    self.requires("fletcher-pubsub-arrow/0.3.2-alpha")
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
