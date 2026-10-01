#include "futu_trader/backtest/walk_forward.hpp"

namespace futu_trader::backtest {

std::vector<Split> walkForwardSplits(std::size_t n, std::size_t trainLen, std::size_t testLen,
                                     std::size_t embargo, bool expanding) {
  std::vector<Split> folds;
  if (trainLen == 0 || testLen == 0) {
    return folds;
  }
  for (std::size_t k = 0;; ++k) {
    const std::size_t trainEnd = trainLen + (k * testLen);
    const std::size_t testBegin = trainEnd + embargo;
    const std::size_t testEnd = testBegin + testLen;
    if (testEnd > n) {
      break;
    }
    const std::size_t trainBegin = expanding ? 0 : k * testLen;
    folds.push_back({trainBegin, trainEnd, testBegin, testEnd});
  }
  return folds;
}

}  // namespace futu_trader::backtest
