# Benchmarks — the provider's DDS types

Three programs over the same row. Only the last creates DDS entities:

| Program | What it is |
|---|---|
| `bench_pub_sub_type` | Google Benchmark. Measures `internal::FletcherSamplePubSubType` against the type it replaced, at nanosecond resolution. |
| `example_arrow_roundtrip` | A narrated walkthrough. Prints one Arrow batch all the way to DDS bytes and back, naming every intermediate type and dumping every buffer. Start here to see the flow. |
| `exp_zero_copy` | Ping-pong latency across data-sharing — the axis that still makes a sample zero-copy through the regular publish path (`Publish` always writes through `WriteSample` now; `LoanableSampleWriter` stays unit-tested, not benchmarked here) — through real participants, writers and readers. The only one here that goes near DDS. |

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
| `BM_Deliver_*` | the floor beneath a delivery: the callback alone, called directly (`BM_Deliver_CallbackOnly`), and `ParseEnvelopeBody` *with* attachments, which `BM_ReadFlow` never parses (`BM_Deliver_ParseAttachments`). Read each as its own time minus `BM_Deliver_CallbackOnly` **from the same run** |
| `BM_AttachmentsConstruct`, `BM_PublishFieldsConstruct` | why the sample struct is split by direction: an empty `Attachments` against what `PublishData` costs. The allocation this arm was added to expose is gone — PDA-DEC-AG2 retired the `unordered_map` alias — so read it now as the floor rather than as the cost |
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

### exp_zero_copy

Takes `<sharing 0|1> <slots>` and runs ONE combo. Fast DDS's profile registry is process-wide
(`src/internal/profile_document.hpp`): two different (sharing, slots) pairs are two different
documents under the same profile names, which collide if loaded in one process — so each combo is
its own process. Run with no arguments to print the valid combos, then loop over them:

```powershell
build\Release\exp_zero_copy.exe   # prints the valid <sharing> <slots> combos
foreach ($sharing in 0, 1) {
    foreach ($slots in 16, 32) {
        & build\Release\exp_zero_copy.exe $sharing $slots
    }
}
```

Report medians with their standard deviations — several arms are a few nanoseconds, so a single run
says nothing. `BM_ReadFlow_Loaned` reads a buffer that never changes and returns a pointer into it,
so it calls `benchmark::ClobberMemory()` per iteration; without that the compiler hoists the parse
out of the loop and the arm measures 0.

### bench_e2e

BEFORE/AFTER numbers for the reader-side redesign: end-to-end publish-to-callback latency and
throughput, intraprocess, through two real `FastDDSPubSubProvider` instances (domain 43). With no
flags this is Fletcher's built-in QoS throughout. For row sizes 198 B and 60 000 B it reports
p50/p99/max latency in microseconds (publishes paced ~100 us apart with a busy-wait so each sample
finds the receiver idle, one sample in flight, 20 000 samples after 2 000 warm-up) and flat-out
throughput (200 000 samples, no pacing) in samples/s and MB/s. Pin it to one P-core at High priority
before trusting a number off it, the same as `exp_zero_copy` above.

`build\Release\bench_e2e.exe` prints one `arm=...` line per measurement and takes:

- `--arm latency|throughput|all` (default `all`), `--bytes 198|60000|all` (default `all`) -- run a
  single measurement instead of the full sweep.
- `--reader-datasharing auto|off` (default `auto`), `--writer-blocking infinite|100ms` (default
  `infinite`) -- isolate the two round-G3 QoS changes (reader `data_sharing` AUTOMATIC, writer
  `max_blocking_time` infinite) that turn a full reader history under a flat-out publisher into a
  stall instead of a drop. The default combination (`auto` + `infinite`) is byte-for-byte
  Fletcher's built-in QoS, the same as passing no document at all; any other combination loads a
  document built from the "published starting point" block above with only the flagged policy
  changed. Fast DDS's profile registry is process-wide, so this is one combination per process --
  run each corner of the matrix as its own invocation.

Every measurement runs under a 30 s watchdog: a thread that has not finished in time -- most likely
`pub.Publish()` itself blocked in `write()` -- prints `STALL arm=... bytes=... arrived=x/y` and
exits non-zero instead of hanging.

Results and the reasoning: the Monorepo's `modules/io/docs/serialization-benchmark.md`.
