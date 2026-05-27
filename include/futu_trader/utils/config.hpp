#pragma once

#include <string>
#include <unordered_map>

namespace futu_trader {

class Config {
 public:
  static std::unordered_map<std::string, std::string> ParseSimpleYaml(const std::string& path);
};

}  // namespace futu_trader
