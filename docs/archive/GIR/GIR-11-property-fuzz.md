# GIR-11 — Property + Fuzz Test Strategy

## Summary

GIR-11 adds the final bounded codec robustness coverage item: one deterministic round-trip property test for valid Arrow rows and one deterministic malformed-buffer fuzz harness for `Codec::DecodeRow`.

This is codec-focused coverage, not persistent fuzzing infrastructure. It exercises the Phase-1 hardened decode path in `arrow-bridge/src/codec.cpp` and `arrow-bridge/src/row_reader.hpp`, especially overflow-safe `Reader::Read`, `Reader::ReadBytes`, count guards, nested decode, and top-level full-buffer-consumption rejection.

Locked scope:

- No wire-format change. Valid buffers must remain byte-identical.
- No RBA accessor rewrite.
- If fuzz exposes a crash/UB, fix decode robustness to reject gracefully.
- If a proposed fix changes valid or previously-working wire decode, stop and ask.

## Design

Add a new unit test TU:

```text
arrow-bridge/tests/test_codec_property_fuzz.cpp
```

Wire it into the existing `arrow_bridge_tests` executable in `arrow-bridge/tests/CMakeLists.txt`. Do not put GIR-11 in the protoc coverage harness: that harness validates generated-code fixtures, while GIR-11 targets the runtime `Codec::EncodeRow` / `Codec::DecodeRow` component directly.

The test file should define a small deterministic schema corpus rather than reusing generated coverage fixtures. The coverage fixtures are useful for generated emitter parity, but a local randomized generator is better here because it can produce many valid Arrow scalar combinations without depending on protoc output.

Recommended schema corpus (unions omitted from property generation; see note below):

```cpp
std::vector<std::shared_ptr<arrow::Schema>> PropertySchemas() {
    return {
        arrow::schema({
            arrow::field("b", arrow::boolean(), true),
            arrow::field("i32", arrow::int32(), true),
            arrow::field("i64", arrow::int64(), true),
            arrow::field("u64", arrow::uint64(), true),
            arrow::field("f32", arrow::float32(), true),
            arrow::field("f64", arrow::float64(), true),
            arrow::field("s", arrow::utf8(), true),
            arrow::field("bin", arrow::binary(), true),
            arrow::field("date", arrow::date32(), true),
            arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO), true),
            arrow::field("dur", arrow::duration(arrow::TimeUnit::MICRO), true),
            arrow::field("dec", arrow::decimal128(18, 4), true),
        }),
        arrow::schema({
            arrow::field("st", arrow::struct_({
                arrow::field("x", arrow::int32(), true),
                arrow::field("name", arrow::utf8(), true),
            }), true),
            arrow::field("ints", arrow::list(arrow::int32()), true),
            arrow::field("strings", arrow::list(arrow::utf8()), true),
            arrow::field("fixed", arrow::fixed_size_list(arrow::float32(), 3), true),
            arrow::field("map", arrow::map(arrow::utf8(), arrow::int32()), true),
        }),
    };
}
```

**Union omission from property corpus.** Dense and sparse unions are comprehensively covered by GIR-10's `CodecEdge.Dense/SparseUnion*` tests, which exercise both variants with scalar and variable-length children, and include the concrete error paths (bad type-code rejection). Omitting unions from the property generator keeps it simpler and removes the risk of false-failure: a sparse-union scalar with non-null inactive children would not round-trip (the codec's decode nulls all inactive children), causing `Equals` to fail on a semantically valid row. Rather than complicate `RandomScalar` with union-specific construction rules (`SparseUnionScalar::FromValue(...)`), GIR-11 stays focused on the type-agnostic `Reader` guards and container-nesting determinism, while GIR-10 owns exhaustive union family coverage.

Random valid row generation:

- Implement `RandomScalar(type, rng, depth)` in the new test TU.
- Support nullable top-level fields by returning `arrow::MakeNullScalar(type)` with a fixed probability, e.g. 20%.
- Cap recursive container sizes:
  - lists: 0..8 elements
  - fixed-size lists: schema size only
  - maps: 0..6 entries
  - structs: exactly schema children
  - recursive depth: cap at 3
- Use finite float/double values only in the property test so Arrow `Scalar::Equals` is the value oracle. NaN, infinities, and `-0.0` are already bit-exactly covered by GIR-10.
- Generate strings/binary from a deterministic small alphabet including empty strings, embedded NULs, and multi-byte UTF-8.
- Generate decimal values within declared precision/scale.

Property statement:

```text
For every generated valid row:
  encoded1 = codec.EncodeRow(row)
  decoded = codec.DecodeRow(encoded1)
  decoded.size() == row.size()
  decoded[i]->Equals(*row[i]) for every field i
  encoded2 = codec.EncodeRow(decoded)
  encoded2 == encoded1
```

The primary GIR-11 property is value equality after encode/decode. The encode/decode/encode byte equality is an additional regression guard that helps catch nondeterministic decode reconstruction without redefining the wire.

Test name:

```cpp
TEST(Property, EncodeDecodeRoundTrip)
```

Determinism and CI safety:

```cpp
constexpr uint64_t kPropertySeed = 0x47495231315F5052ULL;  // "GIR11_PR"
constexpr int kPropertyIterationsPerSchema = 128;
```

Use `std::mt19937_64 rng(kPropertySeed)`. On every assertion failure, print:

- seed
- schema index
- iteration
- schema `ToString()`
- encoded bytes as hex

Use `SCOPED_TRACE` around each generated case.

**Cross-platform RNG portability note.** `std::uniform_int_distribution` (and distributions in general) are not portable across standard library implementations (MSVC vs. libstdc++), so the same seed may yield different sequences on different platforms. This is not flakiness — each platform is internally deterministic and bounded — but a test failure on Windows may not reproduce the identical case on Linux CI. Mitigate by having the property test's failure output include the encoded bytes of the failing row as hex (in addition to the seed). This allows any failing case to be replayed directly via `DecodeRow(hex_buffer)` without requiring the RNG to produce identical bit-for-bit sequences across toolchains.

Decode fuzz harness:

```cpp
TEST(Fuzz, DecodeRowSurvivesRandomTruncatedBuffers)
```

The fuzz harness should use the same schema corpus plus a small set of valid encoded seed rows from the property generator. It should produce malformed inputs through three deterministic strategies:

1. Random garbage:
   - lengths 0..256
   - bytes from `std::uniform_int_distribution<int>(0, 255)`

2. Truncated valid rows:
   - for each valid encoded seed row, feed every prefix length from 0 to `row.size() - 1`
   - include exact valid row as a sanity check

3. Corrupted valid rows:
   - single-bit flips at deterministic positions
   - byte overwrites at deterministic positions
   - multi-byte count/length corruption, especially `0xFF 0xFF 0xFF 0xFF` where the buffer is long enough
   - append trailing bytes to valid rows to exercise full-consumption rejection

Bounded counts:

```cpp
constexpr uint64_t kFuzzSeed = 0x47495231315F465AULL;  // "GIR11_FZ"
constexpr int kValidSeedsPerSchema = 32;
constexpr int kRandomGarbageCasesPerSchema = 256;
constexpr int kBitFlipCasesPerSchema = 256;
```

Assertions:

```text
DecodeRow(buffer) must not crash, hit UB, or read out of bounds.
It may:
  - succeed, in which case decoded.size() must equal schema.num_fields()
  - throw std::invalid_argument
It must not:
  - throw another exception type
  - abort (e.g., from .ValueOrDie() on schema-derived validation failures)
  - allocate/loop pathologically from corrupt LEN/COUNT
```

In code, structure each attempt as:

```cpp
try {
    auto decoded = codec.DecodeRow(buf.data(), buf.size());
    EXPECT_EQ(decoded.size(), static_cast<size_t>(schema->num_fields()));
} catch (const std::invalid_argument&) {
    SUCCEED();
} catch (const std::exception& e) {
    FAIL() << "DecodeRow threw non-invalid_argument exception: " << e.what()
           << " (seed: 0x" << std::hex << kFuzzSeed << "; variant: " << seed_variant
           << "; case: " << case_index << "; buffer: " << HexDump(buf) << ")";
} catch (...) {
    FAIL() << "DecodeRow threw a non-std::exception type (seed: 0x" << std::hex
           << kFuzzSeed << "; variant: " << seed_variant << "; case: " << case_index
           << "; buffer: " << HexDump(buf) << ")";
}
```

The terminal `catch(...)` block ensures that any unexpected exception type is caught and reported as a test failure, rather than escaping uncaught or masking the real exception. Both failure branches dump the offending `buf` as hex (the same `HexDump` helper the property test uses) in addition to the seed/variant/case coordinates — because the random-garbage strategy shares the non-portable `uniform_int_distribution` RNG (see the portability note above), the hex dump is the only repro key that replays identically across toolchains; seed+variant+case alone does not. Note that `abort()` from `.ValueOrDie()` in decode cannot be caught; if one fires, it is an intended crash signal indicating a decode robustness defect that must be fixed separately (the failing buffer is recoverable from the sanitizer/crash backtrace and the deterministic case coordinates).

Run this suite under the project's normal sanitizer configuration when available. The test itself stays deterministic and bounded; sanitizer detection is the UB/OOB-read oracle.

Bug-handling protocol:

- If fuzz finds a crash, sanitizer finding, timeout, or non-`std::invalid_argument` exception:
  - classify the decode robustness defect, e.g. missing bounds check, unsafe length/count guard, narrowing, invalid Arrow builder assumption, or missing type-code validation
  - add or keep the minimal failing input as a red-first regression case in `test_codec_property_fuzz.cpp` or `test_codec_edge.cpp`
  - fix the codec decode path to reject gracefully with `std::invalid_argument`
  - preserve valid-buffer byte identity
- If the fix changes decode behavior for a valid row or a previously-working valid buffer, stop and ask because that risks violating locked decision #2.
- Do not touch the RBA accessor emitter. GIR-11 is runtime codec coverage only.

## Forcing-test mapping

`Property.EncodeDecodeRoundTrip`

- Satisfies the GIR-11 round-trip property requirement.
- Generates random valid Arrow rows over a fixed schema corpus.
- Uses `Codec::EncodeRow` then `Codec::DecodeRow`.
- Checks Arrow scalar value equality with `Scalar::Equals`.
- Also checks encode/decode/encode byte determinism.
- Uses fixed seed and capped iteration count.

`Fuzz.DecodeRowSurvivesRandomTruncatedBuffers`

- Satisfies the GIR-11 malformed-buffer fuzz requirement.
- Feeds `DecodeRow` random garbage, truncated valid buffers, bit-flipped valid buffers, count/length-corrupted buffers, and trailing-byte-corrupted buffers.
- Accepts only successful decode or `std::invalid_argument`.
- Exercises `Reader::Read`, `Reader::ReadBytes`, list/map count guards, nested struct/list/map decode, scalar length decode, and top-level full-consumption checks.
- Uses fixed seed and bounded corpus size.

## Risks & Unknowns

Arrow `Scalar::Equals` is not suitable for random NaN payload equality, so the property generator should avoid NaN and infinities. GIR-10 already covers IEEE special values bit-exactly.

Recursive random scalar generation can become noisy if it tries to cover every Arrow type. Keep GIR-11 focused on representative supported schemas and let GIR-10 own exhaustive edge-family coverage.

Some corrupted buffers may accidentally decode successfully under the schema. That is acceptable; the invariant is graceful handling, not mandatory rejection of every random byte sequence.

Very large corrupted length/count values must not trigger huge allocations or loops. Existing Phase-1 guards should prevent this; any gap is a decode-hardening bug to fix without changing valid wire bytes.

## Type coverage scope

The property test corpus exercises encode/decode round-trip determinism over the following Arrow types:

**Supported by property generator:**
- Scalars: boolean, int8/16/32/64, uint8/16/32/64, float32, float64, date32, timestamp, duration, decimal128
- Variable-length: utf8 (string), binary
- Containers: struct, list, fixed-size list, map

**Deferred to GIR-10 `CodecEdge.*` tests (not in property corpus):**
- Unions: dense and sparse (comprehensive GIR-10 coverage including both variants, type-code error paths, and varlen children)
- Intervals: month_interval, day_time_interval, month_day_nano_interval
- Extended time types: time32, time64, half_float
- Extended scalars: date64
- Large variants: large_string, large_binary, large_list
- View types: string_view, binary_view
- Other fixed-width: fixed_size_binary, decimal256

The property test's value lies in exercising the type-agnostic `Reader` guards (`Reader::Read`, `Reader::ReadBytes`, count bounds, bitfield reconstruction) and determinism across recursive nesting and count-driven loops, which GIR-10's edge tests do not systematically vary. The deferred type families are covered by GIR-10's red-first regression tests and bit-exact float/integer boundary coverage.

## Files-to-touch

```text
arrow-bridge/tests/test_codec_property_fuzz.cpp
```

New GIR-11 unit test TU containing property helpers, deterministic row generation, malformed-buffer generation, `Property.EncodeDecodeRoundTrip`, and `Fuzz.DecodeRowSurvivesRandomTruncatedBuffers`.

```text
arrow-bridge/tests/CMakeLists.txt
```

Add `test_codec_property_fuzz.cpp` to the existing `arrow_bridge_tests` target.

```text
arrow-bridge/src/codec.cpp
arrow-bridge/src/row_reader.hpp
```

Touch only if GIR-11 exposes a decode robustness bug. Any change must reject malformed input gracefully and preserve valid wire byte identity.

Do not touch RBA emitter files. Do not update wire goldens for GIR-11.

## Step-2 review (2026-07-11) — final round

**Verdict: APPROVE.**

Re-review of the reworked doc against `docs/robustness-plan.md` §3e, `GIR-locked-decisions.md`, and Phase-1 scope.

Blocking item from the prior round — RESOLVED:

1. **Sparse-union false-failure — resolved via omission (option b).** The
   property corpus (`PropertySchemas()`) contains only faithfully
   round-trippable types (bool/int*/uint*/float32/64/date32/timestamp/duration/
   decimal128/utf8/binary + struct/list/fixed-size-list/map); no unions. The
   "Union omission from property corpus" note correctly states the mechanism
   (codec decode nulls inactive sparse-union children → `Equals` false-fail on a
   valid row) and hands exhaustive dense/sparse coverage to GIR-10
   `CodecEdge.*`. No valid generated row can false-fail `Equals`, and the
   `encode2 == encoded1` regression guard is now safe (union re-encode with
   nulled inactive children was the byte-diff risk it would have tripped).

Non-blocking items from the prior round — all FOLDED:

2. Terminal `catch(...)` fails the test (was `SUCCEED`); the accept-set is
   exactly success-with-size-check or `std::invalid_argument`, and the
   `.ValueOrDie()` `abort()` is documented as an uncatchable real-crash signal.
3. `uniform_int_distribution` portability caveat is present and mitigated by
   hex-dump repro.
4. Honest "Type coverage scope" section lists generated-vs-deferred types.

Fixed inline this round (was the only new gap, non-blocking): the two fuzz
failure branches now dump the offending buffer as hex (not just seed/variant/
case), extending the doc's own portability mitigation to the fuzz path, whose
random-garbage strategy uses the same non-portable RNG. Without it a
random-garbage failure could not be replayed across toolchains.

Previously-verified-correct parts confirmed unchanged: fuzz accept-set
(success | `std::invalid_argument`); determinism (fixed `kPropertySeed`/
`kFuzzSeed`, capped iterations/corpus, bounded lengths/counts); locked GIR #2
(no wire change / valid buffers byte-identical), #3 (RBA emitter untouched,
runtime-codec-only scope), #9 (coverage item, red-first regression protocol);
Phase-1 decode path (`Reader::Read`/`ReadBytes`, count guards, nested decode,
full-consumption rejection) is the target. Spec §3e (round-trip property +
`DecodeRow` fuzz over random/truncated buffers) and the forcing tests
(`Property.EncodeDecodeRoundTrip`, `Fuzz.DecodeRowSurvivesRandomTruncatedBuffers`)
match. No locked-decision deviation.

Implementation notes (non-blocking, for the implementer): (a) apply nullability
recursively in `RandomScalar` so nested struct/list null children are also
exercised, not just top-level fields; (b) the `Fuzz` "wrong-`std::exception`"
branch is the most likely real hit — keep its full repro context.
