#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/core/types.hpp"

namespace futu_trader {

class DataStore {
 public:
  explicit DataStore(std::size_t max_size);
  void Push(const Tick& tick);
  std::vector<Tick> Snapshot() const;

 private:
  std::size_t max_size_;
  mutable std::mutex mu_;
  std::deque<Tick> ticks_;
};

class MarketDataFeed {
 public:
  explicit MarketDataFeed(FutuClient& client);
  bool Subscribe(const std::string& symbol);
  std::optional<Tick> Poll(const std::string& symbol) const;

 private:
  FutuClient& client_;
};

class HistoricalDataFetcher {
 public:
  explicit HistoricalDataFetcher(FutuClient& client);
  std::vector<Tick> Fetch(const std::string& symbol, std::size_t bars) const;

 private:
  FutuClient& client_;
};

}  // namespace futu_trader
