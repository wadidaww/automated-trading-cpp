#pragma once

#include <string>

#include "futu_trader/opend/client.hpp"

namespace futu_trader::app {

/**
 * Owns the exit of the OpenD link. release() (also run by the destructor, idempotent) re-locks
 * trading if this process unlocked it, then closes the connection, which joins the reader thread.
 * A refused or finished process must not leave the real account unlocked for any other local
 * OpenD client, on ANY exit path: that is what an RAII guard is for.
 */
class LinkGuard {
 public:
  explicit LinkGuard(opend::OpenDClient& client) : client_(client) {}
  ~LinkGuard() { release(); }
  LinkGuard(const LinkGuard&) = delete;
  LinkGuard& operator=(const LinkGuard&) = delete;

  void markUnlocked() { unlocked_ = true; }
  void release() {
    if (released_) {
      return;
    }
    released_ = true;
    if (unlocked_) {
      static_cast<void>(client_.unlockTrade(false, std::string()));
    }
    client_.close();
  }

 private:
  opend::OpenDClient& client_;
  bool unlocked_{false};
  bool released_{false};
};

}  // namespace futu_trader::app
