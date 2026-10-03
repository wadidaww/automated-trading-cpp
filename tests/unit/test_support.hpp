#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#include "futu_trader/oms/live_gate.hpp"

namespace futu_trader::testing_support {

/**
 * Polls an observable condition against a deadline. Never "sleep and hope" (CLAUDE.md rule 8):
 * returns as soon as the condition holds, and judges it one last time at the deadline.
 */
inline bool waitFor(const std::function<bool()>& cond,
                    std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return cond();
}

/** A unique path in the temp dir, removed (with anything under it) on destruction. */
class ScopedPath {
 public:
  explicit ScopedPath(const std::string& name)
      : path_(std::filesystem::temp_directory_path() /
              (name + "_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_))) {
    std::filesystem::remove_all(path_);
  }
  ~ScopedPath() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  ScopedPath(const ScopedPath&) = delete;
  ScopedPath& operator=(const ScopedPath&) = delete;
  const std::filesystem::path& path() const { return path_; }
  std::string str() const { return path_.string(); }

 private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

inline std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void spit(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A well-formed live-gate input for `accId` with five clean SIMULATE days ending today.
inline oms::LiveGateInput validLiveInput(std::uint64_t accId,
                                         const std::string& today = "2026-09-06") {
  oms::LiveGateInput input;
  input.configuredAccId = accId;
  input.configPhrase = oms::LiveGate::expectedPhrase(accId);
  input.envVar = oms::LiveGate::kEnvValue;
  input.cliLiveFlag = true;
  input.tradeUnlocked = true;
  input.startupReconcileClean = true;
  input.today = today;
  for (int day = 1; day <= 5; ++day) {
    input.promotion.push_back({"2026-09-0" + std::to_string(day), true});
  }
  opend::TrdAccount real;
  real.env = TrdEnv::kReal;
  real.accId = accId;
  input.brokerAccounts = {real};
  return input;
}

// The only legitimate way to a REAL target is through the gate; tests do the same.
inline oms::TradeTarget makeRealTarget(std::uint64_t accId) {
  const auto approval = oms::LiveGate::approveReal(validLiveInput(accId));
  return oms::TradeTarget::real(approval.value(), accId, opend::TrdMarket::kHK).value();
}

}  // namespace futu_trader::testing_support
