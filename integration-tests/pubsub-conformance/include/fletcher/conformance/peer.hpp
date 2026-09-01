// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The peer child's request/reply loop, written once. Each peer binary supplies
// only a provider factory, so the protocol and the loop cannot drift between
// providers.

#ifndef FLETCHER_CONFORMANCE_PEER_HPP_
#define FLETCHER_CONFORMANCE_PEER_HPP_

#include <fletcher/pubsub/provider.hpp>
#include <functional>
#include <memory>

namespace fletcher {
namespace conformance {

/// Builds the provider this peer publishes through, from its own argv.
using PeerProviderFactory = std::function<std::shared_ptr<PubSubProvider>(int argc, char** argv)>;

/// Constructs the provider, prints `READY` (the fastdds_peer convention), then
/// serves one line per request on stdin until EOF or `quit`. Every request
/// carries a tag as its first token and every reply echoes it, so a stray line
/// on this process's stdout cannot be mistaken for a reply and desync the
/// stream permanently:
///
///     <tag> create <joined/topic> <A|B|none>   -> <tag> ok | <tag> err <type>: <what>
///     <tag> publish <joined/topic> <seq>       -> <tag> ok | <tag> err <type>: <what>
///     <tag> quit                               -> <tag> ok
///
/// There is deliberately no `subscribe` verb: the peer publishes and cannot
/// observe. Process exit code 0 on a clean EOF, 1 if the provider could not be
/// constructed (the parent sees no READY and fails the clause naming it).
int RunPeerMain(int argc, char** argv, const PeerProviderFactory& make_provider);

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_PEER_HPP_
