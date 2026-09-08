// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub_arrow/schema_import.hpp"

#include <arrow/c/bridge.h>
#include <arrow/type.h>

#include <fletcher/core/status.hpp>

namespace fletcher {

std::shared_ptr<arrow::Schema> ImportArrowSchema(const SharedSchema& schema) {
    // Null, or a schema whose release has already run: nothing to import, and
    // saying so is not an error — a schema-less transport answers with null.
    if (!schema || schema->release == nullptr) {
        return nullptr;
    }
    // The copy that makes this safe: ImportSchema consumes what it is given.
    OwnedSchema copy = OwnedSchema::DeepCopy(schema.get());
    auto result = arrow::ImportSchema(copy.get());
    if (!result.ok()) {
        throw PubSubError(PubSubStatus::kInternal,
                          "ImportArrowSchema: " + result.status().ToString());
    }
    return *result;
}

}  // namespace fletcher
