#include "generic_row_batcher.hpp"
#include "row_codec.hpp"
#include "sqlite_wal.hpp"

#include <arrow/api.h>

#include <iomanip>
#include <iostream>

static void PrintHex(const std::vector<uint8_t>& buf) {
    for (size_t i = 0; i < buf.size(); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(buf[i]);
        std::cout << ((i + 1) % 16 == 0 ? '\n' : ' ');
    }
    std::cout << std::dec << '\n';
}

int main() {
    // Schema: id (int32), name (string), score (float64), tag (string)
    auto schema = arrow::schema({
        arrow::field("id",    arrow::int32()),
        arrow::field("name",  arrow::utf8()),
        arrow::field("score", arrow::float64()),
        arrow::field("tag",   arrow::utf8()),
    });

    arrow_row::RowCodec codec(schema);

    // Row 1: id=42, name="Alice", score=98.6, tag=null
    std::vector<std::shared_ptr<arrow::Scalar>> values = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("Alice"),
        std::make_shared<arrow::DoubleScalar>(98.6),
        arrow::MakeNullScalar(arrow::utf8()),
    };

    // Row 2: id=7, name="Bob", score=77.7, tag="veteran"
    std::vector<std::shared_ptr<arrow::Scalar>> values2 = {
        std::make_shared<arrow::Int32Scalar>(7),
        std::make_shared<arrow::StringScalar>("Bob"),
        std::make_shared<arrow::DoubleScalar>(77.7),
        std::make_shared<arrow::StringScalar>("veteran"),
    };

    try {
        auto buf  = codec.EncodeRow(values);
        auto buf2 = codec.EncodeRow(values2);

        std::cout << "Row 1 (" << buf.size() << " bytes):\n";
        PrintHex(buf);
        std::cout << "Row 2 (" << buf2.size() << " bytes):\n";
        PrintHex(buf2);

        // Decode scalar vector (single-row round-trip check)
        auto decoded = codec.DecodeRow(buf);
        std::cout << "\nDecoded row 1 scalars:\n";
        for (int i = 0; i < schema->num_fields(); ++i) {
            const auto& s = decoded[i];
            std::cout << "  " << schema->field(i)->name() << " = "
                      << (s && s->is_valid ? s->ToString() : "null") << '\n';
        }

        // Batch both rows via the write-ahead log
        arrow_row::SQLiteWAL wrl(":memory:");
        arrow_row::GenericRowBatcher batcher(schema, wrl);
        batcher.Append(buf);
        batcher.Append(buf2);
        auto table = batcher.Flush();

        std::cout << "\nTable: " << table->num_rows() << " rows, "
                  << table->num_columns() << " columns\n";
        for (int c = 0; c < table->num_columns(); ++c) {
            std::cout << "  " << schema->field(c)->name() << ": "
                      << table->column(c)->ToString() << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
