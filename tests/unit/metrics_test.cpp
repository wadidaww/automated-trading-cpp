#include "futu_trader/infra/metrics.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <string>
#include <thread>

#include "futu_trader/infra/metrics_server.hpp"

using namespace futu_trader;
using namespace futu_trader::infra;
using namespace std::chrono_literals;
namespace asio = boost::asio;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Sends raw bytes to 127.0.0.1:port and returns everything the server writes before closing.
std::string rawExchange(std::uint16_t port, const std::string& request, bool shutdownSend = false) {
  asio::io_context io;
  asio::ip::tcp::socket socket(io);
  socket.connect({asio::ip::make_address("127.0.0.1"), port});
  if (!request.empty()) {
    asio::write(socket, asio::buffer(request));
  }
  if (shutdownSend) {
    socket.shutdown(asio::ip::tcp::socket::shutdown_send);
  }
  std::string response;
  boost::system::error_code ec;
  std::array<char, 4096> chunk{};
  while (true) {
    const std::size_t n = socket.read_some(asio::buffer(chunk), ec);
    if (ec) {
      break;
    }
    response.append(chunk.data(), n);
  }
  return response;
}

std::string get(std::uint16_t port, const std::string& path) {
  return rawExchange(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
}

}  // namespace

// --- Exposition format --------------------------------------------------------------------------

TEST(Metrics, CounterAndGaugeExpositionIsExact) {
  MetricsRegistry registry;
  std::atomic<std::uint64_t> orders{42};
  ASSERT_TRUE(
      registry.addCounter("futu_orders_total", "Orders accepted.", [&] { return orders.load(); })
          .ok());
  ASSERT_TRUE(
      registry.addGauge("futu_live_orders", "Orders working at the broker.", [] { return 3.0; })
          .ok());
  EXPECT_EQ(registry.render(),
            "# HELP futu_orders_total Orders accepted.\n"
            "# TYPE futu_orders_total counter\n"
            "futu_orders_total 42\n"
            "# HELP futu_live_orders Orders working at the broker.\n"
            "# TYPE futu_live_orders gauge\n"
            "futu_live_orders 3\n");
  orders = 43;  // the value is read at scrape time, not copied at registration
  EXPECT_TRUE(contains(registry.render(), "futu_orders_total 43\n"));
}

TEST(Metrics, LabelsShareOneFamilyHeaderAndAreEscaped) {
  MetricsRegistry registry;
  ASSERT_TRUE(registry
                  .addCounter("futu_risk_rejects_total", "Risk rejects by reason.",
                              [] { return 5; }, {{"reason", "daily_loss"}})
                  .ok());
  ASSERT_TRUE(registry
                  .addCounter("futu_risk_rejects_total", "Risk rejects by reason.",
                              [] { return 9; }, {{"reason", "we\"ird\\va\nlue"}})
                  .ok());
  const std::string text = registry.render();
  EXPECT_EQ(text.find("# TYPE futu_risk_rejects_total"),
            text.rfind("# TYPE futu_risk_rejects_total"));
  EXPECT_TRUE(contains(text, "futu_risk_rejects_total{reason=\"daily_loss\"} 5\n"));
  EXPECT_TRUE(contains(text, "futu_risk_rejects_total{reason=\"we\\\"ird\\\\va\\nlue\"} 9\n"));
}

TEST(Metrics, HelpTextIsEscapedSoItCannotInjectMetrics) {
  MetricsRegistry registry;
  ASSERT_TRUE(registry.addGauge("futu_x", "line one\nfutu_injected 1", [] { return 1.0; }).ok());
  const std::string text = registry.render();
  EXPECT_TRUE(contains(text, "# HELP futu_x line one\\nfutu_injected 1\n"));
  EXPECT_FALSE(contains(text, "\nfutu_injected 1"));
}

TEST(Metrics, LatencyHistogramIsExposedInSecondsWithCumulativeBuckets) {
  LatencyHistogram h;
  h.record(500);    // 0.5 us
  h.record(3'000);  // 3 us
  h.record(3'000);
  h.record(2'000'000);  // 2 ms
  MetricsRegistry registry;
  ASSERT_TRUE(registry.addLatencyHistogram("futu_handle_seconds", "Handling time.", h).ok());
  const std::string text = registry.render();
  EXPECT_TRUE(contains(text, "# TYPE futu_handle_seconds histogram\n"));
  EXPECT_TRUE(contains(text, "futu_handle_seconds_bucket{le=\"1e-06\"} 1\n"));
  EXPECT_TRUE(contains(text, "futu_handle_seconds_bucket{le=\"5e-06\"} 3\n"));
  EXPECT_TRUE(contains(text, "futu_handle_seconds_bucket{le=\"0.002\"} 4\n"));
  EXPECT_TRUE(contains(text, "futu_handle_seconds_bucket{le=\"+Inf\"} 4\n"));
  EXPECT_TRUE(contains(text, "futu_handle_seconds_count 4\n"));
  EXPECT_TRUE(contains(text, "futu_handle_seconds_sum 0.0020065\n"));
}

TEST(Metrics, HistogramBucketsNeverDecrease) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v < 3'000'000'000ULL; v = v * 3 + 1) {
    h.record(v);
  }
  MetricsRegistry registry;
  ASSERT_TRUE(registry.addLatencyHistogram("futu_t_seconds", "t", h).ok());
  const std::string text = registry.render();
  std::uint64_t previous = 0;
  std::size_t pos = 0;
  while ((pos = text.find("futu_t_seconds_bucket{", pos)) != std::string::npos) {
    const auto space = text.find("} ", pos);
    const auto end = text.find('\n', space);
    const std::uint64_t value = std::stoull(text.substr(space + 2, end - space - 2));
    EXPECT_GE(value, previous);
    previous = value;
    pos = end;
  }
  EXPECT_EQ(previous, h.count());
}

TEST(Metrics, RegistrationRejectsAnythingThatWouldBreakTheScrape) {
  MetricsRegistry registry;
  EXPECT_FALSE(registry.addGauge("", "h", [] { return 0.0; }).ok());
  EXPECT_FALSE(registry.addGauge("1starts_with_digit", "h", [] { return 0.0; }).ok());
  EXPECT_FALSE(registry.addGauge("has space", "h", [] { return 0.0; }).ok());
  EXPECT_FALSE(registry.addGauge("has-dash", "h", [] { return 0.0; }).ok());
  EXPECT_FALSE(registry.addCounter("futu_orders", "no _total suffix", [] { return 0; }).ok());
  EXPECT_FALSE(registry.addGauge("futu_g", "h", [] { return 0.0; }, {{"bad-label", "v"}}).ok());
  EXPECT_FALSE(registry.addGauge("futu_g", "h", [] { return 0.0; }, {{"__reserved", "v"}}).ok());
  EXPECT_FALSE(registry.addGauge("futu_g", "h", [] { return 0.0; }, {{"le", "v"}}).ok());
  ASSERT_TRUE(registry.addGauge("futu_ok", "h", [] { return 0.0; }).ok());
  EXPECT_FALSE(registry.addGauge("futu_ok", "h", [] { return 1.0; }).ok());  // duplicate
  EXPECT_FALSE(registry.addCounter("futu_ok", "h", [] { return 1; }).ok());  // other type
  EXPECT_TRUE(registry.addGauge("futu_ok", "h", [] { return 1.0; }, {{"a", "b"}}).ok());
}

// --- HTTP server --------------------------------------------------------------------------------

namespace {
struct ServerFixture : ::testing::Test {
  void SetUp() override {
    registry.addGauge("futu_up", "Always 1.", [] { return 1.0; });
    MetricsServerConfig cfg;
    cfg.readTimeout = 300ms;
    cfg.maxRequestBytes = 1024;
    server = std::make_unique<MetricsServer>(registry, [this] { return ready.load(); }, cfg);
    const auto bound = server->start();
    ASSERT_TRUE(bound.ok()) << bound.error().message;
    port = bound.value();
  }
  void TearDown() override { server->stop(); }
  MetricsRegistry registry;
  std::atomic<bool> ready{true};
  std::unique_ptr<MetricsServer> server;
  std::uint16_t port{0};
};
}  // namespace

TEST_F(ServerFixture, ServesMetricsWithTheRightHeaders) {
  const std::string reply = get(port, "/metrics");
  EXPECT_TRUE(contains(reply, "HTTP/1.1 200 OK"));
  EXPECT_TRUE(contains(reply, "Content-Type: text/plain; version=0.0.4"));
  EXPECT_TRUE(contains(reply, "Cache-Control: no-store"));
  EXPECT_TRUE(contains(reply, "futu_up 1\n"));
  const std::string withQuery = get(port, "/metrics?name=foo");
  EXPECT_TRUE(contains(withQuery, "200 OK"));  // the query string is ignored, not interpreted
}

TEST_F(ServerFixture, LivenessIsAlwaysUpButReadinessTracksTheSystem) {
  EXPECT_TRUE(contains(get(port, "/healthz"), "200 OK"));
  EXPECT_TRUE(contains(get(port, "/readyz"), "200 OK"));
  ready = false;
  EXPECT_TRUE(contains(get(port, "/readyz"), "503 Service Unavailable"));
  EXPECT_TRUE(contains(get(port, "/healthz"), "200 OK"));  // alive but not fit to trade
}

TEST_F(ServerFixture, UnknownPathsAndNonGetMethodsAreRefused) {
  EXPECT_TRUE(contains(get(port, "/admin"), "404 Not Found"));
  EXPECT_TRUE(contains(get(port, "/../etc/passwd"), "404 Not Found"));
  EXPECT_TRUE(
      contains(rawExchange(port, "POST /metrics HTTP/1.1\r\n\r\n"), "405 Method Not Allowed"));
  EXPECT_TRUE(contains(rawExchange(port, "DELETE /metrics HTTP/1.1\r\n\r\n"), "405"));
}

TEST_F(ServerFixture, OversizedRequestsAreRejectedNotBuffered) {
  const std::string huge = "GET /metrics HTTP/1.1\r\nX-Pad: " + std::string(5000, 'a') + "\r\n\r\n";
  EXPECT_TRUE(contains(rawExchange(port, huge), "431"));
}

TEST_F(ServerFixture, ASilentClientIsDroppedAtTheDeadlineNotHeldForever) {
  const auto start = std::chrono::steady_clock::now();
  const std::string reply = rawExchange(port, "");  // connects, sends nothing, waits
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(reply.empty());
  EXPECT_LT(elapsed, 3s);
  EXPECT_GE(elapsed, 250ms);  // it did wait for the configured deadline
}

TEST_F(ServerFixture, ASlowlorisClientSendingHalfARequestIsAlsoDropped) {
  const auto start = std::chrono::steady_clock::now();
  const std::string reply =
      rawExchange(port, "GET /metrics HTTP/1.1\r\nHost: x\r\n");  // no blank line
  EXPECT_TRUE(reply.empty());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
  EXPECT_TRUE(contains(get(port, "/healthz"), "200 OK"));  // and the server is still responsive
}

TEST_F(ServerFixture, ManyBackToBackConnectionsAreAllServed) {
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(contains(get(port, "/healthz"), "200 OK")) << i;
  }
}

TEST_F(ServerFixture, StopWorksWithAnIdleConnectionStillOpenAndIsIdempotent) {
  asio::io_context io;
  asio::ip::tcp::socket idle(io);
  idle.connect({asio::ip::make_address("127.0.0.1"), port});  // never sends anything
  server->stop();
  server->stop();
  SUCCEED();
}

TEST(MetricsServer, ABusyPortIsReportedNotSilentlyIgnored) {
  MetricsRegistry registry;
  MetricsServer first(registry, nullptr, {});
  const auto bound = first.start();
  ASSERT_TRUE(bound.ok());
  MetricsServerConfig cfg;
  cfg.port = bound.value();
  // reuse_address allows quick restarts but must never allow two listeners on one live port.
  MetricsServer second(registry, nullptr, cfg);
  EXPECT_FALSE(second.start().ok());
}
