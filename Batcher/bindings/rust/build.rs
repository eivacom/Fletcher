fn main() {
    // Build the Batcher C++ project via the root CMakeLists.txt.
    // Conan must have already generated the CMake toolchain files under
    // <repo_root>/build/build/generators/ before this runs.
    let dst = cmake::Config::new("../../..")
        .define("CMAKE_BUILD_TYPE", "Release")
        .build_target("arrow_row_batcher_capi")
        .build();

    let lib_dir = dst.join("build");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());

    println!("cargo:rustc-link-lib=static=arrow_row_batcher_capi");
    println!("cargo:rustc-link-lib=static=row_batcher");
    println!("cargo:rustc-link-lib=static=sqlite_dbs");
    // row_codec and arrow_row_codec_capi are pulled in transitively via the
    // arrow-row-codec crate, but listed here explicitly so the linker can
    // resolve the symbols they provide (e.g. arrow_row_free_string).
    println!("cargo:rustc-link-lib=static=arrow_row_codec_capi");
    println!("cargo:rustc-link-lib=static=row_codec");

    // Dynamic system dependencies installed via Conan / system package manager.
    println!("cargo:rustc-link-lib=dylib=arrow");
    println!("cargo:rustc-link-lib=dylib=sqlite3");

    println!("cargo:rerun-if-changed=../../../Batcher/src/arrow_row_batcher_capi.cpp");
    println!("cargo:rerun-if-changed=../../../Batcher/include/arrow_row_batcher_capi.h");
}
