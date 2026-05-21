// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ws_session.hpp"

#include <cstring>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "publish_frame.hpp"
#include "schema_codec.hpp"

namespace fletcher {

using json = nlohmann::json;

// -----------------------------------------------------------------------
// Binary helpers (used for PUBLISH and MESSAGE frames only)
// -----------------------------------------------------------------------

namespace {

void AppendU64LE(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        v >>= 8;
    }
}

void AppendBytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(b >> 18) & 0x3F]);
        out.push_back(kAlphabet[(b >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kAlphabet[(b >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kAlphabet[b & 0x3F] : '=');
    }
    return out;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

WsSession::WsSession(net::ip::tcp::socket socket, std::shared_ptr<Driver> driver)
    : ws_(std::move(socket)), driver_(std::move(driver)) {}

WsSession::~WsSession() { UnsubscribeAll(); }

void WsSession::Run() {
    ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));

    auto self = shared_from_this();
    ws_.async_accept([self](beast::error_code ec) {
        if (ec) return;
        self->DoRead();
    });
}

// -----------------------------------------------------------------------
// Read loop
// -----------------------------------------------------------------------

void WsSession::DoRead() {
    auto self = shared_from_this();
    ws_.async_read(read_buf_, [self](beast::error_code ec, std::size_t n) { self->OnRead(ec, n); });
}

void WsSession::OnRead(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        UnsubscribeAll();
        return;
    }

    auto data = read_buf_.data();

    if (ws_.got_text()) {
        std::string text(static_cast<const char*>(data.data()), data.size());
        HandleTextFrame(text);
    } else {
        auto* ptr = static_cast<const uint8_t*>(data.data());
        HandleBinaryFrame(ptr, data.size());
    }

    read_buf_.consume(read_buf_.size());
    DoRead();
}

// -----------------------------------------------------------------------
// Frame dispatch
// -----------------------------------------------------------------------

void WsSession::HandleTextFrame(const std::string& text) {
    try {
        auto j = json::parse(text);
        auto action = j.at("action").get<std::string>();

        if (action == "create_topic") {
            OnCreateTopic(j);
        } else if (action == "subscribe") {
            OnSubscribe(j.at("topic").get<std::string>());
        } else if (action == "unsubscribe") {
            auto sub_id = std::stoull(j.at("subId").get<std::string>());
            OnUnsubscribe(sub_id);
        } else if (action == "list_topics") {
            OnListTopics();
        } else {
            SendError("unknown action: " + action);
        }
    } catch (const std::exception& e) {
        SendError(e.what());
    }
}

void WsSession::HandleBinaryFrame(const uint8_t* data, size_t len) {
    try {
        OnPublish(data, len);
    } catch (const std::exception& e) {
        SendError(e.what());
    }
}

// -----------------------------------------------------------------------
// Action handlers
// -----------------------------------------------------------------------

std::vector<std::string> WsSession::SplitTopic(const std::string& topic) {
    std::vector<std::string> segments;
    std::string seg;
    for (char c : topic) {
        if (c == '/') {
            if (!seg.empty()) segments.push_back(std::move(seg));
            seg.clear();
        } else {
            seg += c;
        }
    }
    if (!seg.empty()) segments.push_back(std::move(seg));
    return segments;
}

void WsSession::OnCreateTopic(const nlohmann::json& msg) {
    const auto topic = msg.at("topic").get<std::string>();
    OwnedSchema schema{};
    if (msg.contains("schema")) {
        // Publisher-announced schema. Gateway forwards it verbatim
        // to subscribers via the subscribed response — it does not
        // validate semantic meaning, only the wire-type bookkeeping
        // needed to round-trip JSON ↔ ArrowSchema.
        schema = gateway::BuildArrowSchemaFromJson(msg["schema"]);
    }
    driver_->CreateTopic(SplitTopic(topic), std::move(schema));
    SendText(json{{"type", "topic_created"}}.dump());
}

void WsSession::OnSubscribe(const std::string& topic) {
    auto segments = SplitTopic(topic);

    auto sub_id_ptr = std::make_shared<uint64_t>(0);
    std::weak_ptr<WsSession> weak = shared_from_this();

    auto result =
        driver_->Subscribe(segments, [weak, sub_id_ptr](const uint8_t* data, size_t len,
                                                        SharedSchema /*schema*/, Attachments att) {
            auto self = weak.lock();
            if (!self) return;

            try {
                Envelope env;
                env.row.assign(data, data + len);
                env.attachments = std::move(att);
                auto binary = SerializeEnvelope(env);

                std::vector<uint8_t> frame;
                frame.reserve(8 + binary.size());
                AppendU64LE(frame, *sub_id_ptr);
                AppendBytes(frame, binary.data(), binary.size());

                net::post(self->ws_.get_executor(), [self, f = std::move(frame)]() mutable {
                    self->SendBinary(std::move(f));
                });
            } catch (...) {
                // Serialization failure — drop this message
                // rather than crashing the subscriber callback thread.
            }
        });

    uint64_t sub_id = result.subscription_id;
    *sub_id_ptr = sub_id;
    subscriptions_[sub_id] = topic;

    // Routing always; schema only when a publisher announced one. The
    // gateway is schema-agnostic — it forwards whatever schema was
    // attached on create_topic but never generates one itself.
    json response = {
        {"type", "subscribed"},
        {"subId", std::to_string(sub_id)},
        {"topic", topic},
    };
    if (result.schema) {
        auto ipc_bytes = SerializeSchemaIpc(result.schema.get());
        response["schemaIpc"] = Base64Encode(ipc_bytes.data(), ipc_bytes.size());
        response["schema"] = gateway::ArrowSchemaToJson(result.schema.get());
    }
    SendText(response.dump());
}

void WsSession::OnUnsubscribe(uint64_t sub_id) {
    driver_->Unsubscribe(sub_id);
    subscriptions_.erase(sub_id);
    SendText(json{{"type", "unsubscribed"}}.dump());
}

void WsSession::OnPublish(const uint8_t* data, size_t len) {
    // Frame layout + validation handled by gateway::ParsePublishFrame —
    // see publish_frame.hpp for the wire format and the edge cases
    // that parser rejects.
    auto parts = gateway::ParsePublishFrame(data, len);
    auto segments = SplitTopic(parts.topic);
    auto envelope = DeserializeEnvelope(parts.envelope_data, parts.envelope_size);

    driver_->Publish(
        segments,
        [row_data = std::move(envelope.row)](WriteBuffer& buf) {
            buf.Append(row_data.data(), row_data.size());
        },
        envelope.attachments);

    SendText(json{{"type", "published"}}.dump());
}

void WsSession::OnListTopics() {
    auto topics = driver_->ListTopics();
    SendText(json{
        {"type", "topics_list"},
        {"topics", topics},
    }
                 .dump());
}

// -----------------------------------------------------------------------
// Write queue
// -----------------------------------------------------------------------

void WsSession::SendText(std::string text) {
    OutgoingFrame frame;
    frame.data.assign(text.begin(), text.end());
    frame.is_text = true;
    write_queue_.push_back(std::move(frame));
    if (!writing_) DoWrite();
}

void WsSession::SendBinary(std::vector<uint8_t> data) {
    OutgoingFrame frame;
    frame.data = std::move(data);
    frame.is_text = false;
    write_queue_.push_back(std::move(frame));
    if (!writing_) DoWrite();
}

void WsSession::DoWrite() {
    if (write_queue_.empty()) {
        writing_ = false;
        return;
    }

    writing_ = true;
    ws_.text(write_queue_.front().is_text);

    auto self = shared_from_this();
    ws_.async_write(net::buffer(write_queue_.front().data),
                    [self](beast::error_code ec, std::size_t) {
                        if (ec) return;
                        self->write_queue_.pop_front();
                        self->DoWrite();
                    });
}

void WsSession::SendError(const std::string& msg) {
    SendText(json{{"type", "error"}, {"message", msg}}.dump());
}

// -----------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------

void WsSession::UnsubscribeAll() {
    for (auto& [sub_id, _] : subscriptions_) {
        try {
            driver_->Unsubscribe(sub_id);
        } catch (...) {
        }
    }
    subscriptions_.clear();
}

}  // namespace fletcher
