# GIR-11 Code Review — Property + Fuzz Codec Tests

## Codex Verdict

The changes add deterministic codec property/fuzz coverage and wire the new test file into the existing test target. No discrete introduced bug identified that would break existing behavior or the build.

---

## Static Analysis Against GIR-11 Design Criteria

### 1. Fuzz Genuineness — Is the Fuzz Actually TOOTHED?

**Corruption Strategies Coverage:**

- **Random Garbage (Strategy 1, lines 339–345):** Lengths 0..256, bytes 0..255. Will exercise:
  - Reader::ReadBytes(BitfieldBytes(num_fields)) early rejection on short buffers
  - List/map count guards (Reader::Read<uint32_t>())
  - All nested decode paths

- **Truncated Prefixes (Strategy 2, lines 347–354):** Every prefix of each valid seed row (0 to size-1), then the exact valid row as a sanity check. Excellent coverage:
  - Reaches every truncation boundary
  - Exercises full-consumption check on incomplete buffers
  - Proves the harness succeeds on exact valid input

- **Corrupted Rows (Strategy 3, lines 356–389):**
  - Bit flips (case 0): Corrupt fixed-width fields, bitfields, counts
  - Byte overwrites (case 1): Corrupt any single byte
  - 0xFF count/length corruption (case 2): Directly targets uint32_t count guards in codec.cpp:252, 273
  - Trailing append (case 3): Exercises top-level full-buffer-consumption rejection (codec.cpp:429–432)

**Accept-Set Validation:**

Lines 317–336: The lambda attempt() only accepts:
- Successful decode (with exact field-count check, line 322)
- std::invalid_argument (line 323)

Any other exception type FAILs the test (lines 325–333). The terminal catch(...) block (lines 330–333) FAILS (not SWALLOWs), ensuring uncaught exceptions fail the test visibly. A real crash/abort (e.g., from .ValueOrDie() in decode) is documented (design doc section 179) as an uncatchable signal indicating a decode robustness defect.

**Sanity Check:**

Line 353: attempt(seed, "valid") feeds the exact valid encoded row. If any valid seed fails to decode, the test FAILs immediately.

**Verdict: TOOTHED.** All three corruption strategies reach guarded paths, the accept-set enforces graceful rejection, exact-valid seeds must succeed, and unexpected exceptions FAIL.

---

### 2. Property Test Genuineness

**Valid Diverse Row Generation:**

PropertySchemas() (lines 76–105):
- Two representative schemas (scalar types + containers)
- No unions (correctly omitted per design doc discussion of sparse-union false-fail risk)

RandomScalar() (lines 153–251):
- Generates diverse valid values: BOOL, INT{8,16,32,64}, UINT{8,16,32,64}
- FLOAT/DOUBLE: finite values only in [-1e12, 1e12] (lines 180–182, comment justifies NaN/±Inf deferral to GIR-10)
- DATE32, TIMESTAMP, DURATION, DECIMAL128: all generate valid values
- DECIMAL128: magnitude capped to less than 10^18 (lines 196–198)
- STRING/BINARY: deterministic alphabet with embedded NULs, multi-byte UTF-8 (lines 109–132)
- STRUCT, LIST, FIXED_SIZE_LIST, MAP: recursive generation with depth cap at 3 (lines 217, 228)
- Nullability: 20% null rate applied recursively (line 157: rng() % 5 == 0)

**Iteration Diversity:**

RandomRow() loops over all schemas (line 272), 128 iterations per schema (line 274), each with different RNG state. Sufficient diverse sampling.

**Field-by-Field Equals:**

Lines 286–291: Loop over each field, call Scalar::Equals() on the decoded vs. original. Not a shallow compare; Arrow's Equals is the standard value oracle.

**Encode/Decode/Encode Byte Determinism:**

Lines 292–293: Re-encode the decoded row and assert byte-identical match. Strong regression guard against nondeterministic decode reconstruction.

**Verdict: GENUINE.** Valid diverse rows, field-by-field Equals, byte-determinism guard, no false-fail risk (no unions).

---

### 3. Determinism

**Fixed Seeds:**

- kPropertySeed = 0x47495231315F5052ULL (line 44, ASCII "GIR11_PR")
- kFuzzSeed = 0x47495231315F465AULL (line 45, ASCII "GIR11_FZ")
- Both tests instantiate std::mt19937_64 rng(seed) (lines 269, 303)

**Bounded Iteration Counts:**

- Property: 128 iterations x 2 schemas = 256 cases (line 47)
- Fuzz valid seeds per schema: 32 (line 49)
- Fuzz random garbage per schema: 256 (line 50)
- Fuzz corruption cases per schema: 256 (line 51)

All bounded, sizes are reasonable.

**Failure Reproduction Across Toolchains:**

Property test failure (lines 278–281): Includes HexDump(encoded1).

Fuzz failure (lines 325–333): Includes HexDump(buf).

Both dump the offending buffer as hex, enabling cross-platform replay even though std::uniform_int_distribution is not portable across standard libraries (design doc sections 114–115).

**No Unseeded RNG:**

All RNG instances use fixed seeds. No random_device, no wall-clock, no entropy-pool draws. Fully deterministic.

**Verdict: DETERMINISTIC.** Fixed seeds, bounded corpora, hex-dump repro for cross-toolchain failures, no unseeded RNG.

---

### 4. Harness Correctness & Safety

**HexDump Helper (lines 61–70):** Safe range-based iteration over vector elements, no direct indexing.

**Buffer Truncation Logic (lines 349–352):** Loop p in [0, seed.size()), range [begin(), begin()+p) is valid for all p. Includes p=0 (empty) and p=size-1 (truncated). Safe; no OOB.

**Bit-Flip Logic (lines 364–366):** Modulo bounds bitpos < buf.size()*8, so bitpos/8 < buf.size(). Safe.

**Byte Overwrite (lines 368–369):** Guarded by line 361: if (buf.empty()) mode = 3. Safe.

**4-Byte 0xFF Corruption (lines 372–379):** If size >= 4, pos < buf.size()-3, so pos+3 < buf.size(). Safe.

**Trailing Append (lines 381–385):** extra in [1, 4], vector::push_back is safe. Safe.

**Decimal128 Range (lines 195–198):** Cast and modulo arithmetic safe, result in [-maxv, maxv]. Safe.

**UTF-8 & Binary Generators (lines 109–132):** Bounded loops, tokens accessed via modulo. Safe.

**No Memory Leaks:** All allocations are RAII (std::vector, std::string, std::shared_ptr). No manual new/delete.

**Verdict: SAFE.** No UB, no OOB, no unguarded accesses, no leaks.

---

## Summary

| Criterion | Status | Details |
|-----------|--------|---------|
| Fuzz Toothed | PASS | Three corruption strategies reach guarded paths; sanity case proves success; terminal catch FAILs |
| Property Genuine | PASS | Valid diverse rows; field-by-field Equals; byte-determinism guard; no false-fails (no unions) |
| Determinism | PASS | Fixed seeds, bounded counts, hex-dump cross-toolchain repro, no unseeded RNG |
| Harness Safety | PASS | No UB/OOB, all buffers properly bounded, no leaks, RAII memory management |

---

## Blocking Issues

None.

---

## Non-Blocking Issues

None.

---

## Nits

None.

---

**Conclusion:** GIR-11 implementation is APPROVED. The test harness is genuinely toothed, the property generator is sound, determinism is ensured, and the harness code is safe. The test satisfies the GIR-11 round-trip property requirement and malformed-buffer fuzz requirement without violating locked decisions.
