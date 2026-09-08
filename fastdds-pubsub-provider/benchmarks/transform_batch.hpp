// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// One row shape and the Arrow plumbing around it, shared by bench_pub_sub_type.cpp and
// example_arrow_roundtrip.cpp so both measure and narrate the same thing.
//
// The row is NaviSuite's TransformWithVelocity: three fields, two of them a nested struct holding
// one fixed-size list of doubles. modules/datamodel's generated `.fletcher.pb.h` for it is not
// reachable from this repo, so the shape is written out here — EncodeRow/DecodeRow are what that
// generated code does, by hand.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_BENCHMARKS_TRANSFORM_BATCH_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_BENCHMARKS_TRANSFORM_BATCH_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>

namespace fletcher {
namespace benchmarks {

struct TransformRow {
    int64_t timestamp;
    double pose[16];
    double velocity[6];
};

constexpr int kPoseValues = 16;
constexpr int kVelocityValues = 6;

inline void InitFixedListStruct(ArrowSchema* out, const char* name, int32_t values) {
    ArrowSchemaSetTypeStruct(out, 1);
    ArrowSchemaSetName(out, name);
    ArrowSchemaSetTypeFixedSize(out->children[0], NANOARROW_TYPE_FIXED_SIZE_LIST, values);
    ArrowSchemaSetName(out->children[0], "values");
    ArrowSchemaSetType(out->children[0]->children[0], NANOARROW_TYPE_DOUBLE);
}

inline fletcher::OwnedSchema TransformSchema() {
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 3);
    ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP,
                               NANOARROW_TIME_UNIT_NANO, nullptr);
    ArrowSchemaSetName(schema->children[0], "timestamp");
    InitFixedListStruct(schema->children[1], "pose", kPoseValues);
    InitFixedListStruct(schema->children[2], "velocity", kVelocityValues);
    return schema;
}

inline void EncodeRow(const TransformRow& row, fletcher::WriteBuffer& buf) {
    fletcher::PositionalWriter writer(buf, 3);
    writer.WriteTimestamp(row.timestamp);
    {
        auto nested = writer.BeginStruct(1);
        nested.WriteFixedArray(row.pose, kPoseValues);
    }
    {
        auto nested = writer.BeginStruct(1);
        nested.WriteFixedArray(row.velocity, kVelocityValues);
    }
}

inline TransformRow DecodeRow(const uint8_t* data, size_t len) {
    TransformRow row{};
    fletcher::PositionalReader reader(data, len, 3);
    row.timestamp = reader.ReadTimestamp();
    {
        auto nested = reader.ReadStruct(1);
        nested.ReadFixedArray(row.pose, kPoseValues);
    }
    {
        auto nested = reader.ReadStruct(1);
        nested.ReadFixedArray(row.velocity, kVelocityValues);
    }
    return row;
}

// An ArrowArray built through the append API and released on destruction — the batch shape
// SubscriberArrow accumulates into, one tier down.
class ArrowBatch {
   public:
    explicit ArrowBatch(const ArrowSchema* schema) {
        std::memset(&array_, 0, sizeof(array_));
        if (ArrowArrayInitFromSchema(&array_, schema, nullptr) != NANOARROW_OK ||
            ArrowArrayStartAppending(&array_) != NANOARROW_OK) {
            std::fputs("ArrowArray init failed\n", stderr);
            std::abort();
        }
    }
    ~ArrowBatch() {
        if (array_.release) array_.release(&array_);
    }
    ArrowBatch(const ArrowBatch&) = delete;
    ArrowBatch& operator=(const ArrowBatch&) = delete;

    void Append(const TransformRow& row) {
        ArrowArrayAppendInt(array_.children[0], row.timestamp);
        AppendValues(array_.children[1], row.pose, kPoseValues);
        AppendValues(array_.children[2], row.velocity, kVelocityValues);
        ArrowArrayFinishElement(&array_);
    }

    void Finish() { ArrowArrayFinishBuildingDefault(&array_, nullptr); }

    ArrowArray* get() { return &array_; }

   private:
    static void AppendValues(ArrowArray* nested, const double* values, int count) {
        ArrowArray* list = nested->children[0];
        for (int i = 0; i < count; ++i) ArrowArrayAppendDouble(list->children[0], values[i]);
        ArrowArrayFinishElement(list);
        ArrowArrayFinishElement(nested);
    }

    ArrowArray array_;
};

// Reads rows back out of a finished batch. A fixed-size list stores row r's elements at r * count
// in its child, so no offsets buffer is involved.
class ArrowBatchView {
   public:
    ArrowBatchView(const ArrowSchema* schema, const ArrowArray* array) {
        std::memset(&view_, 0, sizeof(view_));
        if (ArrowArrayViewInitFromSchema(&view_, schema, nullptr) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view_, array, nullptr) != NANOARROW_OK) {
            std::fputs("ArrowArrayView init failed\n", stderr);
            std::abort();
        }
    }
    ~ArrowBatchView() { ArrowArrayViewReset(&view_); }
    ArrowBatchView(const ArrowBatchView&) = delete;
    ArrowBatchView& operator=(const ArrowBatchView&) = delete;

    TransformRow Row(int64_t index) const {
        TransformRow row{};
        row.timestamp = ArrowArrayViewGetIntUnsafe(view_.children[0], index);
        ReadValues(view_.children[1], index, row.pose, kPoseValues);
        ReadValues(view_.children[2], index, row.velocity, kVelocityValues);
        return row;
    }

   private:
    static void ReadValues(const ArrowArrayView* nested, int64_t index, double* out, int count) {
        const ArrowArrayView* values = nested->children[0]->children[0];
        for (int i = 0; i < count; ++i) {
            out[i] = ArrowArrayViewGetDoubleUnsafe(values, index * count + i);
        }
    }

    ArrowArrayView view_;
};

inline TransformRow MakeRow(int64_t index) {
    TransformRow row{};
    row.timestamp = 1234567890123456789 + index;
    for (int i = 0; i < kPoseValues; ++i) row.pose[i] = static_cast<double>(index) + i * 0.25;
    for (int i = 0; i < kVelocityValues; ++i) row.velocity[i] = static_cast<double>(i) * 0.5;
    return row;
}

inline void BuildSourceBatch(ArrowBatch& batch, int64_t rows) {
    for (int64_t i = 0; i < rows; ++i) batch.Append(MakeRow(i));
    batch.Finish();
}

}  // namespace benchmarks
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_BENCHMARKS_TRANSFORM_BATCH_HPP_
