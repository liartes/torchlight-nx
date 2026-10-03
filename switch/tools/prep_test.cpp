// Test : la préparation C++ des shaders (src/natif/hlsl_prep.cpp) donne exactement le résultat de
// switch/shaders/prep_hlsl.py. Usage, après build_lib.sh :
//   clang++-20 -std=c++20 -O2 -Isrc switch/tools/prep_test.cpp src/natif/hlsl_prep.cpp -o /tmp/prep_test
//   /tmp/prep_test switch/out-pc/hlsl switch/out-pc/shaders/prep assets/rtshaderlib
#include "natif/hlsl_prep.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
namespace fs = std::filesystem;
std::string Read(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
int main(int, char** argv) {
  fs::path hlsl = argv[1], prep = argv[2], lib = argv[3];
  std::map<std::string, long> opts;
  std::ifstream o(hlsl / "options.txt"); std::string h; long v;
  while (o >> h >> v) opts[h] = v;
  int ok = 0, ko = 0;
  for (auto& e : fs::directory_iterator(hlsl)) {
    if (e.path().extension() != ".hlsl") continue;
    const std::string name = e.path().filename().string();
    const bool vs = name.find("_vs_") != std::string::npos;
    auto it = opts.find(name.substr(0, name.find('_')));
    std::string io;
    const std::string out = torchlight::natif::PrepareHlslSource(Read(e.path()), vs, it == opts.end() ? -1 : it->second, io);
    const std::string ref = Read(prep / "src" / name), ref_io = Read(prep / "src" / (name.substr(0, name.size() - 5) + ".io"));
    if (out == ref && io == ref_io) ok++; else { ko++; if (ko < 4) std::cout << "DIFF " << name << (out == ref ? " (io)" : "") << "\n"; }
  }
  for (auto& e : fs::directory_iterator(lib)) {
    if (e.path().extension() != ".hlsl") continue;
    if (torchlight::natif::PrepareHlslLibraryFile(Read(e.path())) == Read(prep / "lib" / e.path().filename())) ok++; else { ko++; std::cout << "DIFF lib " << e.path().filename() << "\n"; }
  }
  std::cout << ok << " identiques, " << ko << " différents\n";
}
