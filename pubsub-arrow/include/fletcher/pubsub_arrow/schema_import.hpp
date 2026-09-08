// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The ONE safe conversion from the seam's shared schema handle to an Arrow C++
// schema.
//
// It was a `static` function in an anonymous namespace inside
// `subscriber_arrow.cpp`, reachable by nobody. Now that the Arrow tier hands
// back a `SchemaArrival` — a `SharedSchema`, not an `arrow::Schema` — every
// Arrow-facing caller needs this conversion, and writing it naively is
// memory-unsafe. So it is public, once, rather than reinvented at each call
// site.
#ifndef FLETCHER_INCLUDE_PUBSUB_ARROW_SCHEMA_IMPORT_HPP_
#define FLETCHER_INCLUDE_PUBSUB_ARROW_SCHEMA_IMPORT_HPP_

#include <arrow/type_fwd.h>

#include <fletcher/pubsub/owned_schema.hpp>
#include <memory>

namespace fletcher {

/// Import `schema` as an Arrow C++ schema.
///
/// **It deep-copies first, and that is the whole point.** `arrow::ImportSchema`
/// CONSUMES the `ArrowSchema` it is given — it takes over the C Data Interface
/// `release`. A `SharedSchema` is shared, so a caller that hands it straight to
/// `arrow::ImportSchema` destroys the schema under every other holder, including
/// the provider still delivering with it. The copy is the price of sharing.
///
/// Returns **nullptr** for a null handle and for a released (`release == nullptr`)
/// schema — the two ways there is nothing to import. That is what the private
/// predecessor did, kept because a null `SharedSchema` is a legitimate value:
/// it is how a schema-less transport answers (§7 clause 1).
///
/// Throws `PubSubError(kInternal)` if the copy or the import fails; the schema
/// argument is left untouched either way.
[[nodiscard]] std::shared_ptr<arrow::Schema> ImportArrowSchema(const SharedSchema& schema);

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_ARROW_SCHEMA_IMPORT_HPP_
