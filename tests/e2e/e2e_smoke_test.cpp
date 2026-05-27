#include <cassert>
#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv) {
  const auto exe_dir = std::filesystem::path(argv[0]).parent_path();
  const auto trader_path = (exe_dir / "futu_trader").string();
  const int rc = std::system((trader_path + " --health-check >/dev/null 2>&1").c_str());
  assert(rc == 0);
  return 0;
}
