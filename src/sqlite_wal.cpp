#include "sqlite_wal.hpp"

#include <sqlite3.h>

#include <arrow/builder.h>
#include <arrow/table.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace arrow_row {
namespace {

const char* ArrowTypeToSQLite(const arrow::DataType& type) {
    using T = arrow::Type;
    switch (type.id()) {
        case T::BOOL:
        case T::INT8:   case T::INT16:  case T::INT32:  case T::INT64:
        case T::UINT8:  case T::UINT16: case T::UINT32: case T::UINT64:
        case T::DATE32: case T::DATE64: case T::TIMESTAMP:
            return "INTEGER";
        case T::FLOAT:  case T::DOUBLE:
            return "REAL";
        case T::STRING: case T::LARGE_STRING:
            return "TEXT";
        case T::BINARY: case T::LARGE_BINARY:
            return "BLOB";
        default:
            throw std::invalid_argument(
                "SQLiteWAL: unsupported Arrow type: " + type.ToString());
    }
}

void BindScalar(sqlite3_stmt* stmt, int col, const arrow::Scalar& scalar) {
    if (!scalar.is_valid) {
        sqlite3_bind_null(stmt, col);
        return;
    }
    using T = arrow::Type;
    switch (scalar.type->id()) {
        case T::BOOL:
            sqlite3_bind_int64(stmt, col,
                static_cast<const arrow::BooleanScalar&>(scalar).value ? 1 : 0);
            break;
        case T::INT8:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::Int8Scalar&>(scalar).value);
            break;
        case T::INT16:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::Int16Scalar&>(scalar).value);
            break;
        case T::INT32:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::Int32Scalar&>(scalar).value);
            break;
        case T::INT64:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::Int64Scalar&>(scalar).value);
            break;
        case T::UINT8:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::UInt8Scalar&>(scalar).value);
            break;
        case T::UINT16:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::UInt16Scalar&>(scalar).value);
            break;
        case T::UINT32:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::UInt32Scalar&>(scalar).value);
            break;
        case T::UINT64:
            sqlite3_bind_int64(stmt, col,
                static_cast<int64_t>(static_cast<const arrow::UInt64Scalar&>(scalar).value));
            break;
        case T::DATE32:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::Date32Scalar&>(scalar).value);
            break;
        case T::DATE64:
            sqlite3_bind_int64(stmt, col, static_cast<const arrow::Date64Scalar&>(scalar).value);
            break;
        case T::TIMESTAMP:
            sqlite3_bind_int64(stmt, col,
                static_cast<const arrow::TimestampScalar&>(scalar).value);
            break;
        case T::FLOAT:
            sqlite3_bind_double(stmt, col,
                static_cast<const arrow::FloatScalar&>(scalar).value);
            break;
        case T::DOUBLE:
            sqlite3_bind_double(stmt, col,
                static_cast<const arrow::DoubleScalar&>(scalar).value);
            break;
        case T::STRING:
        case T::LARGE_STRING: {
            const auto& buf = *static_cast<const arrow::BaseBinaryScalar&>(scalar).value;
            sqlite3_bind_text(stmt, col,
                reinterpret_cast<const char*>(buf.data()),
                static_cast<int>(buf.size()), SQLITE_TRANSIENT);
            break;
        }
        case T::BINARY:
        case T::LARGE_BINARY: {
            const auto& buf = *static_cast<const arrow::BaseBinaryScalar&>(scalar).value;
            sqlite3_bind_blob(stmt, col, buf.data(),
                static_cast<int>(buf.size()), SQLITE_TRANSIENT);
            break;
        }
        default:
            throw std::invalid_argument(
                "SQLiteLogHandle::Log: unsupported Arrow type: " + scalar.type->ToString());
    }
}

void AppendColumn(sqlite3_stmt* stmt, int col, arrow::ArrayBuilder& builder,
                  const arrow::DataType& type) {
    using T = arrow::Type;
    arrow::Status st;
    switch (type.id()) {
        case T::BOOL:
            st = static_cast<arrow::BooleanBuilder&>(builder).Append(
                sqlite3_column_int64(stmt, col) != 0);
            break;
        case T::INT8:
            st = static_cast<arrow::Int8Builder&>(builder).Append(
                static_cast<int8_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::INT16:
            st = static_cast<arrow::Int16Builder&>(builder).Append(
                static_cast<int16_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::INT32:
            st = static_cast<arrow::Int32Builder&>(builder).Append(
                static_cast<int32_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::INT64:
            st = static_cast<arrow::Int64Builder&>(builder).Append(
                sqlite3_column_int64(stmt, col));
            break;
        case T::UINT8:
            st = static_cast<arrow::UInt8Builder&>(builder).Append(
                static_cast<uint8_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::UINT16:
            st = static_cast<arrow::UInt16Builder&>(builder).Append(
                static_cast<uint16_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::UINT32:
            st = static_cast<arrow::UInt32Builder&>(builder).Append(
                static_cast<uint32_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::UINT64:
            st = static_cast<arrow::UInt64Builder&>(builder).Append(
                static_cast<uint64_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::DATE32:
            st = static_cast<arrow::Date32Builder&>(builder).Append(
                static_cast<int32_t>(sqlite3_column_int64(stmt, col)));
            break;
        case T::DATE64:
            st = static_cast<arrow::Date64Builder&>(builder).Append(
                sqlite3_column_int64(stmt, col));
            break;
        case T::TIMESTAMP:
            st = static_cast<arrow::TimestampBuilder&>(builder).Append(
                sqlite3_column_int64(stmt, col));
            break;
        case T::FLOAT:
            st = static_cast<arrow::FloatBuilder&>(builder).Append(
                static_cast<float>(sqlite3_column_double(stmt, col)));
            break;
        case T::DOUBLE:
            st = static_cast<arrow::DoubleBuilder&>(builder).Append(
                sqlite3_column_double(stmt, col));
            break;
        case T::STRING: {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            st = static_cast<arrow::StringBuilder&>(builder).Append(
                text, sqlite3_column_bytes(stmt, col));
            break;
        }
        case T::LARGE_STRING: {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            st = static_cast<arrow::LargeStringBuilder&>(builder).Append(
                text, sqlite3_column_bytes(stmt, col));
            break;
        }
        case T::BINARY: {
            const void* blob = sqlite3_column_blob(stmt, col);
            st = static_cast<arrow::BinaryBuilder&>(builder).Append(
                static_cast<const uint8_t*>(blob), sqlite3_column_bytes(stmt, col));
            break;
        }
        case T::LARGE_BINARY: {
            const void* blob = sqlite3_column_blob(stmt, col);
            st = static_cast<arrow::LargeBinaryBuilder&>(builder).Append(
                static_cast<const uint8_t*>(blob), sqlite3_column_bytes(stmt, col));
            break;
        }
        default:
            throw std::invalid_argument(
                "SQLiteLogHandle::ToTable: unsupported Arrow type: " + type.ToString());
    }
    if (!st.ok())
        throw std::runtime_error("SQLiteLogHandle::ToTable: builder error: " + st.ToString());
}

}  // namespace

SQLiteLogHandle::SQLiteLogHandle(sqlite3* db, std::string table_name,
                                 std::vector<int> key_columns, RowCodec codec,
                                 sqlite3_stmt* insert_stmt)
    : LogHandle(std::move(key_columns))
    , db_(db)
    , table_name_(std::move(table_name))
    , codec_(std::move(codec))
    , insert_stmt_(insert_stmt)
{}

SQLiteLogHandle::~SQLiteLogHandle() {
    sqlite3_finalize(insert_stmt_);
    sqlite3_exec(db_, ("DROP TABLE IF EXISTS " + table_name_ + ";").c_str(),
                 nullptr, nullptr, nullptr);
}

void SQLiteLogHandle::Log(const ArrowRow& buffer) {
    auto scalars = codec_.DecodeRow(buffer);
    const arrow::Schema& schema = codec_.schema();

    for (int i = 0; i < schema.num_fields(); ++i)
        BindScalar(insert_stmt_, i + 1, *scalars[i]);

    const int rc = sqlite3_step(insert_stmt_);
    sqlite3_reset(insert_stmt_);
    sqlite3_clear_bindings(insert_stmt_);
    if (rc != SQLITE_DONE)
        throw std::runtime_error("SQLiteLogHandle::Log: " + std::string(sqlite3_errmsg(db_)));
}

std::shared_ptr<arrow::Table> SQLiteLogHandle::ToTable() const {
    const arrow::Schema& schema = codec_.schema();

    std::string sql = "SELECT * FROM " + table_name_;
    if (!key_columns().empty()) {
        sql += " ORDER BY ";
        const auto& keys = key_columns();
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += schema.field(keys[i])->name();
        }
    }
    sql += ";";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLiteLogHandle::ToTable: " + std::string(sqlite3_errmsg(db_)));

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<std::shared_ptr<arrow::ArrayBuilder>> builders;
    builders.reserve(schema.num_fields());
    for (int i = 0; i < schema.num_fields(); ++i) {
        std::unique_ptr<arrow::ArrayBuilder> b;
        auto st = arrow::MakeBuilder(pool, schema.field(i)->type(), &b);
        if (!st.ok()) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("SQLiteLogHandle::ToTable: " + st.ToString());
        }
        builders.push_back(std::shared_ptr<arrow::ArrayBuilder>(std::move(b)));
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (int col = 0; col < schema.num_fields(); ++col) {
            if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
                auto st = builders[col]->AppendNull();
                if (!st.ok()) {
                    sqlite3_finalize(stmt);
                    throw std::runtime_error("SQLiteLogHandle::ToTable: " + st.ToString());
                }
            } else {
                AppendColumn(stmt, col, *builders[col], *schema.field(col)->type());
            }
        }
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
        throw std::runtime_error("SQLiteLogHandle::ToTable: " + std::string(sqlite3_errmsg(db_)));

    arrow::ArrayVector arrays;
    arrays.reserve(schema.num_fields());
    for (auto& b : builders) {
        std::shared_ptr<arrow::Array> arr;
        auto st = b->Finish(&arr);
        if (!st.ok())
            throw std::runtime_error("SQLiteLogHandle::ToTable: " + st.ToString());
        arrays.push_back(std::move(arr));
    }

    int64_t    num_rows   = arrays.empty() ? 0 : arrays[0]->length();
    auto       schema_ptr = arrow::schema(schema.fields(), schema.metadata());
    return arrow::Table::Make(schema_ptr, arrays, num_rows);
}

SQLiteWAL::SQLiteWAL(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLiteWAL: failed to open database: " + err);
    }
}

SQLiteWAL::~SQLiteWAL() {
    if (db_) sqlite3_close(db_);
}

std::unique_ptr<LogHandle> SQLiteWAL::CreateLog(const arrow::Schema&    schema,
                                                 const std::vector<int>& key_columns) {
    std::string table_name = "log_" + std::to_string(log_counter_++);

    std::string sql = "CREATE TABLE IF NOT EXISTS " + table_name + " (";
    for (int i = 0; i < schema.num_fields(); ++i) {
        if (i > 0) sql += ", ";
        const auto& field = schema.field(i);
        sql += field->name();
        sql += ' ';
        sql += ArrowTypeToSQLite(*field->type());
        if (!field->nullable())
            sql += " NOT NULL";
    }
    if (!key_columns.empty()) {
        sql += ", PRIMARY KEY (";
        for (size_t i = 0; i < key_columns.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += schema.field(key_columns[i])->name();
        }
        sql += ")";
    }

    sql += ");";

    char* err_msg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::string err = err_msg;
        sqlite3_free(err_msg);
        throw std::runtime_error("SQLiteWAL::CreateLog: " + err);
    }

    std::string insert_sql = "INSERT INTO " + table_name + " VALUES (";
    for (int i = 0; i < schema.num_fields(); ++i) {
        if (i > 0) insert_sql += ", ";
        insert_sql += "?";
    }
    insert_sql += ");";

    sqlite3_stmt* insert_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, insert_sql.c_str(), -1, &insert_stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLiteWAL::CreateLog: failed to prepare INSERT statement: "
                                 + std::string(sqlite3_errmsg(db_)));

    return std::make_unique<SQLiteLogHandle>(
        db_, std::move(table_name), key_columns,
        RowCodec(arrow::schema(schema.fields(), schema.metadata())),
        insert_stmt);
}

}  // namespace arrow_row
