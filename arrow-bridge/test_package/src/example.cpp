// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/arrow_bridge/crs_utils.hpp>

#include <arrow/api.h>

#include <cassert>
#include <memory>

int main() {
    using fletcher::Codec;
    using fletcher::ArrowRow;

    std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("x", arrow::int32()),
        arrow::field("name", arrow::utf8()),
    });

    Codec codec(schema);

    ArrowRow row{
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
    };

    fletcher::EncodedRow bytes = codec.EncodeRow(row);
    ArrowRow decoded = codec.DecodeRow(bytes);
    assert(decoded.size() == row.size());

    std::string projjson = fletcher::EpsgToProjJson(4326);
    assert(!projjson.empty());

    return 0;
}
