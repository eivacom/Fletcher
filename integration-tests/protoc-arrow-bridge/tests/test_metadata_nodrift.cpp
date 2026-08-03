// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// #117 no-drift guard — metadata_from_option must be inert when it matches
// nothing, and must never add or remove an output file.
//
// The feature refactored both metadata emission paths onto one shared pair
// builder (SchemaMetadataPairs / FieldMetadataPairs), which touches code every
// existing output flows through. This is the automated proof that the refactor
// is byte-neutral:
//
//   for each baseline --fletcher_opt set in
//     { (empty), ts, ipc, schema_only, ts,ipc, schema_only,ts,ipc }
//   run the plugin twice against the same fixture:
//     (1) baseline
//     (2) baseline + a well-formed rule whose extension no fixture declares
//   and assert the produced file SET and every file's BYTES are identical.
//
// Fixtures span the output classes: nested.proto (multiple messages, view, IPC),
// flatten.proto (both flatten forms — the code paths this feature threads a new
// parameter through), and empty_accessor.proto (degenerate).
//
// The "before/after the feature" half is guarded, as for RBA-1, by the untouched
// per-fixture suite in this harness plus test_ipc_parity.cpp.
//
// The plugin-invocation helpers below are deliberately local rather than shared
// with test_accessor.cpp: this TU needs to pass SEVERAL --fletcher_opt flags in
// one command (protoc comma-joins them), which that helper does not do. Keeping
// them separate leaves the RBA-1 guard's own code untouched.

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

std::set<std::string> ListFiles(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) names.insert(entry.path().filename().string());
    }
    return names;
}

std::string Quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    return "'" + s + "'";
#endif
}

// Run the plugin with an arbitrary list of --fletcher_opt values. protoc joins
// repeated --fletcher_opt flags with commas before handing them to the plugin,
// which is exactly how a real consumer supplies a list of mapping rules.
int RunPlugin(const std::vector<std::string>& opts, const fs::path& proto_file,
              const fs::path& out_dir) {
    std::string cmd;
    cmd += Quote(PROTOC_PATH);
    cmd += " --plugin=protoc-gen-fletcher=" + Quote(PLUGIN_PATH);
    for (const std::string& opt : opts) {
        if (!opt.empty()) cmd += " --fletcher_opt=" + Quote(opt);
    }
    cmd += " --fletcher_out=" + Quote(out_dir.string());
    cmd += " -I " + Quote(PROTO_DIR);
    cmd += " -I " + Quote(FLETCHER_PROTO_INCLUDE_DIR);
    cmd += " -I " + Quote(PROTOBUF_WKT_INCLUDE_DIR);
    cmd += " " + Quote(proto_file.string());
    const std::string wrapped =
#ifdef _WIN32
        "\"" + cmd + "\"";
#else
        cmd;
#endif
    return std::system(wrapped.c_str());
}

fs::path MakeCaseDir(const std::string& label) {
    const fs::path dir = fs::temp_directory_path() / ("fletcher_meta_nodrift_" + label);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string Sanitize(std::string s) {
    for (char& c : s) {
        if (c == ',' || c == '=' || c == ':' || c == '.' || c == '/') c = '_';
    }
    return s.empty() ? "none" : s;
}

constexpr std::array<const char*, 6> kBaselines = {
    "", "ts", "ipc", "schema_only", "ts,ipc", "schema_only,ts,ipc",
};

constexpr std::array<const char*, 3> kFixtures = {
    "nested.proto",
    "flatten.proto",
    "empty_accessor.proto",
};

// Well-formed rules whose extension is declared by no fixture in this harness.
// An extension that is absent from the pool is dropped silently — one flag list
// is routinely applied to a whole corpus where only some files import the
// option .proto — so these must leave every output untouched.
const std::vector<std::string>& InertRules() {
    static const std::vector<std::string> rules = {
        "metadata_from_option=field:absent.pkg.col.unit:x:unit",
        "metadata_from_option=field_type:absent.pkg.typ.kind:x:kind",
        "metadata_from_option=message:absent.pkg.typ.group:x:group",
    };
    return rules;
}

}  // namespace

TEST(MetadataNoDriftTest, InertRulesLeaveEveryOutputByteIdentical) {
    for (const char* fixture : kFixtures) {
        const fs::path proto = fs::path(PROTO_DIR) / fixture;
        ASSERT_TRUE(fs::exists(proto)) << proto;

        for (const char* baseline : kBaselines) {
            const std::string label = std::string(fixture) + "_" + Sanitize(baseline);

            const fs::path base_dir = MakeCaseDir(Sanitize(label) + "_base");
            const fs::path rule_dir = MakeCaseDir(Sanitize(label) + "_rules");

            ASSERT_EQ(RunPlugin({baseline}, proto, base_dir), 0) << label;

            std::vector<std::string> with_rules = {baseline};
            for (const std::string& r : InertRules()) with_rules.push_back(r);
            ASSERT_EQ(RunPlugin(with_rules, proto, rule_dir), 0) << label;

            const std::set<std::string> base_files = ListFiles(base_dir);
            const std::set<std::string> rule_files = ListFiles(rule_dir);

            // Mapping rules write no new files — unlike accessor/rust, which add
            // three. The file set must match exactly.
            EXPECT_EQ(base_files, rule_files) << label;

            for (const std::string& name : base_files) {
                EXPECT_EQ(ReadFileBytes(base_dir / name), ReadFileBytes(rule_dir / name))
                    << label << " / " << name;
            }
        }
    }
}

TEST(MetadataNoDriftTest, RepeatedRunsWithTheSameRulesAreDeterministic) {
    const fs::path proto = fs::path(PROTO_DIR) / "option_metadata.proto";
    ASSERT_TRUE(fs::exists(proto)) << proto;

    const std::vector<std::string> opts = {
        "ipc",
        "metadata_from_option=field_type:integration.metadata.typ.kind:x:kind",
        "metadata_from_option=field_type:integration.metadata.typ.unit:x:unit",
        "metadata_from_option=field_type:integration.metadata.typ.enc/integration.metadata.enc_"
        "opts.ext_name:ARROW:extension:name",
    };

    const fs::path a = MakeCaseDir("determinism_a");
    const fs::path b = MakeCaseDir("determinism_b");
    ASSERT_EQ(RunPlugin(opts, proto, a), 0);
    ASSERT_EQ(RunPlugin(opts, proto, b), 0);

    const std::set<std::string> files = ListFiles(a);
    ASSERT_FALSE(files.empty());
    EXPECT_EQ(files, ListFiles(b));
    for (const std::string& name : files) {
        EXPECT_EQ(ReadFileBytes(a / name), ReadFileBytes(b / name)) << name;
    }
}

TEST(MetadataNoDriftTest, RulesActuallyChangeOutputWhenTheyMatch) {
    // Control for the inert-rules test above: if a matching rule did NOT change
    // the output, InertRulesLeaveEveryOutputByteIdentical would pass vacuously.
    const fs::path proto = fs::path(PROTO_DIR) / "option_metadata.proto";
    ASSERT_TRUE(fs::exists(proto)) << proto;

    const fs::path plain = MakeCaseDir("control_plain");
    const fs::path mapped = MakeCaseDir("control_mapped");
    ASSERT_EQ(RunPlugin({"ipc"}, proto, plain), 0);
    ASSERT_EQ(RunPlugin({"ipc", "metadata_from_option=field_type:integration.metadata.typ.unit:x:"
                                "unit"},
                        proto, mapped),
              0);

    EXPECT_EQ(ListFiles(plain), ListFiles(mapped)) << "no new files even when rules match";
    EXPECT_NE(ReadFileBytes(plain / "option_metadata.fletcher.pb.h"),
              ReadFileBytes(mapped / "option_metadata.fletcher.pb.h"));
    EXPECT_NE(ReadFileBytes(plain / "option_metadata.Sample.ipc"),
              ReadFileBytes(mapped / "option_metadata.Sample.ipc"));
}

TEST(MetadataNoDriftTest, MalformedRulesFailProtocWithNonZeroExit) {
    const fs::path proto = fs::path(PROTO_DIR) / "nested.proto";
    ASSERT_TRUE(fs::exists(proto)) << proto;

    const std::array<const char*, 4> kBad = {
        "metadata_from_option=field:absent.pkg.col.unit",          // missing the key
        "metadata_from_option=nope:absent.pkg.col.unit:x:unit",    // unknown scope
        "metadata_from_option=field:absent.pkg.col.unit:",         // empty key
        "metadata_from_option=field::x:unit",                      // empty path
    };
    for (const char* bad : kBad) {
        const fs::path dir = MakeCaseDir("bad_" + Sanitize(bad).substr(0, 40));
        EXPECT_NE(RunPlugin({bad}, proto, dir), 0) << bad;
    }
}

TEST(MetadataNoDriftTest, RulesTargetingReservedKeysFailProtocWithNonZeroExit) {
    const fs::path proto = fs::path(PROTO_DIR) / "nested.proto";
    ASSERT_TRUE(fs::exists(proto)) << proto;

    // The four generator-owned keys may not be overwritten by a mapping.
    for (const char* key : {"proto_package", "proto_message", "field_number", "field_id"}) {
        const fs::path dir = MakeCaseDir(std::string("reserved_") + key);
        const std::string rule =
            std::string("metadata_from_option=field:absent.pkg.col.unit:") + key;
        EXPECT_NE(RunPlugin({rule}, proto, dir), 0) << key;
    }
}
