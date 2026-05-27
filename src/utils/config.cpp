#include "futu_trader/utils/config.hpp"

#include <fstream>
#include <sstream>

namespace futu_trader {

std::unordered_map<std::string, std::string> Config::ParseSimpleYaml(const std::string& path) {
  std::unordered_map<std::string, std::string> kv;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
      key.pop_back();
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '"')) {
      value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '"' || value.back() == '\r' || value.back() == ' ')) {
      value.pop_back();
    }
    kv[key] = value;
  }
  return kv;
}

}  // namespace futu_trader
