fn main() {
    // Build the C++ project with CMake and link the resulting static libraries.
    // Conan must have already generated the CMake toolchain files under
    // <repo_root>/build/build/generators/ before this runs.
    let dst = cmake::Config::new("../../")
        .define("CMAKE_BUILD_TYPE", "Release")
        .build_target("arrow_row_capi")
        .build();

    // CMake installs into <dst>/build/lib on most generators.
    // Adjust the search path if your generator uses a different layout
    // (e.g. MSVC puts release artifacts under build/Release).
    let lib_dir = dst.join("build");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());

    println!("cargo:rustc-link-lib=static=arrow_row_capi");
    println!("cargo:rustc-link-lib=static=row_codec");
    println!("cargo:rustc-link-lib=static=row_batcher");
    println!("cargo:rustc-link-lib=static=sqlite_dbs");

    // Dynamic system dependencies installed via Conan / system package manager.
    println!("cargo:rustc-link-lib=dylib=arrow");
    println!("cargo:rustc-link-lib=dylib=sqlite3");

    // Re-run this script when the C wrapper changes.
    println!("cargo:rerun-if-changed=../../src/arrow_row_capi.cpp");
    println!("cargo:rerun-if-changed=../../include/arrow_row_capi.h");
}
