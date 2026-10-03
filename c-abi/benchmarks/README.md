# Benchmarks — the per-row publish path (BIND-4d-v)

The benchmark BIND-4's acceptance asks for: **the managed publish path against the C++ generated
publisher over `inprocess`, allocations per row in the table** (risk **B-2**, D-BIND-37). Its shape
is **D-BIND-48** in `plans/BIND-locked-decisions.md`: two harnesses, run by hand, timings on
Windows, allocation counts on Linux.

| Harness | Where | Measures |
|---|---|---|
| `bench_publish` (Google Benchmark) | this directory | C1–C4: the generated C++ publisher, and the shim driven from C++ |
| `Fletcher.Benchmarks` (BenchmarkDotNet) | `dotnet/benchmarks/Fletcher.Benchmarks` | M1–M4: the managed `Publisher`, public surface only |

Neither is part of a package or of CI. This directory is outside `c-abi`'s `exports_sources` and is
built with `conan build .`, like `arrow-bridge/benchmarks`; the managed project is outside
`Fletcher.slnx` for the reason the integration suites are.

## One row, one schema, one topic — and the check that holds it

Every arm, C++ and C#, publishes the same row of
`integration-tests/protoc-arrow-bridge/proto/pubsub.proto`'s `Telemetry`
(`device_id = 42`, `value = 3.14`, `timestamp = 1700000000000`, `metric_name = "cpu.temperature"`)
to the generated publisher's own topic, `integration.pubsub/TelemetryFeed/TelemetryStream`, over an
`inprocess` provider **with no subscriber**.

**Both harnesses validate before they time anything** and exit non-zero on a mismatch:
`bench_publish` requires C1 and C2 to put byte-identical payloads on the wire, and both harnesses
require those bytes to equal one pinned constant (`kCanonicalRowHex` in `bench_publish.cpp`,
`Telemetry.CanonicalRowHex` in C#; 40 bytes). The C# schema is hand-written — there is no generated
C# before BIND-6 and no `Telemetry` IPC golden — and that pinned constant is what makes it safe: a
schema or value that drifted would stop the run rather than produce an incomparable column.

## Arms

| Arm | What it is | Native half of |
|---|---|---|
| **C1** | generated `TelemetryFeed_TelemetryStreamPublisher::Publish(row)` — **the baseline** | — |
| **C2** | `fl_publisher_publish_row` over a pre-bound 1024-row batch, from C++ | M1 |
| **C3** | `fl_rows_bind` → `fl_publisher_publish_row` → `fl_rows_unbind` on a one-row array (built once) | M2 |
| **C4** | `fl_publisher_publish_rows` over the 1024-row batch, reported per row | M3 |
| **M1** | `Publisher.Publish(topic, rows, i)` over a pre-bound 1024-row batch — the fused per-row path | |
| **M2** | one-row `RecordBatch` → `FletcherCodec.Bind` → `Publish` → dispose — **B-2 as written**: a lone row paying for an Arrow array | |
| **M2a** | M2's batch build and dispose alone, no bind, no publish — a *decomposition* of M2 | |
| **M2b** | M2a plus the C Data Interface export and its release, exactly as `ExportedArray` does them — still no Fletcher; the second decomposition of M2 | |
| **M3** | `Publisher.Publish(topic, rows)` over the 1024-row batch, per row — the batch mitigation B-2 names | |
| **M4** | `Publisher.Publish(topic, RowWriter, 40)` copying the canonical bytes — the **floor of the D-BIND-1 alternative** (generated C# writing wire bytes, with the encoding replaced by a copy) | |

C3, C4, M2a, M2b and M4 were added while implementing, and D-BIND-48 records them apart from what was
ruled.

**What cancels and what does not.** `InProcessPubSubProvider::Publish` allocates a `VectorWriteBuffer`
and a joined key on every call, on every arm — that cost is the provider's and must not be read as
the binding's. What does **not** cancel is the topic: the generated publisher's `TopicSegments()` is
a function-local `static`, while the shim's `ToSegments` rebuilds a `std::vector<std::string>` from
the `fl_topic` on every `fl_publisher_publish_row`. That asymmetry is B-2.

## Allocation counting

`alloc_count.c` is an `LD_PRELOAD`ed `malloc`-family interposer with a **thread-local** counter and
one export, `fbench_alloc_count()`, which **both** harnesses read — `dlsym` from C++, an unmanaged
function pointer from C#. One instrument, so the C++ and C# columns count the same thing. It sees
libstdc++'s `operator new`, nanoarrow's direct `malloc`s, Apache.Arrow's `NativeMemory` and the
runtime's interop stubs; it does not count `free`. Each arm is warmed (20 000 calls in C#, well past
tier-0) and then sampled over 1 000 calls outside the timed loop.

**Linux only, deliberately** (D-BIND-48): the shim links the CRT dynamically (D-BIND-38), so on
Windows an executable cannot see allocations made inside the DLL. There the column is **absent
("n/a")**, never 0.

Managed allocations come from BenchmarkDotNet's `MemoryDiagnoser` (`Allocated`), and from
`GC.GetAllocatedBytesForCurrentThread` in `--counts-only` mode.

## Running

**Windows — timings.** The shim path is the `fletcher-c-abi` package folder from the build's own
generator output (`build/generators/fletcher-c-abi-release-x86_64-data.cmake`,
`fletcher-c-abi_PACKAGE_FOLDER_RELEASE`), never a search of the cache.

```bat
cd c-abi\benchmarks
conan install . -pr:a=..\..\.conan-profiles\Windows-msvc194-x86_64-Release -r conancenter --build=missing
conan build . -pr:a=..\..\.conan-profiles\Windows-msvc194-x86_64-Release -r conancenter
set PATH=<shim package>\bin;%PATH%
build\Release\bench_publish.exe --benchmark_min_time=0.3s --benchmark_repetitions=7 --benchmark_report_aggregates_only=true

cd ..\..\dotnet\benchmarks\Fletcher.Benchmarks
dotnet build -c Release -p:FletcherNativeShim=<shim package>\bin\fletcher-c-abi.dll
bin\Release\net10.0\Fletcher.Benchmarks.exe --filter *
```

`-r conancenter` because `benchmark/1.9.4` comes from ConanCenter and `conan-eiva` wants
credentials; the `fletcher-*` packages must already be in the local cache (`conan create` each, as
`ci.dotnet.yml` does, plus `protoc`).

BenchmarkDotNet runs **in-process** (`Program.cs` says why: its default child-process toolchain
rebuilds without `-p:FletcherNativeShim`, so nothing stages the shim).

**Linux — counts.** In the devcontainer image, with the components created as `ci.dotnet.yml` does
(plus `protoc`):

```bash
LD_LIBRARY_PATH=<shim package>/lib LD_PRELOAD=build/Release/libfbench_alloc_count.so \
    build/Release/bench_publish --benchmark_min_time=0.2s
LD_PRELOAD=<...>/libfbench_alloc_count.so \
    dotnet/benchmarks/Fletcher.Benchmarks/bin/Release/net10.0/Fletcher.Benchmarks --counts-only
```

`--counts-only` runs each managed arm's setup (where the sampling happens), prints native and
managed allocations per row, and exits without timing anything.

**Measurement discipline**, as `arrow-bridge/benchmarks`: medians with standard deviations, the same
tree run twice back to back on an otherwise idle machine, and a run-to-run delta over 3% named.

## Results

**Sources:** `cba173c` plus this directory and `dotnet/benchmarks/`, uncommitted at the time.
**Shim:** `fletcher-c-abi/0.5.0-alpha#096ac2427aba078fd3699a9af5ff1383` — the recipe revision
exported 2026-09-24, built by this directory's `conan install --build=missing`; `c-abi`'s sources
have not changed since `b46391f` (2026-09-22). **Windows (timings):** Intel Core Ultra 7 265HX,
Windows 11, MSVC 19.44, .NET 10 (SDK 10.0.400), BenchmarkDotNet 0.15.8 in-process, Google
Benchmark 1.9.4 with `UseRealTime()`. **Linux (counts, and C++ timings):** the devcontainer image
(Ubuntu 24.04, gcc 13) under Docker Desktop on the same machine, shim built there from the same
sources.

Every Windows arm was run twice back to back on an otherwise idle machine — C++: 7 repetitions of
`--benchmark_min_time=0.3s`, median ± stddev; C#: BenchmarkDotNet's default job, Mean ± StdDev. The
full set on **2026-09-24**; M2, M2a and M2b again together on **2026-09-25**, when M2b was added, so
that the three arms that decompose M2 come from one session. Run 1 is tabulated; the run-2 delta is
shown where it exceeds 3%.

| Arm | Time per row, Windows (run 1) | vs C1 | Native allocs/row (Linux) | Managed B/row | Run-2 delta |
|---|---|---|---|---|---|
| **C1** generated C++ | **194 ± 1.9 ns** | 1.00× | **4** | — | +2.6% |
| C2 shim per row, from C++ | 298 ± 5.2 ns | 1.54× | 7 | — | +2.5% |
| C3 shim bind + publish + unbind, from C++ | 739 ± 16.0 ns | 3.81× | 15 | — | **+3.2%** |
| C4 shim batch, from C++, per row | 226 ± 6.4 ns | 1.17× | 5.002 | — | +0.1% |
| **M1** managed fused per row | **378 ± 2.3 ns** | **1.95×** | 7 | 0 | +1.8% |
| **M2** managed one-row batch per publish | **3 793 ± 92 ns** | **19.6×** | 31 | 3 665 | −0.7% |
| M3 managed batch, per row | **239 ± 2.2 ns** | **1.23×** | 5.002 | 0 | +1.2% |
| M4 managed `RowWriter` floor | 377 ± 2.0 ns | 1.94× | 7 | 32 | −1.2% |
| *2026-09-25 session:* M2 | 3 838 ± 34 ns | | 31 | 3 665 | −0.8% |
| *2026-09-25 session:* M2a batch build only | 1 224 ± 25 ns | | 5 | 2 488 | +1.6% |
| *2026-09-25 session:* M2b build + export only | 2 730 ± 18 ns | | 16.02 | ~3 540 | **−7.9%** |

M2 reads 3 793 ns in one session and 3 838 in the other (1.2% apart), which is what licenses
subtracting the 2026-09-25 decomposition from the 2026-09-24 table. **M2b's run-to-run swing (2 730
→ 2 513 ns) is the largest in the set**, so its share below is given as a range.

**Linux C++ timings** (5 repetitions, one pass — the counts were what the Linux leg was for):
C1 **85.9 ± 0.5**, C2 **144 ± 4.5**, C3 **524 ± 4.5**, C4 **106 ± 1.8** ns per row — the same
ordering as Windows at under half the absolute cost; C2 is 1.68× C1 there against 1.54× on Windows.
No managed timings were taken on Linux.

BenchmarkDotNet reports Gen0/Gen1/**Gen2** collections on M2 (0.46 / 0.23 / 0.23 per 1 000
operations), **M2b (0.45 / 0.23 / 0.23)** and M2a (0.31 / 0.15 / 0.15). No other arm collects.

**Two instruments, two managed figures.** The Managed column is BenchmarkDotNet's `Allocated`. The
harness's own `--counts-only` mode, which reads `GC.GetAllocatedBytesForCurrentThread` around the
same 1 000 sampled calls, reads 1–2% higher (M2 3 736, M2a 2 520, M2b 3 616) — that counter is
documented as approximate, and it is only ever used here for the Linux leg, where BenchmarkDotNet
does not run. The two agree exactly on the arms that allocate nothing or a fixed object (M1, M3: 0;
M4: 32).

The C++ `per_row` figures from the first recorded pass were CPU-time rates and were discarded: the
Windows thread CPU clock quantised C1 to exactly 188.918 ns in both runs. The arms have used
`UseRealTime()` since.

## Reading the result

**The layers, attributed.** Per row, on Windows:

| Step | Cost | Measured as |
|---|---|---|
| The shim's native layer over generated C++ | +104 ns | C2 − C1 |
| …of which hoisting `ToSegments` and the call out of the loop buys back | −72 ns | C2 − C4 |
| The managed crossing (topic marshalling, `SafeHandle` ref, P/Invoke) | +80 ns | M1 − C2 |
| A per-row bind, natively | +441 ns | C3 − C2 |
| **M2, decomposed:** building a one-row `RecordBatch` — Apache.Arrow | 1.2 µs | M2a |
| …exporting it through the C Data Interface — Apache.Arrow | 1.3–1.5 µs | M2b − M2a |
| …everything Fletcher does: bind, publish, unbind, wrappers | 1.1–1.3 µs | M2 − M2b |
| ……of which native (bind, publish, unbind) | ~0.74 µs | C3 |
| ……of which Fletcher's managed `Bind`/`BoundRows` wrapper | ~0.4–0.55 µs | the remainder |

**Native allocations reconcile exactly, on every managed arm.** M1 = C2 = 7, M3 = C4 = 5.002, and
M2 − M2b = 31 − 16 = **15 = C3**: Fletcher's managed layer adds **no** native allocation on any path,
so every native allocation it causes is the shim's. M2's 31 is therefore 5 (Apache.Arrow's buffers,
M2a) + 11 (Apache.Arrow's export, M2b − M2a) + 15 (the shim, C3). Over C1's 4 (the provider's
buffer, its growth and the joined key, paid by every arm), C2 adds 3: **2 are `ToSegments`** — the
vector, and `"integration.pubsub"`, 18 bytes, past libstdc++'s 15-byte small-string buffer — and
**1 is the shim's own encoder lambda**, which captures three references (24 bytes) where
libstdc++'s `std::function` stores 16 inline. That last one is attributed by reading `binding.cpp`,
not measured separately; C4 − C1 = 1 is consistent with it (C4 hoists `ToSegments` but still wraps
the lambda per row), and MSVC's larger inline buffer means Windows probably does not pay it.

**What B-2 actually is, now that it is measured.** The per-row *fused* path (M1) is 1.95× the
generated C++ publisher, 378 ns against 194 — about 2.6 M rows/s — with zero managed allocation. The
batch path (M3) is 1.23×. **The cost B-2 was worried about is not `ToSegments`** (72 ns) and not the
crossing (80 ns); it is **M2: a row that does not start out as Arrow pays 19.6× — 3.8 µs, 3.6 KB
managed, 31 native allocations and Gen2 collections.** About **two-thirds of that is Apache.Arrow**
— building the array and exporting it, and **all** of the Gen2 collections, which M2b shows with no
Fletcher code in the loop. About one-third is Fletcher, and most of that is the native bind.
**No change inside Fletcher removes the larger part**; only a path that never builds an Arrow array
does.

**What M4 says about D-BIND-1.** M4 — generated C# writing wire bytes, with the encoding replaced by
a copy — costs the same as M1 (377 vs 378 ns). So **for a row that is already Arrow, a managed
encoder would buy nothing**: the crossing, not the codec, is the per-row cost. **For a row that is
not Arrow, it would buy ~10×** (M2 → roughly M4 plus a managed encode of four fields), because it
never builds the array. That is the case BIND-6's generated `<Service>_<Method>Publisher.Publish(T)`
would be in if it went through `ToArrow` one row at a time.

**Cheaper remedies than D-BIND-1, in order of cost, none of them taken here:**
1. **The shim's encoder lambda** — capture one pointer instead of three references. Internal, no
   ABI change; saves one allocation per publish on Linux.
2. **A pre-converted topic handle** (`fl_topic` opened once, as `fl_codec_open` is) — a pure ABI
   addition, which D-BIND-32's performance paragraph already names as the fix for B-2. Worth at
   most the 72 ns C2 − C4 shows.
3. **Keep generated C# publishing batch-first** — BIND-6's generated publisher accumulating rows
   into one bound batch rather than binding per row moves it from M2 to M3.

None of these reaches M2's lone-row case, whose larger part is Apache.Arrow's own.

**Verdict — D-BIND-49 (maintainer, 2026-09-25): ACCEPTED. No D-BIND-1 STOP-AND-ASK is raised and
B-2 closes.** M1 and M3 are acceptable as measured; M2 is carried to BIND-6 as a design input —
the generated publisher's batch form (`Publish(IEnumerable<T>)`, the M3 shape) is the cheap one, and
`Publish(T)` documents its per-row price. Remedies 1 and 2 were not taken and remain open to a later
round on their own merits.

