// torchlight - blocs de géométrie statique hors de l'écran : non mis en file pour la caméra principale.
// D'après torchlight-recomp de YaPeL (https://github.com/YaPeL/torchlight-recomp, GPL-3.0 : src/hooks/bucket_cull*,
// docs/guest-hot-paths.md « Bucket culling »), adapté à nos accès à la mémoire du jeu.
//
// Le décor d'Ogre (StaticGeometry) est découpé en régions : une région est un seul objet, avec une seule boîte qui
// couvre une grande partie du niveau, et Region::_updateRenderQueue met en file tous ses blocs (GeometryBucket : un
// dessin par matériau et format de sommets) sans test. Beaucoup sont entièrement hors de l'écran (chez eux, 54-58 %
// en combat). Ici, chaque bloc reçoit ses boîtes (une pour tout le bloc, puis une par morceau : composantes connexes
// de ses triangles, regroupées en 16 au plus), calculées une fois dans l'espace de la région depuis ses sommets,
// oubliées à sa destruction ; à chaque image, sous la transformation courante du nœud de la région, un bloc dont
// aucune boîte ne touche le frustum de la caméra principale n'est pas mis en file (RenderQueue::addRenderable).
// Les autres caméras (carte de lumière, ombres) et tous les autres objets sont laissés tels quels.
//
// --torchlight_blocs : 0 = comme le jeu ; 1 = blocs hors de l'écran retirés (défaut) ; 2 = validation : rien n'est
// retiré, les dessins qui l'auraient été sont listés dans la capture (trace/capture/retires.txt, numéros des dessins
// de frame.txt) : l'outil hors ligne rend l'image avec et sans eux (EXCLURE=...), les deux doivent être identiques.
// Seulement en rendu natif seul. Coupé pour la session si un programme de sommets place les sommets autrement que
// le pipeline fixe (lignes de position du RT Shader System), car une boîte calculée depuis les sommets ne vaudrait plus.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "torchlight_invite.h"
#include "torchlight_natif.h"

REXCVAR_DEFINE_INT32(torchlight_blocs, 1, "Torchlight",
                     "Rendu natif seul : blocs de géométrie statique hors de l'écran non mis en file (0 = comme le "
                     "jeu, 1 = retirés, 2 = validation : listés dans la capture sans être retirés)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

using torchlight::invite::Load32;
using torchlight::invite::LoadFloat;
using torchlight::invite::Ptr;

// Disposition (variante Xbox d'Ogre 1.7, confirmée par torchlight-recomp ; recoupée avec notre relevé).
constexpr uint32_t kBucketVtable = 0x820016B4;     // StaticGeometry::GeometryBucket
constexpr uint32_t kBucketParent = 0x44;           // MaterialBucket ; son parent +4 : LODBucket ; son parent +4 : Region
constexpr uint32_t kBucketVertexData = 0x64, kBucketIndexData = 0x68;
constexpr uint32_t kMovableParentNode = 0x28;      // MovableObject::mParentNode
constexpr uint32_t kNodeTransform = 0xE4;          // Node::mCachedTransform (Matrix4 par lignes)
constexpr uint32_t kNodeTransformOutOfDate = 0x124;
constexpr uint32_t kSceneManagerViewport = 0xA8;   // SceneManager::mCurrentViewport
constexpr uint32_t kViewsSceneViewport = 0x48;     // viewport de la scène gardé par CreateViewports (sub_8220A470)
constexpr uint32_t kCameraCullFrustum = 0x4C8, kFrustumPlanes = 0x100;
constexpr size_t kMaxPieces = 16;

struct Box {
  double min[3], max[3];
};

struct Shape {
  Box whole;
  std::vector<Box> pieces;  // vide : la boîte entière seule
};

int g_mode = 0;               // 0 coupé, 1 retirer, 2 valider
bool g_programs_ok = true;    // tous les programmes de sommets vus placent les sommets comme le pipeline fixe
uint32_t g_scene_viewport = 0;
bool g_main_walk = false;     // dans _findVisibleObjects de la caméra principale
uint32_t g_walk_camera = 0;
bool g_planes_read = false;
double g_planes[6][4];
std::unordered_map<uint32_t, std::optional<Shape>> g_shapes;  // par bloc ; nullopt : illisible (toujours en file)
// Validation : VertexData des blocs qui auraient été retirés, valables jusqu'au parcours suivant (les dessins de la
// scène principale ; la carte de lumière dessine les mêmes blocs avec ses propres parcours).
std::unordered_set<uint32_t> g_marked;
struct Stats {
  uint64_t walks = 0, tested = 0, dropped = 0, unreadable = 0, shapes = 0, pieces = 0;
} g_stats;

void Expand(Box& b, const double p[3], bool& empty) {
  for (int i = 0; i < 3; i++) {
    b.min[i] = empty ? p[i] : std::min(b.min[i], p[i]);
    b.max[i] = empty ? p[i] : std::max(b.max[i], p[i]);
  }
  empty = false;
}

Box Merge(const Box& a, const Box& b) {
  Box m;
  for (int i = 0; i < 3; i++) {
    m.min[i] = std::min(a.min[i], b.min[i]);
    m.max[i] = std::max(a.max[i], b.max[i]);
  }
  return m;
}

double Extent(const Box& b) {
  double e = 0;
  for (int i = 0; i < 3; i++) e = std::max(e, b.max[i] - b.min[i]);
  return e;
}

// Flottant d'un tampon de sommets selon le mode d'échange de sa constante de lecture (0 aucun, 1 8 dans 16, 2 8 dans
// 32, 3 16 dans 32).
float FetchFloat(const uint8_t* p, uint32_t mode) {
  uint8_t d[4] = {p[0], p[1], p[2], p[3]};
  if (mode == 1) {
    std::swap(d[0], d[1]);
    std::swap(d[2], d[3]);
  } else if (mode == 2) {
    std::swap(d[0], d[3]);
    std::swap(d[1], d[2]);
  } else if (mode == 3) {
    std::swap(d[0], d[2]);
    std::swap(d[1], d[3]);
  }
  const uint32_t u = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24;
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Objet Direct3D Xbox d'un tampon matériel d'Ogre (map par périphérique : en-tête +0x38 sommets, +0x34 indices).
uint32_t D3dObject(const uint8_t* base, uint32_t buffer, uint32_t map_head) {
  const uint32_t res = torchlight::invite::DeviceMapValue(base, Load32(base, buffer + map_head));
  return Ptr(res, 4) ? Load32(base, res) : 0;
}

// Formes d'un bloc dans l'espace de sa région : la boîte de tous les sommets de ses données (vertexStart à
// vertexCount), et les morceaux d'après ses triangles (liste de triangles, indices relatifs à vertexStart).
std::optional<Shape> BuildShape(const uint8_t* base, uint32_t bucket) {
  const uint32_t vd = Load32(base, bucket + kBucketVertexData), idata = Load32(base, bucket + kBucketIndexData);
  if (!Ptr(vd, 0x18) || !Ptr(idata, 0x18)) return std::nullopt;
  const uint32_t vstart = Load32(base, vd + 0x10), vcount = Load32(base, vd + 0x14);
  if (!vcount || vcount > (1u << 22)) return std::nullopt;
  // Élément de position (VES_POSITION 1) en FLOAT3 (VET_FLOAT3 2) : déclaration (+4), éléments de 0x14 octets.
  const uint32_t decl = Load32(base, vd + 4);
  if (!Ptr(decl, 0x10)) return std::nullopt;
  const uint32_t eb = Load32(base, decl + 4), ee = Load32(base, decl + 8);
  if (ee <= eb || ee - eb > 32 * 0x14 || !torchlight::invite::Readable(eb, ee - eb)) return std::nullopt;
  uint32_t source = ~0u, offset = 0;
  for (uint32_t p = eb; p < ee; p += 0x14) {
    if (Load32(base, p + 0xC) != 1) continue;
    if (Load32(base, p + 8) != 2) return std::nullopt;
    source = Load32(base, p) >> 16;
    offset = Load32(base, p + 4);
    break;
  }
  if (source == ~0u) return std::nullopt;
  // Tampon lié à cette source (mBindingMap : std::map<ushort, HardwareVertexBufferSharedPtr>, en-tête +8).
  const uint32_t binding = Load32(base, vd + 8);
  if (!Ptr(binding, 0x10)) return std::nullopt;
  uint32_t vb = 0;
  torchlight::invite::WalkMap(base, Load32(base, binding + 8), 16, [&](uint32_t val) {
    if ((Load32(base, val) >> 16) == source) vb = Load32(base, val + 8);
  });
  if (!Ptr(vb, 0x40)) return std::nullopt;
  const uint32_t size = Load32(base, vb + 4), stride = Load32(base, vb + 0x24);
  const uint32_t d3d = D3dObject(base, vb, 0x38);
  if (!Ptr(d3d, 0x20) || stride < 12 || offset + 12 > stride) return std::nullopt;
  if (uint64_t(vstart + vcount) * stride > size) return std::nullopt;
  const uint8_t* vbytes = torchlight::invite::Physical(Load32(base, d3d + 0x18) & ~3u);
  const uint32_t endian = Load32(base, d3d + 0x1C) & 3u;
  if (!vbytes) return std::nullopt;
  std::vector<std::array<double, 3>> pos(vcount);
  Box all{};
  bool empty = true;
  for (uint32_t v = 0; v < vcount; v++) {
    const uint8_t* p = vbytes + size_t(vstart + v) * stride + offset;
    for (int k = 0; k < 3; k++) {
      const float f = FetchFloat(p + 4 * k, endian);
      if (!std::isfinite(f)) return std::nullopt;
      pos[v][k] = f;
    }
    Expand(all, pos[v].data(), empty);
  }
  Shape shape{all, {}};
  // Triangles (indices gros-boutistes) : composantes connexes par union des sommets partagés.
  const uint32_t ib = Load32(base, idata + 4);
  if (!Ptr(ib, 0x40)) return shape;
  const uint32_t istart = Load32(base, idata + 0x10), icount = Load32(base, idata + 0x14);
  const uint32_t isize = Load32(base, ib + 0x28), ibuf_size = Load32(base, ib + 4);
  const uint32_t id3d = D3dObject(base, ib, 0x34);
  if ((isize != 2 && isize != 4) || icount < 6 || icount % 3 || !Ptr(id3d, 0x20) ||
      uint64_t(istart + icount) * isize > ibuf_size)
    return shape;
  const uint8_t* ibytes = torchlight::invite::Physical(Load32(base, id3d + 0x18));
  if (!ibytes) return shape;
  std::vector<uint32_t> idx(icount);
  for (uint32_t k = 0; k < icount; k++) {
    const uint8_t* q = ibytes + size_t(istart + k) * isize;
    idx[k] = isize == 4 ? uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]
                        : uint32_t(q[0]) << 8 | q[1];
    if (idx[k] >= vcount) return shape;
  }
  std::vector<uint32_t> parent(vcount);
  for (uint32_t i = 0; i < vcount; i++) parent[i] = i;
  auto find = [&](uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  for (uint32_t t = 0; t < icount; t += 3) {
    const uint32_t a = find(idx[t]), b = find(idx[t + 1]);
    parent[a] = b;
    parent[find(idx[t + 2])] = find(b);
  }
  std::unordered_map<uint32_t, std::pair<Box, bool>> components;  // boîte, vide
  for (uint32_t k = 0; k < icount; k++) {
    auto& [box, filled] = components[find(idx[k])];
    bool e = !filled;
    Expand(box, pos[idx[k]].data(), e);
    filled = true;
  }
  if (components.size() < 2) return shape;
  // Regroupement en kMaxPieces au plus : le groupe le plus étendu coupé en deux à la médiane des centres de ses
  // boîtes, le long de son axe le plus long.
  std::vector<std::vector<Box>> groups(1);
  for (auto& [root, c] : components) groups[0].push_back(c.first);
  auto merged = [](const std::vector<Box>& g) {
    Box all = g[0];
    for (const Box& b : g) all = Merge(all, b);
    return all;
  };
  while (groups.size() < kMaxPieces) {
    size_t widest = groups.size();
    double widest_extent = -1;
    for (size_t g = 0; g < groups.size(); g++) {
      if (groups[g].size() < 2) continue;
      const double e = Extent(merged(groups[g]));
      if (e > widest_extent) {
        widest_extent = e;
        widest = g;
      }
    }
    if (widest == groups.size()) break;
    auto& g = groups[widest];
    const Box all = merged(g);
    int axis = 0;
    for (int i = 1; i < 3; i++)
      if (all.max[i] - all.min[i] > all.max[axis] - all.min[axis]) axis = i;
    std::sort(g.begin(), g.end(),
              [axis](const Box& a, const Box& b) { return a.min[axis] + a.max[axis] < b.min[axis] + b.max[axis]; });
    std::vector<Box> upper(g.begin() + g.size() / 2, g.end());
    g.resize(g.size() / 2);
    groups.push_back(std::move(upper));
  }
  for (const auto& g : groups) shape.pieces.push_back(merged(g));
  return shape;
}

// Boîte transformée (les 8 coins sous la matrice affine m, par lignes, translation en colonne 3), élargie d'une
// petite marge (le test reste prudent face aux arrondis du GPU).
Box Transform(const Box& b, const double m[16]) {
  Box out{};
  bool empty = true;
  for (int c = 0; c < 8; c++) {
    const double p[3] = {c & 1 ? b.max[0] : b.min[0], c & 2 ? b.max[1] : b.min[1], c & 4 ? b.max[2] : b.min[2]};
    double w[3];
    for (int r = 0; r < 3; r++) w[r] = m[4 * r] * p[0] + m[4 * r + 1] * p[1] + m[4 * r + 2] * p[2] + m[4 * r + 3];
    Expand(out, w, empty);
  }
  const double margin = 1e-3 + 1e-4 * Extent(out);
  for (int i = 0; i < 3; i++) {
    out.min[i] -= margin;
    out.max[i] += margin;
  }
  return out;
}

// Frustum::isVisible(boîte) d'Ogre : invisible seulement si la boîte est entièrement du côté extérieur d'un plan.
bool Visible(const Box& b) {
  for (const auto& p : g_planes) {
    double dist = p[3], extent = 0;
    for (int i = 0; i < 3; i++) {
      dist += p[i] * (b.min[i] + b.max[i]) * 0.5;
      extent += std::fabs(p[i] * (b.max[i] - b.min[i]) * 0.5);
    }
    if (dist < -extent) return false;
  }
  return true;
}

void ReadPlanes(const uint8_t* base) {
  const uint32_t cull = Load32(base, g_walk_camera + kCameraCullFrustum);
  const uint32_t frustum = cull ? cull : g_walk_camera;
  for (int i = 0; i < 6; i++)
    for (int k = 0; k < 4; k++) g_planes[i][k] = LoadFloat(base, frustum + kFrustumPlanes + 16 * i + 4 * k);
  g_planes_read = true;
}

// Vrai si ce bloc est entièrement hors du frustum de la caméra principale.
bool Outside(const uint8_t* base, uint32_t bucket) {
  g_stats.tested++;
  if (!g_planes_read) ReadPlanes(base);  // les plans sont à jour une fois le parcours commencé
  auto it = g_shapes.find(bucket);
  if (it == g_shapes.end()) {
    it = g_shapes.emplace(bucket, BuildShape(base, bucket)).first;
    if (it->second) {
      g_stats.shapes++;
      g_stats.pieces += it->second->pieces.size();
    }
  }
  if (!it->second) {
    g_stats.unreadable++;
    return false;
  }
  const uint32_t material = Load32(base, bucket + kBucketParent);
  const uint32_t lod = Ptr(material, 8) ? Load32(base, material + 4) : 0;
  const uint32_t region = Ptr(lod, 8) ? Load32(base, lod + 4) : 0;
  const uint32_t node = Ptr(region, 0x30) ? Load32(base, region + kMovableParentNode) : 0;
  if (!Ptr(node, kNodeTransformOutOfDate + 4) || base[node + kNodeTransformOutOfDate]) return false;
  double m[16];
  for (int i = 0; i < 16; i++) m[i] = LoadFloat(base, node + kNodeTransform + 4 * i);
  const Shape& s = *it->second;
  if (!Visible(Transform(s.whole, m))) return true;
  if (s.pieces.empty()) return false;
  for (const Box& piece : s.pieces)
    if (Visible(Transform(piece, m))) return false;
  return true;
}

void Report() {
  if (g_stats.walks % 600 != 0) return;
  const double w = double(g_stats.walks);
  REXLOG_INFO("[blocs] par parcours de la caméra principale (600 parcours) : {:.1f} blocs testés, {:.1f} {}, {:.1f} "
              "illisibles ; {} formes construites ({} morceaux), {} en mémoire{}",
              g_stats.tested / w, g_stats.dropped / w, g_mode == 2 ? "à retirer (validation)" : "retirés",
              g_stats.unreadable / w, g_stats.shapes, g_stats.pieces, g_shapes.size(),
              g_programs_ok ? "" : " (coupé : programme de sommets particulier)");
  g_stats = Stats{};
}

bool Enabled() {
  static const int mode = REXCVAR_GET(torchlight_blocs);
  g_mode = mode;
  return mode != 0 && g_programs_ok && torchlight::natif::DrawsHandled();
}

}  // namespace

namespace torchlight::blocs {

// Programme de sommets compilé par le jeu (source HLSL du RT Shader System) : le tri est coupé pour la session si
// la position de sortie n'est pas celle du pipeline fixe (transformation monde-vue-projection de la position
// d'entrée) ou de l'animation matérielle des squelettes.
void NoteProgram(const std::string& target, const std::string& source) {
  if (!g_programs_ok || target.rfind("vs", 0) != 0) return;
  constexpr std::string_view kFixed = "FFP_Transform(worldviewproj_matrix, iPos_0, oPos_0);";
  constexpr std::string_view kSkinned = "FFP_Transform(viewproj_matrix, lLocalParam_0, oPos_0);";
  bool ok = source.find(kSkinned) != std::string::npos;
  if (!ok && source.find(kFixed) != std::string::npos) {
    size_t uses = 0;
    for (size_t at = source.find("oPos_0"); at != std::string::npos; at = source.find("oPos_0", at + 1)) uses++;
    ok = uses == 2 && source.find("iPos_0)") == std::string::npos;
  }
  if (ok) return;
  g_programs_ok = false;
  REXLOG_WARN("[blocs] coupé pour la session : un programme de sommets place les sommets à sa façon");
}

// Validation (mode 2) : ce dessin (VertexData de la RenderOperation) est celui d'un bloc qui aurait été retiré.
bool Marked(uint32_t vertex_data) { return g_mode == 2 && !g_marked.empty() && g_marked.count(vertex_data); }

}  // namespace torchlight::blocs

// CreateViewports (sub_8220A470) : le viewport de la scène (+0x48), lu seulement.
REX_EXTERN(__imp__sub_8220A470);
REX_HOOK_RAW(sub_8220A470) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8220A470(ctx, base);
  g_scene_viewport = Load32(base, self + kViewsSceneViewport);
  REXLOG_INFO("[blocs] viewport de la scène : {:08X}", g_scene_viewport);
}

// _findVisibleObjects de Runic (sub_821A6070 : r3 SceneManager, r4 caméra) : parcours de la caméra principale si le
// viewport courant est celui de la scène.
REX_EXTERN(__imp__sub_821A6070);
REX_HOOK_RAW(sub_821A6070) {
  const uint32_t sm = ctx.r3.u32, camera = ctx.r4.u32;
  g_marked.clear();
  const bool main = Enabled() && g_scene_viewport && Load32(base, sm + kSceneManagerViewport) == g_scene_viewport;
  if (main) {
    g_main_walk = true;
    g_walk_camera = camera;
    g_planes_read = false;
    g_stats.walks++;
  }
  __imp__sub_821A6070(ctx, base);
  if (main) {
    g_main_walk = false;
    Report();
  }
}

// RenderQueue::addRenderable (sub_821C3730 : r3 file, r4 objet ; ne rend rien).
REX_EXTERN(__imp__sub_821C3730);
REX_HOOK_RAW(sub_821C3730) {
  const uint32_t rend = ctx.r4.u32;
  if (g_main_walk && Ptr(rend, 0x70) && Load32(base, rend) == kBucketVtable && Outside(base, rend)) {
    g_stats.dropped++;
    if (g_mode == 1) return;
    g_marked.insert(Load32(base, rend + kBucketVertexData));
  }
  __imp__sub_821C3730(ctx, base);
}

// ~GeometryBucket (sub_8247F728) : formes oubliées (une autre allocation peut reprendre l'adresse).
REX_EXTERN(__imp__sub_8247F728);
REX_HOOK_RAW(sub_8247F728) {
  g_shapes.erase(ctx.r3.u32);
  __imp__sub_8247F728(ctx, base);
}
