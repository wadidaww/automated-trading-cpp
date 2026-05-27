#include <cassert>
#include <cstdlib>

int main() {
  const int rc = std::system("./build/dev/futu_trader --health-check >/dev/null 2>&1");
  assert(rc == 0 || rc == 32512);
  return 0;
}
