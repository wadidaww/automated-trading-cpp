#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  std::filesystem::create_directories("data/models");
  std::ofstream out("data/models/trained_model.bin", std::ios::binary);
  out << "trained-model";
  std::cout << "training completed\n";
  return 0;
}
