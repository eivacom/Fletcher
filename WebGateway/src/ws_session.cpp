#include "ws_session.hpp"

#include <pubsub/schema_ipc.hpp>
#include <pubsub/envelope.hpp>
#include <pubsub/owned_schema.hpp>
#include <pubsub/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>

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

// Map nanoarrow ArrowType to the Fletcher wire-type integer expected by
// the WebClient.  These values must match WireTypeId in wire-types.ts.
int NanoarrowTypeToWireType(enum ArrowType type) {
    switch (type) {
        case NANOARROW_TYPE_BOOL:               return 0x01;
        case NANOARROW_TYPE_INT8:               return 0x02;
        case NANOARROW_TYPE_INT16:              return 0x03;
        case NANOARROW_TYPE_INT32:              return 0x04;
        case NANOARROW_TYPE_INT64:              return 0x05;
        case NANOARROW_TYPE_UINT8:              return 0x06;
        case NANOARROW_TYPE_UINT16:             return 0x07;
        case NANOARROW_TYPE_UINT32:             return 0x08;
        case NANOARROW_TYPE_UINT64:             return 0x09;
        case NANOARROW_TYPE_FLOAT:              return 0x0A;
        case NANOARROW_TYPE_DOUBLE:             return 0x0B;
        case NANOARROW_TYPE_STRING:             return 0x0C;
        case NANOARROW_TYPE_BINARY:             return 0x0D;
        case NANOARROW_TYPE_DATE32:             return 0x0E;
        case NANOARROW_TYPE_DATE64:             return 0x0F;
        case NANOARROW_TYPE_TIMESTAMP:          return 0x10;
        case NANOARROW_TYPE_TIME32:             return 0x11;
        case NANOARROW_TYPE_TIME64:             return 0x12;
        case NANOARROW_TYPE_DURATION:           return 0x13;
        case NANOARROW_TYPE_FIXED_SIZE_BINARY:  return 0x14;
        case NANOARROW_TYPE_HALF_FLOAT:         return 0x15;
        case NANOARROW_TYPE_DECIMAL128:         return 0x16;
        case NANOARROW_TYPE_DECIMAL256:         return 0x17;
        case NANOARROW_TYPE_LARGE_STRING:       return 0x18;
        case NANOARROW_TYPE_LARGE_BINARY:       return 0x19;
        case NANOARROW_TYPE_STRING_VIEW:        return 0x1A;
        case NANOARROW_TYPE_BINARY_VIEW:        return 0x1B;
        case NANOARROW_TYPE_INTERVAL_MONTHS:    return 0x1C;
        case NANOARROW_TYPE_INTERVAL_DAY_TIME:  return 0x1D;
        case NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO: return 0x1E;
        case NANOARROW_TYPE_STRUCT:             return 0x20;
        case NANOARROW_TYPE_LIST:               return 0x21;
        case NANOARROW_TYPE_LARGE_LIST:         return 0x22;
        case NANOARROW_TYPE_FIXED_SIZE_LIST:    return 0x23;
        case NANOARROW_TYPE_MAP:                return 0x24;
        case NANOARROW_TYPE_SPARSE_UNION:       return 0x25;
        case NANOARROW_TYPE_DENSE_UNION:        return 0x26;
        default:                                return 0x00;
    }
}

json FieldToJson(const ArrowSchema* field) {
    json j;
    j["name"] = field->name ? field->name : "";
    j["nullable"] = (field->flags & ARROW_FLAG_NULLABLE) != 0;

    // Resolve the type via ArrowSchemaView.
    struct ArrowSchemaView view;
    ArrowSchemaViewInit(&view, field, nullptr);
    j["wireType"] = NanoarrowTypeToWireType(view.type);

    // Include field_number from metadata if present.
    if (field->metadata) {
        struct ArrowStringView value;
        if (ArrowMetadataGetValue(field->metadata,
                ArrowCharView("field_number"), &value) == NANOARROW_OK) {
            j["fieldNumber"] = std::stoi(std::string(value.data, value.size_bytes));
        }
    }

    // Composite type children.
    if (view.type == NANOARROW_TYPE_STRUCT) {
        json fields_arr = json::array();
        for (int64_t i = 0; i < field->n_children; ++i)
            fields_arr.push_back(FieldToJson(field->children[i]));
        j["fields"] = std::move(fields_arr);
    } else if (view.type == NANOARROW_TYPE_LIST
            || view.type == NANOARROW_TYPE_LARGE_LIST) {
        j["element"] = FieldToJson(field->children[0]);
    } else if (view.type == NANOARROW_TYPE_FIXED_SIZE_LIST) {
        j["element"] = FieldToJson(field->children[0]);
        j["fixedSize"] = view.fixed_size;
    } else if (view.type == NANOARROW_TYPE_MAP) {
        // Map schema: children[0] is the "entries" struct with two children
        // (key, value).
        const ArrowSchema* entries = field->children[0];
        j["mapKey"] = FieldToJson(entries->children[0]);
        j["mapValue"] = FieldToJson(entries->children[1]);
    }

    return j;
}

json SchemaToJson(const ArrowSchema* schema) {
    json fields_arr = json::array();
    for (int64_t i = 0; i < schema->n_children; ++i)
        fields_arr.push_back(FieldToJson(schema->children[i]));
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
    driver_->CreateTopic(SplitTopic(topic), OwnedSchema{});
    SendText(json{{"type", "topic_created"}}.dump());
}

void WsSession::OnSubscribe(const std::string& topic) {
    auto segments = SplitTopic(topic);

    auto sub_id_ptr = std::make_shared<uint64_t>(0);
    std::weak_ptr<WsSession> weak = shared_from_this();

    auto result = driver_->Subscribe(segments,
        [weak, sub_id_ptr](const uint8_t* data, size_t len,
                           const ArrowSchema* /*schema*/, Attachments att) {
            auto self = weak.lock();
            if (!self) return;

            try {
                // Build envelope directly from raw row bytes.
                Envelope env;
                env.row.assign(data, data + len);
                env.attachments = std::move(att);
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
                // Serialization failure — drop this message
                // rather than crashing the subscriber callback thread.
            }
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
        auto ipc_bytes = SerializeSchemaIpc(result.schema.get());
        response["schemaIpc"] = Base64Encode(ipc_bytes.data(), ipc_bytes.size());
        response["schema"] = SchemaToJson(result.schema.get());
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
    uint16_t topic_len = ReadU16LE(data);
    if (2 + topic_len > len) throw std::invalid_argument("publish: truncated topic");

    std::string topic(reinterpret_cast<const char*>(data + 2), topic_len);
    size_t env_offset = 2 + topic_len;

    auto segments = SplitTopic(topic);
    auto envelope = DeserializeEnvelope(data + env_offset, len - env_offset);

    // Pass raw row bytes through to the driver.
    driver_->Publish(segments,
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
