#ifndef ARROW_ROW_INCLUDE_SQLITE_WAL_HPP_
#define ARROW_ROW_INCLUDE_SQLITE_WAL_HPP_

#include "row_codec.hpp"
#include "write_ahead_log.hpp"

#include <string>

struct sqlite3;       // forward declaration - keeps <sqlite3.h> out of the public header
struct sqlite3_stmt;  // forward declaration

namespace arrow_row {

class SQLiteLogHandle : public LogHandle {
 public:
    SQLiteLogHandle(sqlite3* db, std::string table_name,
                    std::vector<int> key_columns, RowCodec codec,
                    sqlite3_stmt* insert_stmt);
    ~SQLiteLogHandle() override;

    // Non-copyable, non-movable (owns sqlite3_stmt*).
    SQLiteLogHandle(const SQLiteLogHandle&)            = delete;
    SQLiteLogHandle& operator=(const SQLiteLogHandle&) = delete;
    SQLiteLogHandle(SQLiteLogHandle&&)                 = delete;
    SQLiteLogHandle& operator=(SQLiteLogHandle&&)      = delete;

    void Log(const ArrowRow& buffer) override;
    std::shared_ptr<arrow::Table> ToTable() const override;

 private:
    sqlite3*      db_;
    std::string   table_name_;
    RowCodec      codec_;
    sqlite3_stmt* insert_stmt_{nullptr};
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

    std::unique_ptr<LogHandle> CreateLog(const arrow::Schema&    schema,
                                         const std::vector<int>& key_columns) override;

 private:
    sqlite3*     db_{nullptr};
    unsigned int log_counter_{0};
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_SQLITE_WAL_HPP_
