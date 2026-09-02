// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Uniform provider selection: one call turns a *selector* plus a *configuration*
// into a provider (spec §4, §4.1, §4.2).
//
// The point is that the protocol stops being a compile-time choice. An
// application reads one string out of its configuration, hands over that
// protocol's own settings as a document Fletcher never reads, and gets a working
// provider — without knowing which protocol it got, or where from.
//
// **What is NOT here, on purpose:** a loader. `dlopen` never enters
// fletcher-pubsub (locked decision 14). The path branch below exists and is
// routed, but the thing it routes to is a *seat* — a std::function this library
// calls — not a specification of what a loader looks like. PDA-ABI fills the
// seat from its own component and adds no method here.
#ifndef FLETCHER_INCLUDE_PUBSUB_PROVIDER_REGISTRY_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PROVIDER_REGISTRY_HPP_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>

#include "fletcher/pubsub/provider.hpp"

namespace fletcher {

/// WHAT to select: one string, exactly as an operator wrote it.
///
/// ── The classification rule (normative) ─────────────────────────────────────
/// A **name** is a non-empty string of `[A-Za-z0-9_-]` and nothing else. Every
/// other non-empty string is a **path**. The empty string is refused, and so is
/// a string containing an embedded NUL. There is no trimming, no case folding
/// and no normalisation, and **the rule does not consult the registry** — so a
/// given string means the same thing in every build, whether or not the built-in
/// it might name is linked. Lookup of a name is exact and case-sensitive, and
/// `ProviderRegistry::Register` validates against this same predicate, so a
/// registered name is always selectable and the two vocabularies cannot drift.
///
/// The rule is total and disjoint, which is what lets one configuration setting
/// carry both kinds. Every spelling of a shared library on every target — `x.so`,
/// `x.dll`, `x.dylib`, `./x`, `C:\d\x.dll`, `\\host\share\x.dll` — carries a
/// dot, a slash, a backslash or a colon, so it is a path; a plain word like
/// `fastdds` is a name. A relative file with no dot and no separator
/// (`myDriver`) is the one misclassification, and it is loud: it is refused as
/// an unknown name, and the operator's fix is `./myDriver`.
///
/// ── What a caller can and cannot do with this ───────────────────────────────
/// The seam offers **no way to ask** whether a selection is a built-in or a
/// loaded driver: there is no kind accessor, no second creation call, and
/// `Create` returns the same type either way. That is not the same as making
/// such a question unrepresentable — the rule above is public and is a pure
/// function of a string the caller already holds, so a caller *can* re-derive
/// it, and RTTI on the returned provider is always available. Neither matters,
/// because what locked decision 3 actually guarantees is the property that has
/// operational consequences: **moving a protocol from built-in to loaded is a
/// configuration edit, never a caller edit.** Nothing above this seam changes
/// when `provider = fastdds` becomes `provider = /opt/libfastdds_driver.so`.
///
/// ── C form (spec §3.5's idiom) ──────────────────────────────────────────────
/// A pointer and a length, borrowed for the duration of the call; the length is
/// authoritative and the seam copies what it keeps. An **embedded NUL is refused**
/// (kInvalidArgument): a length-carrying binding could otherwise hand the seam
/// `"fastdds\0/../evil.so"`, which classifies as a path and would reach a
/// future loader's `dlopen(path.c_str())` truncated — opening a different
/// library with no signal. Refused once here rather than by every resolver
/// remembering to check. As everywhere in this vocabulary the C form is
/// conceptual: no layout compatibility is implied and each boundary constructs
/// its own.
class ProviderSelector {
   public:
    /// Classify a configuration string. The ONLY way to make a selector, so the
    /// rule above has exactly one implementation.
    ///
    /// Throws `PubSubError(kInvalidArgument)` for the empty string and for a
    /// string containing an embedded NUL. It never fails for any other reason:
    /// a string that names nothing this build has is a *selection* that fails
    /// later, at `Create`, where the registry can say what IS available.
    [[nodiscard]] static ProviderSelector Parse(std::string text);

   private:
    friend class ProviderRegistry;

    ProviderSelector(std::string text, bool is_name) : text_(std::move(text)), is_name_(is_name) {}

    std::string text_;
    bool is_name_;
};

/// §4.1 — a small typed core plus bytes Fletcher transports and does not read.
///
/// The typed core is **exactly these two fields** (owner ruling 2026-09-02:
/// "Fletcher keeps exactly payload size and domain; everything protocol-specific
/// lives in the document only that protocol reads"). It is append-only: a later
/// field never changes `Create`'s signature. Widening it because one protocol
/// wants a setting typed is a stop-and-ask, not an edit.
struct ProviderConfig {
    /// The largest encoded sample the transport should accept, in bytes.
    ///
    /// **0 means UNSET**, and the provider's own default applies. Fletcher does
    /// not know any provider's valid bounds or its default, so it cannot demand
    /// a value it could check; "unset" is safe to spell as 0 because
    /// `IsPayloadBound(0)` is false everywhere, so no provider can mistake it
    /// for a real bound.
    uint32_t max_payload_bytes = 0;

    /// The transport's domain/endpoint identity, in whatever the transport calls
    /// a domain.
    ///
    /// Forward note for PDA-DEC-7: this is `uint32_t` while `XrceConfig`'s field
    /// is `uint16_t`. A migration must **refuse** an out-of-range value, never
    /// narrow it — a truncated domain id puts the client on the wrong domain with
    /// no error, which is a wrong answer rather than a failure.
    uint32_t domain_id = 0;

    /// Everything else, in **the provider's own format** — Fast DDS's native XML
    /// QoS profile, `key=value` for XRCE, JSON5 for a future Zenoh.
    ///
    /// Fletcher **never reads these bytes**: the only operations on them are copy
    /// and forward (§4.2, locked decision 8). There is no parser here, no format
    /// here and no configuration dependency here, and adding one "for
    /// convenience" is a stop-and-ask. Whether a provider reads the document as
    /// content or as the name of a file it opens is *that provider's* decision;
    /// Fletcher does not resolve it, expand it or validate it.
    ///
    /// C form: a pointer and a length borrowed for the duration of the call, the
    /// **length authoritative** — the bytes may contain NUL. A provider that
    /// keeps the document copies it.
    std::string document;
};

/// The registry: built-ins registered by an explicit call, and one seat a future
/// loader fills.
///
/// ── No global state (§4 clause 3) ───────────────────────────────────────────
/// This is an ordinary object the caller owns. There is no process-wide table
/// and none may be added: registration is an explicit call by whoever links the
/// provider, which is also the static half of the linkage ruling ("MCU targets
/// register the same driver statically at link time") without static-initialiser
/// magic — this tree has already measured a linker dropping such objects out of
/// a static archive.
///
/// `Create` never caches: every call constructs a fresh instance and the
/// registry keeps no reference to what it made, so several instances of one
/// provider with different configurations are ordinary, and a registry may be
/// destroyed while the providers it made are still running.
///
/// ── Lifetime of what a factory or a resolver captured (normative) ───────────
/// A **resolver or a factory** must keep everything the provider it returns
/// depends on — including a loaded module and anything that module's code or
/// data lives in — alive for **at least as long as that provider**,
/// independently of this registry's lifetime and of its own. Destroying a
/// registry while its providers run is sanctioned above, and both seats are
/// reachable from a loaded module: PDA-ABI installs a resolver, and it also
/// reaches a loaded driver by *name* through `Register("zenoh",
/// factory_that_dlopens)` — the static half of the linkage ruling. A module
/// handle parked in a cache with the registry's lifetime would unload code out
/// from under a live provider, which is a use-after-unload and not reliably
/// loud.
///
/// **This registry enforces the rule instead of asking for it.** A factory and
/// a resolver are each held by shared handle, and every provider `Create`
/// returns owns a copy of the handle that made it. The callable, and everything
/// its closure captured, therefore outlives every provider handed out from it,
/// whatever its author did — destroying this registry cannot unload a module a
/// live provider is still running in. Two things stay the author's obligation
/// because no seam can reach them: a handle the provider mints for *itself*
/// (`shared_from_this`), and a raw pointer into module memory it hands to
/// something that outlives it. The idiom that covers those too is to make the
/// provider's own ownership release the module — a `shared_ptr` whose deleter
/// unloads it — never a cache with the registry's lifetime.
///
/// ── Threading: populate, then share ─────────────────────────────────────────
/// `Create` is `const` and safe to call concurrently. `Register` and
/// `SetPathResolver` are **not**: the intended shape is one construction phase
/// followed by read-only use. There is no lock, so an MCU build pays nothing for
/// a synchronisation it does not need.
///
/// ── Failure ─────────────────────────────────────────────────────────────────
/// All three methods are seam entry points, so §5.1 applies: they throw
/// `PubSubError` carrying a stable `PubSubStatus`, and nothing else escapes —
/// including an exception out of a caller-supplied factory or resolver.
class ProviderRegistry {
   public:
    /// Makes one provider instance from a configuration. Supplied by whoever
    /// links the provider; the registry only calls it.
    using Factory = std::function<std::shared_ptr<PubSubProvider>(const ProviderConfig&)>;

    /// Makes one provider instance from a driver path and a configuration.
    using PathResolver = std::function<std::shared_ptr<PubSubProvider>(
        const std::string& path, const ProviderConfig& config)>;

    ProviderRegistry() = default;

    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    /// Make a BUILT-IN selectable under `name`.
    ///
    /// `name` must satisfy `ProviderSelector`'s name predicate and `factory` must
    /// be non-empty; both are refused with `kInvalidArgument` here, at
    /// registration, rather than at use. **Registering a name twice is refused**
    /// — there is no overwrite and no last-wins, because silently swapping which
    /// transport a name means is not a state this object may enter.
    ///
    /// The class comment's lifetime rule binds this seat as much as the
    /// resolver's: a factory that closes over a loaded module is a loaded
    /// driver reached by name, and the providers it makes hold it alive.
    void Register(std::string name, Factory factory);

    /// Install the resolver that answers **path** selectors. This is the seat
    /// PDA-ABI fills; in this build it is empty, and a path selector is refused
    /// with `kNotSupported`.
    ///
    /// The class comment's lifetime rule binds this seat: whatever the resolver
    /// captured, including a loaded module, is kept alive by every provider it
    /// makes, so destroying this registry cannot unload code a live provider is
    /// running in.
    ///
    /// Installing a **second** resolver is refused with `kInvalidArgument`, for
    /// the reason a duplicate `Register` is: swapping which loader every path
    /// means, silently, is worse than the call that would have done it failing.
    /// An empty resolver is refused too.
    void SetPathResolver(PathResolver resolver);

    /// Turn a selector plus a configuration into a provider.
    ///
    /// **This signature is FROZEN** (§4 clause 2, locked decision 3). So is the
    /// rest of this class's public surface: `Create`, `Register`,
    /// `SetPathResolver`, and nothing else. PDA-ABI adds no method here — it
    /// *calls* `SetPathResolver`. If admitting a loaded driver ever required a
    /// second overload, a flag or a disambiguator, this seam was underspecified
    /// and that is a stop-and-ask against the spec, not an edit. The
    /// `static_assert` below is the machine check.
    ///
    /// Refusals, all typed and deliberately distinct:
    ///  - an unregistered **name** → `kInvalidArgument`, listing what IS
    ///    registered;
    ///  - a **path** with no resolver installed → `kNotSupported`, saying that
    ///    this build cannot load drivers, and why the string was read as a path.
    ///    The two are different operator actions — "no such protocol here"
    ///    versus "this build cannot load drivers at all" — so they are different
    ///    statuses (owner ruling 2026-09-02);
    ///  - a factory or resolver that returns null → `kInternal`, named;
    ///  - anything a factory or resolver throws → translated by §5.1's rule.
    ///
    /// The returned handle also owns the factory or resolver that made it (the
    /// lifetime rule above); it is otherwise an ordinary
    /// `shared_ptr<PubSubProvider>` and nothing above the seam can tell.
    [[nodiscard]] std::shared_ptr<PubSubProvider> Create(const ProviderSelector& selector,
                                                         const ProviderConfig& config) const;

   private:
    // Ordered, so the "available providers" in a refusal is stable and
    // greppable rather than hash-order. Held by shared handle, not by value, so
    // a provider can own the callable that made it — the lifetime rule above is
    // mechanical, not advisory.
    std::map<std::string, std::shared_ptr<Factory>> factories_;
    std::shared_ptr<PathResolver> path_resolver_;
};

// The frozen signature, pinned as a whole rather than by return type: a
// return-type assertion cannot see a defaulted third parameter or a dropped
// `const`, which is exactly the widening this item exists to prevent.
static_assert(
    std::is_same_v<decltype(&ProviderRegistry::Create),
                   std::shared_ptr<PubSubProvider> (ProviderRegistry::*)(
                       const ProviderSelector&, const ProviderConfig&) const>,
    "ProviderRegistry::Create is frozen (spec §4 clause 2): PDA-ABI installs a resolver, it does "
    "not widen this call");

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PROVIDER_REGISTRY_HPP_
