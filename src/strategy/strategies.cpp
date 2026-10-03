#include "futu_trader/strategy/strategies.hpp"

namespace futu_trader::strategy {

namespace {
__extension__ using Int128 = __int128;
}

void BuyAndHold::onQuote(const QuoteEvent& quote, StrategyContext& ctx) {
  if (done_ || quote.symbol != symbol_) {
    return;
  }
  done_ = true;
  ctx.submit(symbol_, Side::kBuy, qty_, quote.ask);
}

bool MeanReversion::zBelow(Money mid, int kx10) const {
  // With n samples, sum S and sum of squares Q: mean = S/n and n^2 * variance = n*Q - S^2 = V.
  // z = (mid - mean) / sd = dev / sqrt(V) where dev = n*mid - S. So z < -k  <=>  dev < 0 and
  // dev^2 * 100 > kx10^2 * V. Everything is integer, so there is no rounding to disagree on.
  const auto n = static_cast<Int128>(count_);
  const Int128 variance = (n * sumSq_) - (sum_ * sum_);
  const Int128 dev = (n * mid) - sum_;
  if (variance <= 0 || dev >= 0) {
    return false;
  }
  return dev * dev * 100 > static_cast<Int128>(kx10) * kx10 * variance;
}

void MeanReversion::onQuote(const QuoteEvent& quote, StrategyContext& ctx) {
  if (quote.symbol != params_.symbol) {
    return;
  }
  const Money mid = quote.mid();
  if (mids_.size() != params_.window) {
    mids_.assign(params_.window, 0);  // first call only
  }
  if (count_ == params_.window) {
    const Money oldest = mids_[head_];
    sum_ -= oldest;
    sumSq_ -= static_cast<Int128>(oldest) * oldest;
    mids_[head_] = mid;
    if (++head_ == params_.window) {
      head_ = 0;  // compare-and-wrap: an integer modulo here costs more than the rest of the call
    }
  } else {
    std::size_t slot = head_ + count_;
    if (slot >= params_.window) {
      slot -= params_.window;
    }
    mids_[slot] = mid;
    ++count_;
  }
  sum_ += mid;
  sumSq_ += static_cast<Int128>(mid) * mid;
  if (count_ < params_.window || ctx.hasLiveOrder(params_.symbol)) {
    return;
  }
  const std::int64_t held = ctx.position(params_.symbol);
  if (held == 0) {
    if (zBelow(mid, params_.entryZx10)) {
      ctx.submit(params_.symbol, Side::kBuy, params_.qty, quote.ask);
    }
  } else if (held > 0 && !zBelow(mid, params_.exitZx10)) {
    ctx.submit(params_.symbol, Side::kSell, held, quote.bid);
  }
}

void PassiveMaker::onQuote(const QuoteEvent& quote, StrategyContext& ctx) {
  if (quote.symbol != params_.symbol) {
    return;
  }
  ++quoteCount_;
  const auto live = ctx.liveOrderIds(params_.symbol);
  if (!live.empty()) {
    if (quoteCount_ - placedAt_ >= params_.cancelAfterQuotes) {
      for (const auto& id : live) {
        (void)ctx.cancel(id);  // may lose the race against a fill: that is the point
      }
    }
    return;
  }
  const std::int64_t held = ctx.position(params_.symbol);
  oms::SubmitResult placed;
  if (held == 0) {
    placed = ctx.submit(params_.symbol, Side::kBuy, params_.qty, quote.bid);
  } else if (held > 0 && held % 100 == 0) {
    placed = ctx.submit(params_.symbol, Side::kSell, held, quote.ask);
  }
  if (placed.status == oms::SubmitStatus::kAccepted) {
    placedAt_ = quoteCount_;
  }
}

void RandomTrader::onQuote(const QuoteEvent& quote, StrategyContext& ctx) {
  if (quote.symbol != symbol_) {
    return;
  }
  ++seen_;
  if (seen_ % everyN_ != 0 || ctx.hasLiveOrder(symbol_)) {
    return;
  }
  if ((ctx.random() & 1U) == 0) {
    return;  // coin says: do nothing this time
  }
  const std::int64_t held = ctx.position(symbol_);
  if (held == 0) {
    ctx.submit(symbol_, Side::kBuy, qty_, quote.ask);
  } else if (held > 0) {
    ctx.submit(symbol_, Side::kSell, held, quote.bid);
  }
}

void FuturePeeker::onQuote(const QuoteEvent& quote, StrategyContext& ctx) {
  const std::size_t i = index_++;
  if (quote.symbol != symbol_ || i + 1 >= future_->size() || ctx.hasLiveOrder(symbol_)) {
    return;
  }
  const Money nextMid = (*future_)[i + 1].mid();  // <- the cheat: data from the future
  const std::int64_t held = ctx.position(symbol_);
  if (held == 0 && nextMid > quote.mid()) {
    ctx.submit(symbol_, Side::kBuy, qty_, quote.ask);
  } else if (held > 0 && nextMid < quote.mid()) {
    ctx.submit(symbol_, Side::kSell, held, quote.bid);
  }
}

}  // namespace futu_trader::strategy
