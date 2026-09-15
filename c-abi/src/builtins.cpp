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
// At BIND-0 nothing above calls this yet: `fl_registry_create` arrives with the
// rest of the ABI in BIND-1/BIND-4. The file is here from the kickoff on
// purpose, because it is what makes the skeleton's LINK the real one — the whole
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

namespace {

// Load-time touch, and the ONLY reason this file has a static initializer.
//
// Without a reference from somewhere the linker keeps (MSVC's /OPT:REF in a
// Release link is the one that bites), `BuiltinRegistry` is unreferenced at
// BIND-0 and the three provider archives are never pulled in — the shim would
// build and measure small, and the link question BIND-0 exists to answer would
// go unasked. A dynamic initializer is reachable from .init_array / .CRT$XCU and
// survives both linkers' dead-stripping.
//
// DELETE THIS when `fl_registry_create` lands (BIND-4) and references
// `BuiltinRegistry()` from the export table, which is the honest reference.
// Building the registry costs three `std::map` inserts and constructs no
// provider: nothing here opens a socket, reads a document or starts a thread.
[[maybe_unused]] const bool kBuiltinsLinked = (fletcher::abi::internal::BuiltinRegistry(), true);

}  // namespace
