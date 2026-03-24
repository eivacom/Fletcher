#ifndef ARROW_ROW_INCLUDE_WRITE_AHEAD_LOG_HPP_
#define ARROW_ROW_INCLUDE_WRITE_AHEAD_LOG_HPP_

#include "row_codec.hpp"

#include <arrow/api.h>

#include <memory>

namespace arrow_row {

class LogHandle {
 public:
    explicit LogHandle(std::vector<int> key_columns)
        : key_columns_(std::move(key_columns)) {}
    virtual ~LogHandle() = default;

    const std::vector<int>& key_columns() const { return key_columns_; }

    // Insert the row encoded in buffer into the table represented by this handle.
    // Throws std::runtime_error on database errors.
    virtual void Log(const ArrowRow& buffer) = 0;

    // Read all rows from the log and return them as an Arrow Table, ordered by
    // the key columns in the sequence they appear in key_columns().
    // Throws std::runtime_error on database errors.
    virtual std::shared_ptr<arrow::Table> ToTable() const = 0;

 private:
    std::vector<int> key_columns_;
};

class WriteAheadLog {
 public:
    virtual ~WriteAheadLog() = default;

    // Create a log table whose columns match the schema fields.
    // Each column is typed to hold the raw scalar value for that field's
    // Arrow type:
    //   bool / int* / uint* / date32 / date64 / timestamp  ->  INTEGER
    //   float / double                                      ->  REAL
    //   string / large_string                              ->  TEXT
    //   binary / large_binary                              ->  BLOB
    //
    // Non-nullable schema fields are emitted with a NOT NULL constraint.
    // Returns a handle to the created log table.
    // Throws std::invalid_argument for unsupported Arrow types.
    // Throws std::runtime_error on database errors.
    virtual std::unique_ptr<LogHandle> CreateLog(const arrow::Schema&    schema,
                                                  const std::vector<int>& key_columns) = 0;
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_WRITE_AHEAD_LOG_HPP_
