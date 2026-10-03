#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

#include "Qot_UpdateBasicQot.pb.h"
#include "futu_trader/opend/client.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "mock_opend.hpp"
#include "test_support.hpp"

using namespace futu_trader;
using testing_support::waitFor;
using namespace futu_trader::opend;
using namespace std::chrono_literals;

namespace {

// Polls until `cond` holds or the deadline passes; avoids fixed sleeps in tests.
ClientConfig configFor(std::uint16_t port) {
  ClientConfig cfg;
  cfg.connection.port = port;
  cfg.connection.requestTimeout = 600ms;
  cfg.connection.connectTimeout = 1000ms;
  cfg.reconnectBase = 20ms;
  cfg.reconnectMax = 200ms;
  return cfg;
}

class OpenDClientTest : public ::testing::Test {
 protected:
  void SetUp() override { port = server.start(); }
  void TearDown() override { server.stop(); }
  mock::MockOpenD server;
  std::uint16_t port{0};
};

const SecurityRef kTencent{kQotMarketHkSecurity, "00700"};

}  // namespace

TEST_F(OpenDClientTest, ConnectsAndReturnsSession) {
  OpenDClient client(configFor(port));
  const auto session = client.connect();
  ASSERT_TRUE(session.ok()) << session.error().message;
  EXPECT_EQ(session.value().connId, 1001U);
  EXPECT_EQ(session.value().keepAliveSeconds, 10);
  EXPECT_TRUE(client.isConnected());
}

TEST_F(OpenDClientTest, ConnectFailsFastWhenNothingListens) {
  server.stop();
  ClientConfig cfg = configFor(port);
  cfg.autoReconnect = false;
  OpenDClient client(cfg);
  const auto session = client.connect();
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.error().code, ErrorCode::kDisconnected);
  EXPECT_FALSE(client.isConnected());
}

TEST_F(OpenDClientTest, RequestsFailCleanlyWhenNotConnected) {
  OpenDClient client(configFor(port));
  const auto quotes = client.getBasicQuotes({kTencent});
  ASSERT_FALSE(quotes.ok());
  EXPECT_EQ(quotes.error().code, ErrorCode::kDisconnected);
}

TEST_F(OpenDClientTest, KeepAliveIsSentPeriodically) {
  ClientConfig cfg = configFor(port);
  cfg.connection.keepAliveOverride = 30ms;
  OpenDClient client(cfg);
  ASSERT_TRUE(client.connect().ok());
  EXPECT_TRUE(waitFor([&] { return server.keepAliveRequests() >= 3; }));
  EXPECT_TRUE(client.isConnected());
}

TEST_F(OpenDClientTest, MissingKeepAliveResponsesTriggerReconnect) {
  ClientConfig cfg = configFor(port);
  cfg.connection.keepAliveOverride = 30ms;
  cfg.connection.requestTimeout = 100ms;
  OpenDClient client(cfg);
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.ignoreKeepAlive = true;
  server.setFaults(faults);
  ASSERT_TRUE(waitFor([&] { return server.connectionCount() >= 2; }));  // dead peer detected
  server.setFaults({});
  EXPECT_TRUE(waitFor([&] { return client.reconnectCount() >= 1 && client.isConnected(); }));
}

TEST_F(OpenDClientTest, GetBasicQuotesConvertsToMinorUnits) {
  server.setQuote("00700", 350.2);
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  const auto quotes = client.getBasicQuotes({kTencent});
  ASSERT_TRUE(quotes.ok()) << quotes.error().message;
  ASSERT_EQ(quotes.value().size(), 1U);
  EXPECT_EQ(quotes.value()[0].curPrice, 350200);
  EXPECT_EQ(quotes.value()[0].security.code, "00700");
}

TEST_F(OpenDClientTest, HistoryKlPagesThroughNextReqKey) {
  std::vector<mock::MockKline> data;
  for (int i = 0; i < 7; ++i) {
    data.push_back({"2026-09-0" + std::to_string(i + 1) + " 00:00:00", 100.0 + i, 101.0 + i,
                    99.0 + i, 100.5 + i, 1000 + i});
  }
  server.setKlines(data, 3);  // 3 pages: 3 + 3 + 1
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  const auto bars = client.requestHistoryKl(kTencent, KlType::kDay, "2026-09-01", "2026-09-07");
  ASSERT_TRUE(bars.ok()) << bars.error().message;
  ASSERT_EQ(bars.value().size(), 7U);
  EXPECT_EQ(bars.value()[0].open, 100000);
  EXPECT_EQ(bars.value()[6].close, 106500);
  EXPECT_EQ(bars.value()[6].volume, 1006);
}

TEST_F(OpenDClientTest, HistoryKlStopsAtMaxPages) {
  std::vector<mock::MockKline> data(10, {"2026-09-01 00:00:00", 1, 1, 1, 1, 1});
  server.setKlines(data, 1);
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  const auto bars = client.requestHistoryKl(kTencent, KlType::kDay, "a", "b", 3);
  ASSERT_FALSE(bars.ok());
  EXPECT_EQ(bars.error().code, ErrorCode::kProtocol);
}

TEST_F(OpenDClientTest, AccountFundsAndPositionsAreReadOnlyQueries) {
  server.setFundsCash(1234567.891);
  server.setPositions({{"00700", 200, 100, 350.2, 340.0}});
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());

  const auto accounts = client.getAccList();
  ASSERT_TRUE(accounts.ok());
  ASSERT_EQ(accounts.value().size(), 2U);
  EXPECT_EQ(accounts.value()[0].env, TrdEnv::kSimulate);
  EXPECT_EQ(accounts.value()[1].env, TrdEnv::kReal);

  const auto sim = AccountHeader::simulate(accounts.value()[0].accId, TrdMarket::kHK);
  const auto funds = client.getFunds(sim);
  ASSERT_TRUE(funds.ok()) << funds.error().message;
  EXPECT_EQ(funds.value().cash, 1234567891);

  const auto positions = client.getPositions(sim);
  ASSERT_TRUE(positions.ok()) << positions.error().message;
  ASSERT_EQ(positions.value().size(), 1U);
  EXPECT_EQ(positions.value()[0].qty, 200);
  EXPECT_EQ(positions.value()[0].canSellQty, 100);
  EXPECT_EQ(positions.value()[0].costPrice, 340000);
}

TEST_F(OpenDClientTest, ShortPositionsComeBackNegative) {
  // Regression: Futu sends a short as a positive qty + PositionSide_Short; ignoring the side
  // made shorts look long and inverted exposure.
  server.setPositions({{"00700", 300, 300, 350.2, 340.0, 1}, {"09988", 100, 100, 80.0, 79.0, 0}});
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  const auto sim = AccountHeader::simulate(111, TrdMarket::kHK);
  const auto positions = client.getPositions(sim);
  ASSERT_TRUE(positions.ok()) << positions.error().message;
  ASSERT_EQ(positions.value().size(), 2U);
  EXPECT_EQ(positions.value()[0].qty, -300);
  EXPECT_EQ(positions.value()[1].qty, 100);
}

TEST_F(OpenDClientTest, UnknownPositionSideFailsClosed) {
  server.setPositions({{"00700", 300, 300, 350.2, 340.0, -1}});
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  const auto sim = AccountHeader::simulate(111, TrdMarket::kHK);
  const auto positions = client.getPositions(sim);
  ASSERT_FALSE(positions.ok());
  EXPECT_EQ(positions.error().code, ErrorCode::kProtocol);
}

TEST_F(OpenDClientTest, ServerRejectionSurfacesAsServerError) {
  server.rejectNextSubscribe(true);
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  const auto sub = client.subscribe({kTencent}, {SubType::kBasic});
  ASSERT_FALSE(sub.ok());
  EXPECT_EQ(sub.error().code, ErrorCode::kServer);
  EXPECT_NE(sub.error().message.find("quota exceeded"), std::string::npos);
}

TEST_F(OpenDClientTest, PushesReachTheHandler) {
  std::atomic<int> pushes{0};
  std::atomic<double> lastPrice{0};
  OpenDClient client(configFor(port));
  client.setPushHandler([&](const Frame& frame) {
    if (frame.protoId == protoId::kQotUpdateBasicQot) {
      Qot_UpdateBasicQot::Response rsp;
      ASSERT_TRUE(rsp.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
      lastPrice = rsp.s2c().basicqotlist(0).curprice();
      ++pushes;
    }
  });
  ASSERT_TRUE(client.connect().ok());
  server.pushBasicQot("00700", 351.4);
  ASSERT_TRUE(waitFor([&] { return pushes.load() == 1; }));
  EXPECT_DOUBLE_EQ(lastPrice.load(), 351.4);
}

// --- Fault injection ---

TEST_F(OpenDClientTest, SurvivesByteAtATimeResponses) {
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.splitResponses = 1;
  server.setFaults(faults);
  const auto quotes = client.getBasicQuotes({kTencent});
  ASSERT_TRUE(quotes.ok()) << quotes.error().message;
}

TEST_F(OpenDClientTest, ResponseCoalescedWithPushIsDeliveredBoth) {
  std::atomic<int> pushes{0};
  OpenDClient client(configFor(port));
  client.setPushHandler([&](const Frame&) { ++pushes; });
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.coalesceWithPush = 1;
  server.setFaults(faults);
  const auto quotes = client.getBasicQuotes({kTencent});
  ASSERT_TRUE(quotes.ok()) << quotes.error().message;
  EXPECT_TRUE(waitFor([&] { return pushes.load() == 1; }));
}

TEST_F(OpenDClientTest, ReplyWithTheWrongProtoIdIsNotAcceptedAsTheAnswer) {
  // A push that happens to share a request's serial must not be consumed as its response.
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.wrongProtoIdResponses = 1;
  server.setFaults(faults);
  const auto result = client.getBasicQuotes({kTencent});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, ErrorCode::kTimeout);
  EXPECT_EQ(client.mismatchedReplies(), 1U);
  EXPECT_TRUE(client.isConnected());
  EXPECT_TRUE(client.getBasicQuotes({kTencent}).ok());  // the connection is unharmed
}

TEST_F(OpenDClientTest, NonLoopbackHostIsRefusedByDefault) {
  ClientConfig cfg = configFor(port);
  cfg.connection.host = "192.0.2.1";  // TEST-NET-1: never reachable, and definitely not loopback
  cfg.autoReconnect = false;
  OpenDClient client(cfg);
  const auto session = client.connect();
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.error().code, ErrorCode::kInvalidArg);
  EXPECT_NE(session.error().message.find("loopback"), std::string::npos);
}

TEST_F(OpenDClientTest, LocalhostByNameIsAcceptedAsLoopback) {
  ClientConfig cfg = configFor(port);
  cfg.connection.host = "localhost";
  OpenDClient client(cfg);
  EXPECT_TRUE(client.connect().ok());
}

TEST_F(OpenDClientTest, DroppedResponseTimesOutWithoutKillingTheConnection) {
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.dropResponses = 1;
  server.setFaults(faults);
  const auto lost = client.getBasicQuotes({kTencent});
  ASSERT_FALSE(lost.ok());
  EXPECT_EQ(lost.error().code, ErrorCode::kTimeout);
  EXPECT_TRUE(client.isConnected());
  EXPECT_TRUE(client.getBasicQuotes({kTencent}).ok());  // next request is unaffected
}

TEST_F(OpenDClientTest, CorruptChecksumDropsTheConnectionAndItRecovers) {
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.corruptChecksum = 1;
  server.setFaults(faults);
  const auto bad = client.getBasicQuotes({kTencent});
  ASSERT_FALSE(bad.ok());  // the stream is untrustworthy, so the request fails
  ASSERT_TRUE(waitFor([&] { return client.reconnectCount() >= 1 && client.isConnected(); }));
  EXPECT_TRUE(client.getBasicQuotes({kTencent}).ok());
}

TEST_F(OpenDClientTest, OversizeFrameLengthDropsTheConnection) {
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.oversizeLengthFrames = 1;
  server.setFaults(faults);
  EXPECT_FALSE(client.getBasicQuotes({kTencent}).ok());
  EXPECT_TRUE(waitFor([&] { return client.reconnectCount() >= 1 && client.isConnected(); }));
}

TEST_F(OpenDClientTest, DisconnectMidFrameFailsTheRequestAndReconnects) {
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  mock::Faults faults;
  faults.disconnectMidFrame = 1;
  server.setFaults(faults);
  const auto lost = client.getBasicQuotes({kTencent});
  ASSERT_FALSE(lost.ok());
  EXPECT_EQ(lost.error().code, ErrorCode::kDisconnected);
  EXPECT_TRUE(waitFor([&] { return client.reconnectCount() >= 1 && client.isConnected(); }));
}

TEST_F(OpenDClientTest, ReconnectRestoresSubscriptionsAndNotifiesState) {
  std::mutex mu;
  std::vector<bool> states;
  OpenDClient client(configFor(port));
  client.setConnectionStateHandler([&](bool up) {
    std::scoped_lock lock(mu);
    states.push_back(up);
  });
  ASSERT_TRUE(client.connect().ok());
  ASSERT_TRUE(client.subscribe({kTencent}, {SubType::kBasic}).ok());
  ASSERT_EQ(server.subscribeRequests(), 1U);

  server.dropAllConnections();
  ASSERT_TRUE(waitFor([&] { return client.reconnectCount() >= 1 && client.isConnected(); }));
  EXPECT_EQ(server.subscribeRequests(), 2U);  // resubscribed on the new connection
  EXPECT_EQ(server.connectionCount(), 2U);

  std::scoped_lock lock(mu);
  ASSERT_GE(states.size(), 3U);
  EXPECT_TRUE(states[0]);   // initial connect
  EXPECT_FALSE(states[1]);  // dropped: the engine must be told so it can halt trading
  EXPECT_TRUE(states[2]);   // restored only after resubscribe
}

TEST_F(OpenDClientTest, ReconnectBacksOffWhileServerIsDownThenSucceeds) {
  OpenDClient client(configFor(port));
  ASSERT_TRUE(client.connect().ok());
  server.stop();  // fully down: the accept socket is closed too
  ASSERT_TRUE(waitFor([&] { return !client.isConnected(); }));
  EXPECT_EQ(client.reconnectCount(), 0U);
  // No server to reconnect to. Closing must still return promptly (no hang in the retry loop).
  const auto start = std::chrono::steady_clock::now();
  client.close();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 3000ms);
}

TEST(OpenDParsing, ParseSecurity) {
  const auto hk = parseSecurity("HK.00700");
  ASSERT_TRUE(hk.ok());
  EXPECT_EQ(hk.value().market, kQotMarketHkSecurity);
  EXPECT_EQ(hk.value().code, "00700");
  EXPECT_EQ(parseSecurity("US.AAPL").value().market, kQotMarketUsSecurity);
  EXPECT_FALSE(parseSecurity("00700").ok());
  EXPECT_FALSE(parseSecurity("HK.").ok());
  EXPECT_FALSE(parseSecurity(".700").ok());
  EXPECT_EQ(parseSecurity("XX.1").error().code, ErrorCode::kUnsupported);
}
