// torchlight - un shader compilé du cache du rendu natif en un seul fichier (<nom>.tlsh) : SPIR-V, réflexion
// glslang (texte, même format que glslangValidator -q) et emplacements (.io). Un seul fichier écrit d'un trait
// et relu d'un trait : sur la carte SD de la console, chaque ouverture, renommage ou suppression coûte cher.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace torchlight::natif::pack {

struct Header {
  char magic[4];
  uint32_t version, spirv_bytes, refl_bytes, io_bytes;
};
constexpr uint32_t kVersion = 1;

inline bool Write(const std::string& path, const std::vector<uint32_t>& spirv, const std::string& refl,
                  const std::string& io) {
  Header h{{'T', 'L', 'S', 'H'}, kVersion, uint32_t(spirv.size() * 4), uint32_t(refl.size()), uint32_t(io.size())};
  std::string data(sizeof(h) + h.spirv_bytes + h.refl_bytes + h.io_bytes, '\0');
  std::memcpy(data.data(), &h, sizeof(h));
  std::memcpy(data.data() + sizeof(h), spirv.data(), h.spirv_bytes);
  std::memcpy(data.data() + sizeof(h) + h.spirv_bytes, refl.data(), h.refl_bytes);
  std::memcpy(data.data() + sizeof(h) + h.spirv_bytes + h.refl_bytes, io.data(), h.io_bytes);
  // Fichier temporaire renommé une fois complet : le rendu, qui cherche les shaders pendant la compilation, ne voit
  // jamais un fichier à moitié écrit, et ne tient jamais ouvert le fichier que le compilateur veut écrire.
  const std::string tmp = path + ".tmp";
  for (int attempt = 0; attempt < 3; attempt++) {
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) continue;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0 || !ok) continue;
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
  }
  std::remove(tmp.c_str());
  return false;
}

// Faux si le fichier est absent, d'un autre format ou incomplet (écriture interrompue).
inline bool Read(const std::string& path, std::vector<uint32_t>& spirv, std::string& refl, std::string& io) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string data;
  char buf[16384];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  Header h;
  if (failed || data.size() < sizeof(h)) return false;
  std::memcpy(&h, data.data(), sizeof(h));
  if (std::memcmp(h.magic, "TLSH", 4) != 0 || h.version != kVersion || h.spirv_bytes % 4 ||
      data.size() != sizeof(h) + size_t(h.spirv_bytes) + h.refl_bytes + h.io_bytes)
    return false;
  spirv.resize(h.spirv_bytes / 4);
  std::memcpy(spirv.data(), data.data() + sizeof(h), h.spirv_bytes);
  refl.assign(data, sizeof(h) + h.spirv_bytes, h.refl_bytes);
  io.assign(data, sizeof(h) + h.spirv_bytes + h.refl_bytes, h.io_bytes);
  return true;
}

// Fichier présent et complet, sans le lire en entier.
inline bool Valid(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  Header h;
  bool ok = std::fread(&h, 1, sizeof(h), f) == sizeof(h) && std::memcmp(h.magic, "TLSH", 4) == 0 &&
            h.version == kVersion;
  if (ok && std::fseek(f, 0, SEEK_END) == 0)
    ok = long(sizeof(h)) + long(h.spirv_bytes) + long(h.refl_bytes) + long(h.io_bytes) == std::ftell(f);
  std::fclose(f);
  return ok;
}

}  // namespace torchlight::natif::pack
