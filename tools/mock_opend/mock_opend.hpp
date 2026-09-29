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
  int dropResponses{0};          // swallow this many responses (client sees a timeout)
  int corruptChecksum{0};        // send this many responses with a bad SHA-1
  int oversizeLengthFrames{0};   // send a header claiming a huge body
  int disconnectMidFrame{0};     // send half of a response, then close the socket
  int wrongProtoIdResponses{0};  // answer with a different proto id than the request
  int splitResponses{0};         // deliver this many responses in 1-byte TCP writes
  int coalesceWithPush{0};       // write response + a push in a single TCP write
  bool ignoreKeepAlive{false};   // never answer KeepAlive (persistent)
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
  int side{0};  // Trd_Common.PositionSide: 0 long, 1 short, -1 unknown
};

struct MockOrder {
  std::uint64_t orderId{0};
  std::string remark;
  std::string code;
  int trdSide{1};  // Trd_Common.TrdSide: 1 buy, 2 sell, 3 sell short
  double qty{0};
  double price{0};
  int status{5};  // Trd_Common.OrderStatus; 5 = Submitted
  double fillQty{0};
  double fillAvg{0};
  int trdEnv{0};  // Trd_Common.TrdEnv: 0 simulate, 1 real. Recorded so tests can assert stamping.
  std::uint64_t accId{0};
};

struct MockFill {
  std::string fillId;
  std::uint64_t orderId{0};
  std::string code;
  int trdSide{1};
  double qty{0};
  double price{0};
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

  // Trading scenario controls.
  void setUnlockPassword(const std::string& md5);    // unlock fails unless it matches
  void rejectNextPlace(const std::string& message);  // next PlaceOrder returns this error
  /** Next PlaceOrder answers with this raw retType (e.g. -100 timeout, -200 disconnect). */
  void failNextPlaceWithRetType(int retType, const std::string& message);
  /** Fills an order found by remark (pushes UpdateOrder + UpdateOrderFill). False if not found. */
  bool fillOrderByRemark(const std::string& remark, double qty, double price);
  /** Forces an order's broker status (pushes UpdateOrder). False if not found. */
  bool setStatusByRemark(const std::string& remark, int status);
  /** Creates an order at the broker that our client never saw (drift scenario). */
  void injectOrder(const MockOrder& order);
  void injectFill(const MockFill& fill);
  void setSuppressPushes(bool suppress);  // do not push order/fill updates (missed-push drift)

  // Actions.
  void dropAllConnections();                                 // server-side disconnect
  void pushBasicQot(const std::string& code, double price);  // to every connection

  // Observations.
  std::size_t connectionCount() const;  // accepted so far
  std::size_t activeConnections() const;
  std::size_t subscribeRequests() const;
  std::size_t keepAliveRequests() const;
  std::vector<std::string> subscribedCodes() const;
  std::vector<MockOrder> orders() const;
  std::vector<MockFill> fills() const;
  std::size_t placeRequests() const;
  std::size_t unlockRequests() const;
  std::vector<std::uint64_t> accPushIds() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace futu_trader::mock
