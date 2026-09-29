#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "futu_trader/opend/connection.hpp"
#include "futu_trader/opend/types.hpp"

namespace futu_trader::opend {

struct ClientConfig {
  ConnectionConfig connection;
  bool autoReconnect{true};
  std::chrono::milliseconds reconnectBase{200};
  std::chrono::milliseconds reconnectMax{5000};
  std::uint32_t jitterSeed{12345};  // deterministic jitter so tests are reproducible
};

/**
 * Read-only OpenD client for market data and account data (no order placement yet; that is P2).
 * Owns the connection, reconnects with jittered exponential backoff and re-subscribes.
 */
class OpenDClient {
 public:
  using ConnectionStateHandler = std::function<void(bool connected)>;

  explicit OpenDClient(ClientConfig config);
  ~OpenDClient();
  OpenDClient(const OpenDClient&) = delete;
  OpenDClient& operator=(const OpenDClient&) = delete;

  /** Called on every connect/disconnect transition; the engine should halt trading on false. */
  void setConnectionStateHandler(ConnectionStateHandler handler) { onState_ = std::move(handler); }
  void setPushHandler(OpenDConnection::PushHandler handler) {
    conn_.setPushHandler(std::move(handler));
  }

  Result<SessionInfo> connect();
  void close();
  bool isConnected() const { return conn_.isConnected(); }

  /** Subscribes and remembers the subscription so it is restored after a reconnect. */
  Result<bool> subscribe(const std::vector<SecurityRef>& securities,
                         const std::vector<SubType>& subTypes);
  Result<std::vector<BasicQuote>> getBasicQuotes(const std::vector<SecurityRef>& securities);
  /** Pages through Qot_RequestHistoryKL (nextReqKey) up to maxPages. Rehab type: forward-adjusted.
   */
  Result<std::vector<Bar>> requestHistoryKl(const SecurityRef& security, KlType klType,
                                            const std::string& begin, const std::string& end,
                                            int maxPages = 20);

  Result<std::vector<TrdAccount>> getAccList();
  Result<FundsInfo> getFunds(const AccountHeader& header);
  Result<std::vector<PositionInfo>> getPositions(const AccountHeader& header);

  std::size_t reconnectCount() const { return reconnects_.load(); }

 private:
  struct Subscription {
    std::vector<SecurityRef> securities;
    std::vector<SubType> subTypes;
  };

  Result<bool> sendSubscribe(const Subscription& sub);
  void supervisorLoop();
  void onDisconnect(const Error& error);

  ClientConfig config_;
  OpenDConnection conn_;
  ConnectionStateHandler onState_;

  std::mutex subMu_;
  std::vector<Subscription> subs_;

  std::mutex supMu_;
  std::condition_variable supCv_;
  bool needReconnect_{false};
  bool shutdown_{false};
  std::thread supervisor_;
  std::mt19937 rng_;
  std::atomic<std::size_t> reconnects_{0};
};

}  // namespace futu_trader::opend
