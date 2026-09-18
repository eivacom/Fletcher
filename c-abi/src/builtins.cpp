// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The shim's built-in providers.
//
// Locked: the shim STATICALLY LINKS and REGISTERS `inprocess`, `fastdds` and
// `xrce`, so a C# application selects a real transport by configuration string
// with no loader and no extra files beside the shim (D-BIND-16; registry
// semantics in `fletcher/pubsub/provider_registry.hpp`). Path selectors stay
// `kNotSupported` until PDA-ABI fills the resolver seat — the shim never calls
// `SetPathResolver`, and managed code can neither register a provider nor
// install a resolver (D-BIND-24).
//
// BIND-2c wired `fl_provider_create` to `BuiltinRegistry()` (D-BIND-31), so the
// registry is now reached from the export table. The file was here from the
// kickoff on purpose, before anything called it, because it is what makes the
// skeleton's LINK the real one — the whole
// Fast DDS and Micro XRCE-DDS chain is pulled into the shared object exactly as
// the shipped shim will pull it. That is what BIND-0's per-RID size measurement
// measures, and it puts the static-link-into-a-shared-library question (PIC on
// Linux; one MSVC runtime on Windows) in front of CI on day one rather than at
// BIND-4 (risk P-6).
#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"
#include "fletcher/pubsub/in_process_provider.hpp"
#include "fletcher/pubsub/provider_registry.hpp"
#include "fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp"

namespace fletcher::abi::internal {

// One registry per loaded shim, built on first use and never destroyed: a
// provider factory may outlive static destruction in a host that unloads late,
// and the seam's registry is `const`-safe for concurrent `Create` once built.
ProviderRegistry& BuiltinRegistry() {
    static ProviderRegistry* const registry = [] {
        auto* r = new ProviderRegistry();
        RegisterInProcessProvider(*r);
        RegisterFastDDSProvider(*r);
        RegisterXrceProvider(*r);
        return r;
    }();
    return *registry;
}

}  // namespace fletcher::abi::internal

// BIND-0's load-time touch is GONE, and its removal is the point.
//
// Until BIND-2c there was no exported function referencing `BuiltinRegistry()`,
// so MSVC's /OPT:REF in a Release link dropped it and the three provider
// archives were never pulled into the shim — which would have made BIND-0
// measure a shim that did not contain what it claimed to. A dynamic initializer
// held the reference until an honest one existed.
//
// `fl_provider_create` is that honest reference. The comment that stood here
// said to delete this when it landed; it landed, and this is the deletion.
