#include "futu_trader/evaluation/report_generator.hpp"

#include <fstream>

namespace futu_trader {

bool ReportGenerator::writeJson(const BacktestMetrics& metrics, const std::string& path) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << "{\n"
      << "  \"totalReturn\": " << metrics.totalReturn << ",\n"
      << "  \"annualizedReturn\": " << metrics.annualizedReturn << ",\n"
      << "  \"annualizedVolatility\": " << metrics.annualizedVolatility << ",\n"
      << "  \"sharpe\": " << metrics.sharpe << ",\n"
      << "  \"maxDrawdown\": " << metrics.maxDrawdown << ",\n"
      << "  \"winRate\": " << metrics.winRate << "\n"
      << "}\n";
  return true;
}

bool ReportGenerator::writeHtml(const BacktestMetrics& metrics, const std::string& path) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << "<html><body><h1>Backtest Report</h1>"
      << "<ul><li>Total Return: " << metrics.totalReturn << "</li>"
      << "<li>Sharpe: " << metrics.sharpe << "</li>"
      << "<li>Max Drawdown: " << metrics.maxDrawdown << "</li></ul></body></html>";
  return true;
}

}  // namespace futu_trader
