// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-2d: the single-copy check (D-BIND-17), exercised rather than asserted.
//
// ── Why a decoy module and not a mock ───────────────────────────────────────
// The check's whole job is to notice a SECOND module in a live process exporting
// `fl_single_copy_marker`. A test that stubbed the enumeration would prove the
// bookkeeping around the scan and nothing about the scan, which is the half that
// is platform code and the half that can silently find nothing — a typo in the
// symbol name, a truncated module array, an enumeration that quietly fails —
// with the same observable as a healthy process. So the suite loads a real
// module that really exports the symbol, and asserts the scan's answer changes.
//
// The decoy is a few lines of C (`single_copy_decoy.c`) rather than a second
// shim: a real second shim would take the entire eProsima chain with it for one
// assertion, and what the scan looks at is the export table, not the contents.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../src/single_copy.hpp"
#include "fletcher/abi/binding.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

/// The decoy's full path, derived from THIS executable's own location.
///
/// Built at runtime rather than baked in by CMake on purpose: a generator
/// expression yields a native path, and on Windows its backslashes become escape
/// sequences the moment the macro reaches a string literal. Only the file NAME
/// crosses the build boundary, and a file name contains no separators.
std::string DecoyPath() {
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
    return dir + "/" + FLETCHER_DECOY_NAME;
}

using fletcher::abi::MarkerText;
using fletcher::abi::ModulesExportingTheMarker;

/// A module loaded for the duration of one test, unloaded however the test ends.
class LoadedModule {
   public:
    explicit LoadedModule(const char* path) {
#if defined(_WIN32)
        handle_ = static_cast<void*>(LoadLibraryA(path));
#else
        handle_ = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
    }

    ~LoadedModule() {
        if (handle_ == nullptr) return;
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
    }

    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    [[nodiscard]] bool loaded() const { return handle_ != nullptr; }

   private:
    void* handle_ = nullptr;
};

/// The marker is what the scan looks for, so it has to be reachable through the
/// shim's export table — not merely defined somewhere in the process.
TEST(SingleCopy, TheMarkerIsExportedAndIdentifiesTheShim) {
    const char* marker = fl_single_copy_marker();
    ASSERT_NE(marker, nullptr);
    EXPECT_STREQ(marker, MarkerText());
    EXPECT_NE(std::string(marker).find("fletcher-c-abi"), std::string::npos)
        << "the marker does not name the component, so a duplicate report would not say what "
           "collided: "
        << marker;
}

/// One copy is the healthy answer, and it must be found.
///
/// The half of the scan that a stub would get wrong for free: if the
/// enumeration silently fails, or the symbol name is misspelled, this reads zero
/// — and zero is indistinguishable from "no duplicate" in production, which is
/// exactly how such a check rots unnoticed.
TEST(SingleCopy, TheShimItselfIsFound) {
    const std::vector<std::string> found = ModulesExportingTheMarker();
    ASSERT_EQ(found.size(), 1U)
        << "expected exactly this shim to export the marker; the scan found " << found.size();
    EXPECT_NE(found[0].find("fletcher-c-abi"), std::string::npos)
        << "the module found is not the shim: " << found[0];
}

/// The forcing test: a SECOND module exporting the marker is found, and named.
///
/// Without this row the check is a function nobody has ever seen fire. With it,
/// a scan that enumerates nothing, truncates its module array, or looks for the
/// wrong symbol is a red row rather than a silent all-clear.
TEST(SingleCopy, ASecondMarkerExportIsFound) {
    const std::vector<std::string> before = ModulesExportingTheMarker();
    ASSERT_EQ(before.size(), 1U) << "the process did not start from one copy, so nothing this "
                                    "row measures afterwards means anything";

    const std::string path = DecoyPath();
    const LoadedModule decoy(path.c_str());
    ASSERT_TRUE(decoy.loaded()) << "the decoy module did not load from " << path
                                << ", so this row proves nothing either way";

    const std::vector<std::string> during = ModulesExportingTheMarker();
    EXPECT_EQ(during.size(), 2U)
        << "the scan did not notice a second module exporting the marker. That is the ONLY "
           "detectable case of D-BIND-17's hazard, and if it is missed here it is missed in a "
           "host process where two copies of Fletcher keep separate delivery-frame stacks and "
           "the re-entrancy doors stop refusing what they must refuse";

    // Named, not just counted: "two copies of Fletcher" without saying which two
    // costs its reader an afternoon.
    bool names_decoy = false;
    for (const std::string& path : during) {
        if (path.find("single_copy_decoy") != std::string::npos) names_decoy = true;
    }
    EXPECT_TRUE(names_decoy) << "the report did not name the second module, only counted it";
}

/// Unloading the second copy puts the answer back, so the scan reads the process
/// as it is now rather than latching the first thing it ever saw.
TEST(SingleCopy, TheAnswerFollowsTheProcess) {
    {
        const LoadedModule decoy(DecoyPath().c_str());
        ASSERT_TRUE(decoy.loaded());
        ASSERT_EQ(ModulesExportingTheMarker().size(), 2U);
    }
    EXPECT_EQ(ModulesExportingTheMarker().size(), 1U)
        << "the scan still reports the unloaded decoy, so it is caching rather than looking";
}

/// This process is healthy, so nothing is poisoned and every entry point works.
///
/// The other side of the refusal: a latched verdict that fired on a single-copy
/// process would take every call down with it, which is a far worse failure than
/// the one the check exists to prevent.
///
/// It asserts through the ABI and NOWHERE ELSE, deliberately. An earlier version
/// also checked `SingleCopyRefusal().empty()` and said, on failure, that "the
/// shim latched a single-copy conflict" — but that reads the OBJECT LIBRARY's
/// copy of the slot, linked into this test binary, while the shim's own copy is
/// local (the version script on Linux, no dllexport on Windows). Nothing but
/// `SetSingleCopyRefusalForTest` ever writes the copy it read, so it could not
/// fail, and its message named something it had not looked at. The call below is
/// the only thing here that can observe the shim's verdict.
TEST(SingleCopy, AHealthyProcessIsNotPoisoned) {
    // Reached through the ABI, because that is where the refusal would surface.
    fl_error err = {};
    fl_provider* provider = nullptr;
    const fl_provider_config config = {};
    const fl_str selector = {reinterpret_cast<const uint8_t*>("inprocess"), 9};
    ASSERT_EQ(fl_provider_create(selector, &config, &provider, &err), FL_OK)
        << "a healthy shim refused an ordinary call";
    fl_provider_destroy(provider);
}

}  // namespace
