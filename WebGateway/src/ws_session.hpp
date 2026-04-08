#ifndef FLETCHER_WEB_GATEWAY_WS_SESSION_HPP_
#define FLETCHER_WEB_GATEWAY_WS_SESSION_HPP_

#include <pubsub/driver.hpp>
#include <pubsub/envelope.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fletcher {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace ws    = beast::websocket;

// ---------------------------------------------------------------------------
// Split text/binary WebSocket protocol
//
// Text frames (JSON) — control messages:
//
// Client → Server:
//   { "action": "create_topic", "topic": "<topic>" }
//   { "action": "subscribe",    "topic": "<topic>" }
//   { "action": "unsubscribe",  "subId": "<uint64>" }
//   { "action": "list_topics" }
//
// Server → Client:
//   { "type": "topic_created" }
//   { "type": "subscribed",   "subId": "<uint64>", "topic": "<topic>" }
//   { "type": "unsubscribed" }
//   { "type": "published" }
//   { "type": "topics_list",  "topics": ["<topic>", ...] }
//   { "type": "error",        "message": "<text>" }
//
// Binary frames — data messages:
//
// Client → Server (PUBLISH):
//   [TOPIC_LEN:2 LE] [TOPIC:N] [ENVELOPE:rest]
//
// Server → Client (MESSAGE):
//   [SUB_ID:8 LE] [ENVELOPE:rest]
//
// Sub IDs are stringified in JSON to avoid JS Number precision loss.
// All multi-byte integers in binary frames are little-endian.
// ---------------------------------------------------------------------------

/// A single WebSocket connection.
///
/// Each session maintains its own set of Driver subscriptions and
/// unsubscribes on disconnect.  Writes are serialised through an
/// Asio strand so that provider callbacks (arriving on arbitrary
/// threads) never race with control-frame responses.
class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
    WsSession(net::ip::tcp::socket socket,
              std::shared_ptr<Driver> driver);
    ~WsSession();

    /// Start the WebSocket handshake and begin reading frames.
    void Run();

 private:
    // Async read loop.
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);

    // Frame dispatch.
    void HandleTextFrame(const std::string& text);
    void HandleBinaryFrame(const uint8_t* data, size_t len);

    // Action handlers.
    void OnCreateTopic(const std::string& topic);
    void OnSubscribe(const std::string& topic);
    void OnUnsubscribe(uint64_t sub_id);
    void OnPublish(const uint8_t* data, size_t len);
    void OnListTopics();

    // Send helpers — always called on the strand.
    struct OutgoingFrame {
        std::vector<uint8_t> data;
        bool is_text;
    };
    void SendText(std::string json);
    void SendBinary(std::vector<uint8_t> frame);
    void DoWrite();
    void SendError(const std::string& msg);

    // Cleanup on disconnect.
    void UnsubscribeAll();

    // Utility
    static std::vector<std::string> SplitTopic(const std::string& topic);

    ws::stream<beast::tcp_stream> ws_;
    std::shared_ptr<Driver>       driver_;
    beast::flat_buffer             read_buf_;

    // Outgoing write queue (strand-serialised).
    std::deque<OutgoingFrame> write_queue_;
    bool                      writing_ = false;

    // Active Driver subscriptions owned by this session.
    std::unordered_map<uint64_t, std::string> subscriptions_;  // sub_id → topic
};

}  // namespace fletcher

#endif  // FLETCHER_WEB_GATEWAY_WS_SESSION_HPP_
