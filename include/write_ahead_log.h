#pragma once

#include "row_codec.h"

#include <arrow/api.h>

#include <memory>

namespace arrow_row {

class LogHandle {
public:
    explicit LogHandle(std::vector<int> keyColumns)
        : keyColumns_(std::move(keyColumns)) {}
    virtual ~LogHandle() = default;

    const std::vector<int>& keyColumns() const { return keyColumns_; }

    // Insert the row encoded in buffer into the table represented by this handle.
    // Throws std::runtime_error on database errors.
    virtual void log(const ArrowRow& buffer) = 0;

    // Read all rows from the log and return them as an Arrow Table, ordered by
    // the key columns in the sequence they appear in keyColumns().
    // Throws std::runtime_error on database errors.
    virtual std::shared_ptr<arrow::Table> toTable() const = 0;

private:
    std::vector<int> keyColumns_;
};

class WriteAheadLog {
public:
    virtual ~WriteAheadLog() = default;

    // Create a log table whose columns match the schema fields.
    // Each column is typed to hold the raw scalar value for that field's
    // Arrow type:
    //   bool / int* / uint* / date32 / date64 / timestamp  →  INTEGER
    //   float / double                                      →  REAL
    //   string / large_string                              →  TEXT
    //   binary / large_binary                              →  BLOB
    //
    // Non-nullable schema fields are emitted with a NOT NULL constraint.
    // Returns a handle to the created log table.
    // Throws std::invalid_argument for unsupported Arrow types.
    // Throws std::runtime_error on database errors.
    virtual std::unique_ptr<LogHandle> createLog(const arrow::Schema&    schema,
                                                  const std::vector<int>& keyColumns) = 0;

};

} // namespace arrow_row
