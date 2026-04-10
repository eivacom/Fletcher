#include "ws_session.hpp"

#include <pubsub/schema_ipc.hpp>

#include <arrow/api.h>
#include <nlohmann/json.hpp>

#include <schema_evolution.hpp>

#include <cstring>
#include <stdexcept>

namespace fletcher {

using json = nlohmann::json;

// -----------------------------------------------------------------------
// Binary helpers (used for PUBLISH and MESSAGE frames only)
// -----------------------------------------------------------------------

namespace {

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

void AppendU64(std::vector<uint8_t>& buf, uint64_t v) {
    auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + 8);
}

void AppendBytes(std::vector<uint8_t>& buf,
                 const uint8_t* data, size_t len) {
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

json FieldToJson(const arrow::Field& field) {
    json j;
    j["name"] = field.name();
    j["wireType"] = static_cast<int>(ArrowTypeToWireTypeId(*field.type()));
    j["nullable"] = field.nullable();

    // Include field_number from Arrow metadata if present.
    if (auto meta = field.metadata()) {
        auto idx = meta->FindKey("field_number");
        if (idx >= 0)
            j["fieldNumber"] = std::stoi(meta->value(idx));
    }

    // Composite type children.
    const auto& type = *field.type();
    if (type.id() == arrow::Type::STRUCT) {
        json fields_arr = json::array();
        for (int i = 0; i < type.num_fields(); ++i)
            fields_arr.push_back(FieldToJson(*type.field(i)));
        j["fields"] = std::move(fields_arr);
    } else if (type.id() == arrow::Type::LIST) {
        j["element"] = FieldToJson(*type.field(0));
    } else if (type.id() == arrow::Type::LARGE_LIST) {
        j["element"] = FieldToJson(*type.field(0));
    } else if (type.id() == arrow::Type::FIXED_SIZE_LIST) {
        j["element"] = FieldToJson(*type.field(0));
        j["fixedSize"] = static_cast<const arrow::FixedSizeListType&>(type).list_size();
    } else if (type.id() == arrow::Type::MAP) {
        const auto& map_type = static_cast<const arrow::MapType&>(type);
        j["mapKey"] = FieldToJson(*map_type.key_field());
        j["mapValue"] = FieldToJson(*map_type.item_field());
    }

    return j;
}

json SchemaToJson(const arrow::Schema& schema) {
    json fields_arr = json::array();
    for (int i = 0; i < schema.num_fields(); ++i)
        fields_arr.push_back(FieldToJson(*schema.field(i)));
    return json{{"fields", std::move(fields_arr)}};
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
    driver_->CreateTopic(SplitTopic(topic), nullptr);
    SendText(json{{"type", "topic_created"}}.dump());
}

void WsSession::OnSubscribe(const std::string& topic) {
    auto segments = SplitTopic(topic);

    auto sub_id_ptr = std::make_shared<uint64_t>(0);
    std::weak_ptr<WsSession> weak = shared_from_this();

    auto result = driver_->Subscribe(segments,
        [weak, sub_id_ptr](const Envelope& env) {
            auto self = weak.lock();
            if (!self) return;

            auto binary = SerializeEnvelope(env);

            std::vector<uint8_t> frame;
            frame.reserve(8 + binary.size());
            AppendU64(frame, *sub_id_ptr);
            AppendBytes(frame, binary.data(), binary.size());

            net::post(
                self->ws_.get_executor(),
                [self, f = std::move(frame)]() mutable {
                    self->SendBinary(std::move(f));
                });
        });

    uint64_t sub_id = result.subscription_id;
    *sub_id_ptr = sub_id;
    subscriptions_[sub_id] = topic;

    json response = {
        {"type", "subscribed"},
        {"subId", std::to_string(sub_id)},
        {"topic", topic},
    };

    if (result.schema) {
        auto ipc_bytes = SerializeSchemaIpc(*result.schema);
        response["schemaIpc"] = Base64Encode(ipc_bytes.data(), ipc_bytes.size());
        response["schema"] = SchemaToJson(*result.schema);
    }

    SendText(response.dump());
}

void WsSession::OnUnsubscribe(uint64_t sub_id) {
    driver_->Unsubscribe(sub_id);
    subscriptions_.erase(sub_id);
    SendText(json{{"type", "unsubscribed"}}.dump());
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
