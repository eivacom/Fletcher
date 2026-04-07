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
// Binary WebSocket protocol
//
// All frames are binary.  Each message starts with a 1-byte action/type tag.
//
// Client → Server:
//   CREATE_TOPIC  (0x01): [TAG:1] [TOPIC_LEN:2] [TOPIC:N]
//   SUBSCRIBE     (0x02): [TAG:1] [TOPIC_LEN:2] [TOPIC:N]
//   UNSUBSCRIBE   (0x03): [TAG:1] [SUB_ID:8]
//   PUBLISH       (0x04): [TAG:1] [TOPIC_LEN:2] [TOPIC:N] [ENVELOPE:rest]
//   LIST_TOPICS   (0x05): [TAG:1]
//
// Server → Client:
//   TOPIC_CREATED (0x01): [TAG:1]
//   SUBSCRIBED    (0x02): [TAG:1] [SUB_ID:8] [TOPIC_LEN:2] [TOPIC:N]
//   UNSUBSCRIBED  (0x03): [TAG:1]
//   PUBLISHED     (0x04): [TAG:1]
//   TOPICS        (0x05): [TAG:1] [COUNT:2] {[TOPIC_LEN:2] [TOPIC:N]}*
//   MESSAGE       (0x06): [TAG:1] [SUB_ID:8] [ENVELOPE:rest]
//   ERROR         (0xFF): [TAG:1] [MSG_LEN:2] [MSG:N]
//
// All multi-byte integers are little-endian.
// ---------------------------------------------------------------------------

enum class ActionTag : uint8_t {
    kCreateTopic = 0x01,
    kSubscribe   = 0x02,
    kUnsubscribe = 0x03,
    kPublish     = 0x04,
    kListTopics  = 0x05,
};

enum class ResponseTag : uint8_t {
    kTopicCreated = 0x01,
    kSubscribed   = 0x02,
    kUnsubscribed = 0x03,
    kPublished    = 0x04,
    kTopics       = 0x05,
    kMessage      = 0x06,
    kError        = 0xFF,
};

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

    // Dispatch the binary frame.
    void HandleFrame(const uint8_t* data, size_t len);

    // Action handlers.
    void OnCreateTopic(const uint8_t* data, size_t len);
    void OnSubscribe(const uint8_t* data, size_t len);
    void OnUnsubscribe(const uint8_t* data, size_t len);
    void OnPublish(const uint8_t* data, size_t len);
    void OnListTopics();

    // Send helpers — always called on the strand.
    void Send(std::vector<uint8_t> frame);
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
    std::deque<std::vector<uint8_t>> write_queue_;
    bool                             writing_ = false;

    // Active Driver subscriptions owned by this session.
    std::unordered_map<uint64_t, std::string> subscriptions_;  // sub_id → topic
};

}  // namespace fletcher

#endif  // FLETCHER_WEB_GATEWAY_WS_SESSION_HPP_
