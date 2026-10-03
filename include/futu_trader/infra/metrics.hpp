#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/infra/histogram.hpp"

namespace futu_trader::infra {

using Labels = std::vector<std::pair<std::string, std::string>>;

/**
 * Prometheus metrics in the text exposition format (v0.0.4), pull model: a metric is a function
 * that reads a live value (usually an atomic the component already maintains) at scrape time. The
 * hot path therefore does no extra bookkeeping, and there is no second copy of any counter that
 * could drift from the real one.
 *
 * Registration validates names and labels (a bad name would make the whole scrape unparseable) and
 * rejects reusing a name with a different type. Counters must end in `_total`. Latency histograms
 * are exposed in seconds with fixed buckets from 1 us to 1 s.
 */
class MetricsRegistry {
 public:
  using CounterFn = std::function<std::uint64_t()>;
  using GaugeFn = std::function<double()>;

  Result<bool> addCounter(const std::string& name, const std::string& help, CounterFn read,
                          Labels labels = {});
  Result<bool> addGauge(const std::string& name, const std::string& help, GaugeFn read,
                        Labels labels = {});
  /** `histogram` must outlive the registry. Values are nanoseconds; exposed as seconds. */
  Result<bool> addLatencyHistogram(const std::string& name, const std::string& help,
                                   const LatencyHistogram& histogram, Labels labels = {});

  /** The full exposition. Safe to call concurrently with the values changing. */
  std::string render() const;

  static bool validName(const std::string& name);
  static bool validLabelName(const std::string& name);

 private:
  enum class Kind : std::uint8_t { kCounter, kGauge, kHistogram };
  struct Metric {
    std::string name;
    std::string help;
    Kind kind{Kind::kGauge};
    Labels labels;
    CounterFn counter;
    GaugeFn gauge;
    const LatencyHistogram* histogram{nullptr};
  };

  Result<bool> add(Metric metric);

  mutable std::mutex mu_;
  std::vector<Metric> metrics_;
};

}  // namespace futu_trader::infra
