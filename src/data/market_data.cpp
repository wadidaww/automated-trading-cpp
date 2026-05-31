#include "futu_trader/data/market_data.hpp"

namespace futu_trader {

DataStore::DataStore(std::size_t maxSize) : maxSize_(maxSize) {}

void DataStore::push(const Tick& tick) {
  std::scoped_lock lock(mu_);
  ticks_.push_back(tick);
  while (ticks_.size() > maxSize_) {
    ticks_.pop_front();
  }
}

std::vector<Tick> DataStore::snapshot() const {
  std::scoped_lock lock(mu_);
  return {ticks_.begin(), ticks_.end()};
}

MarketDataFeed::MarketDataFeed(FutuClient& client) : client_(client) {}

bool MarketDataFeed::subscribe(const std::string& symbol) { return client_.subscribe(symbol); }

std::optional<Tick> MarketDataFeed::poll(const std::string& symbol) const {
  return client_.getBasicQot(symbol);
}

HistoricalDataFetcher::HistoricalDataFetcher(FutuClient& client) : client_(client) {}

std::vector<Tick> HistoricalDataFetcher::fetch(const std::string& symbol, std::size_t bars) const {
  return client_.getKl(symbol, bars);
}

}  // namespace futu_trader
