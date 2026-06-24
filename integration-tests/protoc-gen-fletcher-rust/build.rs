// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! Build script for the RBA-5 Rust integration crate. Two jobs:
//!
//!   1. Run the locally-built `protoc-gen-fletcher` plugin on every fixture
//!      `.proto` with `--fletcher_opt=rust`, emitting `<stem>.fletcher.rs` into
//!      `OUT_DIR`.
//!   2. Assemble those bare-item files into a single `fletcher_gen.rs` module
//!      tree, grouped by proto package (D-RBA-10), and `include!`-mounted by
//!      `src/lib.rs`.
//!
//! Plugin discovery is via two environment variables — no network, no download:
//!   * `PROTOC`                 absolute path to `protoc` (falls back to a Conan
//!                              cache lookup, then to `protoc` on PATH).
//!   * `FLETCHER_PROTOC_PLUGIN` absolute path to the built plugin binary (falls
//!                              back to a Conan cache lookup).
//! The build fails with a clear message if either tool cannot be found.

use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

// (fixture file stem, proto package) — keyed on the proto PACKAGE, not the file
// stem (stems can collide / carry separators, D-RBA-10). sensors_a + sensors_b
// deliberately share `fletcher.rba.shared` to exercise same-package mounting.
//
// RBA-6a adds the cross-file / cross-package composite fixture:
//   * geo_child       (package `rba.child`) — the imported grandchild Leaf;
//   * nopkg_child     (NO package, "")      — a no-package imported message,
//                                             mounted directly under fletcher_gen
//                                             (D-RBA-10 no-package edge case);
//   * composite_main  (package `rba.main`)  — the top-level message + the
//                                             Outer/Inner struct chain + the
//                                             no-package Tag struct field;
//   * composite_aux   (package `rba.main`)  — a SAME-package second file that
//                                             also emits a composite getter, so
//                                             the co-mount collision check (R1)
//                                             is actually exercised.
// `transitive_gate` (package `rba.transitive`) exercises the D-RBA-8 transitive
// support gate: it defines a message with an unsupported (depth-4) nested list and
// three holders that reference it through STRUCT / REPEATED_STRUCT / message-MAP.
// With the transitive gate, all of them are skipped (fail-fast comments) and the
// crate still compiles — the file therefore generates no accessor items.
const FIXTURES: &[(&str, &str)] = &[
    ("telemetry", "fletcher.rba.telem"),
    ("sensors_a", "fletcher.rba.shared"),
    ("sensors_b", "fletcher.rba.shared"),
    ("geo_child", "rba.child"),
    ("nopkg_child", ""),
    ("composite_main", "rba.main"),
    ("composite_aux", "rba.main"),
    ("transitive_gate", "rba.transitive"),
];

// RBA-7 capstone (D-RBA-8). The SHARED capstone proto lives OUTSIDE this crate,
// in integration-tests/accessor-capstone/proto/, and is the SAME schema the C++
// gtest generates from. (stem, package, source-dir-relative-to-manifest). The
// Rust accessor_capstone.rs test builds an arrow-rs batch from the SAME committed
// fixture/expected JSON the C++ side reads, proving cross-language parity.
const SHARED_FIXTURES: &[(&str, &str, &str)] = &[(
    "accessor_capstone",
    "rba.capstone",
    "../accessor-capstone/proto",
)];

// The plugin emits this shared helper module (the `__rba` span/Row helpers) once
// per protoc run. Because it carries ZERO per-file/per-message content, every
// copy written into OUT_DIR is byte-identical, and the assembler include!s it
// exactly once directly under `crate::fletcher_gen::__rba` (N1).
const RBA_HELPER_FILE: &str = "__rba.fletcher.rs";

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let proto_dir = manifest_dir.join("proto");
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    // Re-run triggers: a fixture edit or a plugin/protoc change.
    for (stem, _pkg) in FIXTURES {
        println!(
            "cargo:rerun-if-changed=proto/{stem}.proto",
            stem = stem
        );
    }
    // Shared (cross-crate) capstone proto + its committed fixture/expected JSON.
    for (stem, _pkg, src) in SHARED_FIXTURES {
        println!("cargo:rerun-if-changed={src}/{stem}.proto");
    }
    println!("cargo:rerun-if-changed=../accessor-capstone/fixtures/accessor_capstone_fixture.json");
    println!("cargo:rerun-if-changed=../accessor-capstone/fixtures/accessor_capstone_expected.json");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=FLETCHER_PROTOC_PLUGIN");
    println!("cargo:rerun-if-env-changed=PROTOC");

    let protoc = locate_protoc();
    let plugin = locate_plugin();

    // Export the resolved protoc + plugin paths (and the proto dir + WKT include)
    // to the test binaries so the multi-file-invocation test (code-review P1) can
    // shell out to the same plugin the build used, with no extra env wiring.
    println!("cargo:rustc-env=FLETCHER_TEST_PROTOC={}", protoc.display());
    println!("cargo:rustc-env=FLETCHER_TEST_PLUGIN={}", plugin.display());
    println!("cargo:rustc-env=FLETCHER_TEST_PROTO_DIR={}", proto_dir.display());
    // The generated-output dir, so tests can read emitted .fletcher.rs files (the
    // transitive-gate test inspects the skip/comment path in transitive_gate).
    println!("cargo:rustc-env=FLETCHER_TEST_OUT_DIR={}", out_dir.display());
    // The SHARED capstone fixtures dir, so accessor_capstone.rs reads the SAME
    // committed fixture/expected JSON the C++ gtest reads (single source of truth).
    let capstone_fixtures = manifest_dir.join("../accessor-capstone/fixtures");
    println!(
        "cargo:rustc-env=FLETCHER_TEST_CAPSTONE_FIXTURES={}",
        capstone_fixtures.display()
    );
    if let Some(wkt) = wkt_include_dir(&protoc) {
        println!("cargo:rustc-env=FLETCHER_TEST_WKT_INCLUDE={}", wkt.display());
    } else if let Ok(inc) = env::var("PROTOBUF_WKT_INCLUDE_DIR") {
        println!("cargo:rustc-env=FLETCHER_TEST_WKT_INCLUDE={inc}");
    } else {
        println!("cargo:rustc-env=FLETCHER_TEST_WKT_INCLUDE=");
    }

    // Job 1: run the plugin on each fixture into OUT_DIR.
    for (stem, _pkg) in FIXTURES {
        let proto_file = proto_dir.join(format!("{stem}.proto"));
        if !proto_file.exists() {
            panic!("fixture proto missing: {}", proto_file.display());
        }
        run_plugin(&protoc, &plugin, &proto_dir, &proto_file, &out_dir);
    }
    // Job 1b: run the plugin on each SHARED (cross-crate) fixture. Its `-I` root
    // is the shared proto dir (so its own imports resolve), and the WKT/fletcher
    // option roots are added by run_plugin as usual.
    for (stem, _pkg, src) in SHARED_FIXTURES {
        let shared_dir = manifest_dir.join(src);
        let proto_file = shared_dir.join(format!("{stem}.proto"));
        if !proto_file.exists() {
            panic!("shared fixture proto missing: {}", proto_file.display());
        }
        run_plugin(&protoc, &plugin, &shared_dir, &proto_file, &out_dir);
    }

    // Job 2: assemble fletcher_gen.rs from the generated bare-item files.
    let aggregator = assemble_module_tree(&out_dir);
    let agg_path = out_dir.join("fletcher_gen.rs");
    fs::write(&agg_path, aggregator)
        .unwrap_or_else(|e| panic!("failed to write {}: {e}", agg_path.display()));
}

/// Locate the `protoc` executable. The `PROTOC` env var is AUTHORITATIVE
/// (design §4): if set, it is used; if it points at a missing path, the build
/// fails clearly. If `PROTOC` is unset, a LOUD local-dev convenience fallback
/// (Conan cache, then PATH) is attempted — every fallback prints a warning
/// naming the resolved path and that it is a fallback (never silent). CI always
/// sets `PROTOC`, so CI never takes the fallback.
fn locate_protoc() -> PathBuf {
    // An empty / whitespace-only PROTOC is treated the SAME as unset: it is the
    // common `export PROTOC=$(command -v protoc)` idiom misfiring when protoc is
    // not on PATH (e.g. Conan-provided in CI). Fall through to the loud local-dev
    // fallback instead of panicking. A NON-empty but nonexistent path is still a
    // real misconfiguration and still panics clearly.
    if let Ok(p) = env::var("PROTOC") {
        if !p.trim().is_empty() {
            let path = PathBuf::from(&p);
            if path.exists() {
                return path;
            }
            panic!(
                "error: PROTOC is set to '{p}' but that path does not exist. \
                 Point it at a real protoc executable, or unset PROTOC to use the \
                 local-dev fallback (see README)."
            );
        }
    }
    if let Some(found) = find_in_conan_cache("protoc") {
        warn_fallback("PROTOC", "Conan cache", &found);
        return found;
    }
    if let Some(found) = which("protoc") {
        warn_fallback("PROTOC", "PATH", &found);
        return found;
    }
    panic!(
        "error: PROTOC is not set and no protoc could be located via the \
         local-dev fallback (Conan cache or PATH). Set PROTOC to the absolute \
         path of a protoc executable (CI sets it; see README)."
    );
}

/// Locate the built `protoc-gen-fletcher` (a.k.a. `fletcher-protoc`) plugin. The
/// `FLETCHER_PROTOC_PLUGIN` env var is AUTHORITATIVE (design §4): if set, it is
/// used; if it points at a missing path, the build fails clearly. If unset, a
/// LOUD local-dev convenience fallback (Conan cache) is attempted with a warning
/// naming the resolved path; if that finds nothing, the build fails clearly.
/// Never a silent fallback (a stale binary must not mask a generator regression).
fn locate_plugin() -> PathBuf {
    // An empty / whitespace-only FLETCHER_PROTOC_PLUGIN is treated the SAME as
    // unset (the `export VAR=$(command -v ...)`-when-not-on-PATH idiom), falling
    // through to the loud local-dev Conan-cache fallback rather than panicking. A
    // NON-empty but nonexistent path is still a real misconfiguration and panics.
    if let Ok(p) = env::var("FLETCHER_PROTOC_PLUGIN") {
        if !p.trim().is_empty() {
            let path = PathBuf::from(&p);
            if path.exists() {
                return path;
            }
            panic!(
                "error: FLETCHER_PROTOC_PLUGIN is set to '{p}' but that path does not \
                 exist. Build the plugin first (conan create protoc/.) and point \
                 FLETCHER_PROTOC_PLUGIN at the resulting fletcher-protoc binary, or \
                 unset it to use the local-dev fallback (see README)."
            );
        }
    }
    if let Some(found) = find_in_conan_cache("fletcher-protoc") {
        warn_fallback("FLETCHER_PROTOC_PLUGIN", "Conan cache", &found);
        return found;
    }
    panic!(
        "error: FLETCHER_PROTOC_PLUGIN is not set and no fletcher-protoc binary \
         could be located via the local-dev Conan-cache fallback; build the \
         plugin first (conan create protoc/. --build=missing -pr:a=<profile>) \
         and export FLETCHER_PROTOC_PLUGIN=<path to fletcher-protoc[.exe]> (CI \
         sets it; see README). No fallback download is performed."
    );
}

/// Emit a loud cargo warning when a tool is resolved via the local-dev fallback
/// rather than its authoritative env var — so a stale binary can never silently
/// mask a generator regression (design §4).
fn warn_fallback(var: &str, source: &str, path: &Path) {
    println!(
        "cargo:warning={var} not set; using local-dev FALLBACK from {source}: {}. \
         Set {var} explicitly for a deterministic build (CI does).",
        path.display()
    );
}

/// Search the local Conan cache for a built binary. Used ONLY as a loud
/// local-dev fallback (and as the reliable CI safety net) when the authoritative
/// env var is unset (see locate_protoc / locate_plugin). Returns the NEWEST
/// match by mtime, or None.
///
/// CI revealed that the binary is NOT always at the strict
/// `~/.conan2/p/.../p/bin/<name>[.exe]` layout the prior version assumed (the
/// exact subpath differs across runners), so this recursively walks the Conan
/// home and matches ANY file named exactly `<name>`/`<name>.exe` wherever it
/// sits. The Conan home is resolved robustly: `CONAN_HOME` if set, else
/// `~/.conan2`.
///
/// Candidates are RANKED so we never regress on a binary that happens to be
/// newest but unusable: a copy whose parent directory is `bin` (the Conan
/// PACKAGE layout `.../p/bin/<name>`) is preferred over one that is not (the
/// in-tree BUILD layout `.../b/build/Release/<name>`), because the package
/// layout carries the sibling `../include` tree (WKT protos for protoc; fletcher
/// option protos for the plugin) that the build layout lacks. Within the same
/// rank, the newest by mtime wins.
fn find_in_conan_cache(name: &str) -> Option<PathBuf> {
    let root = conan_home()?;
    if !root.exists() {
        return None;
    }
    let target = exe_name(name);
    let mut matches: Vec<PathBuf> = Vec::new();
    find_named_recursive(&root, &target, &mut matches);
    matches.into_iter().max_by_key(|p| {
        let in_bin_dir = p
            .parent()
            .and_then(|d| d.file_name())
            .map(|n| n == "bin")
            .unwrap_or(false);
        let mtime = p
            .metadata()
            .and_then(|m| m.modified())
            .unwrap_or(std::time::UNIX_EPOCH);
        // bin-dir copies outrank non-bin ones; newest breaks ties.
        (in_bin_dir, mtime)
    })
}

/// Resolve the Conan home directory: honour `CONAN_HOME` if set (and
/// non-empty), else fall back to `<USERPROFILE|HOME>/.conan2`.
fn conan_home() -> Option<PathBuf> {
    if let Ok(h) = env::var("CONAN_HOME") {
        if !h.trim().is_empty() {
            return Some(PathBuf::from(h));
        }
    }
    let home = env::var("USERPROFILE").or_else(|_| env::var("HOME")).ok()?;
    Some(Path::new(&home).join(".conan2"))
}

/// Recursively walk `dir`, collecting every file whose file name equals
/// `target` into `matches`. Skips paths it cannot read (best-effort) and does
/// not follow symlinked directories (cycle / escape guard).
fn find_named_recursive(dir: &Path, target: &str, matches: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let file_type = match entry.file_type() {
            Ok(ft) => ft,
            Err(_) => continue,
        };
        if file_type.is_dir() {
            // Don't follow symlinked directories (cycle / escape guard).
            find_named_recursive(&path, target, matches);
        } else if file_type.is_file()
            && path.file_name().map(|n| n == target).unwrap_or(false)
        {
            matches.push(path);
        }
    }
}

/// Derive the well-known-types include directory from the protoc binary path:
/// the bundled WKT protos live at `<protoc_dir>/../include`. Returns that path if
/// it actually contains `google/protobuf/timestamp.proto`, else None.
fn wkt_include_dir(protoc: &Path) -> Option<PathBuf> {
    let bin_dir = protoc.parent()?;
    let include = bin_dir.parent()?.join("include");
    if include
        .join("google")
        .join("protobuf")
        .join("timestamp.proto")
        .is_file()
    {
        Some(include)
    } else {
        None
    }
}

/// Derive the Fletcher proto-options include directory from the plugin binary
/// path: the conan-packaged option protos live at `<plugin_dir>/../include`
/// (fletcher/options.proto). Returns that path if it actually contains
/// `fletcher/options.proto`, else None. The composite NESTED_LIST fixture imports
/// it (the `(fletcher.flatten)` wrapper is the only way to express list<list<…>>).
fn fletcher_include_dir(plugin: &Path) -> Option<PathBuf> {
    let bin_dir = plugin.parent()?;
    let include = bin_dir.parent()?.join("include");
    if include.join("fletcher").join("options.proto").is_file() {
        Some(include)
    } else {
        None
    }
}

/// Run the plugin once for one fixture proto.
fn run_plugin(
    protoc: &Path,
    plugin: &Path,
    proto_dir: &Path,
    proto_file: &Path,
    out_dir: &Path,
) {
    let mut cmd = Command::new(protoc);
    cmd.arg(format!(
        "--plugin=protoc-gen-fletcher={}",
        plugin.display()
    ))
    .arg("--fletcher_opt=rust")
    .arg(format!("--fletcher_out={}", out_dir.display()))
    .arg("-I")
    .arg(proto_dir)
    .arg(proto_file);

    // Well-known-types include root (telemetry.proto imports
    // google/protobuf/{timestamp,duration}.proto). Honour an explicit
    // PROTOBUF_WKT_INCLUDE_DIR if set, else derive it from the protoc binary
    // (`<protoc_dir>/../include`, where the bundled WKT protos live) so the
    // fixture stays hermetic with no extra env needed.
    if let Ok(inc) = env::var("PROTOBUF_WKT_INCLUDE_DIR") {
        cmd.arg("-I").arg(inc);
    } else if let Some(wkt) = wkt_include_dir(protoc) {
        cmd.arg("-I").arg(wkt);
    }
    // Fletcher options include root (composite_main.proto imports
    // fletcher/options.proto for the NESTED_LIST flatten wrappers). Honour an
    // explicit FLETCHER_PROTO_INCLUDE_DIR if set, else derive it from the plugin
    // binary (`<plugin_dir>/../include`, where the conan package ships the option
    // protos) so the fixture stays hermetic.
    if let Ok(inc) = env::var("FLETCHER_PROTO_INCLUDE_DIR") {
        cmd.arg("-I").arg(inc);
    } else if let Some(fl) = fletcher_include_dir(plugin) {
        cmd.arg("-I").arg(fl);
    }

    let output = cmd
        .output()
        .unwrap_or_else(|e| panic!("failed to launch protoc ({}): {e}", protoc.display()));
    if !output.status.success() {
        panic!(
            "protoc plugin run failed for {}:\n--- stdout ---\n{}\n--- stderr ---\n{}",
            proto_file.display(),
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr),
        );
    }
}

/// Build the `fletcher_gen.rs` aggregator: declare the package module tree once
/// (each package `mod` exactly once, no matter how many files contribute) and
/// `include!` every generated file into its package's innermost module.
fn assemble_module_tree(out_dir: &Path) -> String {
    // Group generated files by package, in fixture order (deterministic).
    // package -> Vec<absolute generated .rs path>
    let mut by_pkg: BTreeMap<&str, Vec<PathBuf>> = BTreeMap::new();
    for (stem, pkg) in FIXTURES {
        let gen = out_dir.join(format!("{stem}.fletcher.rs"));
        if !gen.exists() {
            panic!(
                "expected generated file missing: {} (plugin did not emit it?)",
                gen.display()
            );
        }
        by_pkg.entry(pkg).or_default().push(gen);
    }
    // Shared (cross-crate) fixtures mount into their proto package exactly like
    // the crate-local ones (the capstone's `rba.capstone` accessor is reached at
    // crate::fletcher_gen::rba::capstone::CapstoneBatchAccessor).
    for (stem, pkg, _src) in SHARED_FIXTURES {
        let gen = out_dir.join(format!("{stem}.fletcher.rs"));
        if !gen.exists() {
            panic!(
                "expected generated shared file missing: {} (plugin did not emit it?)",
                gen.display()
            );
        }
        by_pkg.entry(pkg).or_default().push(gen);
    }

    let mut s = String::new();
    s.push_str("// @generated by build.rs (RBA-5 module assembler, D-RBA-10). DO NOT EDIT.\n");
    s.push_str("pub mod fletcher_gen {\n");

    // RBA-6a N1: the plugin emits `__rba.fletcher.rs` once per protoc run, so it
    // is overwritten N times (one per fixture) with BYTE-IDENTICAL content (it has
    // no per-file/per-message interpolation). Mount it EXACTLY ONCE, directly under
    // `fletcher_gen`, ABOVE the package tree so every package module can reach it
    // via `crate::fletcher_gen::__rba::…`.
    let rba_helper = out_dir.join(RBA_HELPER_FILE);
    if !rba_helper.exists() {
        panic!(
            "expected RBA helper file missing: {} (plugin did not emit __rba.fletcher.rs?)",
            rba_helper.display()
        );
    }
    let rba_path = rba_helper.to_string_lossy().replace('\\', "/");
    s.push_str("    pub mod __rba {\n");
    s.push_str(&format!("        include!(\"{rba_path}\");\n"));
    s.push_str("    }\n");

    // Build a nested tree of package segments so each `mod` is declared once.
    let mut root = Node::default();
    for (pkg, files) in &by_pkg {
        let mut node = &mut root;
        if !pkg.is_empty() {
            for seg in pkg.split('.') {
                node = node.children.entry(seg.to_string()).or_default();
            }
        }
        node.includes.extend(files.iter().cloned());
    }

    emit_node(&mut s, &root, 1);

    s.push_str("}\n");
    s
}

/// Recursively emit a package module node. `depth` controls indentation only.
fn emit_node(s: &mut String, node: &Node, depth: usize) {
    let indent = "    ".repeat(depth);
    // Includes mounted directly at this module.
    for inc in &node.includes {
        // Use a literal absolute path; forward slashes are valid in Rust string
        // literals on every platform (rustc accepts them in include! on Windows).
        let path = inc.to_string_lossy().replace('\\', "/");
        s.push_str(&format!("{indent}include!(\"{path}\");\n"));
    }
    for (seg, child) in &node.children {
        let ident = rust_ident(seg);
        s.push_str(&format!("{indent}pub mod {ident} {{\n"));
        emit_node(s, child, depth + 1);
        s.push_str(&format!("{indent}}}\n"));
    }
}

// The package-module tree node: child segments + the generated-file includes
// that mount at this module level.
#[derive(Default)]
struct Node {
    children: BTreeMap<String, Node>,
    includes: Vec<PathBuf>,
}

/// Sanitize one package segment for Rust (D-RBA-10 keyword rule):
///   * a keyword usable as a RAW identifier → emit `r#<seg>` (deterministic,
///     preserves `package a.b.c → a::b::c`);
///   * `crate`/`self`/`Self`/`super` CANNOT be raw identifiers, so a package
///     segment equal to one of those is a generation error — we **panic** with a
///     clear diagnostic rather than silently rename it (which would break the
///     convention and risk collisions).
/// The RBA-5 fixtures use no keyword segments.
fn rust_ident(seg: &str) -> String {
    // Keywords that CANNOT be raw identifiers (rustc rejects `r#crate` etc.).
    const NON_RAW: &[&str] = &["crate", "self", "Self", "super"];
    if NON_RAW.contains(&seg) {
        panic!(
            "error: proto package segment '{seg}' is a Rust keyword that cannot be \
             used as an identifier (even as a raw identifier r#{seg}); the \
             deterministic package→module mapping (D-RBA-10) cannot represent it. \
             Rename the proto package segment."
        );
    }
    // Keywords that ARE valid as raw identifiers → r#<seg>.
    const RAW_KEYWORDS: &[&str] = &[
        "as", "break", "const", "continue", "dyn", "else", "enum", "extern", "false", "fn", "for",
        "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
        "static", "struct", "trait", "true", "type", "unsafe", "use", "where", "while", "async",
        "await", "abstract", "become", "box", "do", "final", "macro", "override", "priv", "typeof",
        "unsized", "virtual", "yield", "try", "union",
    ];
    if RAW_KEYWORDS.contains(&seg) {
        return format!("r#{seg}");
    }
    seg.to_string()
}

fn exe_name(base: &str) -> String {
    if cfg!(windows) {
        format!("{base}.exe")
    } else {
        base.to_string()
    }
}

/// Minimal PATH probe (no extra deps): true if `name` resolves on PATH.
fn which(name: &str) -> Option<PathBuf> {
    let exe = exe_name(name);
    let path = env::var_os("PATH")?;
    for dir in env::split_paths(&path) {
        let candidate = dir.join(&exe);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}
