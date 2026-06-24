// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// RBA-1 forcing test — option plumbing + additive no-drift guarantee (D-RBA-1).
//
// For each baseline --fletcher_opt set in
//   { (empty), ts, ipc, schema_only, ts,ipc, schema_only,ts,ipc }
// run the plugin twice against the same fixture proto:
//   (1) baseline options
//   (2) baseline options + accessor,rust
// and assert:
//   - every file the baseline produced exists in the accessor/rust run,
//   - every baseline file is byte-identical in both runs,
//   - the accessor/rust run added ONLY the three new files
//       <stem>.fletcher.accessor.pb.h, <stem>.fletcher.rs, and the shared
//       __rba.fletcher.rs span/Row helper module (RBA-6a; emitted once per run).
//
// The matrix runs over a typical fixture (nested.proto — multiple messages,
// IPC, view classes) AND a degenerate fixture (empty_accessor.proto —
// recursive-only, no view/IPC output) to prove the +3 emission is
// unconditional, not content-gated.
//
// This within-build test proves the "with flags == without flags" half of
// D-RBA-1. The before/after-feature half is guaranteed by the untouched-emitter
// discipline plus the existing per-fixture integration suite in this harness
// (test_telemetry.cpp / test_nested.cpp / test_arrow_view.cpp / … and
// test_ipc_parity.cpp), which compile and consume the existing outputs and stay
// green if no existing emitter changed.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<uint8_t> ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// Names of all regular files directly under `dir` (the plugin emits flat).
std::set<std::string> ListFiles(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) names.insert(entry.path().filename().string());
    }
    return names;
}

// Quote a single command-line argument for the platform shell.
std::string Quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    return "'" + s + "'";
#endif
}

// Run the fletcher plugin via protoc. `opt` is the --fletcher_opt value (may be
// empty). Output is written into `out_dir`. Returns the process exit code.
//
// All path arguments originate from CMake compile-definitions: PROTOC_PATH and
// PLUGIN_PATH are $<TARGET_FILE:...> (absolute), and the three -I roots plus
// out_dir/proto_file are absolute CMake directories. Every argument is quoted so
// the command line is robust to spaces on CI Linux and Windows alike.
int RunPlugin(const std::string& opt, const fs::path& proto_file, const fs::path& out_dir) {
    // Guard the portability invariant: any relative path here would break under a
    // different working directory on CI. Fail loudly rather than silently.
    EXPECT_TRUE(fs::path(PROTOC_PATH).is_absolute()) << "PROTOC_PATH must be absolute";
    EXPECT_TRUE(fs::path(PLUGIN_PATH).is_absolute()) << "PLUGIN_PATH must be absolute";
    EXPECT_TRUE(fs::path(PROTO_DIR).is_absolute()) << "PROTO_DIR must be absolute";
    EXPECT_TRUE(fs::path(FLETCHER_PROTO_INCLUDE_DIR).is_absolute())
        << "FLETCHER_PROTO_INCLUDE_DIR must be absolute";
    EXPECT_TRUE(fs::path(PROTOBUF_WKT_INCLUDE_DIR).is_absolute())
        << "PROTOBUF_WKT_INCLUDE_DIR must be absolute";
    EXPECT_TRUE(proto_file.is_absolute()) << "proto_file must be absolute";
    EXPECT_TRUE(out_dir.is_absolute()) << "out_dir must be absolute";

    std::string cmd;
    cmd += Quote(PROTOC_PATH);
    cmd += " --plugin=protoc-gen-fletcher=" + Quote(PLUGIN_PATH);
    if (!opt.empty()) cmd += " --fletcher_opt=" + Quote(opt);
    cmd += " --fletcher_out=" + Quote(out_dir.string());
    cmd += " -I " + Quote(PROTO_DIR);
    cmd += " -I " + Quote(FLETCHER_PROTO_INCLUDE_DIR);
    cmd += " -I " + Quote(PROTOBUF_WKT_INCLUDE_DIR);
    cmd += " " + Quote(proto_file.string());
    // Wrap the whole command so the shell parses it as one command on Windows.
    const std::string wrapped =
#ifdef _WIN32
        "\"" + cmd + "\"";
#else
        cmd;
#endif
    return std::system(wrapped.c_str());
}

fs::path ScratchRoot() {
    return fs::path(GENERATED_DIR_PATH) / "accessor_nodrift";
}

// Runs one (baseline vs baseline+accessor,rust) comparison for a fixture stem
// and asserts the no-drift + exactly-+2 contract. The generated .fletcher.rs is
// asserted emitted + non-empty; its compilation against arrow-rs is the job of
// the integration-tests/protoc-gen-fletcher-rust cargo crate (RBA-5), not a
// bare-rustc parse here (which cannot resolve `use arrow::...`).
void CheckNoDriftForCase(const std::string& stem, const std::string& baseline_opt,
                         const std::string& case_label) {
    SCOPED_TRACE("fixture=" + stem + " baseline_opt=[" + baseline_opt + "]");

    const fs::path proto_file = fs::path(PROTO_DIR) / (stem + ".proto");
    ASSERT_TRUE(fs::exists(proto_file)) << proto_file;

    const fs::path base_dir = ScratchRoot() / (stem + "_" + case_label) / "baseline";
    const fs::path acc_dir = ScratchRoot() / (stem + "_" + case_label) / "accessor";
    fs::remove_all(base_dir);
    fs::remove_all(acc_dir);
    fs::create_directories(base_dir);
    fs::create_directories(acc_dir);

    ASSERT_EQ(RunPlugin(baseline_opt, proto_file, base_dir), 0)
        << "baseline protoc run failed for opt=[" << baseline_opt << "]";

    const std::string accessor_opt = baseline_opt.empty() ? "accessor,rust"
                                                          : baseline_opt + ",accessor,rust";
    ASSERT_EQ(RunPlugin(accessor_opt, proto_file, acc_dir), 0)
        << "accessor protoc run failed for opt=[" << accessor_opt << "]";

    const std::set<std::string> base_files = ListFiles(base_dir);
    const std::set<std::string> acc_files = ListFiles(acc_dir);

    // (1) every baseline file exists in the accessor run, and (2) is byte-identical.
    for (const std::string& name : base_files) {
        const fs::path acc_path = acc_dir / name;
        EXPECT_TRUE(acc_files.count(name) > 0)
            << "baseline file missing from accessor run: " << name;
        if (acc_files.count(name) == 0) continue;
        EXPECT_EQ(ReadFileBytes(base_dir / name), ReadFileBytes(acc_path))
            << "baseline file is not byte-identical between runs: " << name;
    }

    // (3) the accessor run added ONLY the new files. RBA-6a: the `rust` token now
    // emits THREE files — the C++ accessor header, the per-file Rust accessor
    // module, and the shared `__rba.fletcher.rs` span/Row helper module (emitted
    // once per protoc run; byte-identical across runs, mounted once by the
    // build.rs assembler). The shared helper is fixed-named (not stem-derived).
    const std::string accessor_hdr = stem + ".fletcher.accessor.pb.h";
    const std::string rust_file = stem + ".fletcher.rs";
    const std::string rba_helper = "__rba.fletcher.rs";

    std::set<std::string> expected = base_files;
    expected.insert(accessor_hdr);
    expected.insert(rust_file);
    expected.insert(rba_helper);
    EXPECT_EQ(acc_files, expected)
        << "accessor run file set must equal baseline + exactly {" << accessor_hdr << ", "
        << rust_file << ", " << rba_helper << "}";

    // Spell out the +3 explicitly for a clear failure message.
    EXPECT_TRUE(acc_files.count(accessor_hdr) > 0) << "missing " << accessor_hdr;
    EXPECT_TRUE(acc_files.count(rust_file) > 0) << "missing " << rust_file;
    EXPECT_TRUE(acc_files.count(rba_helper) > 0) << "missing " << rba_helper;
    EXPECT_EQ(base_files.count(accessor_hdr), 0u)
        << accessor_hdr << " must not appear without the accessor token";
    EXPECT_EQ(base_files.count(rust_file), 0u)
        << rust_file << " must not appear without the rust token";
    EXPECT_EQ(base_files.count(rba_helper), 0u)
        << rba_helper << " must not appear without the rust token";

    // The emitted C++ accessor header must be non-empty (minimal-but-valid
    // skeleton). Its compilability is covered by the harness include target.
    if (acc_files.count(accessor_hdr) > 0) {
        EXPECT_FALSE(ReadFileBytes(acc_dir / accessor_hdr).empty()) << accessor_hdr << " is empty";
    }

    // Rust well-formedness: the generated .fletcher.rs is asserted emitted and
    // non-empty here, but it is NOT bare-rustc parse-checked. As of RBA-5 the
    // emitter produces real arrow-rs accessors (`use arrow::...`), which a bare
    // `rustc --emit metadata` (no `--extern arrow`) cannot resolve. The
    // authoritative Rust compile/validation now lives in the
    // integration-tests/protoc-gen-fletcher-rust cargo crate, which compiles ALL
    // generated Rust against the pinned `arrow`. The RBA-1 protected guarantee
    // (byte-identity of existing outputs + exactly-+2 new files) is unaffected
    // and fully retained above.
    if (acc_files.count(rust_file) > 0) {
        EXPECT_FALSE(ReadFileBytes(acc_dir / rust_file).empty()) << rust_file << " is empty";
    }
    // The shared __rba helper module must also be non-empty.
    if (acc_files.count(rba_helper) > 0) {
        EXPECT_FALSE(ReadFileBytes(acc_dir / rba_helper).empty()) << rba_helper << " is empty";
    }
}

}  // namespace

TEST(AccessorTest, OptGatedEmissionLeavesExistingOutputsByteIdentical) {
    // The six baseline option sets from the design matrix.
    const std::array<std::pair<std::string, std::string>, 6> kBaselines = {{
        {"", "empty"},
        {"ts", "ts"},
        {"ipc", "ipc"},
        {"schema_only", "schema_only"},
        {"ts,ipc", "ts_ipc"},
        {"schema_only,ts,ipc", "schema_only_ts_ipc"},
    }};

    // Typical fixture (multiple messages, view + IPC) and degenerate fixture
    // (recursive-only, no view/IPC) — the +2 must hold for both.
    const std::array<std::string, 2> kFixtures = {{"nested", "empty_accessor"}};

    for (const std::string& stem : kFixtures) {
        for (const auto& [opt, label] : kBaselines) {
            CheckNoDriftForCase(stem, opt, label);
        }
    }
}

// Dedicated, visible record that the generated Rust's well-formedness check is
// authoritatively owned by the RBA-5 cargo crate. RBA-1 carried a bare-rustc
// parse here as a stopgap explicitly "deferred to RBA-5"; now that RBA-5 emits
// real arrow-rs accessors (`use arrow::...`) — which a bare `rustc --emit
// metadata` cannot resolve without `--extern arrow` — the real Rust compile is
// performed by integration-tests/protoc-gen-fletcher-rust (`cargo test`), which
// builds ALL generated Rust against the pinned `arrow`. This test stays as a
// counted, documented GTEST_SKIP pointing at that crate so the decision is
// visible in the ctest report. The no-drift test above still asserts the .rs is
// emitted + non-empty and preserves byte-identity + exactly-+2-files.
TEST(AccessorTest, GeneratedRustFileParsesWithRustc) {
    GTEST_SKIP() << "Generated-Rust compilation is owned by the RBA-5 cargo crate "
                    "integration-tests/protoc-gen-fletcher-rust (it compiles all "
                    "generated .fletcher.rs against the pinned arrow-rs). A bare "
                    "`rustc --emit metadata` here cannot resolve `use arrow::...`. "
                    "The no-drift test still asserts the .rs is emitted + non-empty "
                    "and preserves byte-identity + exactly-+2-files.";
}

