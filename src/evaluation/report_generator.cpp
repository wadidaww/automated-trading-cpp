#include "futu_trader/evaluation/report_generator.hpp"

#include <fstream>

namespace futu_trader {

bool ReportGenerator::WriteJson(const BacktestMetrics& metrics, const std::string& path) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << "{\n"
      << "  \"total_return\": " << metrics.total_return << ",\n"
      << "  \"annualized_return\": " << metrics.annualized_return << ",\n"
      << "  \"annualized_volatility\": " << metrics.annualized_volatility << ",\n"
      << "  \"sharpe\": " << metrics.sharpe << ",\n"
      << "  \"max_drawdown\": " << metrics.max_drawdown << ",\n"
      << "  \"win_rate\": " << metrics.win_rate << "\n"
      << "}\n";
  return true;
}

bool ReportGenerator::WriteHtml(const BacktestMetrics& metrics, const std::string& path) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << "<html><body><h1>Backtest Report</h1>"
      << "<ul><li>Total Return: " << metrics.total_return << "</li>"
      << "<li>Sharpe: " << metrics.sharpe << "</li>"
      << "<li>Max Drawdown: " << metrics.max_drawdown << "</li></ul></body></html>";
  return true;
}

}  // namespace futu_trader
