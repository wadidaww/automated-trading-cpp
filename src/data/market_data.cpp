#include "futu_trader/data/market_data.hpp"

namespace futu_trader {

DataStore::DataStore(std::size_t max_size) : max_size_(max_size) {}

void DataStore::Push(const Tick& tick) {
  std::scoped_lock lock(mu_);
  ticks_.push_back(tick);
  while (ticks_.size() > max_size_) {
    ticks_.pop_front();
  }
}

std::vector<Tick> DataStore::Snapshot() const {
  std::scoped_lock lock(mu_);
  return {ticks_.begin(), ticks_.end()};
}

MarketDataFeed::MarketDataFeed(FutuClient& client) : client_(client) {}

bool MarketDataFeed::Subscribe(const std::string& symbol) { return client_.Subscribe(symbol); }

std::optional<Tick> MarketDataFeed::Poll(const std::string& symbol) const {
  return client_.GetBasicQot(symbol);
}

HistoricalDataFetcher::HistoricalDataFetcher(FutuClient& client) : client_(client) {}

std::vector<Tick> HistoricalDataFetcher::Fetch(const std::string& symbol, std::size_t bars) const {
  return client_.GetKL(symbol, bars);
}

}  // namespace futu_trader
