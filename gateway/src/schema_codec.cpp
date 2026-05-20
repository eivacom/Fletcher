// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "schema_codec.hpp"

#include <stdexcept>
#include <string>

namespace fletcher::gateway {

using json = nlohmann::json;

int NanoarrowTypeToWireType(enum ArrowType type) {
    switch (type) {
        case NANOARROW_TYPE_BOOL:                       return 0x01;
        case NANOARROW_TYPE_INT8:                       return 0x02;
        case NANOARROW_TYPE_INT16:                      return 0x03;
        case NANOARROW_TYPE_INT32:                      return 0x04;
        case NANOARROW_TYPE_INT64:                      return 0x05;
        case NANOARROW_TYPE_UINT8:                      return 0x06;
        case NANOARROW_TYPE_UINT16:                     return 0x07;
        case NANOARROW_TYPE_UINT32:                     return 0x08;
        case NANOARROW_TYPE_UINT64:                     return 0x09;
        case NANOARROW_TYPE_FLOAT:                      return 0x0A;
        case NANOARROW_TYPE_DOUBLE:                     return 0x0B;
        case NANOARROW_TYPE_STRING:                     return 0x0C;
        case NANOARROW_TYPE_BINARY:                     return 0x0D;
        case NANOARROW_TYPE_DATE32:                     return 0x0E;
        case NANOARROW_TYPE_DATE64:                     return 0x0F;
        case NANOARROW_TYPE_TIMESTAMP:                  return 0x10;
        case NANOARROW_TYPE_TIME32:                     return 0x11;
        case NANOARROW_TYPE_TIME64:                     return 0x12;
        case NANOARROW_TYPE_DURATION:                   return 0x13;
        case NANOARROW_TYPE_FIXED_SIZE_BINARY:          return 0x14;
        case NANOARROW_TYPE_HALF_FLOAT:                 return 0x15;
        case NANOARROW_TYPE_DECIMAL128:                 return 0x16;
        case NANOARROW_TYPE_DECIMAL256:                 return 0x17;
        case NANOARROW_TYPE_LARGE_STRING:               return 0x18;
        case NANOARROW_TYPE_LARGE_BINARY:               return 0x19;
        case NANOARROW_TYPE_STRING_VIEW:                return 0x1A;
        case NANOARROW_TYPE_BINARY_VIEW:                return 0x1B;
        case NANOARROW_TYPE_INTERVAL_MONTHS:            return 0x1C;
        case NANOARROW_TYPE_INTERVAL_DAY_TIME:          return 0x1D;
        case NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO:    return 0x1E;
        case NANOARROW_TYPE_STRUCT:                     return 0x20;
        case NANOARROW_TYPE_LIST:                       return 0x21;
        case NANOARROW_TYPE_LARGE_LIST:                 return 0x22;
        case NANOARROW_TYPE_FIXED_SIZE_LIST:            return 0x23;
        case NANOARROW_TYPE_MAP:                        return 0x24;
        case NANOARROW_TYPE_SPARSE_UNION:               return 0x25;
        case NANOARROW_TYPE_DENSE_UNION:                return 0x26;
        default:                                        return 0x00;
    }
}

enum ArrowType WireTypeToNanoarrowType(int wt) {
    switch (wt) {
        case 0x01: return NANOARROW_TYPE_BOOL;
        case 0x02: return NANOARROW_TYPE_INT8;
        case 0x03: return NANOARROW_TYPE_INT16;
        case 0x04: return NANOARROW_TYPE_INT32;
        case 0x05: return NANOARROW_TYPE_INT64;
        case 0x06: return NANOARROW_TYPE_UINT8;
        case 0x07: return NANOARROW_TYPE_UINT16;
        case 0x08: return NANOARROW_TYPE_UINT32;
        case 0x09: return NANOARROW_TYPE_UINT64;
        case 0x0A: return NANOARROW_TYPE_FLOAT;
        case 0x0B: return NANOARROW_TYPE_DOUBLE;
        case 0x0C: return NANOARROW_TYPE_STRING;
        case 0x0D: return NANOARROW_TYPE_BINARY;
        case 0x18: return NANOARROW_TYPE_LARGE_STRING;
        case 0x19: return NANOARROW_TYPE_LARGE_BINARY;
        default:
            throw std::invalid_argument(
                "create_topic: wireType 0x" + std::to_string(wt) +
                " not yet supported in publisher-supplied schemas "
                "(only scalar types are accepted today)");
    }
}

namespace {

json FieldToJson(const ArrowSchema* field) {
    json j;
    j["name"] = field->name ? field->name : "";
    j["nullable"] = (field->flags & ARROW_FLAG_NULLABLE) != 0;

    struct ArrowSchemaView view;
    ArrowSchemaViewInit(&view, field, nullptr);
    j["wireType"] = NanoarrowTypeToWireType(view.type);

    if (field->metadata) {
        struct ArrowStringView value;
        if (ArrowMetadataGetValue(field->metadata,
                ArrowCharView("field_number"), &value) == NANOARROW_OK) {
            j["fieldNumber"] = std::stoi(std::string(value.data, value.size_bytes));
        }
    }

    if (view.type == NANOARROW_TYPE_STRUCT) {
        json fields_arr = json::array();
        for (int64_t i = 0; i < field->n_children; ++i)
            fields_arr.push_back(FieldToJson(field->children[i]));
        j["fields"] = std::move(fields_arr);
    } else if (view.type == NANOARROW_TYPE_LIST
            || view.type == NANOARROW_TYPE_LARGE_LIST) {
        j["element"] = FieldToJson(field->children[0]);
    } else if (view.type == NANOARROW_TYPE_FIXED_SIZE_LIST) {
        j["element"] = FieldToJson(field->children[0]);
        j["fixedSize"] = view.fixed_size;
    } else if (view.type == NANOARROW_TYPE_MAP) {
        const ArrowSchema* entries = field->children[0];
        j["mapKey"] = FieldToJson(entries->children[0]);
        j["mapValue"] = FieldToJson(entries->children[1]);
    }

    return j;
}

}  // anonymous namespace

json ArrowSchemaToJson(const ArrowSchema* schema) {
    json fields_arr = json::array();
    for (int64_t i = 0; i < schema->n_children; ++i)
        fields_arr.push_back(FieldToJson(schema->children[i]));
    return json{{"fields", std::move(fields_arr)}};
}

OwnedSchema BuildArrowSchemaFromJson(const json& j) {
    if (!j.contains("fields") || !j["fields"].is_array()) {
        throw std::invalid_argument(
            "create_topic: schema must contain a 'fields' array");
    }
    const auto& fields = j["fields"];
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(),
                             static_cast<int64_t>(fields.size()));
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        if (!f.contains("name") || !f["name"].is_string()) {
            throw std::invalid_argument(
                "create_topic: field at index " + std::to_string(i) +
                " is missing a string 'name'");
        }
        if (!f.contains("wireType") || !f["wireType"].is_number_integer()) {
            throw std::invalid_argument(
                "create_topic: field at index " + std::to_string(i) +
                " is missing an integer 'wireType'");
        }
        const auto name = f["name"].get<std::string>();
        const int wt = f["wireType"].get<int>();
        ArrowSchemaSetName(schema->children[i], name.c_str());
        ArrowSchemaSetType(schema->children[i], WireTypeToNanoarrowType(wt));
    }
    return schema;
}

}  // namespace fletcher::gateway
