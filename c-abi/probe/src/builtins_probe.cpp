// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The PROBE SHIM's built-in providers: the shipped three, plus `probe`.
//
// Compiled into `fletcher-c-abi-probe` INSTEAD of `src/builtins.cpp`, and into
// nothing else. The shipped shim still registers exactly the three (D-BIND-24:
// nothing outside can register a provider in it); this is a separate test
// artifact, built only under `with_probe_shim`, so it does not reopen that door
// (D-BIND-58, D-BIND-62).
//
// `probe` is `SeamProbeProvider` in `kTracing` mode: it encodes into a window that
// records where every byte came from and delivers that window itself, so a
// binding's real publish can be scored by the copy oracle's own Judge().
#include <fletcher/copy_probe/seam_probe_provider.hpp>
#include <memory>

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"
#include "fletcher/pubsub/in_process_provider.hpp"
#include "fletcher/pubsub/provider_registry.hpp"
#include "fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp"

namespace fletcher::abi::internal {

// As in `src/builtins.cpp`: one registry per loaded shim, never destroyed.
ProviderRegistry& BuiltinRegistry() {
    static ProviderRegistry* const registry = [] {
        auto* r = new ProviderRegistry();
        RegisterInProcessProvider(*r);
        RegisterFastDDSProvider(*r);
        RegisterXrceProvider(*r);
        r->Register("probe", [](const ProviderConfig&) {
            return std::make_shared<copy_probe::SeamProbeProvider>(copy_probe::ProbeMode::kTracing);
        });
        return r;
    }();
    return *registry;
}

}  // namespace fletcher::abi::internal
