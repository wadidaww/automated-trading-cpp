#include "futu_trader/infra/metrics.hpp"

#include <array>
#include <cstdio>
#include <map>
#include <set>

namespace futu_trader::infra {

namespace {

// Histogram bucket upper bounds in nanoseconds, exposed as seconds.
constexpr std::array<std::uint64_t, 17> kBucketBoundsNs{
    1'000,      2'000,      5'000,      10'000,      20'000,       50'000,
    100'000,    200'000,    500'000,    1'000'000,   2'000'000,    5'000'000,
    10'000'000, 20'000'000, 50'000'000, 100'000'000, 1'000'000'000};

std::string formatDouble(double value) {
  std::array<char, 40> buf{};
  std::snprintf(buf.data(), buf.size(), "%.9g", value);
  return buf.data();
}

std::string escapeLabelValue(const std::string& value) {
  std::string out;
  for (const char c : value) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

std::string escapeHelp(const std::string& help) {
  std::string out;
  for (const char c : help) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

std::string labelString(const Labels& labels, const std::string& extraKey = "",
                        const std::string& extraValue = "") {
  if (labels.empty() && extraKey.empty()) {
    return "";
  }
  std::string out = "{";
  bool first = true;
  for (const auto& [key, value] : labels) {
    out += (first ? "" : ",") + key + "=\"" + escapeLabelValue(value) + "\"";
    first = false;
  }
  if (!extraKey.empty()) {
    out += (first ? "" : ",") + extraKey + "=\"" + extraValue + "\"";
  }
  return out + "}";
}

const char* typeName(int kind) {
  switch (kind) {
    case 0:
      return "counter";
    case 1:
      return "gauge";
    default:
      return "histogram";
  }
}

}  // namespace

bool MetricsRegistry::validName(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || (digit && i > 0))) {
      return false;
    }
  }
  return true;
}

bool MetricsRegistry::validLabelName(const std::string& name) {
  if (name.empty() || name.rfind("__", 0) == 0) {  // "__" prefix is reserved for Prometheus
    return false;
  }
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || (digit && i > 0))) {
      return false;
    }
  }
  return true;
}

Result<bool> MetricsRegistry::add(Metric metric) {
  if (!validName(metric.name)) {
    return Error{ErrorCode::kInvalidArg, "invalid metric name '" + metric.name + "'"};
  }
  for (const auto& [key, value] : metric.labels) {
    if (!validLabelName(key) || key == "le") {
      return Error{ErrorCode::kInvalidArg, "invalid label name '" + key + "'"};
    }
  }
  if (metric.kind == Kind::kCounter &&
      (metric.name.size() < 6 || metric.name.compare(metric.name.size() - 6, 6, "_total") != 0)) {
    return Error{ErrorCode::kInvalidArg, "counter '" + metric.name + "' must end in _total"};
  }
  std::scoped_lock lock(mu_);
  for (const auto& existing : metrics_) {
    if (existing.name == metric.name) {
      if (existing.kind != metric.kind) {
        return Error{ErrorCode::kInvalidArg,
                     "metric '" + metric.name + "' redefined with another type"};
      }
      if (existing.labels == metric.labels) {
        return Error{ErrorCode::kInvalidArg, "metric '" + metric.name + "' registered twice"};
      }
    }
  }
  metrics_.push_back(std::move(metric));
  return true;
}

Result<bool> MetricsRegistry::addCounter(const std::string& name, const std::string& help,
                                         CounterFn read, Labels labels) {
  Metric m;
  m.name = name;
  m.help = help;
  m.kind = Kind::kCounter;
  m.labels = std::move(labels);
  m.counter = std::move(read);
  return add(std::move(m));
}

Result<bool> MetricsRegistry::addGauge(const std::string& name, const std::string& help,
                                       GaugeFn read, Labels labels) {
  Metric m;
  m.name = name;
  m.help = help;
  m.kind = Kind::kGauge;
  m.labels = std::move(labels);
  m.gauge = std::move(read);
  return add(std::move(m));
}

Result<bool> MetricsRegistry::addLatencyHistogram(const std::string& name, const std::string& help,
                                                  const LatencyHistogram& histogram,
                                                  Labels labels) {
  Metric m;
  m.name = name;
  m.help = help;
  m.kind = Kind::kHistogram;
  m.labels = std::move(labels);
  m.histogram = &histogram;
  return add(std::move(m));
}

std::string MetricsRegistry::render() const {
  std::scoped_lock lock(mu_);
  // Group by name (HELP/TYPE once per family), keeping first-registration order.
  std::vector<std::string> order;
  std::map<std::string, std::vector<const Metric*>> families;
  for (const auto& metric : metrics_) {
    if (families.find(metric.name) == families.end()) {
      order.push_back(metric.name);
    }
    families[metric.name].push_back(&metric);
  }
  std::string out;
  for (const auto& name : order) {
    const auto& family = families[name];
    out += "# HELP " + name + " " + escapeHelp(family.front()->help) + "\n";
    out += "# TYPE " + name + " " + typeName(static_cast<int>(family.front()->kind)) + "\n";
    for (const Metric* m : family) {
      switch (m->kind) {
        case Kind::kCounter:
          out += name + labelString(m->labels) + " " + std::to_string(m->counter()) + "\n";
          break;
        case Kind::kGauge:
          out += name + labelString(m->labels) + " " + formatDouble(m->gauge()) + "\n";
          break;
        case Kind::kHistogram: {
          const LatencyHistogram& h = *m->histogram;
          const std::uint64_t total = h.count();
          for (const std::uint64_t bound : kBucketBoundsNs) {
            out += name + "_bucket" +
                   labelString(m->labels, "le", formatDouble(static_cast<double>(bound) / 1e9)) +
                   " " + std::to_string(std::min(h.countAtOrBelow(bound), total)) + "\n";
          }
          out += name + "_bucket" + labelString(m->labels, "le", "+Inf") + " " +
                 std::to_string(total) + "\n";
          out += name + "_sum" + labelString(m->labels) + " " +
                 formatDouble(h.mean() * static_cast<double>(total) / 1e9) + "\n";
          out += name + "_count" + labelString(m->labels) + " " + std::to_string(total) + "\n";
          break;
        }
      }
    }
  }
  return out;
}

}  // namespace futu_trader::infra
