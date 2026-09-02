// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The rule is in the header; this file is its single implementation.
//
// NB what this TU does NOT include: any transport SDK, any configuration
// parser, and <dlfcn.h>/<windows.h>. `document` is copied and forwarded and
// nothing else (§4.2, locked decision 8), and the path branch calls a seat, not
// a loader (locked decision 14).
#include "fletcher/pubsub/provider_registry.hpp"

#include <cstddef>
#include <fletcher/core/status.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace fletcher {
namespace {

// The ONE name predicate, per character. `ProviderSelector::Parse` and
// `ProviderRegistry::Register` both go through it, which is what stops the
// selectable vocabulary and the registrable one drifting apart.
bool IsNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

// The offset of the first character that makes `text` a path, or npos if the
// whole string is name-shaped. A non-empty non-name always has one.
size_t FirstNonNameChar(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!IsNameChar(text[i])) return i;
    }
    return std::string::npos;
}

// A selector in a diagnostic, with control characters escaped. The realistic
// misclassification is a trailing CRLF out of a configuration file, and a raw
// CR in an error message hides exactly the character the operator must delete.
//
// A backslash and a quote are escaped too, and not for tidiness: `\x` is this
// function's own escape spelling, so an ordinary Windows driver path such as
// `C:\x64\driver.dll` would otherwise render its `\x64` as the escape for byte
// 0x64 — a real path turned into a lie inside the one message an operator reads
// when a driver will not load.
std::string Quoted(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == '"') {
            out << '\\' << c;
        } else if (byte >= 0x20 && byte < 0x7f) {
            out << c;
        } else {
            static const char kHex[] = "0123456789abcdef";
            out << "\\x" << kHex[byte >> 4] << kHex[byte & 0x0f];
        }
    }
    out << '"';
    return out.str();
}

[[noreturn]] void Refuse(PubSubStatus status, const std::string& message) {
    throw PubSubError(status, message);
}

// The lifetime rule, made mechanical (header: "Lifetime of what a factory or a
// resolver captured"). The handle this returns owns BOTH the provider and the
// callable that made it, so the callable — and whatever its closure captured,
// including a loaded module — cannot be destroyed while a provider the seam
// handed out is still alive, whatever the callable's author did.
//
// The member order is the load-bearing part: members are destroyed in reverse
// declaration order, so `provider` (declared last) is released FIRST and the
// seat only afterwards. The provider's own destructor therefore still runs with
// its module loaded.
template <typename Seat>
std::shared_ptr<PubSubProvider> KeepSeatAlive(std::shared_ptr<PubSubProvider> provider,
                                              std::shared_ptr<Seat> seat) {
    struct Anchor {
        std::shared_ptr<Seat> seat;
        std::shared_ptr<PubSubProvider> provider;
    };
    PubSubProvider* const raw = provider.get();
    auto anchor = std::make_shared<Anchor>(Anchor{std::move(seat), std::move(provider)});
    // Aliasing: the caller gets a handle that points at the provider and owns
    // the anchor. `Create`'s return type is unchanged, so nothing above the seam
    // can tell, and the frozen signature is untouched.
    return std::shared_ptr<PubSubProvider>(std::move(anchor), raw);
}

}  // namespace

ProviderSelector ProviderSelector::Parse(std::string text) {
    if (text.empty()) {
        Refuse(PubSubStatus::kInvalidArgument,
               "provider selector is empty: it must be a built-in provider name "
               "([A-Za-z0-9_-]) or a driver path");
    }
    const size_t nul = text.find('\0');
    if (nul != std::string::npos) {
        Refuse(PubSubStatus::kInvalidArgument,
               "provider selector " + Quoted(text) + " contains a NUL at offset " +
                   std::to_string(nul) +
                   "; a selector is bytes-plus-length and a NUL would silently truncate it on the "
                   "way to a loader");
    }
    const bool is_name = FirstNonNameChar(text) == std::string::npos;
    return ProviderSelector(std::move(text), is_name);
}

void ProviderRegistry::Register(std::string name, Factory factory) {
    if (name.empty()) {
        Refuse(PubSubStatus::kInvalidArgument, "cannot register a provider under an empty name");
    }
    const size_t offending = FirstNonNameChar(name);
    if (offending != std::string::npos) {
        Refuse(PubSubStatus::kInvalidArgument,
               "cannot register a provider under the name " + Quoted(name) +
                   ": character at offset " + std::to_string(offending) +
                   " is outside [A-Za-z0-9_-], so a selector spelling it would be classified as a "
                   "driver path and this registration could never be selected");
    }
    if (!factory) {
        Refuse(PubSubStatus::kInvalidArgument,
               "cannot register provider " + Quoted(name) + " with an empty factory");
    }
    // No overwrite and no last-wins: silently swapping which transport a name
    // means is not a state this object may enter.
    if (factories_.find(name) != factories_.end()) {
        Refuse(PubSubStatus::kInvalidArgument,
               "provider " + Quoted(name) +
                   " is already registered; a name means one transport for the life of a registry");
    }
    // Held by shared handle so a provider can own the factory that made it —
    // see KeepSeatAlive. The registry still keeps no reference to anything it
    // MADE; this is a reference to what makes.
    factories_.emplace(std::move(name), std::make_shared<Factory>(std::move(factory)));
}

void ProviderRegistry::SetPathResolver(PathResolver resolver) {
    if (!resolver) {
        Refuse(PubSubStatus::kInvalidArgument, "cannot install an empty path resolver");
    }
    // The same reasoning as a duplicate Register, applied to the loader every
    // path selector goes through.
    if (path_resolver_) {
        Refuse(PubSubStatus::kInvalidArgument,
               "a path resolver is already installed; replacing it would silently change what "
               "every driver path in this configuration resolves through");
    }
    path_resolver_ = std::make_shared<PathResolver>(std::move(resolver));
}

std::shared_ptr<PubSubProvider> ProviderRegistry::Create(const ProviderSelector& selector,
                                                         const ProviderConfig& config) const {
    if (selector.is_name_) {
        const auto entry = factories_.find(selector.text_);
        if (entry == factories_.end()) {
            std::ostringstream available;
            if (factories_.empty()) {
                available << "none — no provider has been registered with this registry";
            } else {
                bool first = true;
                for (const auto& [name, unused] : factories_) {
                    if (!first) available << ", ";
                    available << name;
                    first = false;
                }
            }
            Refuse(PubSubStatus::kInvalidArgument, "no built-in provider named " +
                                                       Quoted(selector.text_) +
                                                       "; available: " + available.str());
        }
        // §5.1: whatever a caller-supplied factory throws leaves this seam as a
        // PubSubError, including std::bad_alloc.
        std::shared_ptr<Factory> factory = entry->second;
        std::shared_ptr<PubSubProvider> provider =
            TranslateSeamFailure([&] { return (*factory)(config); });
        if (!provider) {
            Refuse(PubSubStatus::kInternal, "the factory registered for provider " +
                                                Quoted(selector.text_) + " returned no provider");
        }
        return KeepSeatAlive(std::move(provider), std::move(factory));
    }

    // A path. The branch exists and is routed HERE, in this item — PDA-ABI fills
    // the seat and changes nothing above it.
    if (!path_resolver_) {
        const size_t offending = FirstNonNameChar(selector.text_);
        std::ostringstream why;
        why << "provider selector " << Quoted(selector.text_)
            << " was read as a driver path, because the character at offset " << offending
            << " (0x";
        const auto byte = static_cast<unsigned char>(selector.text_[offending]);
        why << std::hex << (byte < 16 ? "0" : "") << static_cast<unsigned>(byte) << std::dec
            << ") is outside a provider name's [A-Za-z0-9_-]. This build cannot load drivers, so "
               "no path can be resolved. If a built-in provider name was meant, remove that "
               "character";
        Refuse(PubSubStatus::kNotSupported, why.str());
    }
    std::shared_ptr<PathResolver> resolver = path_resolver_;
    std::shared_ptr<PubSubProvider> provider =
        TranslateSeamFailure([&] { return (*resolver)(selector.text_, config); });
    if (!provider) {
        Refuse(PubSubStatus::kInternal,
               "the path resolver returned no provider for " + Quoted(selector.text_));
    }
    return KeepSeatAlive(std::move(provider), std::move(resolver));
}

}  // namespace fletcher
