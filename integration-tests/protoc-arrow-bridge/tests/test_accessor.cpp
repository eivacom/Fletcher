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
//   - the accessor/rust run added ONLY the two new files
//       <stem>.fletcher.accessor.pb.h and <stem>.fletcher.rs.
//
// The matrix runs over a typical fixture (nested.proto — multiple messages,
// IPC, view classes) AND a degenerate fixture (empty_accessor.proto —
// recursive-only, no view/IPC output) to prove the +2 emission is
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

// True if a `rustc` toolchain is callable on this machine. Probed once per test.
bool RustcAvailable() {
#ifdef _WIN32
    const int rc = std::system("rustc --version >NUL 2>&1");
#else
    const int rc = std::system("rustc --version >/dev/null 2>&1");
#endif
    return rc == 0;
}

// Parse-checks a generated .rs file by compiling it as a library to metadata
// only (no codegen). A comment/banner-only skeleton is a valid empty Rust
// module and must succeed. Returns the rustc exit code (0 == well-formed).
int RustcCheck(const fs::path& rs_file, const fs::path& out_dir) {
    fs::create_directories(out_dir);
    // An explicit --crate-name is required: rustc would otherwise derive the
    // crate name from the filename (e.g. "nested.fletcher.rs" -> "nested.fletcher"),
    // and the embedded '.' is an invalid crate-name character. The name itself is
    // irrelevant to the parse check.
    std::string cmd =
        "rustc --crate-type lib --edition 2021 --crate-name fletcher_accessor_check"
        " --emit metadata";
    cmd += " --out-dir " + Quote(out_dir.string());
    cmd += " " + Quote(rs_file.string());
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
// and asserts the no-drift + exactly-+2 contract. When `rustc_available` is
// true, the generated .rs is parse-checked with rustc; otherwise the per-file
// non-empty check stands and the visible skip is reported once by the caller.
void CheckNoDriftForCase(const std::string& stem, const std::string& baseline_opt,
                         const std::string& case_label, bool rustc_available) {
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

    // (3) the accessor run added ONLY the two new files.
    const std::string accessor_hdr = stem + ".fletcher.accessor.pb.h";
    const std::string rust_file = stem + ".fletcher.rs";

    std::set<std::string> expected = base_files;
    expected.insert(accessor_hdr);
    expected.insert(rust_file);
    EXPECT_EQ(acc_files, expected)
        << "accessor run file set must equal baseline + exactly {" << accessor_hdr << ", "
        << rust_file << "}";

    // Spell out the +2 explicitly for a clear failure message.
    EXPECT_TRUE(acc_files.count(accessor_hdr) > 0) << "missing " << accessor_hdr;
    EXPECT_TRUE(acc_files.count(rust_file) > 0) << "missing " << rust_file;
    EXPECT_EQ(base_files.count(accessor_hdr), 0u)
        << accessor_hdr << " must not appear without the accessor token";
    EXPECT_EQ(base_files.count(rust_file), 0u)
        << rust_file << " must not appear without the rust token";

    // The emitted C++ accessor header must be non-empty (minimal-but-valid
    // skeleton). Its compilability is covered by the harness include target.
    if (acc_files.count(accessor_hdr) > 0) {
        EXPECT_FALSE(ReadFileBytes(acc_dir / accessor_hdr).empty()) << accessor_hdr << " is empty";
    }

    // Rust well-formedness: when rustc is available, parse-check the generated
    // .rs (compile to metadata only). A comment/banner-only skeleton is a valid
    // empty module and must succeed. When rustc is absent the check is deferred
    // to RBA-5 (reported as a visible, counted GTEST_SKIP by the caller); the
    // non-empty file check below keeps a minimal guard in that case.
    if (acc_files.count(rust_file) > 0) {
        EXPECT_FALSE(ReadFileBytes(acc_dir / rust_file).empty()) << rust_file << " is empty";
        if (rustc_available) {
            const fs::path rustc_out = acc_dir / "rustc_meta";
            EXPECT_EQ(RustcCheck(acc_dir / rust_file, rustc_out), 0)
                << "rustc rejected generated Rust file: " << rust_file;
        }
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

    // Probe rustc once; when available, every generated .rs in the matrix is
    // parse-checked inline (rustc --crate-type lib --emit metadata). When absent,
    // the Rust parse is deferred to RBA-5 and reported by the visible, counted
    // skip in AccessorTest.GeneratedRustFileParsesWithRustc below — never a
    // silent "file exists" pass.
    const bool rustc_available = RustcAvailable();

    for (const std::string& stem : kFixtures) {
        for (const auto& [opt, label] : kBaselines) {
            CheckNoDriftForCase(stem, opt, label, rustc_available);
        }
    }
}

// Dedicated, visible record of the Rust well-formedness decision so the harness
// maintainer sees it in the ctest report rather than a silent downgrade. When
// rustc is present this re-affirms the generated .rs parses; when absent it is a
// counted GTEST_SKIP that explicitly defers the Rust parse to RBA-5.
TEST(AccessorTest, GeneratedRustFileParsesWithRustc) {
    if (!RustcAvailable()) {
        GTEST_SKIP() << "rustc not available on this machine; Rust well-formedness "
                        "parse is deferred to the RBA-5 Rust crate (pinned toolchain). "
                        "The inline no-drift test still asserts the .rs is emitted and "
                        "non-empty.";
    }

    // Generate the Rust file for a representative fixture and parse-check it.
    const fs::path proto_file = fs::path(PROTO_DIR) / "nested.proto";
    ASSERT_TRUE(fs::exists(proto_file)) << proto_file;

    const fs::path out_dir = ScratchRoot() / "rust_parse" / "gen";
    fs::remove_all(ScratchRoot() / "rust_parse");
    fs::create_directories(out_dir);

    ASSERT_EQ(RunPlugin("rust", proto_file, out_dir), 0) << "plugin run (rust) failed";

    const fs::path rs_file = out_dir / "nested.fletcher.rs";
    ASSERT_TRUE(fs::exists(rs_file)) << rs_file;

    const fs::path rustc_out = ScratchRoot() / "rust_parse" / "meta";
    EXPECT_EQ(RustcCheck(rs_file, rustc_out), 0)
        << "rustc rejected the generated Rust skeleton: " << rs_file;
}
