#pragma once

#include "row_codec.h"
#include "write_ahead_log.h"

#include <string>

struct sqlite3;      // forward declaration — keeps <sqlite3.h> out of the public header
struct sqlite3_stmt; // forward declaration

namespace arrow_row {

class SQLiteLogHandle : public LogHandle {
public:
    SQLiteLogHandle(sqlite3* db, std::string tableName,
                    std::vector<int> keyColumns, RowCodec codec,
                    sqlite3_stmt* insertStmt);
    ~SQLiteLogHandle() override;

    // Non-copyable, non-movable (owns sqlite3_stmt*).
    SQLiteLogHandle(const SQLiteLogHandle&)            = delete;
    SQLiteLogHandle& operator=(const SQLiteLogHandle&) = delete;
    SQLiteLogHandle(SQLiteLogHandle&&)                 = delete;
    SQLiteLogHandle& operator=(SQLiteLogHandle&&)      = delete;

    void log(const ArrowRow& buffer) override;
    std::shared_ptr<arrow::Table> toTable() const override;

private:
    sqlite3*      db_;
    std::string   tableName_;
    RowCodec      codec_;
    sqlite3_stmt* insertStmt_{nullptr};
};

class SQLiteWAL : public WriteAheadLog {
public:
    // Open (or create) the database at path.
    // Pass ":memory:" for an in-memory database.
    // Throws std::runtime_error if the database cannot be opened.
    explicit SQLiteWAL(const std::string& path);
    ~SQLiteWAL() override;

    // Non-copyable, movable.
    SQLiteWAL(const SQLiteWAL&)            = delete;
    SQLiteWAL& operator=(const SQLiteWAL&) = delete;
    SQLiteWAL(SQLiteWAL&&)                 = default;
    SQLiteWAL& operator=(SQLiteWAL&&)      = default;

    std::unique_ptr<LogHandle> createLog(const arrow::Schema&    schema,
                                         const std::vector<int>& keyColumns) override;

private:
    sqlite3*     db_{nullptr};
    unsigned int logCounter_{0};
};

} // namespace arrow_row
