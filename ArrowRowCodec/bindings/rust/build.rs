fn main() {
    // Build the ArrowRowCodec C++ project via the root CMakeLists.txt.
    // Conan must have already generated the CMake toolchain files under
    // <repo_root>/build/build/generators/ before this runs.
    let dst = cmake::Config::new("../../..")
        .define("CMAKE_BUILD_TYPE", "Release")
        .build_target("arrow_row_codec_capi")
        .build();

    let lib_dir = dst.join("build");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());

    println!("cargo:rustc-link-lib=static=arrow_row_codec_capi");
    println!("cargo:rustc-link-lib=static=row_codec");

    // Dynamic system dependencies installed via Conan / system package manager.
    println!("cargo:rustc-link-lib=dylib=arrow");

    println!("cargo:rerun-if-changed=../../../ArrowRowCodec/src/arrow_row_codec_capi.cpp");
    println!("cargo:rerun-if-changed=../../../ArrowRowCodec/include/arrow_row_codec_capi.h");
}
