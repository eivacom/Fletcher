#include "ws_session.hpp"

#include <cstring>
#include <stdexcept>

namespace fletcher {

// -----------------------------------------------------------------------
// Binary helpers
// -----------------------------------------------------------------------

namespace {

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint64_t ReadU64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

void AppendU8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void AppendU16(std::vector<uint8_t>& buf, uint16_t v) {
    auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + 2);
}

void AppendU64(std::vector<uint8_t>& buf, uint64_t v) {
    auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + 8);
}

void AppendString(std::vector<uint8_t>& buf, const std::string& s) {
    AppendU16(buf, static_cast<uint16_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
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

    // Force binary mode — all frames are binary.
    ws_.binary(true);

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
    auto* ptr = static_cast<const uint8_t*>(data.data());
    auto  len = data.size();

    HandleFrame(ptr, len);
    read_buf_.consume(read_buf_.size());
    DoRead();
}

// -----------------------------------------------------------------------
// Frame dispatch
// -----------------------------------------------------------------------

void WsSession::HandleFrame(const uint8_t* data, size_t len) {
    if (len < 1) {
        SendError("empty frame");
        return;
    }

    auto tag = static_cast<ActionTag>(data[0]);
    const uint8_t* payload = data + 1;
    size_t payload_len = len - 1;

    try {
        switch (tag) {
            case ActionTag::kCreateTopic: OnCreateTopic(payload, payload_len); break;
            case ActionTag::kSubscribe:   OnSubscribe(payload, payload_len);   break;
            case ActionTag::kUnsubscribe: OnUnsubscribe(payload, payload_len); break;
            case ActionTag::kPublish:     OnPublish(payload, payload_len);     break;
            case ActionTag::kListTopics:  OnListTopics();                      break;
            default:
                SendError("unknown action tag");
                break;
        }
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

void WsSession::OnCreateTopic(const uint8_t* data, size_t len) {
    // [TOPIC_LEN:2] [TOPIC:N]
    if (len < 2) throw std::invalid_argument("create_topic: frame too short");
    uint16_t topic_len = ReadU16(data);
    if (2 + topic_len > len) throw std::invalid_argument("create_topic: truncated");
    std::string topic(reinterpret_cast<const char*>(data + 2), topic_len);

    driver_->CreateTopic(SplitTopic(topic), nullptr);

    std::vector<uint8_t> resp;
    AppendU8(resp, static_cast<uint8_t>(ResponseTag::kTopicCreated));
    Send(std::move(resp));
}

void WsSession::OnSubscribe(const uint8_t* data, size_t len) {
    // [TOPIC_LEN:2] [TOPIC:N]
    if (len < 2) throw std::invalid_argument("subscribe: frame too short");
    uint16_t topic_len = ReadU16(data);
    if (2 + topic_len > len) throw std::invalid_argument("subscribe: truncated");
    std::string topic(reinterpret_cast<const char*>(data + 2), topic_len);

    auto segments = SplitTopic(topic);

    // The Driver callback may fire on any thread.  Post writes to the
    // WebSocket through the stream's executor to avoid races.
    // We share the sub_id via a shared_ptr so the callback can reference
    // it even though Subscribe returns after the lambda is created.
    auto sub_id_ptr = std::make_shared<uint64_t>(0);
    std::weak_ptr<WsSession> weak = shared_from_this();

    auto sub_id = driver_->Subscribe(segments,
        [weak, sub_id_ptr](const Envelope& env) {
            auto self = weak.lock();
            if (!self) return;

            auto binary = SerializeEnvelope(env);

            std::vector<uint8_t> frame;
            frame.reserve(1 + 8 + binary.size());
            AppendU8(frame, static_cast<uint8_t>(ResponseTag::kMessage));
            AppendU64(frame, *sub_id_ptr);
            AppendBytes(frame, binary.data(), binary.size());

            net::post(
                self->ws_.get_executor(),
                [self, f = std::move(frame)]() mutable {
                    self->Send(std::move(f));
                });
        });

    *sub_id_ptr = sub_id;

    subscriptions_[sub_id] = topic;

    // Response: [TAG:1] [SUB_ID:8] [TOPIC_LEN:2] [TOPIC:N]
    std::vector<uint8_t> resp;
    resp.reserve(1 + 8 + 2 + topic.size());
    AppendU8(resp, static_cast<uint8_t>(ResponseTag::kSubscribed));
    AppendU64(resp, sub_id);
    AppendString(resp, topic);
    Send(std::move(resp));
}

void WsSession::OnUnsubscribe(const uint8_t* data, size_t len) {
    // [SUB_ID:8]
    if (len < 8) throw std::invalid_argument("unsubscribe: frame too short");
    uint64_t sub_id = ReadU64(data);

    driver_->Unsubscribe(sub_id);
    subscriptions_.erase(sub_id);

    std::vector<uint8_t> resp;
    AppendU8(resp, static_cast<uint8_t>(ResponseTag::kUnsubscribed));
    Send(std::move(resp));
}

void WsSession::OnPublish(const uint8_t* data, size_t len) {
    // [TOPIC_LEN:2] [TOPIC:N] [ENVELOPE:rest]
    if (len < 2) throw std::invalid_argument("publish: frame too short");
    uint16_t topic_len = ReadU16(data);
    if (2 + topic_len > len) throw std::invalid_argument("publish: truncated topic");

    std::string topic(reinterpret_cast<const char*>(data + 2), topic_len);
    size_t env_offset = 2 + topic_len;

    auto envelope = DeserializeEnvelope(data + env_offset, len - env_offset);
    driver_->Publish(SplitTopic(topic), envelope);

    std::vector<uint8_t> resp;
    AppendU8(resp, static_cast<uint8_t>(ResponseTag::kPublished));
    Send(std::move(resp));
}

void WsSession::OnListTopics() {
    auto topics = driver_->ListTopics();

    std::vector<uint8_t> resp;
    AppendU8(resp, static_cast<uint8_t>(ResponseTag::kTopics));
    AppendU16(resp, static_cast<uint16_t>(topics.size()));
    for (auto& t : topics)
        AppendString(resp, t);
    Send(std::move(resp));
}

// -----------------------------------------------------------------------
// Write queue
// -----------------------------------------------------------------------

void WsSession::Send(std::vector<uint8_t> frame) {
    write_queue_.push_back(std::move(frame));
    if (!writing_) DoWrite();
}

void WsSession::DoWrite() {
    if (write_queue_.empty()) {
        writing_ = false;
        return;
    }

    writing_ = true;
    auto self = shared_from_this();
    ws_.async_write(
        net::buffer(write_queue_.front()),
        [self](beast::error_code ec, std::size_t) {
            if (ec) return;
            self->write_queue_.pop_front();
            self->DoWrite();
        });
}

void WsSession::SendError(const std::string& msg) {
    std::vector<uint8_t> frame;
    frame.reserve(1 + 2 + msg.size());
    AppendU8(frame, static_cast<uint8_t>(ResponseTag::kError));
    AppendString(frame, msg);
    Send(std::move(frame));
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
