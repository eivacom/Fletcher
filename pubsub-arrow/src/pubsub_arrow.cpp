// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub_arrow/pubsub_arrow.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/compute/api.h>

#include <chrono>
#include <condition_variable>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fletcher {

// -----------------------------------------------------------------------
// Arrow C Data Interface helpers
// -----------------------------------------------------------------------

namespace {

OwnedSchema ExportToNano(const arrow::Schema& schema) {
    OwnedSchema out;
    auto status = arrow::ExportSchema(schema, out.get());
    if (!status.ok()) throw std::runtime_error("PubSubArrow: ExportSchema: " + status.ToString());
    return out;
}

std::shared_ptr<arrow::Schema> ImportFromNano(const ArrowSchema* schema) {
    if (!schema || !schema->release) return nullptr;
    OwnedSchema copy = OwnedSchema::DeepCopy(schema);
    auto result = arrow::ImportSchema(copy.get());
    if (!result.ok())
        throw std::runtime_error("PubSubArrow: ImportSchema: " + result.status().ToString());
    return *result;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// RecordBatchBatcher — accumulates decoded rows into RecordBatches and
// flushes on row-count, timeout, or close (see the batched Subscribe).
// -----------------------------------------------------------------------

class PubSubArrow::RecordBatchBatcher {
   public:
    RecordBatchBatcher(RecordBatchCallback cb, int64_t max_rows, std::chrono::milliseconds timeout)
        : cb_(std::move(cb)), max_rows_(max_rows < 1 ? 1 : max_rows), timeout_(timeout) {
        timer_ = std::thread([this] { TimerLoop(); });
    }

    ~RecordBatchBatcher() { Stop(); }

    RecordBatchBatcher(const RecordBatchBatcher&) = delete;
    RecordBatchBatcher& operator=(const RecordBatchBatcher&) = delete;

    // Provides the schema once known (from the subscription result). Until a
    // non-null schema is set the batcher buffers but cannot build batches.
    void SetSchema(std::shared_ptr<arrow::Schema> schema) {
        std::unique_lock<std::mutex> lk(mu_);
        schema_ = std::move(schema);
        ready_ = (schema_ != nullptr);
        if (ready_ && static_cast<int64_t>(rows_.size()) >= max_rows_)
            Flush(lk, BatchStatus::Reason::kRowLimit);
        cv_.notify_all();
    }

    void AddRow(ArrowRow row, Attachments att) {
        std::unique_lock<std::mutex> lk(mu_);
        if (stopped_) return;
        rows_.push_back(std::move(row));
        atts_.push_back(std::move(att));
        ArmTimer();
        if (ready_ && static_cast<int64_t>(rows_.size()) >= max_rows_)
            Flush(lk, BatchStatus::Reason::kRowLimit);
    }

    // A row that failed to decode: counted as lost. Its attachment is dropped
    // with it, since the metadata identifying the attachment was in that row.
    void NoteDropped() {
        std::unique_lock<std::mutex> lk(mu_);
        if (stopped_) return;
        ++dropped_;
        ArmTimer();
    }

    // Stops the timer thread and delivers any pending rows/drops (reason
    // kClosing). Idempotent.
    void Stop() {
        std::thread t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!stopped_) {
                stopped_ = true;
                Flush(lk, BatchStatus::Reason::kClosing);
                cv_.notify_all();
            }
            t = std::move(timer_);
        }
        // Don't join ourselves if a callback on the timer thread called Stop().
        if (t.joinable()) {
            if (t.get_id() == std::this_thread::get_id())
                t.detach();
            else
                t.join();
        }
    }

   private:
    bool HasPending() const { return !rows_.empty() || dropped_ > 0; }

    // Arms the timeout deadline on the first event (row or drop) of a window.
    void ArmTimer() {
        if (!has_deadline_ && HasPending()) {
            deadline_ = std::chrono::steady_clock::now() + timeout_;
            has_deadline_ = true;
            cv_.notify_all();
        }
    }

    void TimerLoop() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!stopped_) {
            if (!has_deadline_) {
                cv_.wait(lk, [this] { return stopped_ || has_deadline_; });
                continue;
            }
            // Wake on stop, on the deadline being cleared (flushed by count), or
            // when the deadline is reached.
            if (cv_.wait_until(lk, deadline_, [this] { return stopped_ || !has_deadline_; }))
                continue;
            if (ready_)
                Flush(lk, BatchStatus::Reason::kTimeout);
            else
                // Deadline reached but schema not ready yet — wait for it.
                cv_.wait(lk, [this] { return stopped_ || ready_ || !has_deadline_; });
        }
    }

    // Builds a batch from the pending rows and delivers it. Per design, a
    // window with only dropped rows still delivers a zero-row batch so the
    // loss is reported. Caller holds `lk`; the callback runs with it released.
    void Flush(std::unique_lock<std::mutex>& lk, BatchStatus::Reason reason) {
        has_deadline_ = false;
        if (!ready_) return;                         // schema not set — cannot build
        if (rows_.empty() && dropped_ == 0) return;  // truly idle — nothing to deliver

        std::vector<ArrowRow> rows = std::move(rows_);
        std::vector<Attachments> atts = std::move(atts_);
        int64_t dropped = dropped_;
        rows_.clear();
        atts_.clear();
        dropped_ = 0;
        auto schema = schema_;

        lk.unlock();
        // Isolate the user callback: a throw here would otherwise escape the
        // timer thread (→ std::terminate) or propagate out of ~PubSubArrow via
        // Stop()'s kClosing flush. Internal threads must not depend on user
        // code being noexcept.
        try {
            cb_(BuildBatch(schema, rows), std::move(atts), BatchStatus{reason, dropped});
        } catch (...) {
        }
        lk.lock();
    }

    static std::shared_ptr<arrow::RecordBatch> BuildBatch(
        const std::shared_ptr<arrow::Schema>& schema, const std::vector<ArrowRow>& rows) {
        const int num_fields = schema->num_fields();
        std::vector<std::shared_ptr<arrow::Array>> columns(static_cast<size_t>(num_fields));
        for (int c = 0; c < num_fields; ++c) {
            const auto& field_type = schema->field(c)->type();
            if (field_type->id() == arrow::Type::DICTIONARY) {
                // Dictionary columns arrive as plain value scalars on the wire;
                // re-fold them into a real DictionaryArray here.
                columns[static_cast<size_t>(c)] = BuildDictionaryColumn(field_type, rows, c);
                continue;
            }
            auto builder = arrow::MakeBuilder(field_type).ValueOrDie();
            (void)builder->Reserve(static_cast<int64_t>(rows.size()));
            for (const auto& row : rows) {
                // row[c] is always present: DecodeRow yields one scalar per
                // field (null fields as MakeNullScalar), and the scalar was
                // produced from a successful decode of this exact schema, so
                // AppendScalar is expected to succeed. AppendNull is a
                // best-effort fallback that keeps column lengths aligned.
                if (!builder->AppendScalar(*row[static_cast<size_t>(c)]).ok())
                    (void)builder->AppendNull();
            }
            columns[static_cast<size_t>(c)] = builder->Finish().ValueOrDie();
        }
        return arrow::RecordBatch::Make(schema, static_cast<int64_t>(rows.size()),
                                        std::move(columns));
    }

    // Re-folds a dictionary column: build the value array from the per-row
    // (plain value, or null) scalars, then dictionary-encode it to the field's
    // declared dictionary type.
    static std::shared_ptr<arrow::Array> BuildDictionaryColumn(
        const std::shared_ptr<arrow::DataType>& dict_type, const std::vector<ArrowRow>& rows,
        int col) {
        const auto& value_type = static_cast<const arrow::DictionaryType&>(*dict_type).value_type();
        auto builder = arrow::MakeBuilder(value_type).ValueOrDie();
        (void)builder->Reserve(static_cast<int64_t>(rows.size()));
        for (const auto& row : rows) {
            const auto& s = row[static_cast<size_t>(col)];
            if (!s || !s->is_valid || !builder->AppendScalar(*s).ok()) (void)builder->AppendNull();
        }
        auto value_array = builder->Finish().ValueOrDie();

        // arrow::compute::DictionaryEncode yields dictionary(int32, value_type);
        // cast to the field's declared type when its index type differs.
        auto encoded = arrow::compute::DictionaryEncode(arrow::Datum(value_array)).ValueOrDie();
        auto array = encoded.make_array();
        if (!array->type()->Equals(*dict_type)) {
            auto casted = arrow::compute::Cast(arrow::Datum(array), dict_type);
            if (!casted.ok())
                throw std::invalid_argument("PubSubArrow: cannot build dictionary column of type " +
                                            dict_type->ToString() + ": " +
                                            casted.status().ToString());
            array = casted.ValueOrDie().make_array();
        }
        return array;
    }

    RecordBatchCallback cb_;
    int64_t max_rows_;
    std::chrono::milliseconds timeout_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<ArrowRow> rows_;
    std::vector<Attachments> atts_;
    int64_t dropped_ = 0;
    std::chrono::steady_clock::time_point deadline_;
    bool has_deadline_ = false;
    bool ready_ = false;
    bool stopped_ = false;
    std::thread timer_;
};

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

PubSubArrow::PubSubArrow(std::shared_ptr<PubSub> provider)
    : driver_(std::make_unique<Driver>(std::move(provider))) {}

PubSubArrow::~PubSubArrow() {
    std::vector<std::shared_ptr<RecordBatchBatcher>> to_stop;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& entry : batchers_) to_stop.push_back(entry.second);
        batchers_.clear();
    }
    for (auto& b : to_stop) b->Stop();  // join timer threads + deliver closing flush

    // Tear down the driver here, while mu_/codecs_ are still alive. Member
    // destruction order would otherwise destroy driver_ last (it's the first
    // member), so subscriber lambdas unwound by ~Driver could touch mu_ and
    // codecs_ after they've already been destroyed.
    driver_.reset();
}

// -----------------------------------------------------------------------
// Topic management
// -----------------------------------------------------------------------

void PubSubArrow::CreateTopic(const std::vector<std::string>& segments,
                              std::shared_ptr<arrow::Schema> schema, std::any config) {
    OwnedSchema nano;
    if (schema) {
        nano = ExportToNano(*schema);
        std::string key = JoinSegments(segments);
        std::lock_guard lock(mu_);
        codecs_[key] = TopicCodec{schema, std::make_unique<Codec>(schema)};
    }
    driver_->CreateTopic(segments, std::move(nano), std::move(config));
}

// -----------------------------------------------------------------------
// Publish
// -----------------------------------------------------------------------

void PubSubArrow::Publish(const std::vector<std::string>& segments, const ArrowRow& row,
                          const Attachments& attachments) {
    std::string key = JoinSegments(segments);
    Codec* codec;
    {
        std::lock_guard lock(mu_);
        auto it = codecs_.find(key);
        if (it == codecs_.end())
            throw std::runtime_error("PubSubArrow::Publish: no codec for topic " + key);
        codec = it->second.codec.get();
    }

    auto encoded = codec->EncodeRow(row);
    driver_->Publish(
        segments,
        [data = std::move(encoded)](WriteBuffer& buf) { buf.Append(data.data(), data.size()); },
        attachments);
}

void PubSubArrow::PublishDirect(const std::vector<std::string>& segments,
                                PubSub::RowEncoder encoder, const Attachments& attachments) {
    driver_->Publish(segments, std::move(encoder), attachments);
}

// -----------------------------------------------------------------------
// Subscribe
// -----------------------------------------------------------------------

PubSubArrow::SubscribeResult PubSubArrow::Subscribe(const std::vector<std::string>& segments,
                                                    SubscribeCallback callback, std::any config) {
    std::string key = JoinSegments(segments);

    auto result = driver_->Subscribe(
        segments,
        [this, key, cb = std::move(callback)](const uint8_t* data, size_t len,
                                              SharedSchema /*schema*/, Attachments att) {
            Codec* codec;
            {
                std::lock_guard lock(mu_);
                auto it = codecs_.find(key);
                if (it == codecs_.end()) return;
                codec = it->second.codec.get();
            }
            auto row = codec->DecodeRow(data, len);
            cb(std::move(row), std::move(att));
        },
        std::move(config));

    // Convert the nanoarrow schema to Arrow C++.
    std::shared_ptr<arrow::Schema> arrow_schema;
    if (result.schema) {
        arrow_schema = ImportFromNano(result.schema.get());
        std::lock_guard lock(mu_);
        if (codecs_.find(key) == codecs_.end()) {
            codecs_[key] = TopicCodec{arrow_schema, std::make_unique<Codec>(arrow_schema)};
        }
    }

    return {result.subscription_id, std::move(arrow_schema)};
}

PubSubArrow::SubscribeResult PubSubArrow::Subscribe(const std::vector<std::string>& segments,
                                                    RecordBatchCallback callback,
                                                    BatchOptions options, std::any config) {
    std::string key = JoinSegments(segments);

    auto batcher = std::make_shared<RecordBatchBatcher>(std::move(callback), options.max_rows,
                                                        options.timeout);

    auto result = driver_->Subscribe(
        segments,
        [this, key, batcher](const uint8_t* data, size_t len, SharedSchema schema,
                             Attachments att) {
            // Lazy-init the codec from the per-message schema: in
            // subscriber-only mode (no prior CreateTopic) the codec isn't
            // registered yet and the provider can deliver before
            // driver_->Subscribe returns. If no codec can be built, count
            // the message as dropped so the loss is reported.
            Codec* codec = AcquireCodec(key, schema);
            if (!codec) {
                batcher->NoteDropped();
                return;
            }
            ArrowRow row;
            try {
                row = codec->DecodeRow(data, len);
            } catch (const std::exception&) {
                // Row failed to decode: count it lost and discard its
                // attachment too — the metadata identifying the attachment
                // lived in this row, so the system can't act on the blob.
                batcher->NoteDropped();
                return;
            }
            batcher->AddRow(std::move(row), std::move(att));
        },
        std::move(config));

    // From here on the driver may already be delivering to the captured
    // batcher. If post-setup throws, the lambda keeps the batcher alive via
    // its shared_ptr capture and the caller never sees an id to unsubscribe
    // — so we must roll back the driver subscription.
    try {
        // Resolve the Arrow schema (mirrors the ArrowRow overload) and hand
        // it to the batcher so it can build batches.
        std::shared_ptr<arrow::Schema> arrow_schema;
        if (result.schema) {
            arrow_schema = ImportFromNano(result.schema.get());
            std::lock_guard lock(mu_);
            if (codecs_.find(key) == codecs_.end())
                codecs_[key] = TopicCodec{arrow_schema, std::make_unique<Codec>(arrow_schema)};
        }
        batcher->SetSchema(arrow_schema);

        {
            std::lock_guard lock(mu_);
            batchers_[result.subscription_id] = std::move(batcher);
        }
        return {result.subscription_id, std::move(arrow_schema)};
    } catch (...) {
        batcher->Stop();
        driver_->Unsubscribe(result.subscription_id);
        throw;
    }
}

Codec* PubSubArrow::AcquireCodec(const std::string& key, const SharedSchema& schema) {
    std::lock_guard lock(mu_);
    auto it = codecs_.find(key);
    if (it != codecs_.end()) return it->second.codec.get();
    if (!schema) return nullptr;
    std::shared_ptr<arrow::Schema> arrow_schema;
    try {
        arrow_schema = ImportFromNano(schema.get());
    } catch (...) {
        return nullptr;
    }
    if (!arrow_schema) return nullptr;
    auto codec = std::make_unique<Codec>(arrow_schema);
    Codec* codec_ptr = codec.get();
    codecs_.emplace(key, TopicCodec{arrow_schema, std::move(codec)});
    return codec_ptr;
}

// -----------------------------------------------------------------------
// Subscription management / introspection
// -----------------------------------------------------------------------

void PubSubArrow::Unsubscribe(uint64_t subscription_id) {
    std::shared_ptr<RecordBatchBatcher> batcher;
    {
        std::lock_guard lock(mu_);
        auto it = batchers_.find(subscription_id);
        if (it != batchers_.end()) {
            batcher = std::move(it->second);
            batchers_.erase(it);
        }
    }
    // Flush the partial batch (reason kClosing) and join the timer before the
    // driver stops delivering. No-op for non-batched (ArrowRow) subscriptions.
    if (batcher) batcher->Stop();
    driver_->Unsubscribe(subscription_id);
}

std::vector<std::string> PubSubArrow::ListTopics() const { return driver_->ListTopics(); }

bool PubSubArrow::HasTopic(const std::vector<std::string>& segments) const {
    return driver_->HasTopic(segments);
}

// -----------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------

std::string PubSubArrow::JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) return {};
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // namespace fletcher
