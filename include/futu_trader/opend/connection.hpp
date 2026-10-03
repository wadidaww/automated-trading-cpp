#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/opend/framing.hpp"

namespace futu_trader::opend {

struct ConnectionConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{11111};
  std::string clientId{"futu_trader"};
  int clientVer{100};
  bool recvNotify{true};
  std::chrono::milliseconds connectTimeout{3000};
  std::chrono::milliseconds requestTimeout{5000};
  std::size_t maxBody{kDefaultMaxBody};
  /** Overrides the server-advised keep-alive interval. Intended for tests. */
  std::optional<std::chrono::milliseconds> keepAliveOverride;
  /**
   * OpenD speaks plaintext here, so the host must be loopback unless explicitly overridden;
   * otherwise a mistyped or hostile config would send orders (and the unlock hash) over the
   * network.
   */
  bool allowNonLoopback{false};
};

struct SessionInfo {
  std::uint64_t connId{0};
  std::int32_t serverVer{0};
  std::int32_t keepAliveSeconds{0};
  std::uint64_t loginUserId{0};
};

/**
 * One TCP connection to OpenD: framing, request/response matching by serial, pushes, keep-alive.
 *
 * Threads: a reader thread (decodes frames, completes requests, dispatches pushes) and a
 * keep-alive thread. Callbacks run on the reader thread and must not block or call close().
 * The connection can be reused: after a disconnect call close(), then connect() again.
 * Encrypted OpenD sessions (RSA/AES) are not supported and fail with kUnsupported/kServer.
 */
class OpenDConnection {
 public:
  using PushHandler = std::function<void(const Frame&)>;
  using DisconnectHandler = std::function<void(const Error&)>;

  explicit OpenDConnection(ConnectionConfig config);
  ~OpenDConnection();
  OpenDConnection(const OpenDConnection&) = delete;
  OpenDConnection& operator=(const OpenDConnection&) = delete;

  void setPushHandler(PushHandler handler) { onPush_ = std::move(handler); }
  void setDisconnectHandler(DisconnectHandler handler) { onDisconnect_ = std::move(handler); }

  /** Opens TCP, performs InitConnect and starts the reader and keep-alive threads. */
  Result<SessionInfo> connect();
  /** Idempotent; joins the worker threads. Never call from a callback. */
  void close();
  bool isConnected() const { return connected_.load(); }

  /** Sends a serialized protobuf request body and waits for the matching response body. */
  Result<std::vector<std::uint8_t>> request(std::uint32_t protoId, const std::string& body);
  Result<std::vector<std::uint8_t>> request(std::uint32_t protoId, const std::string& body,
                                            std::chrono::milliseconds timeout);

  const SessionInfo& session() const { return session_; }
  /** Replies whose serial matched a request but whose proto id did not (treated as pushes). */
  std::size_t mismatchedReplies() const { return mismatched_.load(); }

 private:
  struct Impl;
  struct Pending {
    std::promise<Result<std::vector<std::uint8_t>>> promise;
    std::uint32_t protoId{0};  // a reply must carry the proto id of the request it answers
  };

  void readerLoop();
  void keepAliveLoop();
  void fail(const Error& error);
  Result<std::vector<std::uint8_t>> requestInternal(std::uint32_t protoId, const std::string& body,
                                                    std::chrono::milliseconds timeout);

  ConnectionConfig config_;
  std::unique_ptr<Impl> impl_;
  SessionInfo session_;

  PushHandler onPush_;
  DisconnectHandler onDisconnect_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint32_t> nextSerial_{1};
  std::atomic<std::size_t> mismatched_{0};

  std::mutex pendingMu_;
  std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_;

  std::mutex kaMu_;
  std::condition_variable kaCv_;

  std::thread reader_;
  std::thread keepAlive_;
};

}  // namespace futu_trader::opend
