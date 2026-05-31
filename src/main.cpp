#include <iostream>
#include <string>

#include "futu_trader/api/futu_client.hpp"

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--health-check") {
    std::cout << "ok\n";
    return 0;
  }
  futu_trader::FutuClient client({});
  client.connect();
  std::cout << "futu_trader running\n";
  return 0;
}
