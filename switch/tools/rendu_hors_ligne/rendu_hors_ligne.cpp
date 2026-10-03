// Rendu hors ligne d'une capture de Torchlight (phase 2 du rendu natif, switch/RENDU_NATIF.md).
//
// Usage : rendu_hors_ligne <dossier capture> <bibliothèque de shaders> <sortie.png> [largeur hauteur]
//   capture      : trace/capture/ produit par --torchlight_capture_s (frame.txt + vb_/ib_/tex_/cst_)
//   bibliothèque : sortie de switch/shaders/build_lib.sh (<hash>_<profil>_main.spv/.refl/.io)
//
// Redessine chaque « draw » de frame.txt avec le rendu natif du jeu (src/natif/rendu.cpp) dans une image
// hors écran, puis écrit un PNG. Sert de test de non-régression du rendu natif sur des captures.

#include <zlib.h>

#include "natif/rendu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Lecture de la capture
// ---------------------------------------------------------------------------------------------

struct Elem {
  uint32_t source, offset, type, semantic, index;
};
struct VB {
  uint32_t source;
  std::string file;
  uint32_t stride, count;
};
struct Tex {
  uint32_t unit;
  std::string file;
  uint32_t format, width, height, block_w, block_h, bpb;
  bool tiled;
  bool cube = false;
  std::string name;
};
struct Draw {
  int id = -1;
  std::string vs, ps;
  uint32_t type = 4;
  bool indexed = false;
  uint32_t vstart = 0, vcount = 0, istart = 0, icount = 0;
  std::vector<Elem> elems;
  std::vector<VB> vbs;
  std::string ib_file;
  uint32_t index_size = 2;
  std::vector<Tex> texs;
  uint32_t blend[6] = {0, 1, 0, 0, 1, 0};
  bool separate = false;
  uint32_t cull = 2;
  bool depth_check = true, depth_write = true;
  uint32_t depth_func = 3;
  std::string mask = "1111";
  uint32_t alpha_func = 1, alpha_value = 0;
  std::string cst_vs, cst_ps;
  std::vector<std::string> problems;
  // Pipeline fixe (dessin sans programme) : matrices Ogre et étages de combinaison.
  bool ffp = false;
  float matrix[3][16] = {};
  struct Stage {
    bool set = false;
    uint32_t op = 2, src1 = 1, src2 = 2;
    float arg[11] = {};  // colourArg1 rgba, colourArg2 rgba, alphaArg1, alphaArg2, factor
  } stage[4][2];         // [unité][0 = couleur, 1 = alpha]
};

// Événements d'une image du jeu, dans l'ordre : changement de cible, effacement, bornes de cible, dessin.
struct Event {
  enum Kind { kTarget, kClear, kBegin, kEnd, kDraw, kViewport } kind;
  int32_t vp[4] = {0, 0, 0, 0};  // viewport d'Ogre en pixels : gauche, haut, largeur, hauteur
  std::string name;
  uint32_t width = 0, height = 0;
  uint32_t buffers = 0;
  float colour[4] = {};
  float depth = 1.0f;
  size_t draw = 0;
};
std::vector<Event> g_events;
std::map<std::string, int> g_texture_problems;

std::vector<Draw> ReadFrame(const fs::path& file) {
  std::vector<Draw> draws;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string key;
    ss >> key;
    if (key == "draw") {
      draws.emplace_back();
      ss >> draws.back().id;
      g_events.push_back({Event::kDraw});
      g_events.back().draw = draws.size() - 1;
      continue;
    }
    if (key == "target") {
      // target <objet> <vtable> <nom> raw <mots> : largeur et hauteur de RenderTarget en +0x24 / +0x28.
      Event e{Event::kTarget};
      std::string obj, vt, raw;
      ss >> obj >> vt >> e.name >> raw;
      std::vector<uint32_t> words;
      std::string w;
      while (ss >> w) words.push_back(uint32_t(std::strtoul(w.c_str(), nullptr, 16)));
      if (words.size() > 10) {
        e.width = words[9];
        e.height = words[10];
      }
      g_events.push_back(e);
      continue;
    }
    if (key == "viewport") {
      // Viewport d'Ogre : mActLeft, mActTop, mActWidth, mActHeight aux mots 10 à 13.
      Event e{Event::kViewport};
      std::string obj, raw, w;
      ss >> obj >> raw;
      std::vector<uint32_t> words;
      while (ss >> w) words.push_back(uint32_t(std::strtoul(w.c_str(), nullptr, 16)));
      if (words.size() > 13)
        for (int k = 0; k < 4; k++) e.vp[k] = int32_t(words[10 + k]);
      g_events.push_back(e);
      continue;
    }
    if (key == "clear") {
      Event e{Event::kClear};
      ss >> e.buffers >> e.colour[0] >> e.colour[1] >> e.colour[2] >> e.colour[3] >> e.depth;
      g_events.push_back(e);
      continue;
    }
    if (key == "begin_frame") {
      g_events.push_back({Event::kBegin});
      continue;
    }
    if (key == "end_frame") {
      g_events.push_back({Event::kEnd});
      continue;
    }
    if (draws.empty()) continue;
    Draw& d = draws.back();
    if (key == "prog") {
      ss >> d.vs >> d.ps;
      if (d.vs == "0000000000000000" && d.ps == "0000000000000000") {
        d.ffp = true;  // shaders d'émulation du pipeline fixe de la bibliothèque (switch/shaders/ffp)
        d.vs = d.ps = "ffp";
      }
    } else if (key == "ffp_matrix") {
      std::string which;
      ss >> which;
      const int k = which == "world" ? 0 : which == "view" ? 1 : 2;
      for (float& v : d.matrix[k]) ss >> v;
    } else if (key == "ffp_stage") {
      uint32_t unit;
      std::string kind;
      ss >> unit >> kind;
      if (unit < 4) {
        Draw::Stage& st = d.stage[unit][kind == "alpha" ? 1 : 0];
        ss >> st.op >> st.src1 >> st.src2;
        for (float& v : st.arg) ss >> v;
        st.set = true;
      }
    } else if (key == "op") {
      int idx;
      ss >> d.type >> idx >> d.vstart >> d.vcount >> d.istart >> d.icount;
      d.indexed = idx != 0;
    } else if (key == "elem") {
      Elem e;
      ss >> e.source >> e.offset >> e.type >> e.semantic >> e.index;
      d.elems.push_back(e);
    } else if (key == "vb") {
      VB v;
      ss >> v.source >> v.file >> v.stride >> v.count;
      if (v.file.rfind("vb_", 0) == 0)
        d.vbs.push_back(v);
      else
        d.problems.push_back("vb " + v.file);
    } else if (key == "ib") {
      ss >> d.ib_file >> d.index_size;
      if (d.ib_file.rfind("ib_", 0) != 0) d.problems.push_back("ib " + d.ib_file);
    } else if (key == "tex") {
      Tex t;
      std::string tiled, packed;
      ss >> t.unit >> t.file;
      if (t.file.rfind("tex_", 0) != 0) {
        // Texture illisible (cubemap, format non géré...) : unité laissée en blanc, le dessin reste rendu.
        g_texture_problems[t.file]++;
        continue;
      }
      ss >> t.format >> t.width >> t.height >> t.block_w >> t.block_h >> t.bpb >> tiled >> packed;
      std::getline(ss, t.name);
      t.tiled = tiled == "1";
      t.cube = packed == "cube";
      d.texs.push_back(t);
    } else if (key == "state") {
      std::string w;
      while (ss >> w) {
        if (w == "blend") {
          for (auto& b : d.blend) ss >> b;
          int sep;
          ss >> sep;
          d.separate = sep != 0;
        } else if (w == "cull") {
          ss >> d.cull;
        } else if (w == "depth") {
          int c, wr;
          ss >> c >> wr >> d.depth_func;
          d.depth_check = c != 0;
          d.depth_write = wr != 0;
        } else if (w == "mask") {
          ss >> d.mask;
        } else if (w == "alpha") {
          ss >> d.alpha_func >> d.alpha_value;
        }
      }
    } else if (key == "consts") {
      std::string stage, f;
      ss >> stage >> f;
      (stage == "vs" ? d.cst_vs : d.cst_ps) = f;
    }
  }
  return draws;
}

std::vector<uint8_t> ReadBinary(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

using torchlight::natif::Constants;
using torchlight::natif::OgreConst;

Constants ReadConstants(const fs::path& p) {
  Constants c;
  std::ifstream in(p);
  std::string key;
  while (in >> key) {
    if (key == "floats") {
      size_t n;
      in >> n;
      c.floats.resize(n);
      for (auto& f : c.floats) in >> f;
    } else if (key == "name") {
      std::string name;
      OgreConst oc;
      in >> name >> oc.type >> oc.phys >> oc.elem_size >> oc.array_size;
      c.names[name] = oc;
    }
  }
  return c;
}

// Écrit un PNG RGBA 8 bits.
bool WritePng(const fs::path& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (w * 4 + 1));
  for (uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
  }
  uLongf zsize = compressBound(raw.size());
  std::vector<uint8_t> z(zsize);
  if (compress2(z.data(), &zsize, raw.data(), raw.size(), 6) != Z_OK) return false;
  z.resize(zsize);
  std::ofstream out(path, std::ios::binary);
  auto be32 = [&](uint32_t v) {
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    out.write(reinterpret_cast<const char*>(b), 4);
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32(uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.write(reinterpret_cast<const char*>(td.data()), td.size());
    be32(uint32_t(crc32(0, td.data(), uInt(td.size()))));
  };
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out.write(reinterpret_cast<const char*>(sig), 8);
  std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                               uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                               8, 6, 0, 0, 0};
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  return bool(out);
}

// Chargeur Vulkan (libvulkan) : seul point d'entrée utilisé, le reste passe par le rendu natif.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);

namespace natif = torchlight::natif;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage : %s <capture> <bibliothèque shaders> <sortie.png>\n", argv[0]);
    return 2;
  }
  const fs::path cap = argv[1], lib = argv[2], out_png = argv[3];
  const int only = std::getenv("DESSIN") ? std::atoi(std::getenv("DESSIN")) : -1;  // un seul dessin
  static const bool white_tiled = std::getenv("BLANC_TUILEES") != nullptr;

  std::vector<Draw> draws = ReadFrame(cap / "frame.txt");
  // Ancienne capture sans cibles : une seule, la fenêtre 1280x720.
  if (std::none_of(g_events.begin(), g_events.end(), [](const Event& e) { return e.kind == Event::kTarget; })) {
    Event t{Event::kTarget};
    t.name = "Game";
    t.width = 1280;
    t.height = 720;
    Event c{Event::kClear};
    c.buffers = 3;
    c.colour[0] = c.colour[1] = 0.12f;
    c.colour[2] = 0.14f;
    c.colour[3] = 1.0f;
    g_events.insert(g_events.begin(), {t, c, Event{Event::kBegin}});
    g_events.push_back({Event::kEnd});
  }
  std::printf("%zu dessins dans la capture\n", draws.size());

  natif::Context ctx;
  if (!natif::CreateStandaloneContext(vkGetInstanceProcAddr, std::getenv("GPU"), ctx)) {
    std::fprintf(stderr, "aucun GPU Vulkan utilisable\n");
    return 1;
  }
  ctx.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
  auto renderer = natif::Renderer::Create(ctx, lib);
  if (!renderer) return 1;

  // Fichiers de la capture, lus une fois.
  std::unordered_map<std::string, std::vector<uint8_t>> files;
  auto file = [&](const std::string& name) -> const std::vector<uint8_t>& {
    auto it = files.find(name);
    if (it == files.end()) it = files.emplace(name, ReadBinary(cap / name)).first;
    return it->second;
  };
  std::unordered_map<std::string, Constants> consts;
  auto constants = [&](const std::string& name) -> const Constants* {
    if (name.empty()) return nullptr;
    auto it = consts.find(name);
    if (it == consts.end()) it = consts.emplace(name, ReadConstants(cap / name)).first;
    return &it->second;
  };

  renderer->BeginFrame();
  natif::DrawDesc dd;
  for (const Event& ev : g_events) {
    switch (ev.kind) {
      case Event::kViewport: renderer->SetViewport(ev.vp[0], ev.vp[1], ev.vp[2], ev.vp[3]); continue;
      case Event::kTarget: renderer->SetTarget(ev.name, ev.width, ev.height); continue;
      case Event::kClear: renderer->Clear(ev.buffers, ev.colour, ev.depth); continue;
      case Event::kBegin: continue;
      case Event::kEnd: renderer->EndTarget(); continue;
      case Event::kDraw: break;
    }
    const Draw& d = draws[ev.draw];
    if (only >= 0 && d.id != only) continue;
    // EXCLURE=142,143 : dessins ignorés (chercher celui qui produit un défaut).
    if (const char* ex = std::getenv("EXCLURE")) {
      const std::string list = std::string(",") + ex + ",";
      if (list.find("," + std::to_string(d.id) + ",") != std::string::npos) continue;
    }
    if (!d.problems.empty()) {
      std::printf("dessin %d ignoré : données manquantes (%s)\n", d.id, d.problems[0].c_str());
      continue;
    }
    dd.Reset();
    dd.ffp = d.ffp;
    if (!d.ffp) {
      dd.vs = std::strtoull(d.vs.c_str(), nullptr, 16);
      dd.ps = std::strtoull(d.ps.c_str(), nullptr, 16);
    }
    dd.type = d.type;
    dd.indexed = d.indexed;
    dd.vstart = d.vstart;
    dd.vcount = d.vcount;
    dd.istart = d.istart;
    dd.icount = d.icount;
    for (const Elem& e : d.elems) dd.elems.push_back({e.source, e.offset, e.type, e.semantic, e.index});
    for (const VB& vb : d.vbs) {
      const auto& data = file(vb.file);
      dd.streams.push_back({vb.source, data.data(), data.size(), vb.stride, std::hash<std::string>()(vb.file)});
    }
    if (d.indexed) {
      const auto& data = file(d.ib_file);
      dd.index = {data.data(), data.size(), d.index_size, std::hash<std::string>()(d.ib_file)};
    }
    for (const Tex& t : d.texs) {
      if (white_tiled && t.tiled) continue;
      natif::TextureRef r;
      r.unit = t.unit;
      r.key = std::hash<std::string>()(t.file);
      r.format = t.format;
      r.width = t.width;
      r.height = t.height;
      r.cube = t.cube;
      r.name = t.name;
      r.load = std::make_shared<const std::function<bool(std::vector<uint8_t>&)>>(
          [&, f = t.file](std::vector<uint8_t>& out) {
            out = file(f);
            return !out.empty();
          });
      dd.textures.push_back(std::move(r));
    }
    std::copy(std::begin(d.blend), std::end(d.blend), dd.blend);
    dd.cull = d.cull;
    dd.depth_check = d.depth_check;
    dd.depth_write = d.depth_write;
    dd.depth_func = d.depth_func;
    for (int k = 0; k < 4; k++) dd.mask[k] = d.mask.size() > size_t(k) && d.mask[k] == '1';
    dd.alpha_func = d.alpha_func;
    dd.alpha_value = d.alpha_value;
    dd.consts[0] = constants(d.cst_vs);
    dd.consts[1] = constants(d.cst_ps);
    std::memcpy(dd.matrix, d.matrix, sizeof(dd.matrix));
    for (int u = 0; u < 4; u++)
      for (int k = 0; k < 2; k++) {
        const Draw::Stage& s = d.stage[u][k];
        dd.stage[u][k].set = s.set;
        dd.stage[u][k].op = s.op;
        dd.stage[u][k].src1 = s.src1;
        dd.stage[u][k].src2 = s.src2;
        std::memcpy(dd.stage[u][k].arg, s.arg, sizeof(s.arg));
      }
    renderer->Draw(dd);
  }
  renderer->Submit();

  // Image finale (la fenêtre « Game », sinon la dernière cible), puis les cibles intermédiaires
  // à côté (<sortie>.<nom>.png). Image opaque dans le PNG (l'alpha de la cible n'a pas de sens à l'écran).
  auto write = [&](const std::string& name, const fs::path& path) {
    std::vector<uint8_t> rgba;
    uint32_t w, h;
    if (!renderer->ReadTarget(name, rgba, w, h)) return false;
    for (size_t k = 3; k < rgba.size(); k += 4) rgba[k] = 255;
    return WritePng(path, w, h, rgba.data());
  };
  if (!write("", out_png)) {
    std::fprintf(stderr, "aucune cible\n");
    return 1;
  }
  for (const std::string& name : renderer->RenderedTargets()) {
    if (name == "Game") continue;
    std::string safe = name;
    for (char& c : safe)
      if (c == '/') c = '_';
    fs::path p = out_png;
    p.replace_extension("." + safe + ".png");
    write(name, p);
  }
  const natif::Stats& st = renderer->stats();
  std::printf("%u dessins rendus -> %s\n", st.drawn, out_png.string().c_str());
  for (const auto& [why, count] : st.skipped) std::printf("  ignorés : %u (%s)\n", count, why.c_str());
  for (const auto& [name, count] : st.missing_shaders) std::printf("  shader absent : %s (%u)\n", name.c_str(), count);
  for (const auto& [why, count] : g_texture_problems)
    std::printf("  texture remplacée par du blanc : %d (%s)\n", count, why.c_str());
  for (const auto& [name, count] : st.missing_consts)
    std::printf("  constante sans valeur d'Ogre : %s (%u dessins)\n", name.c_str(), count);
  return 0;
}
