#include <cassert>
#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv) {
  const auto exeDir = std::filesystem::path(argv[0]).parent_path();
  const auto traderPath = (exeDir / "futu_trader").string();
  const int rc = std::system((traderPath + " --health-check >/dev/null 2>&1").c_str());
  assert(rc == 0);
  return 0;
}
