#include "ws_session.hpp"

#include <pubsub/envelope.hpp>

#include <nlohmann/json.hpp>

#include <cstring>
#include <stdexcept>

namespace fletcher {

using json = nlohmann::json;

// -----------------------------------------------------------------------
// Binary helpers (used for PUBLISH and MESSAGE frames only)
// -----------------------------------------------------------------------

namespace {

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

void AppendU64LE(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        v >>= 8;
    }
}

void AppendBytes(std::vector<uint8_t>& buf,
                 const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

WsSession::WsSession(net::ip::tcp::socket socket,
                      std::shared_ptr<Driver> driver)
    : ws_(std::move(socket))
    , driver_(std::move(driver)) {}

WsSession::~WsSession() {
    UnsubscribeAll();
}

void WsSession::Run() {
    ws_.set_option(ws::stream_base::timeout::suggested(
        beast::role_type::server));

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
    ws_.async_read(read_buf_, [self](beast::error_code ec, std::size_t n) {
        self->OnRead(ec, n);
    });
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
            OnCreateTopic(j.at("topic").get<std::string>());
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

void WsSession::OnCreateTopic(const std::string& topic) {
    // TODO: accept schema from the client so rows can be encoded/decoded.
    // Without a schema the topic can relay pre-encoded envelopes but not
    // re-encode ArrowRow data (EncodeRow/DecodeRow will throw).
    driver_->CreateTopic(SplitTopic(topic), nullptr);
    SendText(json{{"type", "topic_created"}}.dump());
}

void WsSession::OnSubscribe(const std::string& topic) {
    auto segments = SplitTopic(topic);

    auto sub_id_ptr = std::make_shared<uint64_t>(0);
    std::weak_ptr<WsSession> weak = shared_from_this();

    auto sub_id = driver_->Subscribe(segments,
        [weak, sub_id_ptr, segments](ArrowRow row, Attachments att) {
            auto self = weak.lock();
            if (!self) return;

            try {
                // Re-encode for the WebSocket wire format.
                auto encoded_row = self->driver_->EncodeRow(segments, row);
                Envelope env{std::move(encoded_row), std::move(att)};
                auto binary = SerializeEnvelope(env);

                std::vector<uint8_t> frame;
                frame.reserve(8 + binary.size());
                AppendU64LE(frame, *sub_id_ptr);
                AppendBytes(frame, binary.data(), binary.size());

                net::post(
                    self->ws_.get_executor(),
                    [self, f = std::move(frame)]() mutable {
                        self->SendBinary(std::move(f));
                    });
            } catch (...) {
                // Encoding/serialization failure — drop this message
                // rather than crashing the subscriber callback thread.
            }
        });

    *sub_id_ptr = sub_id;
    subscriptions_[sub_id] = topic;

    SendText(json{
        {"type", "subscribed"},
        {"subId", std::to_string(sub_id)},
        {"topic", topic},
    }.dump());
}

void WsSession::OnUnsubscribe(uint64_t sub_id) {
    driver_->Unsubscribe(sub_id);
    subscriptions_.erase(sub_id);
    SendText(json{{"type", "unsubscribed"}}.dump());
}

void WsSession::OnPublish(const uint8_t* data, size_t len) {
    // [TOPIC_LEN:2] [TOPIC:N] [ENVELOPE:rest]
    if (len < 2) throw std::invalid_argument("publish: frame too short");
    uint16_t topic_len = ReadU16LE(data);
    if (2 + topic_len > len) throw std::invalid_argument("publish: truncated topic");

    std::string topic(reinterpret_cast<const char*>(data + 2), topic_len);
    size_t env_offset = 2 + topic_len;

    auto segments = SplitTopic(topic);
    auto envelope = DeserializeEnvelope(data + env_offset, len - env_offset);
    auto row = driver_->DecodeRow(segments, envelope.row);
    driver_->Publish(segments, row, envelope.attachments);

    SendText(json{{"type", "published"}}.dump());
}

void WsSession::OnListTopics() {
    auto topics = driver_->ListTopics();
    SendText(json{
        {"type", "topics_list"},
        {"topics", topics},
    }.dump());
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
    ws_.async_write(
        net::buffer(write_queue_.front().data),
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
        try { driver_->Unsubscribe(sub_id); } catch (...) {}
    }
    subscriptions_.clear();
}

}  // namespace fletcher
