#pragma once

#include <string>

#include "futu_trader/evaluation/backtester.hpp"

namespace futu_trader {

class ReportGenerator {
 public:
  static bool writeJson(const BacktestMetrics& metrics, const std::string& path);
  static bool writeHtml(const BacktestMetrics& metrics, const std::string& path);
};

}  // namespace futu_trader
