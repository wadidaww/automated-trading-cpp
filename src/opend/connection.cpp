#include "futu_trader/opend/connection.hpp"

#include <boost/asio.hpp>

#include "Common.pb.h"
#include "InitConnect.pb.h"
#include "KeepAlive.pb.h"
#include "futu_trader/opend/proto_ids.hpp"

namespace futu_trader::opend {

namespace {
// Best-effort socket teardown: errors here are expected and uninteresting.
template <typename T>
void ignore(const T& /*value*/) {}
}  // namespace

namespace asio = boost::asio;

struct OpenDConnection::Impl {
  asio::io_context io;
  asio::ip::tcp::socket socket{io};
  std::mutex writeMu;
};

OpenDConnection::OpenDConnection(ConnectionConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

OpenDConnection::~OpenDConnection() { close(); }

Result<SessionInfo> OpenDConnection::connect() {
  close();
  impl_ = std::make_unique<Impl>();
  stopping_ = false;

  // Resolve and connect with a deadline: a blocking connect can hang for minutes.
  boost::system::error_code ec;
  asio::ip::tcp::resolver resolver(impl_->io);
  const auto endpoints = resolver.resolve(config_.host, std::to_string(config_.port), ec);
  if (ec) {
    return Error{ErrorCode::kDisconnected, "resolve failed: " + ec.message()};
  }
  if (!config_.allowNonLoopback) {
    for (const auto& entry : endpoints) {
      if (!entry.endpoint().address().is_loopback()) {
        return Error{ErrorCode::kInvalidArg,
                     "refusing non-loopback OpenD host '" + config_.host +
                         "': the protocol is unencrypted (set allowNonLoopback to override)"};
      }
    }
  }
  boost::system::error_code connectEc = asio::error::would_block;
  asio::async_connect(impl_->socket, endpoints,
                      [&](const boost::system::error_code& code, const asio::ip::tcp::endpoint&) {
                        connectEc = code;
                      });
  impl_->io.restart();
  impl_->io.run_for(config_.connectTimeout);
  if (connectEc == asio::error::would_block) {
    ignore(impl_->socket.close(ec));
    impl_->io.run();  // drain the cancelled handler
    return Error{ErrorCode::kTimeout, "connect timed out"};
  }
  if (connectEc) {
    return Error{ErrorCode::kDisconnected, "connect failed: " + connectEc.message()};
  }
  ignore(impl_->socket.set_option(asio::ip::tcp::no_delay(true), ec));

  connected_ = true;
  reader_ = std::thread(&OpenDConnection::readerLoop, this);

  InitConnect::Request req;
  auto* c2s = req.mutable_c2s();
  c2s->set_clientver(config_.clientVer);
  c2s->set_clientid(config_.clientId);
  c2s->set_recvnotify(config_.recvNotify);
  const auto raw =
      requestInternal(protoId::kInitConnect, req.SerializeAsString(), config_.requestTimeout);
  if (!raw) {
    const Error& err = raw.error();
    close();
    return err;
  }
  InitConnect::Response rsp;
  if (!rsp.ParseFromArray(raw.value().data(), static_cast<int>(raw.value().size()))) {
    close();
    return Error{ErrorCode::kProtocol, "InitConnect: unparseable response"};
  }
  if (rsp.rettype() != Common::RetType_Succeed || !rsp.has_s2c()) {
    close();
    return Error{ErrorCode::kServer, "InitConnect rejected: " + rsp.retmsg()};
  }
  session_.connId = rsp.s2c().connid();
  session_.serverVer = rsp.s2c().serverver();
  session_.keepAliveSeconds = rsp.s2c().keepaliveinterval();
  session_.loginUserId = rsp.s2c().loginuserid();

  keepAlive_ = std::thread(&OpenDConnection::keepAliveLoop, this);
  return session_;
}

void OpenDConnection::close() {
  stopping_ = true;
  connected_ = false;
  // Other threads may be blocked in read_some on this socket. Asio socket objects are not safe to
  // close() concurrently with an in-flight operation, so only shutdown() here (it wakes the
  // reader) and do the real close() after the worker threads have been joined.
  if (impl_) {
    boost::system::error_code ec;
    ignore(impl_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec));
  }
  { std::scoped_lock lock(kaMu_); }
  kaCv_.notify_all();
  const auto self = std::this_thread::get_id();
  if (reader_.joinable() && reader_.get_id() != self) {
    reader_.join();
  }
  if (keepAlive_.joinable() && keepAlive_.get_id() != self) {
    keepAlive_.join();
  }
  if (impl_ && reader_.get_id() == std::thread::id{} && keepAlive_.get_id() == std::thread::id{}) {
    // No reader remains; serialize with any request thread that is mid-write.
    std::scoped_lock lock(impl_->writeMu);
    boost::system::error_code ec;
    ignore(impl_->socket.close(ec));
  }
  // Anything still waiting will never be answered.
  std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> orphans;
  {
    std::scoped_lock lock(pendingMu_);
    orphans.swap(pending_);
  }
  for (auto& [serial, promise] : orphans) {
    promise->promise.set_value(Error{ErrorCode::kDisconnected, "connection closed"});
  }
}

void OpenDConnection::fail(const Error& error) {
  if (!connected_.exchange(false)) {
    return;
  }
  // shutdown() only: the fd is closed by close() once the reader thread has been joined.
  boost::system::error_code ec;
  ignore(impl_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec));
  { std::scoped_lock lock(kaMu_); }
  kaCv_.notify_all();

  std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> orphans;
  {
    std::scoped_lock lock(pendingMu_);
    orphans.swap(pending_);
  }
  for (auto& [serial, promise] : orphans) {
    promise->promise.set_value(error);
  }
  if (onDisconnect_ && !stopping_.load()) {
    onDisconnect_(error);
  }
}

void OpenDConnection::readerLoop() {
  FrameDecoder decoder(config_.maxBody);
  std::vector<std::uint8_t> chunk(std::size_t{64} * 1024);
  while (connected_.load()) {
    boost::system::error_code ec;
    const std::size_t got = impl_->socket.read_some(asio::buffer(chunk), ec);
    if (ec) {
      fail(Error{ErrorCode::kDisconnected, "read failed: " + ec.message()});
      return;
    }
    decoder.feed(chunk.data(), got);
    while (auto frame = decoder.next()) {
      std::shared_ptr<Pending> waiter;
      {
        std::scoped_lock lock(pendingMu_);
        const auto found = pending_.find(frame->serial);
        if (found != pending_.end()) {
          if (found->second->protoId == frame->protoId) {
            waiter = std::move(found->second);
            pending_.erase(found);
          } else {
            ++mismatched_;  // a serial collision with a push must not steal a request's reply
          }
        }
      }
      if (waiter) {
        waiter->promise.set_value(std::move(frame->body));
      } else if (onPush_) {
        onPush_(*frame);
      }
    }
    if (decoder.error() != DecodeError::kNone) {
      fail(Error{ErrorCode::kProtocol, "corrupt stream (decode error " +
                                           std::to_string(static_cast<int>(decoder.error())) +
                                           ")"});
      return;
    }
  }
}

void OpenDConnection::keepAliveLoop() {
  const auto interval = config_.keepAliveOverride.value_or(std::chrono::milliseconds(
      std::max(1, session_.keepAliveSeconds * 800)));  // 80% of the advised interval
  while (connected_.load()) {
    {
      std::unique_lock lock(kaMu_);
      kaCv_.wait_for(lock, interval, [&] { return !connected_.load(); });
    }
    if (!connected_.load()) {
      return;
    }
    KeepAlive::Request req;
    req.mutable_c2s()->set_time(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
    const auto rsp =
        requestInternal(protoId::kKeepAlive, req.SerializeAsString(), config_.requestTimeout);
    if (!rsp) {
      fail(Error{ErrorCode::kDisconnected, "keep-alive failed: " + rsp.error().message});
      return;
    }
  }
}

Result<std::vector<std::uint8_t>> OpenDConnection::request(std::uint32_t protoId,
                                                           const std::string& body) {
  return requestInternal(protoId, body, config_.requestTimeout);
}

Result<std::vector<std::uint8_t>> OpenDConnection::request(std::uint32_t protoId,
                                                           const std::string& body,
                                                           std::chrono::milliseconds timeout) {
  return requestInternal(protoId, body, timeout);
}

Result<std::vector<std::uint8_t>> OpenDConnection::requestInternal(
    std::uint32_t protoId, const std::string& body, std::chrono::milliseconds timeout) {
  if (!connected_.load()) {
    return Error{ErrorCode::kDisconnected, "not connected"};
  }
  const std::uint32_t serial = nextSerial_.fetch_add(1);
  auto promise = std::make_shared<Pending>();
  promise->protoId = protoId;
  auto future = promise->promise.get_future();
  {
    std::scoped_lock lock(pendingMu_);
    pending_[serial] = promise;
  }

  const auto frame =
      encodeFrame(protoId, serial, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  {
    // The write happens without pendingMu_ held; writes are serialized so frames never interleave.
    std::scoped_lock lock(impl_->writeMu);
    boost::system::error_code ec;
    asio::write(impl_->socket, asio::buffer(frame), ec);
    if (ec) {
      {
        std::scoped_lock plock(pendingMu_);
        pending_.erase(serial);
      }
      const Error err{ErrorCode::kDisconnected, "write failed: " + ec.message()};
      fail(err);
      return err;
    }
  }

  if (future.wait_for(timeout) != std::future_status::ready) {
    std::scoped_lock lock(pendingMu_);
    pending_.erase(serial);
    return Error{ErrorCode::kTimeout, "request timed out (proto " + std::to_string(protoId) + ")"};
  }
  return future.get();
}

}  // namespace futu_trader::opend
