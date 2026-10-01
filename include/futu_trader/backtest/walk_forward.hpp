#pragma once

#include <cstddef>
#include <vector>

namespace futu_trader::backtest {

/** Half-open index ranges into a time-ordered sample: [trainBegin, trainEnd), [testBegin, testEnd).
 */
struct Split {
  std::size_t trainBegin{0};
  std::size_t trainEnd{0};
  std::size_t testBegin{0};
  std::size_t testEnd{0};
};

/**
 * Walk-forward splits with an embargo. The test window starts `embargo` samples after the
 * training window ends, so features/labels that look ahead by up to `embargo` samples cannot leak
 * test information into training (purging). Each fold advances by `testLen`, so test windows
 * never overlap. `expanding` keeps the training start fixed at 0; otherwise it is a rolling
 * window of `trainLen`. Returns no folds for degenerate arguments or if nothing fits.
 */
std::vector<Split> walkForwardSplits(std::size_t n, std::size_t trainLen, std::size_t testLen,
                                     std::size_t embargo, bool expanding);

}  // namespace futu_trader::backtest
