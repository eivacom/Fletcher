// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D9S-7, rung 9 (d9 L5, L16, L17; BC-15): a process that holds BOTH builds of the shim, in
// either load order, gets one working shim. The later copy refuses every call that can fail and
// names both files, and still says which build and version it is.
//
// ── Why two executables from this one source ────────────────────────────────
// The two builds export the same names, so one program cannot link both. Each executable links
// ONE build and loads the OTHER by path, once, through the same runtime path derivation the decoy
// uses. `server-first` links the server shim and loads the edge twin; `edge-first` links the edge
// twin and loads the server shim. That is the two load orders.
//
// The edge twin is a test-only rebuild of the edge build's sources (CMakeLists.txt in this
// directory, server package only): the shipped edge file lives in another package and is never in
// this one's graph. The decoy proves the scan sees a second marker; THIS proves what a real second
// shim does about it.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "fletcher/abi/binding.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifndef FLETCHER_TWO_BUILDS_LINKED_VARIANT
#error "FLETCHER_TWO_BUILDS_LINKED_VARIANT (1 edge, 2 server): which build this executable links"
#endif
#ifndef FLETCHER_TWO_BUILDS_LINKED_NAME
#error "FLETCHER_TWO_BUILDS_LINKED_NAME: the file name of the build this executable links"
#endif
#ifndef FLETCHER_TWO_BUILDS_OTHER_NAME
#error "FLETCHER_TWO_BUILDS_OTHER_NAME: the file name of the build this executable loads"
#endif

namespace {

// The two builds' wire values as LITERALS (binding.h's promise), mapped by CMake from the target
// each executable links, never read back from a shim.
constexpr int kLinkedVariant = FLETCHER_TWO_BUILDS_LINKED_VARIANT;
constexpr int kOtherVariant = kLinkedVariant == 1 ? 2 : 1;

/// The other build's file, in THIS executable's own directory, derived at runtime (a native path
/// from a generator expression has backslashes that become escapes in a string literal).
std::string OtherPath() {
    std::string exe;
#if defined(_WIN32)
    char buffer[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    exe.assign(buffer, len);
#else
    char buffer[4096] = {};
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) exe.assign(buffer, static_cast<size_t>(len));
#endif
    const size_t cut = exe.find_last_of("/\\");
    const std::string dir = cut == std::string::npos ? std::string(".") : exe.substr(0, cut);
    return dir + "/" + FLETCHER_TWO_BUILDS_OTHER_NAME;
}

/// The loaded copy's entry points, by name. Resolved from its own handle, never through the
/// process's global scope, so a call cannot bind to the linked copy by accident.
struct LoadedBuild {
    void* handle = nullptr;
    fl_variant (*variant)(void) = nullptr;
    uint32_t (*abi_version)(void) = nullptr;
    const char* (*marker)(void) = nullptr;
    fl_status (*provider_create)(fl_str, const fl_provider_config*, fl_provider**,
                                 fl_error*) = nullptr;
    void (*provider_destroy)(fl_provider*) = nullptr;
    void (*error_dispose)(fl_error*) = nullptr;
};

template <typename F>
F Resolve(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<F>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return reinterpret_cast<F>(dlsym(handle, name));
#endif
}

/// Loaded ONCE per process and never unloaded: a refused copy stays loaded until exit, as it does
/// in a host, and each message is freed by the copy that wrote it.
const LoadedBuild& Other() {
    static const LoadedBuild loaded = [] {
        LoadedBuild b;
        const std::string path = OtherPath();
#if defined(_WIN32)
        b.handle = static_cast<void*>(LoadLibraryA(path.c_str()));
#else
        b.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (b.handle == nullptr) return b;
        b.variant = Resolve<decltype(b.variant)>(b.handle, "fl_binding_variant");
        b.abi_version = Resolve<decltype(b.abi_version)>(b.handle, "fl_binding_abi_version");
        b.marker = Resolve<decltype(b.marker)>(b.handle, "fl_single_copy_marker");
        b.provider_create = Resolve<decltype(b.provider_create)>(b.handle, "fl_provider_create");
        b.provider_destroy =
            Resolve<decltype(b.provider_destroy)>(b.handle, "fl_provider_destroy");
        b.error_dispose = Resolve<decltype(b.error_dispose)>(b.handle, "fl_error_dispose");
        return b;
    }();
    return loaded;
}

/// A loaded build is usable by the test only if every name resolved; a load that failed must be a
/// red row, not a pass on the strength of nothing having been asked.
void RequireLoaded(const LoadedBuild& b) {
    ASSERT_NE(b.handle, nullptr) << "the other build did not load from " << OtherPath()
                                 << ", so this row proves nothing either way";
    ASSERT_NE(b.variant, nullptr);
    ASSERT_NE(b.abi_version, nullptr);
    ASSERT_NE(b.provider_create, nullptr);
    ASSERT_NE(b.provider_destroy, nullptr);
    ASSERT_NE(b.error_dispose, nullptr);
}

const fl_str kInProcess = {reinterpret_cast<const uint8_t*>("inprocess"), 9};

TEST(TwoBuilds, TheLaterBuildRefusesNamingBothAndTheFirstKeepsWorking) {
    const LoadedBuild& other = Other();
    ASSERT_NO_FATAL_FAILURE(RequireLoaded(other));

    // The loaded file is the OTHER build, and the linked one is the one this executable says it
    // is: a "second copy" that is the same build loaded twice, or a mis-staged file, fails here.
    ASSERT_EQ(static_cast<int>(fl_binding_variant()), kLinkedVariant)
        << "the linked build is not the one this executable is built to link";
    ASSERT_EQ(static_cast<int>(other.variant()), kOtherVariant)
        << "the loaded file does not answer to the other build";

    // The later copy refuses, internal error, and the message names BOTH files.
    fl_error refused = {};
    fl_provider* never = nullptr;
    const fl_provider_config config = {};
    const fl_status status = other.provider_create(kInProcess, &config, &never, &refused);
    EXPECT_EQ(status, FL_INTERNAL)
        << "the later-loaded copy served a call that can fail (L5): a second working shim";
    ASSERT_NE(refused.message, nullptr);
    const std::string message(reinterpret_cast<const char*>(refused.message), refused.message_len);
    other.error_dispose(&refused);  // freed by the copy that wrote it
    EXPECT_NE(message.find(FLETCHER_TWO_BUILDS_LINKED_NAME), std::string::npos)
        << "the refusal does not name the first copy: " << message;
    EXPECT_NE(message.find(FLETCHER_TWO_BUILDS_OTHER_NAME), std::string::npos)
        << "the refusal does not name the later copy: " << message;

    // The first copy is unaffected: it was healthy when it started and stays so.
    fl_error err = {};
    fl_provider* provider = nullptr;
    ASSERT_EQ(fl_provider_create(kInProcess, &config, &provider, &err), FL_OK)
        << "the first-loaded copy was poisoned by the second one arriving";
    fl_provider_destroy(provider);
}

TEST(TwoBuilds, ARefusedBuildStillAnswersItsVariantAndVersion) {
    const LoadedBuild& other = Other();
    ASSERT_NO_FATAL_FAILURE(RequireLoaded(other));

    // First make sure it IS refusing, so "still answers" is about a refused copy and not a
    // healthy one (a refusal that never happened would make this row green for nothing).
    fl_error refused = {};
    fl_provider* never = nullptr;
    const fl_provider_config config = {};
    ASSERT_EQ(other.provider_create(kInProcess, &config, &never, &refused), FL_INTERNAL)
        << "the loaded copy is not refusing, so it is not the refused copy this row is about";
    other.error_dispose(&refused);

    // L16: the variant query answers, with the refused copy's OWN build.
    EXPECT_EQ(static_cast<int>(other.variant()), kOtherVariant)
        << "a refused build must still say which build it is";
    // L5: the version answers, and it is the header's.
    EXPECT_EQ(other.abi_version(), static_cast<uint32_t>(FL_BINDING_ABI_VERSION));
    // A32 (L13's list stays true): the marker text is the linked copy's, byte for byte. Looked up
    // here and not in RequireLoaded, so a build whose marker is missing fails the REFUSAL rows
    // above for what it did (served a call), not for a name that did not resolve.
    ASSERT_NE(other.marker, nullptr) << "the refused copy does not export the marker";
    ASSERT_NE(other.marker(), nullptr);
    EXPECT_STREQ(other.marker(), fl_single_copy_marker());
}

}  // namespace
