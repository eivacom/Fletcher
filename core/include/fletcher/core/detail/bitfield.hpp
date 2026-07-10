// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_DETAIL_BITFIELD_HPP_
#define FLETCHER_INCLUDE_CORE_DETAIL_BITFIELD_HPP_

// Shared null-bitfield size helper for the positional wire format.
// Single source of truth for the ceil(n / 8) computation used by both the
// arrow-bridge Codec and the header-only PositionalWriter/PositionalReader.

#include <cstddef>
#include <cstdint>

namespace fletcher::detail {

// Number of bytes needed for a null bitfield covering n items.
// Takes int64_t so wire-supplied uint32_t counts do not narrow through int
// (which would make the (n + 7) / 8 arithmetic produce garbage).
inline std::size_t BitfieldBytes(std::int64_t n) { return static_cast<std::size_t>((n + 7) / 8); }

}  // namespace fletcher::detail

#endif  // FLETCHER_INCLUDE_CORE_DETAIL_BITFIELD_HPP_
