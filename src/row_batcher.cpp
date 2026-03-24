#include "row_batcher.hpp"
#include "row_codec.hpp"     // kVersion, FingerprintHash
#include "row_reader.hpp"    // detail::Reader

#include <arrow/builder.h>
#include <arrow/record_batch.h>

#include <stdexcept>
#include <string>

namespace arrow_row {
namespace {

using detail::Reader;

void AppendField(Reader& r, arrow::ArrayBuilder& builder, const arrow::DataType& type) {
    using T = arrow::Type;
    arrow::Status st;

    switch (type.id()) {
        case T::BOOL:
            st = static_cast<arrow::BooleanBuilder&>(builder).Append(r.Read<uint8_t>() != 0);
            break;
        case T::INT8:
            st = static_cast<arrow::Int8Builder&>(builder).Append(r.Read<int8_t>());
            break;
        case T::INT16:
            st = static_cast<arrow::Int16Builder&>(builder).Append(r.Read<int16_t>());
            break;
        case T::INT32:
            st = static_cast<arrow::Int32Builder&>(builder).Append(r.Read<int32_t>());
            break;
        case T::INT64:
            st = static_cast<arrow::Int64Builder&>(builder).Append(r.Read<int64_t>());
            break;
        case T::UINT8:
            st = static_cast<arrow::UInt8Builder&>(builder).Append(r.Read<uint8_t>());
            break;
        case T::UINT16:
            st = static_cast<arrow::UInt16Builder&>(builder).Append(r.Read<uint16_t>());
            break;
        case T::UINT32:
            st = static_cast<arrow::UInt32Builder&>(builder).Append(r.Read<uint32_t>());
            break;
        case T::UINT64:
            st = static_cast<arrow::UInt64Builder&>(builder).Append(r.Read<uint64_t>());
            break;
        case T::FLOAT:
            st = static_cast<arrow::FloatBuilder&>(builder).Append(r.Read<float>());
            break;
        case T::DOUBLE:
            st = static_cast<arrow::DoubleBuilder&>(builder).Append(r.Read<double>());
            break;

        case T::STRING: {
            uint32_t    len = r.Read<uint32_t>();
            const char* ptr = reinterpret_cast<const char*>(r.ReadBytes(len));
            st = static_cast<arrow::StringBuilder&>(builder).Append(ptr, static_cast<int32_t>(len));
            break;
        }
        case T::LARGE_STRING: {
            uint32_t    len = r.Read<uint32_t>();
            const char* ptr = reinterpret_cast<const char*>(r.ReadBytes(len));
            st = static_cast<arrow::LargeStringBuilder&>(builder).Append(ptr, static_cast<int64_t>(len));
            break;
        }
        case T::BINARY: {
            uint32_t    len = r.Read<uint32_t>();
            const char* ptr = reinterpret_cast<const char*>(r.ReadBytes(len));
            st = static_cast<arrow::BinaryBuilder&>(builder).Append(ptr, static_cast<int32_t>(len));
            break;
        }
        case T::LARGE_BINARY: {
            uint32_t    len = r.Read<uint32_t>();
            const char* ptr = reinterpret_cast<const char*>(r.ReadBytes(len));
            st = static_cast<arrow::LargeBinaryBuilder&>(builder).Append(ptr, static_cast<int64_t>(len));
            break;
        }

        case T::DATE32:
            st = static_cast<arrow::Date32Builder&>(builder).Append(r.Read<int32_t>());
            break;
        case T::DATE64:
            st = static_cast<arrow::Date64Builder&>(builder).Append(r.Read<int64_t>());
            break;
        case T::TIMESTAMP:
            st = static_cast<arrow::TimestampBuilder&>(builder).Append(r.Read<int64_t>());
            break;

        default:
            throw std::invalid_argument(
                "RowBatcher::Append: unsupported Arrow type: " + type.ToString());
    }

    if (!st.ok())
        throw std::runtime_error("RowBatcher::Append: builder error: " + st.ToString());
}

}  // namespace

RowBatcher::RowBatcher(std::shared_ptr<arrow::Schema> schema, WriteAheadLog& wal)
    : schema_(std::move(schema)), wal_(wal), handle_(wal_.CreateLog(*schema_, {})) {}

void RowBatcher::Append(const ArrowRow& buf) {
    handle_->Log(buf);
    ++row_count_;
}

std::shared_ptr<arrow::Table> RowBatcher::Flush() {
    auto table = handle_->ToTable();
    handle_    = wal_.CreateLog(*schema_, {});
    row_count_ = 0;
    return table;
}

}  // namespace arrow_row
