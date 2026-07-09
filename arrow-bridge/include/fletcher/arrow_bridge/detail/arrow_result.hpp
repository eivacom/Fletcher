// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_ARROW_BRIDGE_DETAIL_ARROW_RESULT_HPP_
#define FLETCHER_INCLUDE_ARROW_BRIDGE_DETAIL_ARROW_RESULT_HPP_

#include <arrow/result.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace fletcher::detail {

// Checked access to an Arrow Result<T>. On failure, throws std::invalid_argument
// naming the operation (H-INV-2: recoverable errors throw, they never abort).
// This replaces .ValueOrDie(), which calls abort() on a failed Result. The
// success path performs the unchecked extraction only after the ok() check.
template <typename T>
T ValueOrThrow(arrow::Result<T>&& result, const char* operation) {
    if (!result.ok()) {
        throw std::invalid_argument(std::string(operation) + ": " + result.status().ToString());
    }
    return std::move(result).ValueUnsafe();
}

}  // namespace fletcher::detail

#endif  // FLETCHER_INCLUDE_ARROW_BRIDGE_DETAIL_ARROW_RESULT_HPP_
