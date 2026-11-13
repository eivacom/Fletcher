// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The EDGE (and probe) backend of `fl_subscriber_subscribe_batches` (D9S-4; d9 L12).
//
// UNREACHABLE: the entry point asks `fl_binding_variant()` first and refuses a non-server build
// with FL_NOT_SUPPORTED before it reads an argument. This definition exists so the one door links
// in every artefact. It names no Arrow and no domain-7 type (L8, BC-11).
#include <fletcher/core/status.hpp>
#include <string>
#include <utility>
#include <vector>

#include "handles.hpp"

#ifndef FLETCHER_C_ABI_VARIANT
#error "FLETCHER_C_ABI_VARIANT is not defined: see c-abi/CMakeLists.txt"
#endif
// The build is asserted, never decided here: a backend compiled into the wrong artefact does not
// compile.
static_assert(FLETCHER_C_ABI_VARIANT == FL_VARIANT_EDGE,
              "batches_edge.cpp belongs to the edge and probe builds only");

namespace fletcher::abi::internal {

BatchedSubscription SubscribeBatches(fl_subscriber&, std::vector<std::string>, int64_t, int64_t,
                                     const fletcher::TopicOptions&, fl_batch_fn, void*) {
    // A mis-build report, never an answer to "can this build batch" (that is the door's).
    throw PubSubError(PubSubStatus::kInternal,
                      "fl_subscriber_subscribe_batches: built without the batch assembler");
}

}  // namespace fletcher::abi::internal
