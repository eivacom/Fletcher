# Benchmarks — the provider's DDS types

Three programs over the same row. Only the last creates DDS entities:

| Program | What it is |
|---|---|
| `bench_pub_sub_type` | Google Benchmark. Measures `internal::FletcherSamplePubSubType` against the type it replaced, at nanosecond resolution. |
| `example_arrow_roundtrip` | A narrated walkthrough. Prints one Arrow batch all the way to DDS bytes and back, naming every intermediate type and dumping every buffer. Start here to see the flow. |
| `exp_zero_copy` | Ping-pong latency across the two axes that make a sample zero-copy — loaning and data-sharing — through real participants, writers and readers. The only one here that goes near DDS. |

`transform_batch.hpp` holds the row all three use — NaviSuite's `TransformWithVelocity`, its Arrow schema,
and the batch helpers — so the thing measured and the thing narrated are the same thing.

The unit tests for the type itself are elsewhere and run in CI:
`../tests/test_fletcher_sample_pub_sub_type.cpp`.

Not part of the Conan package: this directory is outside the recipe's `exports_sources`, has its own
`conanfile.py`, and is built with `conan build .` the way `integration-tests/*` are.

| Arm | What it measures |
|---|---|
| `BM_Serialize`, `BM_Deserialize` | the two `TopicDataType`s head to head, row size swept |
| `BM_Serialize_CurrentFastCdrFramed` | what fastcdr placing the encapsulation costs, which is how the current type was written until it was measured |
| `BM_PublishFlow`, `BM_ReadFlow` | what the loaned flow removes from each side — the zero-copy budget |
| `BM_PublishFlow_LoanedStruct` | the same loaned write through the struct the sample used to be, when its bound was a template argument. The baseline arm for dropping that struct; see the provider README's measured decisions |
| `BM_ProviderPublishOverhead` | what `Publish` spends per sample before the type is reached. Superseded by the Monorepo's `tools/fletcher_bench/bench_publish`, which drives the real `Publish` against a raw DDS control |
| `BM_Deliver_*` | the subscribe-side delivery layer: `OrderedDelivery` on both read flows, and `ParseEnvelopeBody` *with* attachments, which `BM_ReadFlow` never parses. Read each as its own time minus `BM_Deliver_CallbackOnly` **from the same run** |
| `BM_AttachmentsConstruct`, `BM_PublishFieldsConstruct` | why the sample struct is split by direction: an empty `Attachments` against what `PublishData` costs |
| `BM_Memcpy` | the floor: the row bytes moved once |
| `BM_BatchRoundTrip` | a nanoarrow batch out and back, per type and per publish flow, row count swept |

The current arm is the **shipped** type: `../src/internal` is on the include path, so nothing is
copied. The legacy arm has to be — `legacy_fletcher_topic_type.hpp` is lifted verbatim from
`src/fast_dds_pubsub_provider.cpp` at `f779c2f`, where it was a file-local class, because that type
exists nowhere else now.

A validation pass runs before the benchmarks and returns non-zero if it fails: both types must deliver
byte-identical rows, the current type's hand-written encapsulation must match fastcdr's output byte for
byte, and a 1000-row Arrow batch must round-trip value-exact through each type. That middle check is
what makes the hand-written header safe to keep, so do not weaken it.

Nothing builds this directory in CI, so that middle check also lives in the unit suite as
`FletcherSamplePubSubTypeTest.FastCdrReproducesTheBytesExactly`
(`../tests/test_fletcher_sample_pub_sub_type.cpp`), which every PR runs. The copy here is kept
because it also compares against the legacy type, which the unit suite does not know about.

## Running

Use the profile the provider package in your Conan cache was built with.

```bat
conan install . -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release --build=missing
conan build . -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
build\Release\example_arrow_roundtrip.exe
build\Release\bench_pub_sub_type.exe --benchmark_min_time=0.3 --benchmark_repetitions=7 --benchmark_report_aggregates_only=true
```

If `conan build` reports `Missing prebuilt package for fletcher-fastdds-pubsub-provider`, the
provider was last created with `-o:a run_tests=True`, which changes its dependencies' package ids —
re-run with `--build=missing`.

Report medians with their standard deviations — several arms are a few nanoseconds, so a single run
says nothing. `BM_ReadFlow_Loaned` reads a buffer that never changes and returns a pointer into it,
so it calls `benchmark::ClobberMemory()` per iteration; without that the compiler hoists the parse
out of the loop and the arm measures 0.

Results and the reasoning: the Monorepo's `modules/io/docs/serialization-benchmark.md`.
