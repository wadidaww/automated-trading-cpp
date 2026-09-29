#pragma once

#include <string>
#include <utility>
#include <variant>

namespace futu_trader {

enum class ErrorCode : unsigned char {
  kOk,
  kDisconnected,
  kTimeout,
  kProtocol,  // malformed frame / unexpected message
  kServer,    // OpenD answered with retType != 0
  kInvalidArg,
  kUnsupported,
};

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct Error {
  Error() = default;
  Error(ErrorCode errorCode, std::string text) : code(errorCode), message(std::move(text)) {}

  ErrorCode code{ErrorCode::kOk};
  std::string message;
};

/** Minimal value-or-error type (std::expected is C++23). */
template <typename T>
class Result {
 public:
  Result(T value) : v_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
  Result(Error error) : v_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const { return std::holds_alternative<T>(v_); }
  explicit operator bool() const { return ok(); }
  T& value() { return std::get<T>(v_); }
  const T& value() const { return std::get<T>(v_); }
  const Error& error() const { return std::get<Error>(v_); }

 private:
  std::variant<T, Error> v_;
};

}  // namespace futu_trader
