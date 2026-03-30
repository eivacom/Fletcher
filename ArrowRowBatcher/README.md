# ArrowRowBatcher

Accumulates `ArrowRow` buffers and flushes them as an Apache Arrow `Table` once a configured batch size is reached. Includes a write-ahead log (WAL) interface backed by SQLite for durability during accumulation.

## Components

| Header | Purpose |
|---|---|
| `include/row_batcher.hpp` | Abstract `RowBatcher` base class |
| `include/generic_row_batcher.hpp` | Concrete batcher that buffers through a WAL |
| `include/write_ahead_log.hpp` | Abstract WAL interface (`WriteAheadLog`, `LogHandle`) |
| `include/sqlite_wal.hpp` | SQLite implementation of `WriteAheadLog` |

## RowBatcher

`RowBatcher` accepts encoded rows one at a time and fires a callback with a complete `arrow::Table` each time `batch_size` rows have accumulated. The callback is dispatched on a background thread and does not block the caller.

```cpp
#include <generic_row_batcher.hpp>
#include <sqlite_wal.hpp>
using namespace arrow_row;

auto schema = arrow::schema({ arrow::field("x", arrow::int32()) });

SQLiteWAL wal(":memory:");   // or a file path for durability

GenericRowBatcher batcher(
    schema,
    wal,
    /*batch_size=*/1000,
    [](std::shared_ptr<arrow::Table> table) -> bool {
        // Called on a background thread when 1000 rows are ready.
        // Return true on success, false to signal failure.
        write_to_parquet(table);
        return true;
    });

// Thread-safe. Throws std::invalid_argument on a malformed or
// schema-mismatched buffer.
batcher.Append(row);
```

The destructor waits for all in-flight flush threads to finish before returning, so it is safe to destroy the batcher at any time.

## WriteAheadLog

`WriteAheadLog` is an abstract interface for durable row buffering. A log is created per schema via `CreateLog`, which returns a `LogHandle`.

```cpp
class WriteAheadLog {
    virtual std::unique_ptr<LogHandle> CreateLog(
        const arrow::Schema&    schema,
        const std::vector<int>& key_columns) = 0;
};

class LogHandle {
    virtual void Log(const ArrowRow& buffer) = 0;
    virtual std::shared_ptr<arrow::Table> ToTable() const = 0;
};
```

`key_columns` is a list of field indices that are used to order rows when `ToTable()` materialises the accumulated data. This affects the row order within each flushed batch but does not deduplicate rows.

### Arrow type → SQLite column type mapping

When creating a log table, scalar fields are mapped to SQLite column types as follows:

| Arrow type | SQLite type |
|---|---|
| bool, int\*, uint\*, date32/64, timestamp | `INTEGER` |
| float32, float64 | `REAL` |
| utf8, large_utf8 | `TEXT` |
| binary, large_binary | `BLOB` |

Non-nullable schema fields are emitted with a `NOT NULL` constraint. Composite types (struct, list, map) are not supported in the WAL layer and will cause `CreateLog` to throw `std::invalid_argument`. If your schema contains nested fields, flatten them or use the codec directly.

## SQLiteWAL

The provided `SQLiteWAL` implementation opens (or creates) a SQLite database at a given file path. Use `":memory:"` for a non-durable in-process store.

```cpp
SQLiteWAL wal("/var/data/my_log.db");
auto handle = wal.CreateLog(*schema, {0});  // key on column 0
handle->Log(row);
auto table = handle->ToTable();  // returns all logged rows as an Arrow Table
```

`SQLiteWAL` is non-copyable but movable. `SQLiteLogHandle` is non-copyable and non-movable (it owns a prepared statement bound to a specific database connection).

## Thread safety

`RowBatcher::Append` is thread-safe. The underlying `DoAppend` and `DoFlush` calls are serialised under an internal mutex, so the batcher may be called from multiple producer threads simultaneously. The flush callback is always invoked from a dedicated background thread, never from the calling thread.

## Building

Produces three static libraries:

- `row_batcher` — `RowBatcher` base and `GenericRowBatcher`; links `row_codec`, `arrow::arrow`
- `sqlite_dbs` — `SQLiteWAL` / `SQLiteLogHandle`; links `row_codec`, `SQLite::SQLite3`
- `arrow_row_batcher_capi` — C FFI wrapper; links `row_batcher`, `sqlite_dbs`
