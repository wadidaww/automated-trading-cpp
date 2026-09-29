#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace futu_trader::mock {

/** One-shot faults, consumed by the next matching response. */
struct Faults {
  int dropResponses{0};         // swallow this many responses (client sees a timeout)
  int corruptChecksum{0};       // send this many responses with a bad SHA-1
  int oversizeLengthFrames{0};  // send a header claiming a huge body
  int disconnectMidFrame{0};    // send half of a response, then close the socket
  int splitResponses{0};        // deliver this many responses in 1-byte TCP writes
  int coalesceWithPush{0};      // write response + a push in a single TCP write
  bool ignoreKeepAlive{false};  // never answer KeepAlive (persistent)
  std::chrono::milliseconds responseDelay{0};
};

struct MockKline {
  std::string time;
  double open{0};
  double high{0};
  double low{0};
  double close{0};
  std::int64_t volume{0};
};

struct MockPosition {
  std::string code;
  double qty{0};
  double canSell{0};
  double price{0};
  double cost{0};
};

/**
 * In-process fake OpenD that speaks the real wire framing. Test tool only: it validates nothing
 * about trading semantics and is not a substitute for a manual check against a real OpenD.
 */
class MockOpenD {
 public:
  MockOpenD();
  ~MockOpenD();
  MockOpenD(const MockOpenD&) = delete;
  MockOpenD& operator=(const MockOpenD&) = delete;

  std::uint16_t start();  // binds an ephemeral loopback port and returns it
  void stop();

  // Scenario data (set before or while running).
  void setFaults(const Faults& faults);
  void setKlines(std::vector<MockKline> klines, std::size_t pageSize);
  void setPositions(std::vector<MockPosition> positions);
  void setQuote(const std::string& code, double price);
  void setFundsCash(double cash);
  void rejectNextSubscribe(bool reject);

  // Actions.
  void dropAllConnections();                                 // server-side disconnect
  void pushBasicQot(const std::string& code, double price);  // to every connection

  // Observations.
  std::size_t connectionCount() const;  // accepted so far
  std::size_t activeConnections() const;
  std::size_t subscribeRequests() const;
  std::size_t keepAliveRequests() const;
  std::vector<std::string> subscribedCodes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace futu_trader::mock
