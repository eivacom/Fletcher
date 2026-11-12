# RBA-7 Code Review

**Target:** feature/recordbatchaccessor — RBA-7 capstone & documentation
**Diff Base:** e1e28f2 (RBA-6b)

## Findings by Severity

### Blocking (1)

**[P1] Missing tracked files** (Codex)
- test_accessor_capstone.cpp, test_accessor_readme_example.cpp, and integration-tests/accessor-capstone/ directory are referenced in CMakeLists.txt and build.rs but not included in the diff
- Clean checkout fails: CMake cannot find test sources, Rust build.rs panics on missing capstone proto/fixtures
- Fix: Stage the files (they exist on disk, tests pass 10/10 + 10/10)

### Should-Fix (1)

**[S1] Metadata equivalence implicit verification**
- Fixture documents that absent entries prove C++ nullptr == Rust empty-map
- Verified only implicitly by snapshot, not explicitly
- Recommendation: Add explicit field_metadata(2) == {} assertions in both tests

---

## Core Verification

**Capstone Equivalence (D-RBA-8):**
- Shared fixture: Both C++ and Rust build from integration-tests/accessor-capstone/fixtures/accessor_capstone_fixture.json ✓
- Shared schema: Both generated from integration-tests/accessor-capstone/proto/accessor_capstone.proto ✓
- Shared oracle: Both assert against integration-tests/accessor-capstone/fixtures/accessor_capstone_expected.json ✓
- Native builders: Both use Arrow array builders (not IPC), rows/nulls/metadata under test control ✓

**Perturbation-Fails-Both:**
- Bug in C++ null-handling → observed_cpp diverges from oracle
- Bug in Rust null-handling → observed_rust diverges from oracle
- NOT vacuous ✓

**Normalization Equivalence (all 8 field kinds verified identical):**
- Nullable scalar: both encode as JSON null ✓
- Nullable struct: both encode as JSON null ✓
- Repeated scalar with nulls: both encode as JSON null ✓
- Repeated struct with nulls: both encode as JSON null ✓
- Map scalar value with nulls: both encode as JSON null ✓
- Map message value with nulls: both encode as JSON null ✓
- Nested list with nulls: both encode as JSON null ✓

**Metadata Normalization:**
- C++ nullptr → {}
- Rust empty HashMap → {}
- Both sort keys (C++ automatically, Rust explicit)
- Deterministic ✓

**Build Wiring:**
- nlohmann_json properly declared (conanfile.py line 46, CMakeLists.txt lines 15/239)
- serde_json declared (Cargo.toml line 27)
- Capstone header generation correct (CMakeLists.txt 158-177, build.rs 135-142)
- Fixture paths resolved at build-time (CMakeLists.txt 250, build.rs 111-115)

**Documentation (Spec §7 + D-RBA-7):**
- New text: C++ field(i) pre-windowed (use directly); Rust columns() NOT pre-windowed (must slice)
- Both spec §7 and locked-decisions D-RBA-7 synchronized ✓

**README Example Guard:**
- test_accessor_readme_example.cpp mirrors protoc/README.md snippet exactly
- Compile-checks the documented API shape ✓

**Fragility Analysis:**
- No floating-point fields (only int32, utf8)
- Maps as [{key,value},...] arrays → deterministic ordering
- Metadata keys sorted explicitly/implicitly
- No non-determinism sources ✓

---

## Verdict

**READY FOR MERGE after P1 fix (stage missing files).**

RBA-7 is correct. Capstone achieves cross-language parity proof. Equivalence is real, not vacuous. All field kinds and nulls handled identically. Build wiring complete. Docs synchronized. Tests green 10/10 + 10/10.

