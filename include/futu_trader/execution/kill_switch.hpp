#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace futu_trader::execution {

/**
 * Global trading halt. Once tripped it stays tripped until a human calls reset(): automatic
 * recovery from a halt is exactly how small incidents become large ones.
 * tripped() is a single atomic load so it is safe to check on every order.
 *
 * With a persistence path the trip survives a crash or restart: a new process starts already
 * tripped until an operator resets it, so "human reset required" cannot be bypassed by bouncing
 * the process.
 */
class KillSwitch {
 public:
  KillSwitch() = default;
  /** If `persistPath` names an existing file, starts tripped with that file's content as reason. */
  explicit KillSwitch(std::string persistPath);

  void trip(const std::string& reason);
  bool tripped() const { return tripped_.load(std::memory_order_acquire); }
  /** True if a trip could not be written to disk (a restart would not be blocked). */
  bool persistFailed() const { return persistFailed_.load(std::memory_order_acquire); }
  std::string reason() const;

  /**
   * Human-only reset. The operator name must be non-empty (it is kept for the audit trail); an
   * empty name is refused and the switch stays tripped. Returns whether the reset happened.
   */
  bool reset(const std::string& operatorName);
  std::string lastResetBy() const;

  /**
   * Trips if the flag file exists, so an external watchdog can halt us without a socket. Fails
   * closed: if the path cannot be examined for any reason other than "does not exist" (permission
   * error, I/O error, unmounted volume) the switch trips too. A dangling symlink counts as present.
   */
  bool checkFlagFile(const std::string& path);

 private:
  std::atomic<bool> tripped_{false};
  std::atomic<bool> persistFailed_{false};
  mutable std::mutex mu_;
  std::string reason_;
  std::string resetBy_;
  std::string persistPath_;
};

}  // namespace futu_trader::execution
