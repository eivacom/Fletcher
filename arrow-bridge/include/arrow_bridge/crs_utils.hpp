// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CRS_UTILS_HPP_
#define FLETCHER_INCLUDE_CRS_UTILS_HPP_

#include <string>
#include <string_view>

namespace fletcher {

// Return PROJJSON for a well-known EPSG code (e.g. 4326, 3857).
// Returns an empty string for unrecognised codes.
std::string EpsgToProjJson(int code);

// Resolve a CRS string to PROJJSON.
//   - Starts with '{'  → return as-is (already PROJJSON).
//   - Starts with "EPSG:" → parse the code and call EpsgToProjJson.
//   - Otherwise → return empty string.
std::string ResolveCrs(std::string_view crs);

// Build the ARROW:extension:metadata JSON string from a resolved CRS.
// If resolved_crs is empty, returns "{}".
// Otherwise returns {"crs":<resolved_crs>}.
std::string BuildExtensionMetadata(std::string_view resolved_crs);

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CRS_UTILS_HPP_
