// torchlight - cache des résultats de D3DXCompileShader (voir d3dx_cache.h).

#include "d3dx_cache.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace torchlight::natif::d3dx {
namespace {

// Entrée du fichier : en-tête puis microcode puis code intermédiaire.
struct Header {
  char magic[4];
  uint32_t version;
  uint64_t key;
  uint32_t code_bytes, intermediate_bytes;
};
constexpr uint32_t kVersion = 1;

std::mutex g_mutex;
std::string g_path;
std::unordered_map<uint64_t, Result> g_entries;
uint32_t g_loaded = 0, g_found = 0, g_added = 0;
std::mutex g_file_mutex;

}  // namespace

void Open(const std::string& path) {
  std::string data;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    if (std::ferror(f)) data.clear();
    std::fclose(f);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t pos = 0;
  while (data.size() - pos >= sizeof(Header)) {
    Header h;
    std::memcpy(&h, data.data() + pos, sizeof(h));
    const size_t size = sizeof(h) + size_t(h.code_bytes) + h.intermediate_bytes;
    if (std::memcmp(h.magic, "TLDX", 4) != 0 || h.version != kVersion || h.code_bytes == 0 ||
        data.size() - pos < size)
      break;
    const char* p = data.data() + pos + sizeof(h);
    g_entries[h.key] = {std::string(p, h.code_bytes), std::string(p + h.code_bytes, h.intermediate_bytes)};
    pos += size;
  }
  g_loaded = uint32_t(g_entries.size());
  // Fin illisible (écriture interrompue, autre version) : fichier réécrit avec la partie valide, sinon les entrées
  // ajoutées ensuite seraient perdues derrière elle.
  if (pos != data.size()) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(data.data(), 1, pos, f);
      std::fclose(f);
    }
  }
  g_path = path;
}

bool Active() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return !g_path.empty();
}

bool Find(uint64_t key, Result& out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_entries.find(key);
  if (it == g_entries.end()) return false;
  out = it->second;
  g_found++;
  return true;
}

std::string Add(uint64_t key, Result result) {
  if (result.code.empty()) return {};
  Header h{{'T', 'L', 'D', 'X'}, kVersion, key, uint32_t(result.code.size()),
           uint32_t(result.intermediate.size())};
  std::string block(reinterpret_cast<const char*>(&h), sizeof(h));
  block += result.code;
  block += result.intermediate;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_path.empty() || !g_entries.emplace(key, std::move(result)).second) return {};
  g_added++;
  return block;
}

void Append(const std::string& block) {
  if (block.empty()) return;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    path = g_path;
  }
  std::lock_guard<std::mutex> lock(g_file_mutex);
  if (FILE* f = std::fopen(path.c_str(), "ab")) {
    std::fwrite(block.data(), 1, block.size(), f);
    std::fclose(f);
  }
}

std::string Summary() {
  std::lock_guard<std::mutex> lock(g_mutex);
  char line[128];
  std::snprintf(line, sizeof(line), "%u lus au démarrage, %u trouvés, %u ajoutés", g_loaded, g_found, g_added);
  return line;
}

}  // namespace torchlight::natif::d3dx
