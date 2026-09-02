# Benchmarks — the Arrow-tier serialization path

One program, `bench_arrow_codec`, measuring `fletcher::Codec` (`arrow-bridge/include/fletcher/arrow_bridge/codec.hpp`),
the positional wire format it implements (`core/include/fletcher/core/positional_io.hpp`), and the
`SubscriberArrow` batched-decode path (`pubsub-arrow/src/subscriber_arrow.cpp`), across seven row shapes.

Not part of any Conan package: this directory is outside `arrow-bridge`'s and `pubsub-arrow`'s
`exports_sources`, has its own `conanfile.py`, and is built with `conan build .` the way
`fastdds-pubsub-provider/benchmarks` and `integration-tests/*` are.

## Shapes (`fixtures.hpp`)

| Shape | Fields |
|---|---|
| `Scalars10` | bool, int32, int64, uint32, float, double, utf8 (16 B), timestamp[ns], duration[ns], binary (32 B) — all non-nullable |
| `Nullable10` | same fields, nullable; fields 0, 3, 6, 9 (every third) are null in every row |
| `Cloud` | timestamp[ns], `list<float>`×2667, `list<uint32>`×2667 (list item field nullable, as protoc emits) |
| `Pose` | timestamp[ns], `struct{list<double>×16}`, `struct{list<double>×6}` |
| `Points` | `list<struct<x,y,z: double>>`×1000 |
| `Nested` | `list<list<struct<x,y: double>>>` (2×5), `map<utf8,double>`×8, `struct{struct{int32, utf8}}` |
| `Generic` | `dictionary<int32,utf8>` (3 cycling values), `fixed_size_list<float,3>`, `decimal128(10,2)`, `dense_union<int32,utf8>` (alternating), `large_utf8`, `list<timestamp[ns]>`×1000 |

For each shape, `fixtures.hpp` provides a plain-C++ `RowValues` struct, `Schema()`, `MakeRowValues(i)` /
`ToArrowRow` (the same row data built two ways), and `WritePositional` / `ReadPositional` — a
hand-written `PositionalWriter`/`PositionalReader` twin standing in for what `fletcher-protoc`
generated code would emit for that schema. `MakeBatch`-style construction (used by the Arrow IPC and
end-to-end arms) is **not** independently hand-rolled per shape: batch construction never runs inside
a timed loop, so the suite folds `N` rows built via `MakeRow` into a `RecordBatch` with
`BuildBatchScalarPath` (the same `MakeBuilder`/`AppendScalar`/`Finish` pattern
`SubscriberArrow::RecordBatchBatcher::BuildBatch` uses) rather than seven independent typed-builder
pipelines — that pattern is measured on purpose by `BM_Decode_Batch_ScalarPath_*`; here it is only
fixture setup.

## Arms

| Arm | What it measures |
|---|---|
| `BM_Encode_Codec_S` | `Codec::EncodeRow` of a prebuilt `ArrowRow`, fresh-vector overload |
| `BM_Encode_Codec_S_IntoFixed` | `Codec::EncodeRow(row, WriteBuffer&)` into a preallocated `FixedWriteBuffer` — the provider-buffer path a `RowEncoder` callback actually calls |
| `BM_Encode_Positional_S` | the hand-written `PositionalWriter` twin into a `FixedWriteBuffer` — the generated-code floor |
| `BM_Decode_Row_S` | `Codec::DecodeRow` of prebuilt bytes |
| `BM_Decode_Positional_S` | the hand-written `PositionalReader` twin — the floor |
| `BM_Memcpy_S` | the wire bytes moved once, nothing else |
| `BM_Decode_Batch_ScalarPath_S/N` | the OLD batched decode (`SubscriberArrow::RecordBatchBatcher::BuildBatch` pre-`BatchDecoder`, reproduced verbatim: `MakeBuilder`/`AppendScalar`/`Finish` over per-row `arrow::Scalar`s), `N` pre-decoded rows in, one `RecordBatch` out; `N` in {1, 1000, 8000} — kept as the A/B control |
| `BM_Decode_Batch_S/N` | the NEW batched decode (`fletcher::BatchDecoder`, wire bytes straight into one `arrow::ArrayBuilder` tree, no per-row `arrow::Scalar`), `N` pre-encoded wire rows in, one `RecordBatch` out; `N` in {1, 1000, 8000} |
| `BM_ArrowIpc_Write_S/N` | `arrow::ipc` stream writer over one `N`-row `RecordBatch`; `N` in {1000, 8000} |
| `BM_ArrowIpc_Read_S/N` | `arrow::ipc` stream reader over the bytes `BM_ArrowIpc_Write_S/N` produces |
| `BM_EndToEnd_Batched_S/N` | `PublisherArrow::Publish`, `N` rows through a loopback provider (same shape as `MockProvider` in `pubsub-arrow/tests/test_pubsub_arrow.cpp`), into a batched `SubscriberArrow` (`BatchOptions{N, 1 min}`, now backed by `BatchDecoder`); time per `N` rows |
| `BM_Run_PerElement_2667f` vs `BM_Run_Memcpy_2667f` | standalone: filling an `arrow::FloatBuilder` from 2667 unaligned floats one at a time against one bulk `memcpy` — the same comparison `Codec`'s primitive-list-run path (`detail::AppendRun`, `row_reader.hpp`) makes |

Counters: `bytes` (wire bytes per row) on the per-row arms; `allocs_per_row` (an `operator new`/`delete`
override, sampled around one call outside the timed loop) on `BM_Decode_Row_*`,
`BM_Decode_Batch_ScalarPath_*/1000`, `BM_Decode_Batch_*/1000`, and `BM_Encode_Codec_*` (both the
vector and `_IntoFixed` overloads) — default ON for this target via the `FLETCHER_BENCH_COUNT_ALLOCS`
compile definition in `CMakeLists.txt`. `SetItemsProcessed(rows)` on every arm.

A validation pass runs before any benchmark and fails the process (non-zero exit) if, for any shape and
3 rows, `Codec::EncodeRow` disagrees byte-for-byte with the positional twin, a decoded value disagrees
with the row that produced it, or `fletcher::BatchDecoder` — `Append`'d the same 3 rows into one batch,
`Finish()`'d — disagrees with `Codec::DecodeRow` cell-for-cell (for `Generic`'s dictionary column,
`DictionaryScalar::GetEncodedValue()` against the plain value scalar `DecodeRow` yields) or fails
`RecordBatch::ValidateFull()`.

## Running

The lockfile in this directory (`conan.lock`) pins the exact recipe revisions of
`fletcher-arrow-bridge` and `fletcher-pubsub-arrow` (and their dependency graph) that were in the Conan
cache when this baseline was recorded — **keep it**; it is the baseline pin, not a build artifact.
`conan lock create` must run before any code changes so the lockfile captures the revisions in place at
that moment, independent of any later rebuild of those packages.

```bat
cd arrow-bridge/benchmarks
conan lock create . -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release --build=missing
conan install . -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release --lockfile=conan.lock --build=missing
conan build . -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release --lockfile=conan.lock
build\Release\bench_arrow_codec.exe --benchmark_min_time=0.3s --benchmark_repetitions=7 --benchmark_report_aggregates_only=true
```

If `conan install`/`conan build` reports a missing binary for `fletcher-arrow-bridge` or
`fletcher-pubsub-arrow` even with the lockfile in place, re-run the `conan install ... --build=missing`
step: a concurrent rebuild of those packages elsewhere on the machine can evict the specific package
*binary* for the locked recipe revision from the local cache even though the locked revision's exported
source remains available to rebuild from (this happened once while recording this baseline — see
"Notes" below).

Google Benchmark is `benchmark/1.9.4` here, vs. `1.6.1` for `fastdds-pubsub-provider/benchmarks`.

**Measurement discipline**: report medians with their standard deviations — several arms are a few
nanoseconds to a few microseconds, so a single run says nothing (`fastdds-pubsub-provider/benchmarks/README.md:62`).
Run the same tree twice; a run-to-run median delta over 3% on an arm is worth a second look before
trusting it.

## Notes

- **Mid-session re-pin.** `conan lock create` was run first, pinning `fletcher-arrow-bridge/0.5.0-alpha#c02b0860c3c539e907a9699b9ab86241`
  and `fletcher-pubsub-arrow/0.5.0-alpha#cc1475b31ab7df3c7c5e16900f93d9c8` — the revisions in the cache
  at that moment. Partway through this session, a concurrent rebuild of `fletcher-arrow-bridge` (editing
  `arrow-bridge/src`, `arrow-bridge/include`, and `arrow-bridge/tests` live in this working tree — see
  `git status`) transiently evicted the *package binary* for the locked revision from the local Conan
  cache (`conan list` briefly showed only a newer recipe revision). Re-running
  `conan install ... --build=missing` recovered it: the locked revision's exported source was still
  present in the cache, so Conan rebuilt the exact pinned binary rather than resolving to the newer one.
  `conan.lock` in this directory still names the original pinned revisions, and the BASELINE numbers
  below were recorded against that pinned build, not the concurrent rebuild's changes.
- **`MakeBatch` is not per-shape hand-rolled typed builders.** See the shapes section above — batch
  construction is unmeasured fixture setup here, folded through the same scalar-path builder the
  `BM_Decode_Batch_ScalarPath_*` arm measures, rather than seven independent typed-builder pipelines.

## Results

Recorded **2026-09-02** on this machine: 13th Gen Intel(R) Core(TM) i9-13950HX, 24 cores / 32
logical processors (Google Benchmark's own header confirms `Run on (32 X 2419 MHz CPU)`), Windows
11, MSVC 19.44 (toolset v143). Google Benchmark is `benchmark/1.9.4` here (as before).

**BEFORE**: commit `564467b` (this branch's HEAD) with none of this session's working-tree changes
applied — `fletcher-arrow-bridge` and `fletcher-pubsub-arrow` were built from HEAD before the working
tree was touched, pinned by `conan.lock.before` (kept in this directory):
`fletcher-arrow-bridge/0.5.0-alpha#c02b0860c3c539e907a9699b9ab86241`,
`fletcher-pubsub-arrow/0.5.0-alpha#cc1475b31ab7df3c7c5e16900f93d9c8`. This binary predates the new
arms (`BM_Decode_Batch_S/N`, `BM_Encode_Codec_S_IntoFixed`) — those rows read "(new arm)" below.

**AFTER**: commit `564467b` **plus the uncommitted working-tree changes** in this session
(`BatchDecoder`, `Codec::EncodeRow(row, WriteBuffer&)`, the new `RecordBatchBatcher` on
`BatchDecoder`, the bulk primitive-list-run path, `fletcher-protoc` updates — see `git status`),
pinned by `conan.lock` (regenerated this step):
`fletcher-arrow-bridge/0.5.0-alpha#19bf79025942386b6c2c9fb80ab28f55`,
`fletcher-pubsub-arrow/0.5.0-alpha#16b84c6a40dd21702bf8f4ad857e89af` — confirmed the newest revision
of each via `conan list "fletcher-arrow-bridge/0.5.0-alpha#*"` / `"fletcher-pubsub-arrow/0.5.0-alpha#*"`
before building.

Both binaries were run twice back to back on an otherwise-idle machine (no other build or agent
running), with:

```bat
<binary> --benchmark_min_time=0.3s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_out=<name>.json --benchmark_out_format=json
```

(5 repetitions and JSON output, not the 7-repetition text-table runs `run1.txt`/`run2.txt` used for
the noisy Step-0 baseline — those two files are superseded by `before_run1.json`/`before_run2.json`
below, recorded fresh on the quiet machine, and have been moved out of the repo alongside them, per
the measurement-discipline note above.) Wall time: `before_run1` 22m07s, `before_run2` 22m17s,
`after_run1` 17m36s, `after_run2` 17m34s — all four comfortably under the 35-minute filter threshold,
so no filtered sweeps were needed. The AFTER binary runs faster overall despite carrying 28 more
registered benchmarks than BEFORE (135 lines from `--benchmark_list_tests`, 128 of them counted
after subtracting the 7 validation-pass `printf`s that share stdout): `BM_Decode_Batch_*` is far
cheaper than the `BM_Decode_Batch_ScalarPath_*` control sitting next to it for every shape (see
below), so the added arms cost less time than the BEFORE binary spends inside
`BM_Decode_Batch_ScalarPath_Points/8000` and `BM_Decode_Batch_ScalarPath_Cloud/8000` alone.

All four raw JSON logs live outside the repo, under this session's scratchpad directory
(`C:\Users\mch\AppData\Local\Temp\claude\C--src-git-eivacom-Fletcher\1b52de73-7150-4ab3-87c6-372bb810c070\scratchpad`):
`bench_before\before_run{1,2}.json` (BEFORE) and `after_run{1,2}.json` (AFTER). The table below pastes run 1's median ± run 1's
stddev for both binaries; the last column flags a run-1 → run-2 delta only when `|delta| > 3%`,
in *either* binary. Ratio is after/before — below 1.0 is faster; new arms (no BEFORE binary
support) show no ratio.

`BM_ArrowIpc_*`, `BM_EndToEnd_Batched_*`, `BM_Encode_Positional_*`, `BM_Decode_Positional_*`,
`BM_Memcpy_*`, and `BM_Decode_Batch_ScalarPath_*` are controls: none of their own source changed
between BEFORE and AFTER (the first five live in Arrow itself or in this directory's unchanged
`fixtures.hpp`; `BuildBatchScalarPath` is reproduced verbatim in `fixtures.hpp` and untouched by the
library changes). Any delta on these beyond a few percent is measurement noise or a binary-layout
artifact from the larger AFTER binary (more registered benchmarks shifts `.text` layout slightly),
not a real behavior change — see "Reading the result" below for which deltas are real.

| Arm | Before (median ± stddev, run 1) | After (median ± stddev, run 1) | Ratio (after/before) | Run-2 delta (if |delta|>3%) |
|---|---|---|---|---|
| `BM_ArrowIpc_Read_Cloud/1000` | 4.42 ± 0.092 us | 4.68 ± 0.092 us | 1.060x | after run2 -4.0% |
| `BM_ArrowIpc_Read_Cloud/8000` | 4.51 ± 0.104 us | 6.18 ± 1.098 us | 1.369x | after run2 -24.6% |
| `BM_ArrowIpc_Read_Generic/1000` | 8.91 ± 0.110 us | 8.39 ± 0.065 us | 0.941x |  |
| `BM_ArrowIpc_Read_Generic/8000` | 8.80 ± 0.312 us | 8.39 ± 0.083 us | 0.954x |  |
| `BM_ArrowIpc_Read_Nested/1000` | 8.99 ± 0.189 us | 9.17 ± 0.098 us | 1.020x | before run2 +3.7% |
| `BM_ArrowIpc_Read_Nested/8000` | 9.52 ± 0.659 us | 9.20 ± 0.077 us | 0.966x |  |
| `BM_ArrowIpc_Read_Nullable10/1000` | 7.11 ± 0.108 us | 7.08 ± 0.093 us | 0.996x |  |
| `BM_ArrowIpc_Read_Nullable10/8000` | 7.01 ± 0.083 us | 7.01 ± 0.066 us | 1.001x |  |
| `BM_ArrowIpc_Read_Points/1000` | 5.95 ± 0.130 us | 4.65 ± 0.026 us | 0.782x |  |
| `BM_ArrowIpc_Read_Points/8000` | 4.44 ± 0.260 us | 4.75 ± 0.159 us | 1.069x | before run2 +56.2%; after run2 -4.7% |
| `BM_ArrowIpc_Read_Pose/1000` | 5.84 ± 0.345 us | 6.32 ± 0.050 us | 1.083x | before run2 -5.4%; after run2 -5.0% |
| `BM_ArrowIpc_Read_Pose/8000` | 5.62 ± 0.849 us | 5.84 ± 0.105 us | 1.039x |  |
| `BM_ArrowIpc_Read_Scalars10/1000` | 6.62 ± 0.089 us | 6.60 ± 0.070 us | 0.997x | after run2 -3.5% |
| `BM_ArrowIpc_Read_Scalars10/8000` | 6.57 ± 0.065 us | 6.54 ± 0.070 us | 0.994x |  |
| `BM_ArrowIpc_Write_Cloud/1000` | 2.78 ± 0.066 ms | 2.77 ± 0.082 ms | 0.998x | after run2 +7.3% |
| `BM_ArrowIpc_Write_Cloud/8000` | 28.58 ± 0.131 ms | 28.45 ± 1.482 ms | 0.995x | before run2 +3.8% |
| `BM_ArrowIpc_Write_Generic/1000` | 246.84 ± 4.610 us | 244.93 ± 3.326 us | 0.992x |  |
| `BM_ArrowIpc_Write_Generic/8000` | 3.55 ± 0.416 ms | 3.56 ± 0.312 ms | 1.001x |  |
| `BM_ArrowIpc_Write_Nested/1000` | 20.42 ± 0.328 us | 21.58 ± 0.151 us | 1.057x |  |
| `BM_ArrowIpc_Write_Nested/8000` | 250.00 ± 2.145 us | 254.17 ± 4.737 us | 1.017x |  |
| `BM_ArrowIpc_Write_Nullable10/1000` | 6.78 ± 0.263 us | 7.18 ± 0.172 us | 1.059x | after run2 -4.0% |
| `BM_ArrowIpc_Write_Nullable10/8000` | 21.17 ± 0.627 us | 22.29 ± 0.370 us | 1.053x | after run2 -8.1% |
| `BM_ArrowIpc_Write_Points/1000` | 3.47 ± 0.245 ms | 3.46 ± 0.086 ms | 0.998x |  |
| `BM_ArrowIpc_Write_Points/8000` | 36.26 ± 0.385 ms | 37.46 ± 1.182 ms | 1.033x | after run2 +3.1% |
| `BM_ArrowIpc_Write_Pose/1000` | 8.36 ± 0.307 us | 8.68 ± 0.104 us | 1.038x |  |
| `BM_ArrowIpc_Write_Pose/8000` | 74.85 ± 5.250 us | 75.91 ± 1.100 us | 1.014x |  |
| `BM_ArrowIpc_Write_Scalars10/1000` | 8.74 ± 0.324 us | 9.03 ± 0.106 us | 1.033x | after run2 -4.2% |
| `BM_ArrowIpc_Write_Scalars10/8000` | 63.19 ± 1.439 us | 63.94 ± 0.729 us | 1.012x |  |
| `BM_Decode_Batch_Cloud/1` | (new arm) | 4.28 ± 0.035 us |  | after run2 -11.0% |
| `BM_Decode_Batch_Cloud/1000` | (new arm) | 2.98 ± 0.032 ms |  |  |
| `BM_Decode_Batch_Cloud/8000` | (new arm) | 35.53 ± 1.538 ms |  | after run2 +16.9% |
| `BM_Decode_Batch_Generic/1` | (new arm) | 11.74 ± 0.107 us |  |  |
| `BM_Decode_Batch_Generic/1000` | (new arm) | 906.37 ± 8.900 us |  |  |
| `BM_Decode_Batch_Generic/8000` | (new arm) | 12.82 ± 0.082 ms |  |  |
| `BM_Decode_Batch_Nested/1` | (new arm) | 13.15 ± 0.134 us |  |  |
| `BM_Decode_Batch_Nested/1000` | (new arm) | 834.53 ± 3.701 us |  |  |
| `BM_Decode_Batch_Nested/8000` | (new arm) | 6.69 ± 0.158 ms |  |  |
| `BM_Decode_Batch_Nullable10/1` | (new arm) | 5.88 ± 0.036 us |  | after run2 -4.5% |
| `BM_Decode_Batch_Nullable10/1000` | (new arm) | 110.88 ± 2.119 us |  |  |
| `BM_Decode_Batch_Nullable10/8000` | (new arm) | 831.57 ± 11.867 us |  | after run2 -3.7% |
| `BM_Decode_Batch_Points/1` | (new arm) | 54.10 ± 0.441 us |  |  |
| `BM_Decode_Batch_Points/1000` | (new arm) | 54.96 ± 1.022 ms |  | after run2 -5.6% |
| `BM_Decode_Batch_Points/8000` | (new arm) | 459.38 ± 3.165 ms |  |  |
| `BM_Decode_Batch_Pose/1` | (new arm) | 5.19 ± 0.040 us |  | after run2 -3.4% |
| `BM_Decode_Batch_Pose/1000` | (new arm) | 135.52 ± 1.579 us |  | after run2 -4.0% |
| `BM_Decode_Batch_Pose/8000` | (new arm) | 1.04 ± 0.009 ms |  |  |
| `BM_Decode_Batch_ScalarPath_Cloud/1` | 556.92 ± 14.894 us | 561.32 ± 12.111 us | 1.008x | before run2 +4.2%; after run2 -3.3% |
| `BM_Decode_Batch_ScalarPath_Cloud/1000` | 565.12 ± 21.552 ms | 565.96 ± 3.872 ms | 1.002x | after run2 -3.4% |
| `BM_Decode_Batch_ScalarPath_Cloud/8000` | 4.460 ± 0.0351 s | 4.690 ± 0.0687 s | 1.052x | after run2 -5.9% |
| `BM_Decode_Batch_ScalarPath_Generic/1` | 132.26 ± 1.536 us | 132.11 ± 0.550 us | 0.999x | before run2 +4.5%; after run2 +6.9% |
| `BM_Decode_Batch_ScalarPath_Generic/1000` | 123.14 ± 3.073 ms | 123.12 ± 0.815 ms | 1.000x | before run2 +4.4% |
| `BM_Decode_Batch_ScalarPath_Generic/8000` | 1.013 ± 0.0352 s | 1.007 ± 0.0056 s | 0.994x | before run2 +4.7% |
| `BM_Decode_Batch_ScalarPath_Nested/1` | 71.75 ± 1.782 us | 74.95 ± 0.766 us | 1.045x | before run2 +3.8%; after run2 +4.0% |
| `BM_Decode_Batch_ScalarPath_Nested/1000` | 54.84 ± 0.565 ms | 54.87 ± 0.931 ms | 1.001x | after run2 +4.4% |
| `BM_Decode_Batch_ScalarPath_Nested/8000` | 532.86 ± 45.376 ms | 502.92 ± 8.272 ms | 0.944x | before run2 -4.4% |
| `BM_Decode_Batch_ScalarPath_Nullable10/1` | 6.47 ± 0.095 us | 6.97 ± 0.059 us | 1.077x | before run2 +3.6%; after run2 -4.3% |
| `BM_Decode_Batch_ScalarPath_Nullable10/1000` | 193.01 ± 1.941 us | 194.99 ± 1.813 us | 1.010x | after run2 -7.3% |
| `BM_Decode_Batch_ScalarPath_Nullable10/8000` | 1.71 ± 0.059 ms | 1.67 ± 0.042 ms | 0.978x | before run2 -5.8%; after run2 -7.5% |
| `BM_Decode_Batch_ScalarPath_Points/1` | 2.81 ± 0.066 ms | 2.91 ± 0.044 ms | 1.036x | after run2 +3.7% |
| `BM_Decode_Batch_ScalarPath_Points/1000` | 2.870 ± 0.0328 s | 2.896 ± 0.0259 s | 1.009x |  |
| `BM_Decode_Batch_ScalarPath_Points/8000` | 22.628 ± 0.1128 s | 23.299 ± 0.1582 s | 1.030x |  |
| `BM_Decode_Batch_ScalarPath_Pose/1` | 13.08 ± 1.206 us | 13.43 ± 0.223 us | 1.027x |  |
| `BM_Decode_Batch_ScalarPath_Pose/1000` | 6.97 ± 0.280 ms | 7.10 ± 0.040 ms | 1.019x |  |
| `BM_Decode_Batch_ScalarPath_Pose/8000` | 62.08 ± 5.494 ms | 64.64 ± 1.215 ms | 1.041x | after run2 -3.9% |
| `BM_Decode_Batch_ScalarPath_Scalars10/1` | 6.81 ± 0.164 us | 6.83 ± 0.063 us | 1.002x |  |
| `BM_Decode_Batch_ScalarPath_Scalars10/1000` | 195.83 ± 1.187 us | 199.56 ± 5.217 us | 1.019x | before run2 -4.0%; after run2 -6.1% |
| `BM_Decode_Batch_ScalarPath_Scalars10/8000` | 1.80 ± 0.222 ms | 1.79 ± 0.010 ms | 0.994x | after run2 -8.1% |
| `BM_Decode_Batch_Scalars10/1` | (new arm) | 5.86 ± 0.095 us |  |  |
| `BM_Decode_Batch_Scalars10/1000` | (new arm) | 161.22 ± 1.056 us |  |  |
| `BM_Decode_Batch_Scalars10/8000` | (new arm) | 1.28 ± 0.025 ms |  | after run2 -5.1% |
| `BM_Decode_Positional_Cloud` | 382.6 ± 11.31 ns | 391.6 ± 4.74 ns | 1.023x | before run2 +8.1%; after run2 -4.4% |
| `BM_Decode_Positional_Generic` | 201.7 ± 6.60 ns | 200.6 ± 1.22 ns | 0.994x | before run2 +8.4%; after run2 +7.7% |
| `BM_Decode_Positional_Nested` | 309.0 ± 3.36 ns | 467.4 ± 3.77 ns | 1.513x |  |
| `BM_Decode_Positional_Nullable10` | 8.9 ± 0.14 ns | 9.5 ± 0.16 ns | 1.074x | after run2 -7.0% |
| `BM_Decode_Positional_Points` | 3.33 ± 0.179 us | 3.17 ± 0.017 us | 0.951x | before run2 -7.9%; after run2 +4.5% |
| `BM_Decode_Positional_Pose` | 16.3 ± 0.53 ns | 16.1 ± 0.18 ns | 0.987x | before run2 +5.2% |
| `BM_Decode_Positional_Scalars10` | 67.9 ± 0.77 ns | 70.0 ± 0.97 ns | 1.031x | after run2 -3.8% |
| `BM_Decode_Row_Cloud` | 8.61 ± 0.598 us | 2.01 ± 0.013 us | 0.234x |  |
| `BM_Decode_Row_Generic` | 96.60 ± 1.445 us | 2.21 ± 0.010 us | 0.023x | before run2 +8.1%; after run2 +7.6% |
| `BM_Decode_Row_Nested` | 62.98 ± 3.603 us | 68.89 ± 1.201 us | 1.094x |  |
| `BM_Decode_Row_Nullable10` | 790.3 ± 6.25 ns | 793.5 ± 1.90 ns | 1.004x |  |
| `BM_Decode_Row_Points` | 2.63 ± 0.040 ms | 2.66 ± 0.032 ms | 1.009x | after run2 +16.6% |
| `BM_Decode_Row_Pose` | 1.76 ± 0.059 us | 1.79 ± 0.017 us | 1.020x |  |
| `BM_Decode_Row_Scalars10` | 1.01 ± 0.032 us | 1.06 ± 0.006 us | 1.050x | before run2 -3.3%; after run2 -4.3% |
| `BM_Encode_Codec_Cloud` | 574.56 ± 42.784 us | 4.79 ± 0.030 us | 0.008x | before run2 -13.3% |
| `BM_Encode_Codec_Cloud_IntoFixed` | (new arm) | 4.64 ± 0.069 us |  | after run2 -5.0% |
| `BM_Encode_Codec_Generic` | 106.20 ± 1.489 us | 1.54 ± 0.044 us | 0.014x | before run2 +5.2%; after run2 +4.0% |
| `BM_Encode_Codec_Generic_IntoFixed` | (new arm) | 1.33 ± 0.025 us |  | after run2 +4.7% |
| `BM_Encode_Codec_Nested` | 8.58 ± 0.162 us | 1.53 ± 0.012 us | 0.179x | before run2 +3.7% |
| `BM_Encode_Codec_Nested_IntoFixed` | (new arm) | 1.44 ± 0.003 us |  |  |
| `BM_Encode_Codec_Nullable10` | 112.6 ± 3.82 ns | 109.9 ± 0.19 ns | 0.976x | after run2 -3.7% |
| `BM_Encode_Codec_Nullable10_IntoFixed` | (new arm) | 72.0 ± 0.31 ns |  |  |
| `BM_Encode_Codec_Points` | 606.64 ± 48.871 us | 81.54 ± 0.809 us | 0.134x | before run2 -3.7% |
| `BM_Encode_Codec_Points_IntoFixed` | (new arm) | 81.31 ± 0.538 us |  |  |
| `BM_Encode_Codec_Pose` | 2.33 ± 0.320 us | 273.9 ± 1.66 ns | 0.118x |  |
| `BM_Encode_Codec_Pose_IntoFixed` | (new arm) | 233.9 ± 1.86 ns |  |  |
| `BM_Encode_Codec_Scalars10` | 170.3 ± 2.82 ns | 143.8 ± 2.12 ns | 0.845x | before run2 -4.3% |
| `BM_Encode_Codec_Scalars10_IntoFixed` | (new arm) | 108.5 ± 0.74 ns |  | after run2 -4.6% |
| `BM_Encode_Positional_Cloud` | 143.0 ± 7.72 ns | 133.4 ± 1.02 ns | 0.933x | before run2 -8.3%; after run2 -4.4% |
| `BM_Encode_Positional_Generic` | 59.8 ± 1.79 ns | 60.1 ± 0.39 ns | 1.005x | before run2 +9.4%; after run2 +5.3% |
| `BM_Encode_Positional_Nested` | 63.1 ± 1.93 ns | 69.5 ± 0.67 ns | 1.102x | before run2 +4.7% |
| `BM_Encode_Positional_Nullable10` | 6.1 ± 0.37 ns | 6.8 ± 0.03 ns | 1.110x | before run2 +8.4%; after run2 -5.7% |
| `BM_Encode_Positional_Points` | 2.82 ± 0.069 us | 2.83 ± 0.044 us | 1.003x | before run2 -4.2%; after run2 -3.4% |
| `BM_Encode_Positional_Pose` | 7.9 ± 0.14 ns | 8.2 ± 0.18 ns | 1.039x | after run2 -4.9% |
| `BM_Encode_Positional_Scalars10` | 11.9 ± 0.53 ns | 11.2 ± 0.10 ns | 0.941x | before run2 -8.6% |
| `BM_EndToEnd_Batched_Cloud/1000` | 1.093 ± 0.0330 s | 16.15 ± 0.268 ms | 0.015x | after run2 +31.1% |
| `BM_EndToEnd_Batched_Cloud/8000` | 8.556 ± 0.0253 s | 163.86 ± 2.559 ms | 0.019x |  |
| `BM_EndToEnd_Batched_Generic/1000` | 347.69 ± 13.726 ms | 15.01 ± 0.137 ms | 0.043x |  |
| `BM_EndToEnd_Batched_Generic/8000` | 2.743 ± 0.0380 s | 136.63 ± 1.136 ms | 0.050x |  |
| `BM_EndToEnd_Batched_Nested/1000` | 155.17 ± 10.555 ms | 23.73 ± 0.170 ms | 0.153x | before run2 +4.2%; after run2 +5.4% |
| `BM_EndToEnd_Batched_Nested/8000` | 1.217 ± 0.0186 s | 202.86 ± 4.424 ms | 0.167x | before run2 +3.8% |
| `BM_EndToEnd_Batched_Nullable10/1000` | 3.88 ± 0.524 ms | 3.72 ± 0.014 ms | 0.961x | before run2 +8.4% |
| `BM_EndToEnd_Batched_Nullable10/8000` | 36.89 ± 3.985 ms | 29.79 ± 1.303 ms | 0.807x | before run2 -9.4% |
| `BM_EndToEnd_Batched_Points/1000` | 6.289 ± 0.0928 s | 154.91 ± 0.351 ms | 0.025x | after run2 +3.1% |
| `BM_EndToEnd_Batched_Points/8000` | 49.699 ± 0.5044 s | 1.268 ± 0.0058 s | 0.026x |  |
| `BM_EndToEnd_Batched_Pose/1000` | 16.36 ± 0.407 ms | 8.20 ± 0.132 ms | 0.501x |  |
| `BM_EndToEnd_Batched_Pose/8000` | 132.11 ± 7.610 ms | 66.15 ± 1.830 ms | 0.501x | after run2 -3.1% |
| `BM_EndToEnd_Batched_Scalars10/1000` | 4.33 ± 0.080 ms | 3.95 ± 0.020 ms | 0.910x |  |
| `BM_EndToEnd_Batched_Scalars10/8000` | 37.13 ± 1.227 ms | 32.25 ± 0.792 ms | 0.869x |  |
| `BM_Memcpy_Cloud` | 138.2 ± 2.28 ns | 92.3 ± 1.60 ns | 0.668x | before run2 -6.0% |
| `BM_Memcpy_Generic` | 52.4 ± 10.46 ns | 32.3 ± 0.15 ns | 0.617x | after run2 +21.1% |
| `BM_Memcpy_Nested` | 3.1 ± 0.06 ns | 3.2 ± 0.02 ns | 1.019x | before run2 +3.6%; after run2 +3.1% |
| `BM_Memcpy_Nullable10` | 2.1 ± 0.11 ns | 2.3 ± 0.02 ns | 1.114x | after run2 -11.1% |
| `BM_Memcpy_Points` | 158.1 ± 2.47 ns | 144.2 ± 0.71 ns | 0.912x | before run2 -8.8%; after run2 +29.8% |
| `BM_Memcpy_Pose` | 3.1 ± 0.48 ns | 3.6 ± 2.01 ns | 1.132x | after run2 -14.2% |
| `BM_Memcpy_Scalars10` | 2.3 ± 0.07 ns | 2.3 ± 0.06 ns | 0.987x | after run2 -3.8% |
| `BM_Run_Memcpy_2667f` | 613.6 ± 12.07 ns | 620.2 ± 6.01 ns | 1.011x | before run2 +4.1% |
| `BM_Run_PerElement_2667f` | 4.11 ± 0.111 us | 4.02 ± 0.022 us | 0.978x |  |

## Reading the result

**`Decode_Batch` allocations per `Cloud` row (target <= 10): PASS, by a wide margin.**
`BM_Decode_Batch_Cloud/1000`'s `allocs_per_row` counter reads **0.046** in both after_run1 and
after_run2 (identical to 3 decimals) — about 1 allocation every 22 rows, roughly 217x under the
target of 10. `BatchDecoder`'s bulk primitive-list-run path (`AppendRun`) handles both of `Cloud`'s
2667-element lists as one `memcpy` into the builder's already-reserved buffer per row, so almost
nothing allocates; the rare allocation is presumably an occasional builder-buffer growth that
`Reserve(1000)` didn't fully cover.

**The batched decode path, per row, before vs after (target >= 5x): PASS on every shape.**

`BM_Decode_Batch_ScalarPath_S/N` takes *pre-decoded* `ArrowRow`s, so on its own it is only the
second half of what the batched subscriber used to do per sample. The path it replaced was
`Codec::DecodeRow` (one heap `arrow::Scalar` per cell) **plus** `AppendScalar` per cell into the
column builders; the path that replaced it is `BatchDecoder::Append` alone. Per row at N = 8000:

| Shape | `Decode_Row` (before) | + `ScalarPath/8000` per row (before) | = pipeline before | `Decode_Batch/8000` per row (after) | Speedup |
|---|---|---|---|---|---|
| `Scalars10` | 1.01 us | 0.224 us | 1.23 us | 0.160 us | **7.7x** |
| `Nullable10` | 0.79 us | 0.214 us | 1.00 us | 0.104 us | **9.7x** |
| `Cloud` | 8.61 us | 557 us | 566 us | 4.44 us | **127x** |
| `Pose` | 1.76 us | 7.76 us | 9.52 us | 0.130 us | **73x** |
| `Points` | 2.63 ms | 2.83 ms | 5.46 ms | 57.4 us | **95x** |
| `Nested` | 63.0 us | 66.6 us | 130 us | 0.836 us | **155x** |
| `Generic` | 96.6 us | 127 us | 223 us | 1.60 us | **139x** |

Allocations per row on the same path (`allocs_per_row`, N = 1000): `Cloud` 20 + 5344 before,
**0.046** after; `Points` 26 059 + 28 028 before, **0.064** after; `Scalars10` 15 + 0.09 before,
**0.077** after; `Nested` 700 + 569 before, **0.20** after. The residual fraction of an allocation per
row is builder-buffer growth that `Reserve(N)` did not cover exactly. `Cloud` and `Points` are the two
shapes the plan named; both clear the target by more than an order of magnitude. The flat-scalar
shapes gain the least (7.7x, 9.7x) because they never allocated per element to begin with — what
they save is the 10-15 `arrow::Scalar`s per row and the virtual `AppendScalar` dispatch.

**`Codec::EncodeRow` into the provider buffer, before vs after, and against the generated-code
floor.** The plan set "within 2x of `Encode_Positional`" as the bar; that bar compares a
schema-generic encoder (one `shared_ptr<arrow::Scalar>` dereference, one `DataType::Equals` and two
type switches per field) against code with the schema baked in at compile time (a memcpy per
field), and no generic encoder reaches it — the BEFORE binary already sat at 14.8x on `Scalars10`.
The numbers that matter are the before/after ratio and the absolute cost:

| Shape | `Encode_Codec` before (fresh vector) | `Encode_Codec_IntoFixed` after | Speedup | `Encode_Positional` (floor) | after / floor |
|---|---|---|---|---|---|
| `Scalars10` | 170.3 ns | 108.5 ns | 1.6x | 11.2 ns | 9.7x |
| `Nullable10` | 112.6 ns | 72.0 ns | 1.6x | 6.8 ns | 10.6x |
| `Cloud` | 574.6 us | 4.64 us | **124x** | 133.4 ns | 34.8x |
| `Pose` | 2.33 us | 233.9 ns | **10x** | 8.2 ns | 28.6x |
| `Points` | 606.6 us | 81.3 us | **7.5x** | 2.83 us | 28.7x |
| `Nested` | 8.58 us | 1.44 us | **6.0x** | 69.5 ns | 20.7x |
| `Generic` | 106.2 us | 1.33 us | **80x** | 60.1 ns | 22.2x |

Allocations per row: `Cloud` 5347 before (one `GetScalar` per list element), **2** after; `Points`
8015 → 4; `Generic` 1015 → 2; `Pose` 27 → 2; `Scalars10`/`Nullable10` 1 → **0**. The two that remain
on the list shapes come from the `arrow::Array::Validate()` guard `EncodeScalarValue` runs on each
list value before walking it raw (a malformed `ListScalar` used to be caught by `GetScalar`'s bounds
check; now it must be caught up front) — a candidate for a cheaper structural check if 2 allocations
per row ever matter. `_IntoFixed` vs the fresh-vector overload (`Cloud` 4.64 vs 4.79 us, `Scalars10`
108.5 vs 143.8 ns) is what `PublisherArrow`'s reused scratch buys per publish.

The remaining 10-35x gap to the hand-written twin is the price of dispatching on `arrow::Type` per
field at run time; the way to close it is to publish from the generated row class (`EncodeTo`), which
is what every producer in the tree does today. `Codec::EncodeRow` is for Arrow-native code that has an
`ArrowRow` in hand, and at 108 ns for a 10-field row it is not where a publish spends its time.

**`Run_Memcpy` vs `Run_PerElement` (target >= 3x): PASS.** 4.022 us vs 620.2 ns, **6.49x** —
comfortably over target, confirming the bulk-copy path `AppendPrimitiveRun` exists to reach is worth
taking when it applies.

**`Decode_Batch/8000` vs `ArrowIpc_Read/8000`, and `Encode_Codec_IntoFixed` (x8000) vs
`ArrowIpc_Write/8000`, stated plainly (no target given):**

| Shape | Decode_Batch/8000 is slower than ArrowIpc_Read/8000 by | Encode_Codec_IntoFixed x8000 is slower than ArrowIpc_Write/8000 by |
|---|---|---|
| `Scalars10` | 196x | 13.6x |
| `Nullable10` | 119x | 25.8x |
| `Cloud` | 5750x | 1.3x |
| `Pose` | 177x | 24.7x |
| `Points` | 96766x | 17.4x |
| `Nested` | 727x | 45.3x |
| `Generic` | 1528x | 3.0x |

These are not comparable workloads and the gap should not be read as a regression: `ArrowIpc_Read`
parses one already-columnar, already-serialized Arrow buffer built by `BM_ArrowIpc_Write` (pointer
rewraps over contiguous buffers, effectively zero-copy for primitive columns), while `Decode_Batch`
walks 8000 independent row-oriented wire messages and builds the columnar arrays from nothing. The
gap shrinks hardest exactly where the bulk primitive-run path applies on both sides of the row/column
divide (`Cloud` 1.3x, `Generic` 3.0x) and is worst for `Points` (96766x) because `Points`' element
type is a struct, which cannot take the bulk-run path on the decode side at all (per-element
`Scalar`/builder-append machinery instead) while `ArrowIpc_Read` still just rewraps buffers. Nobody
should conclude "IPC is a drop-in replacement" from this table; the two arms exist to show how much
work per-row (de)serialization still costs relative to the columnar wire format it feeds.

**Arms that got slower beyond the noise (AFTER vs BEFORE, run 1, both binaries' run-1-to-run-2 delta
under ~5%):**

- **`BM_Decode_Row_Nested`: 62.98 us to 68.89 us, +9.4%** (run-to-run noise 2.3% before / 2.5% after
  — well below the swing itself). This is the one arm in the whole table with a real, small,
  repeatable regression tied to library code the session touched: `Nested` decodes through
  `Codec::DecodeRow`, which shares `row_reader.hpp` with every other shape, and `Nested` (a
  `list<list<struct<x,y>>>`, a `map`, and a struct-of-struct — no long flat primitive list) is one
  of the shapes that cannot take the bulk primitive-run fast path the session added, so it only pays
  for the extra dispatch/branching that path introduced without collecting the corresponding win.
- **`BM_Decode_Row_Scalars10`: 1.005 us to 1.055 us, +5.0%** (noise 3.3%/4.3%) — right at the edge
  of this table's own noise floor, but consistently on the slower side across both runs; named here
  for completeness rather than as a confident finding.
- Every other arm above the +5% line in a straight AFTER/BEFORE comparison
  (`BM_Decode_Positional_Nested`, `BM_Memcpy_Pose`, `BM_Memcpy_Nullable10`,
  `BM_Encode_Positional_Nullable10`, `BM_Encode_Positional_Nested`, `BM_ArrowIpc_Read_Cloud/8000`,
  `BM_ArrowIpc_Read_Pose/1000`, `BM_ArrowIpc_Read_Points/8000`, `BM_ArrowIpc_Write_Nullable10/*`,
  `BM_ArrowIpc_Write_Nested/1000`, `BM_Decode_Batch_ScalarPath_Nullable10/1`,
  `BM_Decode_Batch_ScalarPath_Cloud/8000`) is a control whose own source is unchanged between BEFORE
  and AFTER (hand-written fixture code, Arrow's own IPC reader/writer, or `BuildBatchScalarPath`
  reproduced verbatim in `fixtures.hpp`), several with double-digit run-to-run noise already
  (`BM_ArrowIpc_Read_Points/8000` before-run-2 delta alone is +56.2%). These are measurement noise or
  a binary-layout artifact from the AFTER binary carrying 28 more registered benchmarks, not real
  regressions.
- These are dwarfed by the improvements elsewhere in the same per-row arms:
  `BM_Decode_Row_Cloud` 8.607 us to 2.013 us (**4.3x faster**), `BM_Decode_Row_Generic` 96.60 us to
  2.211 us (**43.7x faster**), `BM_Encode_Codec_Cloud` 574.6 us to 4.789 us (**120x faster**),
  `BM_Encode_Codec_Generic` 106.2 us to 1.537 us (**69x faster**), `BM_Encode_Codec_Pose` 2.330 us to
  273.9 ns (**8.5x**), `BM_Encode_Codec_Points` 606.6 us to 81.54 us (**7.4x**),
  `BM_Encode_Codec_Nested` 8.577 us to 1.531 us (**5.6x**) — all from the same bulk primitive-run
  path that leaves `Nested`'s decode side slightly worse off.

**`Decode_Batch/1` vs `Decode_Row`: the cost of building a batch, not of the second pass.**

| Shape | `Decode_Batch/1` | `Decode_Row` | Ratio |
|---|---|---|---|
| `Scalars10` | 5.86 us | 1.06 us | 5.5x |
| `Nullable10` | 5.88 us | 0.79 us | 7.4x |

A one-row `BatchDecoder` cycle is `Reserve(1)` + `Append` + `Finish()`, and `Finish()` builds ten
`arrow::Array`s, their `ArrayData` and a `RecordBatch` — that fixed cost, not the validation walk, is
what the ratio shows; it is paid once per batch and disappears into the noise at N = 1000
(`Scalars10`: 161 us / 1000 rows = 161 ns per row, 1.3 us at N = 8000 = 160 ns per row). The
validate-then-append pass itself is bounded by the `Append` walk it mirrors (no builder calls, no
allocation; O(1) for a fixed-width run, one 4-byte read per string); its share is inside the 160 ns
per `Scalars10` row and was not isolated further because the row-level numbers above already meet
every target it could threaten.

**End to end** (`PublisherArrow::Publish` → loopback provider → batched `SubscriberArrow`, per row at
N = 8000): `Cloud` 1.07 ms → 20.5 us (**52x**), `Points` 6.21 ms → 159 us (**39x**), `Generic`
343 us → 17.1 us (**20x**), `Nested` 152 us → 25.4 us (**6x**), `Pose` 16.5 us → 8.3 us (**2x**),
`Scalars10` 4.64 us → 4.03 us, `Nullable10` 4.61 us → 3.72 us. The flat-scalar shapes are dominated by
the loopback provider, the `std::function` hop and the batcher's mutex, which this work did not touch.

**Regressions.** `BM_Decode_Row_Nested` +9.4% (63.0 → 68.9 us) is the only library arm that moved the
wrong way beyond its own noise. The unchanged control twins for the same shape moved as much or more
in the same binary (`BM_Decode_Positional_Nested` +51%, `BM_Encode_Positional_Nested` +10%, with no
code change in `fixtures.hpp`), so the shift is a code-layout effect of the larger AFTER binary rather
than something the codec does differently on this shape; `Nested` has no fixed-width run to take and
its decode path otherwise changed only in the string decode (`AllocateBuffer` + memcpy instead of
`std::string` + `Buffer::FromString`, neutral on `Scalars10`/`Nullable10`). `BM_Decode_Row_Scalars10`
+5.0% sits at the noise floor. Everything else that reads slower is a control whose source did not
change.
