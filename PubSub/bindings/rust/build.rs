use std::path::PathBuf;

fn main() {
    // Locate the pre-built C++ libraries from the CMake build tree.
    // The CMake build must have been run first (cmake --build build/build).
    let repo_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../..")
        .canonicalize()
        .expect("cannot resolve repo root");

    let build_dir = repo_root.join("build/build");

    // Use Release libraries — they are always built.  Debug builds may not
    // include the C API targets.
    let profile = "Release";

    // PubSub libraries
    println!(
        "cargo:rustc-link-search=native={}",
        build_dir.join("PubSub").join(profile).display()
    );

    // nanoarrow
    println!(
        "cargo:rustc-link-search=native={}",
        build_dir
            .join("third_party/nanoarrow")
            .join(profile)
            .display()
    );

    println!("cargo:rustc-link-lib=static=pubsub_capi");
    println!("cargo:rustc-link-lib=static=pubsub");
    println!("cargo:rustc-link-lib=static=nanoarrow");

    // MSVC C++ runtime (for C++ code linked as static libs).
    if cfg!(target_env = "msvc") {
        println!("cargo:rustc-link-lib=dylib=msvcprt");
    }

    println!("cargo:rerun-if-changed=../../src/pubsub_capi.cpp");
    println!("cargo:rerun-if-changed=../../include/pubsub/pubsub_capi.h");
}
