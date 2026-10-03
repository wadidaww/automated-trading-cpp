#include "futu_trader/infra/metrics_server.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <thread>

namespace futu_trader::infra {

namespace asio = boost::asio;

namespace {

std::string response(int code, const std::string& reason, const std::string& body,
                     const std::string& contentType = "text/plain; charset=utf-8") {
  return "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + contentType +
         "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

struct MetricsServer::Impl : std::enable_shared_from_this<MetricsServer::Impl> {
  Impl(const MetricsRegistry& r, ReadyFn rd, MetricsServerConfig c)
      : registry(r), ready(std::move(rd)), config(c), acceptor(io), retryTimer(io) {}

  // Concurrent sessions are capped: every one holds a file descriptor of a process that also needs
  // descriptors for the WAL and the OpenD link. Only touched from the io thread.
  static constexpr int kMaxSessions = 16;
  int activeSessions{0};

  // One connection: read headers (bounded, with a deadline), answer, close.
  struct Session : std::enable_shared_from_this<Session> {
    Session(Impl& o, asio::io_context& io)
        : owner(o), socket(io), timer(io), buffer(o.config.maxRequestBytes) {}

    void begin() {
      ++owner.activeSessions;
      counted = true;
      timer.expires_after(owner.config.readTimeout);
      auto self = shared_from_this();
      timer.async_wait([self](const boost::system::error_code& ec) {
        if (!ec) {
          self->closeNow();  // deadline hit: whoever is on the other end is too slow
        }
      });
      asio::async_read_until(
          socket, buffer, "\r\n\r\n",
          [self](const boost::system::error_code& ec, std::size_t) { self->onRequest(ec); });
    }

    void onRequest(const boost::system::error_code& ec) {
      timer.cancel();
      if (ec == asio::error::not_found || ec == asio::error::message_size) {
        reply(response(431, "Request Header Fields Too Large", "request too large\n"));
        return;
      }
      if (ec) {
        closeNow();
        return;
      }
      std::istream stream(&buffer);
      std::string method;
      std::string target;
      stream >> method >> target;
      const auto query = target.find('?');
      const std::string path = target.substr(0, query);
      if (method != "GET") {
        reply(response(405, "Method Not Allowed", "only GET is supported\n"));
      } else if (path == "/metrics") {
        reply(response(200, "OK", owner.registry.render(),
                       "text/plain; version=0.0.4; charset=utf-8"));
      } else if (path == "/healthz") {
        reply(response(200, "OK", "ok\n"));
      } else if (path == "/readyz") {
        const bool isReady = owner.ready && owner.ready();
        reply(isReady ? response(200, "OK", "ready\n")
                      : response(503, "Service Unavailable", "not ready\n"));
      } else {
        reply(response(404, "Not Found", "not found\n"));
      }
    }

    void reply(std::string text) {
      auto data = std::make_shared<std::string>(std::move(text));
      auto self = shared_from_this();
      // A client that never reads the answer must not hold the descriptor forever.
      timer.expires_after(owner.config.readTimeout);
      timer.async_wait([self](const boost::system::error_code& ec) {
        if (!ec) {
          self->closeNow();
        }
      });
      asio::async_write(
          socket, asio::buffer(*data),
          [self, data](const boost::system::error_code&, std::size_t) { self->closeNow(); });
    }

    void closeNow() {
      if (counted) {
        counted = false;
        --owner.activeSessions;
      }
      timer.cancel();
      boost::system::error_code ignored;
      ignored = socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      ignored = socket.close(ignored);
    }

    Impl& owner;
    asio::ip::tcp::socket socket;
    asio::steady_timer timer;
    asio::streambuf buffer;
    bool counted{false};
  };

  void accept() {
    auto session = std::make_shared<Session>(*this, io);
    acceptor.async_accept(session->socket, [this, session](const boost::system::error_code& ec) {
      if (ec == asio::error::operation_aborted) {
        return;  // acceptor closed on stop()
      }
      if (ec) {
        // EMFILE/ENFILE/ECONNABORTED...: never stop serving for good. Try again shortly.
        retryTimer.expires_after(std::chrono::milliseconds(50));
        retryTimer.async_wait([this](const boost::system::error_code& waitEc) {
          if (!waitEc) {
            accept();
          }
        });
        return;
      }
      if (activeSessions >= kMaxSessions) {
        session->closeNow();  // shed load instead of exhausting descriptors
      } else {
        session->begin();
      }
      accept();
    });
  }

  const MetricsRegistry& registry;
  ReadyFn ready;
  MetricsServerConfig config;
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor;
  asio::steady_timer retryTimer;
  std::thread thread;
  bool running{false};
};

MetricsServer::MetricsServer(const MetricsRegistry& registry, ReadyFn ready,
                             MetricsServerConfig config)
    : impl_(std::make_unique<Impl>(registry, std::move(ready), config)) {}

MetricsServer::~MetricsServer() {
  try {
    stop();
  } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor must not throw; nothing else to do
  }
}

Result<std::uint16_t> MetricsServer::start() {
  auto& im = *impl_;
  if (im.running) {
    return Error{ErrorCode::kInvalidArg, "already started"};
  }
  boost::system::error_code ec;
  const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), im.config.port);
  ec = im.acceptor.open(endpoint.protocol(), ec);
  if (!ec) {
    ec = im.acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
  }
  if (!ec) {
    ec = im.acceptor.bind(endpoint, ec);
  }
  if (!ec) {
    ec = im.acceptor.listen(asio::socket_base::max_listen_connections, ec);
  }
  if (ec) {
    return Error{ErrorCode::kDisconnected, "metrics server cannot listen: " + ec.message()};
  }
  const std::uint16_t port = im.acceptor.local_endpoint().port();
  im.accept();
  im.running = true;
  im.thread = std::thread([this] { impl_->io.run(); });
  return port;
}

void MetricsServer::stop() {
  auto& im = *impl_;
  if (!im.running) {
    return;
  }
  im.running = false;
  // Post the close so it runs on the io thread: asio objects are not thread-safe.
  asio::post(im.io, [&im] {
    boost::system::error_code ignored;
    ignored = im.acceptor.close(ignored);
    im.retryTimer.cancel();
  });
  im.io.stop();
  if (im.thread.joinable()) {
    im.thread.join();
  }
}

}  // namespace futu_trader::infra
