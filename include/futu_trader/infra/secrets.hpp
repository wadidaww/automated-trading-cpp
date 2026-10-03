#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "futu_trader/core/result.hpp"

namespace futu_trader::infra {

/**
 * A secret held in a buffer that is wiped when it dies. Move-only, never printed (no stream
 * operator, `toString` is redacted), so it cannot leak through a log line by accident.
 *
 * Honest limit: this wipes OUR copy. Once the value is handed to a protobuf message, a std::string
 * or the socket it is copied into allocator-owned memory we do not control.
 */
class Secret {
 public:
  Secret() = default;
  explicit Secret(std::string_view value);
  ~Secret();
  Secret(Secret&& other) noexcept;
  Secret& operator=(Secret&& other) noexcept;
  Secret(const Secret&) = delete;
  Secret& operator=(const Secret&) = delete;

  /** The raw value. Callers should not copy it into a long-lived string or a log. */
  std::string_view reveal() const { return {data_, size_}; }
  bool empty() const { return size_ == 0; }
  std::size_t size() const { return size_; }
  /** Always "<redacted>". */
  const char* toString() const { return "<redacted>"; }

 private:
  void wipe();
  char* data_{nullptr};
  std::size_t size_{0};
};

inline constexpr std::size_t kMaxSecretFileBytes = 4096;

/**
 * Reads a secret from a file, refusing anything that is not private to this user:
 * not a regular file, a symlink, owned by someone else, readable or writable by group/other, empty,
 * or larger than kMaxSecretFileBytes. One trailing newline is stripped. Errors never contain the
 * file's content.
 */
Result<Secret> readSecretFile(const std::string& path);

/**
 * Process hardening to call once at startup, before secrets are loaded: no core dumps
 * (RLIMIT_CORE=0, PR_SET_DUMPABLE=0, which also blocks ptrace by non-root peers) and a best-effort
 * mlockall so secrets and the order path are not paged to swap. Each step reports separately; a
 * failed mlockall (RLIMIT_MEMLOCK) is common and not fatal.
 */
struct HardeningReport {
  bool coreDumpsDisabled{false};
  bool dumpableCleared{false};
  bool memoryLocked{false};
};
HardeningReport hardenProcess(bool lockMemory);

}  // namespace futu_trader::infra
