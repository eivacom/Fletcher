# RBA-2 Code Review — RecordBatch Accessor Generator for Scalar Fields

**Codex Review + Manual Analysis**  
**Base Commit:** 8e66d842581f430655a4d1161dce6956630375b4

## BLOCKING ISSUES (P1)

### Missing header in patch — compilation fails on clean checkout
- **File:** protoc/src/recordbatch_accessor_emitter.cpp:10, protoc/src/generator.cpp:19
- **Issue:** Both TUs now include "generator_internal.hpp" but the file is untracked/not in diff
- **Impact:** Clean patch application fails with "file not found"
- **Fix:** Add protoc/include/generator_internal.hpp to committed patch

### Missing test fixture proto in patch
- **File:** integration-tests/protoc-arrow-bridge/CMakeLists.txt:106
- **Issue:** CMakeLists references proto/accessor_scalar.proto which is untracked
- **Impact:** Code generation step fails
- **Fix:** Add integration-tests/protoc-arrow-bridge/proto/accessor_scalar.proto to patch

### Missing test source file in patch
- **File:** integration-tests/protoc-arrow-bridge/CMakeLists.txt:176
- **Issue:** CMakeLists adds tests/test_accessor_scalar.cpp which is untracked
- **Impact:** Build target has non-existent source
- **Fix:** Add integration-tests/protoc-arrow-bridge/tests/test_accessor_scalar.cpp to patch

## CRITICAL ISSUES (P2)

### StructArray slicing does not correctly window child columns
- **Location:** protoc/src/recordbatch_accessor_emitter.cpp:153-167
- **Issue:** When Make(StructArray) receives a sliced struct, field(i) does NOT apply the parent's offset/length to children. Generated code reads children as-is (original offsets).
- **Example Failure:** `struct.Slice(1, 2)` then accessing row 0 reads original row 0, not original row 1.
- **Test Impact:** Test case ScalarAccessorFromStructArrayReadsIdentically includes slicing but will FAIL at runtime.
- **Fix Required:** Slice each child to match struct's window before passing to FromColumns_.

## MAJOR ISSUES

### std::string_view lifetime hazard in getters
- **Location:** recordbatch_accessor_emitter.cpp:182-192 (utf8/binary fields)
- **Issue:** Getters return std::string_view into underlying buffer. View is valid only while accessor (and its shared_ptr) is alive.
- **Hazard:** Caller can store view and use it after accessor destroyed → use-after-free.
- **Mitigation:** Test doesn't exercise orphaning; typical usage is safe.
- **Fix:** Document lifetime constraint in getter comments; consider returning std::string for nullable variants.

### Error message formatting is invalid C++
- **Location:** recordbatch_accessor_emitter.cpp:210-211
- **Issue:** Generated code is `return arrow::Status::Invalid("" << cls << ": expected \", kExpectedColumns, ...)`
- **Problem:** Cannot use stream ops on string literals in this context
- **Impact:** Generated accessor header WILL NOT COMPILE
- **Fix:** Build string with concatenation and std::to_string instead.

## MINOR ISSUES

### String_view lifetime not documented
- **Impact:** Callers may unknowingly create use-after-free
- **Fix:** Add comment to each utf8/binary getter

### Limited type-mismatch test coverage
- **Issue:** Only one mismatch tested (int32 vs double)
- **Missing:** Bool/timestamp/duration/utf8-vs-binary pairs
- **Recommendation:** Add parameterized type-pair tests

### Missing edge case: nullable Arrow for non-nullable proto
- **Issue:** Design allows but test doesn't verify
- **Fix:** Add test case for nullable Arrow field with null_count==0 matched to non-nullable proto

## PASS ITEMS

### ODR/Linkage correctness
- Symbol relocation from anonymous ns to fletcher ns is correct
- Header guard and single definition maintained
- Status: OK

### Test design strength
- Comprehensive fixture (14 scalar fields)
- Good coverage of happy path and error paths
- Lifetime test (data survives batch drop) validates caching
