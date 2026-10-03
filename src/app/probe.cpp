#include "futu_trader/app/probe.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>

namespace futu_trader::app {
namespace {

struct Fd {
  int fd;
  ~Fd() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

bool waitFor(int fd, short events, std::chrono::steady_clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (left.count() <= 0) {
    return false;
  }
  pollfd p{fd, events, 0};
  return ::poll(&p, 1, static_cast<int>(left.count())) == 1;
}

}  // namespace

bool probeReady(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const Fd sock{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (sock.fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  // Loopback connect completes or fails immediately; the deadline guards the reads below.
  if (::connect(sock.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return false;
  }
  const std::string request =
      "GET /readyz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  if (::write(sock.fd, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
    return false;
  }
  std::string response;
  std::array<char, 256> buf{};
  while (response.size() < 12 && waitFor(sock.fd, POLLIN, deadline)) {
    const ssize_t n = ::read(sock.fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    response.append(buf.data(), static_cast<std::size_t>(n));
  }
  return response.starts_with("HTTP/1.1 200");
}

}  // namespace futu_trader::app
