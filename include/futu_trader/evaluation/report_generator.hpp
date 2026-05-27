#pragma once

#include <string>

#include "futu_trader/evaluation/backtester.hpp"

namespace futu_trader {

class ReportGenerator {
 public:
  static bool WriteJson(const BacktestMetrics& metrics, const std::string& path);
  static bool WriteHtml(const BacktestMetrics& metrics, const std::string& path);
};

}  // namespace futu_trader
