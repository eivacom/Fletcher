// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "single_copy.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

// Deliberately NOT including binding.h: this file looks the marker up BY NAME
// through the platform loader and never calls it, so it needs the symbol's
// spelling and not its declaration. That keeps the object library free of the
// public header's include path, which it does not otherwise carry.

#if defined(_WIN32)
#include <windows.h>
// psapi.h after windows.h, and in that order only.
#include <psapi.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

namespace fletcher::abi {
namespace {

/// The symbol looked for. Spelled once: a typo here would make the check
/// silently find nothing, which is the same observable as a healthy process.
constexpr const char* kMarkerSymbol = "fl_single_copy_marker";

#if defined(_WIN32)

std::vector<std::string> ScanModules() {
    std::vector<std::string> found;

    const HANDLE process = GetCurrentProcess();
    // Two passes: the first learns how many modules there are, because a fixed
    // array would silently truncate on a host with many DLLs loaded - and
    // truncation here reads exactly like "no duplicate".
    DWORD needed = 0;
    if (EnumProcessModules(process, nullptr, 0, &needed) == 0 || needed == 0) return found;

    std::vector<HMODULE> modules(needed / sizeof(HMODULE));
    if (EnumProcessModules(process, modules.data(), needed, &needed) == 0) return found;
    modules.resize(needed / sizeof(HMODULE));

    for (HMODULE module : modules) {
        if (GetProcAddress(module, kMarkerSymbol) == nullptr) continue;
        char path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
        found.emplace_back(len > 0 ? std::string(path, len) : std::string("<unnamed module>"));
    }
    return found;
}

#else

int CollectModuleName(struct dl_phdr_info* info, size_t, void* out) {
    auto* names = static_cast<std::vector<std::string>*>(out);
    // The MAIN EXECUTABLE is skipped, and this is the one place the Linux scan
    // deliberately looks at less than the Windows one.
    //
    // `dl_iterate_phdr` reports it with an empty name, and the only handle for it
    // is `dlopen(nullptr)` - which is not a handle to the executable at all but
    // to the GLOBAL SYMBOL SCOPE. A `dlsym` through it finds the marker in any
    // globally loaded object, the shim included, so the executable gets reported
    // as exporting a symbol it does not define. Measured in a container before
    // this was written: a healthy ONE-shim process scored TWO, which would poison
    // the shim at load and refuse every call in it. A check whose failure mode is
    // breaking every healthy process is worse than no check.
    //
    // Nothing real is lost. The marker is exported by the shim and by nothing
    // else, and the shim is a shared library by construction - it is the per-RID
    // native asset - so a second copy is always another shared object and is
    // still seen. An executable that statically linked Fletcher exports no marker
    // either way: that is D-BIND-17's explicitly undetectable case, ruled out by
    // fact rather than by check.
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') return 0;
    names->emplace_back(info->dlpi_name);
    return 0;
}

std::vector<std::string> ScanModules() {
    std::vector<std::string> names;
    dl_iterate_phdr(&CollectModuleName, &names);

    std::vector<std::string> found;
    std::vector<void*> seen;
    for (const std::string& name : names) {
        // RTLD_NOLOAD: ask about a module already loaded, never load one.
        void* handle = dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        if (handle == nullptr) continue;
        void* symbol = dlsym(handle, kMarkerSymbol);
        dlclose(handle);
        if (symbol == nullptr) continue;

        // Deduplicated BY ADDRESS, not by module name. `dlsym` on a library's
        // handle searches that library AND its dependency chain, so a module
        // that merely links the shim resolves to the SHIM's marker and would
        // otherwise be counted as a second copy. Two entries at one address are
        // one definition seen twice; two addresses are two copies.
        if (std::find(seen.begin(), seen.end(), symbol) != seen.end()) continue;
        seen.push_back(symbol);
        found.push_back(name);
    }
    return found;
}

#endif

std::string& RefusalSlot() {
    static std::string refusal;
    return refusal;
}

}  // namespace

const char* MarkerText() { return "fletcher-c-abi binding shim, ABI 0.1"; }

std::vector<std::string> ModulesExportingTheMarker() { return ScanModules(); }

const std::string& SingleCopyRefusal() { return RefusalSlot(); }

void CheckSingleCopy() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::vector<std::string> found = ScanModules();
        if (found.size() < 2) return;

        std::string message =
            "fletcher-c-abi: this process has loaded " + std::to_string(found.size()) +
            " copies of the Fletcher binding shim, and exactly one is supported "
            "(D-BIND-17). Fletcher's re-entrancy machinery is a thread_local in a "
            "header-only target, so each copy keeps its own delivery-frame stack and "
            "the re-entrancy doors stop seeing each other's frames - a call that must "
            "be refused gets served instead, silently. The copies are:";
        for (const std::string& path : found) {
            message += "\n  " + path;
        }
        RefusalSlot() = std::move(message);
    });
}

}  // namespace fletcher::abi
