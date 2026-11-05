// torchlight - fonctions d'Ogre les plus chaudes du thread principal réécrites en C++ natif (méthode de nfsmw-nx),
// choisies d'après le profil des piles de la console (switch/RENDU_NATIF.md, 2026-10-02).
//
// --torchlight_ogre_natif : 0 = code recompilé ; 1 = natif (défaut) ; 2 = valider : le recompilé fait foi, le natif
// est calculé à côté et les différences sont comptées (bilan dans le journal toutes les 10 s).
//
// TORCHLIGHT_NATIVES_FIXES (Switch, option de switch/CMakeLists.txt) : natives actives à la compilation. Le réglage
// ci-dessus et l'alternance « natives » de torchlight_alterner sont sans effet : le code de validation, les tests à
// l'entrée et le repli sur le recompilé disparaissent. Pour un A/B sur la console : -DTORCHLIGHT_NATIVES_FIXES=OFF.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "torchlight_coeurs.h"
#include "torchlight_particules.h"
#include "torchlight_poses.h"

REXCVAR_DEFINE_INT32(torchlight_ogre_natif, 1, "Torchlight",
                     "Fonctions chaudes d'Ogre en C++ natif : 0 = recompilé, 1 = natif, 2 = valider contre le recompilé")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(torchlight_controleurs, 1, "Torchlight",
                     "Boucle des contrôleurs : 0 = parcours du std::set, systèmes arrêtés laissés au jeu (comme le "
                     "premier zip de la 1.0.3, pour les A/B) ; 1 = liste plate, contrôleurs dormants sautés")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace torchlight::ogre {
// Aussi pour les interceptions de torchlight_trace.cpp (bindGpuProgramParameters).
int Mode() {
#ifdef TORCHLIGHT_NATIVES_FIXES
  return 1;
#else
  static const int mode = REXCVAR_GET(torchlight_ogre_natif);
  return mode;
#endif
}
}  // namespace torchlight::ogre

REX_EXTERN(__imp__sub_821DBCD8);  // cos du jeu (BillboardSet::genVertices)
REX_EXTERN(__imp__sub_821DBBF8);  // sin du jeu

namespace {

using torchlight::ogre::Mode;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

float LoadFloat(const uint8_t* base, uint32_t addr) {
  const uint32_t w = Load32(base, addr);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

std::atomic<uint64_t> g_calls{0}, g_diffs{0}, g_billboards{0}, g_billboards_original{0};

// Différence de la validation : comptée, et la ligne du source des premières dans le journal.
std::atomic<int> g_lignes_vues{0};
void CompterDifference(int ligne) {
  g_diffs.fetch_add(1, std::memory_order_relaxed);
  if (g_lignes_vues.fetch_add(1, std::memory_order_relaxed) < 60) REXLOG_WARN("[ogre natif] différence, ligne {}", ligne);
}

// Validation du parcours de l'octree (sub_821C7158, en fin de fichier) : événements (visibilités d'octants, tests de
// boîtes) relevés par les interceptions de sub_821C0E78 et sub_821C74A0 pendant l'original, seulement pour les appels
// faits par le parcours lui-même (pointeur de pile de son cadre), comparés à ceux du parcours à blanc.
thread_local std::vector<uint32_t>* g_octree_events = nullptr;
thread_local uint32_t g_octree_sp = 0;

void OctreeEvent(const PPCContext& ctx, std::initializer_list<uint32_t> event) {
  if (g_octree_events && ctx.r1.u32 == g_octree_sp) g_octree_events->insert(g_octree_events->end(), event);
}
std::atomic<int64_t> g_last_report{0};

void Report() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = g_last_report.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report.compare_exchange_strong(last, now);
    return;
  }
  if (now - last < 10000 || !g_last_report.compare_exchange_strong(last, now)) return;
  REXLOG_INFO("[ogre natif] validation : {} appels, {} différences avec le recompilé (billboards : {} réécrits, "
              "{} par l'original)", g_calls.exchange(0), g_diffs.exchange(0), g_billboards.exchange(0),
              g_billboards_original.exchange(0));
}

// sub_821C74A0 : boîte englobante (r4 : AxisAlignedBox, minimum +0, maximum +12, étendue +24 : 0 nulle,
// 2 infinie) contre les 6 plans du frustum (r3 + 256 + 16 * k : normale xyz, d). Variante Xbox de
// Frustum::isVisible(const AxisAlignedBox&) sans mise à jour des plans ni culledBy, en VMX128 dans le jeu ; appelée
// par le parcours de l'octree (sub_821C7158). Invisible si, pour un plan, n·c + d < -|n|·|h| (c : centre, h :
// demi-taille), comme Plane::getSide. Console, ville : 2,6 % du thread principal en temps propre.
bool BoxVisible(const uint8_t* base, uint32_t planes, uint32_t box) {
  const uint32_t extent = Load32(base, box + 24);
  if (extent == 0) return false;
  if (extent == 2) return true;
  float c[3], h[3];
  for (int i = 0; i < 3; i++) {
    const float mn = LoadFloat(base, box + 4 * i), mx = LoadFloat(base, box + 12 + 4 * i);
    c[i] = (mn + mx) * 0.5f;
    h[i] = (mn - mx) * 0.5f;  // négative, comme dans le jeu
  }
  for (int k = 0; k < 6; k++) {
    const uint32_t p = planes + 256 + 16 * k;
    const float nx = LoadFloat(base, p), ny = LoadFloat(base, p + 4), nz = LoadFloat(base, p + 8);
    const float d = LoadFloat(base, p + 12);
    const float dist = nx * c[0] + ny * c[1] + nz * c[2] + d;
    const float neg_max = std::fabs(nx) * h[0] + std::fabs(ny) * h[1] + std::fabs(nz) * h[2];
    if (neg_max > dist) return false;
  }
  return true;
}

// sub_821C2CC8(r3 résultat, r4 A, r5 B) : produit de matrices 4x4 d'Ogre (Matrix4::concatenate, lignes de A par
// colonnes de B), rend r3. Opérations dans l'ordre du jeu et arrondies comme le code recompilé (fmuls :
// float(a*b), fmadds : float(fma(a, b, c)) en double) : t = a1*b1, puis a0*b0, a2*b2, a3*b3 ajoutés par fma.
// Appelé pour les paramètres automatiques et l'écouteur du RT Shader System ; console : 1 % en temps propre.
void Concatenate(uint8_t* base, uint32_t out, uint32_t a, uint32_t b) {
  double m[16], n[16];
  for (int k = 0; k < 16; k++) {
    m[k] = LoadFloat(base, a + 4 * k);
    n[k] = LoadFloat(base, b + 4 * k);
  }
  float r[16];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      double t = double(float(m[4 * i + 1] * n[4 + j]));
      t = double(float(std::fma(m[4 * i + 0], n[0 + j], t)));
      t = double(float(std::fma(m[4 * i + 2], n[8 + j], t)));
      t = double(float(std::fma(m[4 * i + 3], n[12 + j], t)));
      r[4 * i + j] = float(t);
    }
  for (int k = 0; k < 16; k++) {
    uint32_t w;
    std::memcpy(&w, &r[k], 4);
    w = __builtin_bswap32(w);
    std::memcpy(base + out + 4 * k, &w, 4);
  }
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + addr, &value, 4);
}

// Successeur dans un arbre std::map MSVC (comme sub_8262D6C8) : nœud { gauche +0, parent +4, droite +8, ...,
// _Isnil (octet) +21 } ; std::set d'un pointeur (sub_824C6960) : _Isnil en +17.
template <uint32_t kIsNil = 21>
uint32_t TreeNext(const uint8_t* base, uint32_t node) {
  if (base[node + kIsNil]) return node;
  uint32_t right = Load32(base, node + 8);
  if (!base[right + kIsNil]) {
    uint32_t n = right;
    for (uint32_t left = Load32(base, n); !base[left + kIsNil]; left = Load32(base, n)) n = left;
    return n;
  }
  uint32_t parent = Load32(base, node + 4);
  while (!base[parent + kIsNil] && node == Load32(base, parent + 8)) {
    node = parent;
    parent = Load32(base, parent + 4);
  }
  return parent;
}

// sub_821CC8E0(this r3) : vidage d'une collection de la file de rendu d'Ogre à chaque image (QueuedRenderable-
// Collection::clear, variante Xbox) : pour chaque élément de la map (en-tête this+8), le vector pointé en +16 du nœud
// est vidé (end = begin ; l'original appelle memmove(begin, end, 0) puis écrit end) ; puis le vector this+24 (éléments
// de 8 octets) est vidé (end = begin). Console : 1,6 % du thread principal en temps propre, plus l'itérateur.
void ClearQueuedCollection(uint8_t* base, uint32_t self) {
  const uint32_t head = Load32(base, self + 8);
  for (uint32_t node = Load32(base, head); node != head; node = TreeNext(base, node)) {
    const uint32_t vec = Load32(base, node + 16);
    const uint32_t first = Load32(base, vec), last = Load32(base, vec + 4);
    if (first != last) Store32(base, vec + 4, first);
  }
  const uint32_t begin = Load32(base, self + 24);
  if (begin != Load32(base, self + 28)) Store32(base, self + 28, begin);
}

// sub_821DAB40(this r3 BillboardSet, r4 décalages des 4 coins (Vector3 x4), r5 Billboard) : BillboardSet::genVertices
// d'Ogre (variante Xbox), appelée pour chaque particule ou billboard (rendu des particules : 2,6 % du thread
// principal sur la console avec son appelante). Chemins réécrits, sans rendu en points (this+582) :
// - sans rotation (this+320 : mAllDefaultRotation, ou rotation bb+80 nulle) ;
// - rotation par les coordonnées de texture (this+260 : mRotationType non nul, BBR_TEXCOORD), le cas courant des
//   particules (~85 % des appels en ville) : cos et sin de la rotation par les fonctions recompilées sub_821DBCD8 et
//   sub_821DBBF8 (approximations du jeu, résultat identique au bit près), puis coordonnées tournées autour du centre
//   du rectangle.
// - rotation des sommets (mRotationType nul, BBR_VERTEX), ~85 % des appels en ville : axe = normale du billboard
//   (produit vectoriel des diagonales, normalisé comme sub_821DBAB0), matrice de rotation angle-axe (sub_821DBB20,
//   Matrix3::FromAxisAngle), appliquée aux 4 décalages dans l'ordre des fmadds du jeu.
// 4 sommets écrits en mLockPtr (this+392) : position (décalage + bb+28/32/36), couleur (convertColourValue du
// RenderSystem, appel virtuel +296 de Root+20, Root en 0x835067D4), coordonnées (rectangle bb+4 si bb+1, sinon
// mTextureCoords this+536 indice bb+2 (u16), 16 octets : gauche, haut, droite, bas) ; mLockPtr avancé de 96 octets.
// Arrondis : chaque opération fadds/fsubs/fmuls du jeu est float(double(a) op double(b)) dans le code recompilé,
// identique à l'opération en float (sans contraction en fma : #pragma dans la fonction).
constexpr uint32_t kRootSingleton = 0x835067D4;
constexpr uint32_t kConstants = 0x820AA2C0;  // table de constantes du jeu (lis -32245 ; addi -23872)

uint32_t ColourScratch() {
  thread_local uint32_t slot = 0;  // 4 octets de mémoire du jeu pour la sortie de convertColourValue
  if (!slot) {
    if (auto* mem = rex::system::kernel_memory()) slot = mem->SystemHeapAlloc(16);
  }
  return slot;
}

uint32_t FloatBits(float f) {
  uint32_t w;
  std::memcpy(&w, &f, 4);
  return w;
}

// Fonction recompilée du jeu sur un double (f1 -> f1), comme frsp derrière l'appel. Le contexte courant sert
// directement (un appel du jeu ne préserve que les registres non volatils, que la fonction appelée sauvegarde) :
// copier le PPCContext (plusieurs Ko avec les registres vectoriels) coûtait un memcpy par appel.
float GuestFloatCall(PPCFunc* func, PPCContext& ctx, uint8_t* base, double x) {
  ctx.f1.f64 = x;
  func(ctx, base);
  return float(ctx.f1.f64);
}

// fmadds / fmsubs du code recompilé : fma en double, arrondi en float.
float FmaLikeGuest(float a, float b, float c) { return float(std::fma(double(a), double(b), double(c))); }
float FmsLikeGuest(float a, float b, float c) { return float(std::fma(double(a), double(b), -double(c))); }

double LoadDouble(const uint8_t* base, uint32_t addr) {
  const uint64_t w = (uint64_t(Load32(base, addr)) << 32) | Load32(base, addr + 4);
  double d;
  std::memcpy(&d, &w, 8);
  return d;
}

// Matrice de rotation (3x3, lignes) des sommets : loc_821DAC3C jusqu'à sub_821DBB20 compris.
void VertexRotation(PPCContext& ctx, uint8_t* base, uint32_t offsets, float rotation, float m[9]) {
#pragma clang fp contract(off)
  float o[12];
  for (int i = 0; i < 12; i++) o[i] = LoadFloat(base, offsets + 4 * i);
  // a = coin 2 - coin 1, b = coin 3 - coin 0 ; normale = b x a.
  const float ax = o[6] - o[3], ay = o[7] - o[4], az = o[8] - o[5];
  const float bx = o[9] - o[0], by = o[10] - o[1], bz = o[11] - o[2];
  float n[3] = {FmsLikeGuest(by, az, bz * ay), FmsLikeGuest(bz, ax, az * bx), FmsLikeGuest(ay, bx, by * ax)};
  // sub_821DBAB0 : normalisedCopy (inchangé si la longueur n'est pas > constante double +640).
  const float len = float(std::sqrt(double(FmaLikeGuest(n[2], n[2], FmaLikeGuest(n[0], n[0], n[1] * n[1])))));
  if (double(len) > LoadDouble(base, kConstants + 640)) {
    const float inv = float(double(LoadFloat(base, kConstants + 64)) / double(len));
    for (float& c : n) c = c * inv;
  }
  // sub_821DBB20 : Matrix3::FromAxisAngle.
  const float cosr = GuestFloatCall(__imp__sub_821DBCD8, ctx, base, rotation);
  const float sinr = GuestFloatCall(__imp__sub_821DBBF8, ctx, base, rotation);
  const float x = n[0], y = n[1], z = n[2];
  const float omc = LoadFloat(base, kConstants + 64) - cosr;
  const float yz = (z * y) * omc, xz = (x * z) * omc, xy = (x * y) * omc;
  const float xs = x * sinr, ys = y * sinr, zs = z * sinr;
  m[0] = FmaLikeGuest(x * x, omc, cosr);
  m[1] = xy - zs;
  m[2] = ys + xz;
  m[3] = zs + xy;
  m[4] = FmaLikeGuest(y * y, omc, cosr);
  m[5] = yz - xs;
  m[6] = xz - ys;
  m[7] = xs + yz;
  m[8] = FmaLikeGuest(z * z, omc, cosr);
}

// Calcule les 24 mots (gros-boutistes à écrire) d'un chemin réécrit ; faux si l'original s'applique.
bool BillboardVertices(PPCContext& ctx, uint8_t* base, uint32_t out[24]) {
#pragma clang fp contract(off)
  const uint32_t self = ctx.r3.u32, offsets = ctx.r4.u32, bb = ctx.r5.u32;
  if (base[self + 582]) return false;  // rendu en points
  const float rotation = LoadFloat(base, bb + 80);
  const bool rotate = !base[self + 320] && !(rotation == LoadFloat(base, kConstants + 68));
  const uint32_t slot = ColourScratch();
  const uint32_t root = Load32(base, kRootSingleton);
  const uint32_t rs = root ? Load32(base, root + 20) : 0;
  if (!slot || !rs) return false;
  const uint32_t convert = Load32(base, Load32(base, rs) + 296);
  static uint32_t cached_addr = 0;
  static PPCFunc* cached_func = nullptr;
  if (convert != cached_addr) {
    cached_func = rex::runtime::ResolveIndirectFunction(convert);
    cached_addr = convert;
  }
  if (!cached_func) return false;
  ctx.r3.u64 = rs;
  ctx.r4.u64 = bb + 64;
  ctx.r5.u64 = slot;
  cached_func(ctx, base);
  const uint32_t colour = Load32(base, slot);
  uint32_t rect;
  if (base[bb + 1]) {
    rect = bb + 4;
  } else {
    uint16_t idx;
    std::memcpy(&idx, base + bb + 2, 2);
    rect = Load32(base, self + 536) + (uint32_t(__builtin_bswap16(idx)) << 4);
  }
  const float left = LoadFloat(base, rect), top = LoadFloat(base, rect + 4);
  const float right = LoadFloat(base, rect + 8), bottom = LoadFloat(base, rect + 12);
  const float px = LoadFloat(base, bb + 28), py = LoadFloat(base, bb + 32), pz = LoadFloat(base, bb + 36);
  float u[4] = {left, right, left, right}, v[4] = {top, top, bottom, bottom};
  if (rotate && Load32(base, self + 260) == 0) {
    float m[9];
    VertexRotation(ctx, base, offsets, rotation, m);
    for (int k = 0; k < 4; k++) {
      const uint32_t o = offsets + 12 * k;
      const float ox = LoadFloat(base, o), oy = LoadFloat(base, o + 4), oz = LoadFloat(base, o + 8);
      float r[3];
      for (int i = 0; i < 3; i++) {
        const float* row = m + 3 * i;
        r[i] = k == 0 ? FmaLikeGuest(ox, row[0], FmaLikeGuest(oz, row[2], oy * row[1]))
                      : FmaLikeGuest(row[1], oy, FmaLikeGuest(ox, row[0], row[2] * oz));
      }
      out[6 * k + 0] = FloatBits(r[0] + px);
      out[6 * k + 1] = FloatBits(r[1] + py);
      out[6 * k + 2] = FloatBits(r[2] + pz);
      out[6 * k + 3] = colour;
      out[6 * k + 4] = FloatBits(u[k]);
      out[6 * k + 5] = FloatBits(v[k]);
    }
    return true;
  }
  if (rotate) {
    // loc_821DAFC0 : demi-largeur w, demi-hauteur h, centre (cx, cy), cos et sin de la rotation.
    const float cosr = GuestFloatCall(__imp__sub_821DBCD8, ctx, base, rotation);
    const float sinr = GuestFloatCall(__imp__sub_821DBBF8, ctx, base, rotation);
    const float half = LoadFloat(base, kConstants + 104);
    const float w = (right - left) * half, h = (bottom - top) * half;
    const float cx = left + w, cy = top + h;
    const float ws = w * sinr, hs = h * sinr, hc = h * cosr, wc = w * cosr;
    u[0] = (cx - wc) + hs;
    v[0] = (cy - ws) - hc;
    u[1] = (hs + wc) + cx;
    v[1] = (ws + cy) - hc;
    u[2] = (cx - wc) - hs;
    v[2] = (cy - ws) + hc;
    u[3] = (wc + cx) - hs;
    v[3] = (ws + hc) + cy;
  }
  for (int k = 0; k < 4; k++) {
    const uint32_t o = offsets + 12 * k;
    out[6 * k + 0] = FloatBits(LoadFloat(base, o) + px);
    out[6 * k + 1] = FloatBits(LoadFloat(base, o + 4) + py);
    out[6 * k + 2] = FloatBits(LoadFloat(base, o + 8) + pz);
    out[6 * k + 3] = colour;
    out[6 * k + 4] = FloatBits(u[k]);
    out[6 * k + 5] = FloatBits(v[k]);
  }
  return true;
}

}  // namespace

REX_EXTERN(__imp__sub_821DAB40);
REX_HOOK_RAW(sub_821DAB40) {
  const int mode = Mode();
  // Arguments relevés avant BillboardVertices, qui appelle des fonctions du jeu avec ce contexte.
  const uint32_t self = ctx.r3.u32, offsets = ctx.r4.u32, bb = ctx.r5.u32;
  const auto original = [&] {
    ctx.r3.u64 = self;
    ctx.r4.u64 = offsets;
    ctx.r5.u64 = bb;
    __imp__sub_821DAB40(ctx, base);
  };
  uint32_t words[24];
  if (mode == 0 || !BillboardVertices(ctx, base, words)) {
    if (mode == 2) g_billboards_original.fetch_add(1, std::memory_order_relaxed);
    original();
    return;
  }
  const uint32_t lock = Load32(base, self + 392);
  if (mode == 2) {
    original();
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_billboards.fetch_add(1, std::memory_order_relaxed);
    bool same = Load32(base, self + 392) == lock + 96;
    for (int k = 0; k < 24 && same; k++) same = Load32(base, lock + 4 * k) == words[k];
    if (!same) CompterDifference(__LINE__);
    Report();
    return;
  }
  for (int k = 0; k < 24; k++) Store32(base, lock + 4 * k, words[k]);
  Store32(base, self + 392, lock + 96);
}

REX_EXTERN(__imp__sub_821CC8E0);
REX_HOOK_RAW(sub_821CC8E0) {
  if (Mode() != 1) {  // validation : l'original (écritures en mémoire, pas de comparaison simple)
    __imp__sub_821CC8E0(ctx, base);
    return;
  }
  ClearQueuedCollection(base, ctx.r3.u32);
}

REX_EXTERN(__imp__sub_821C2CC8);
REX_HOOK_RAW(sub_821C2CC8) {
  const int mode = Mode();
  if (mode == 0) {
    __imp__sub_821C2CC8(ctx, base);
    return;
  }
  if (mode == 2) {
    uint8_t original[64], native[64];
    const uint32_t out = ctx.r3.u32;
    std::memcpy(original, base + out, 64);
    Concatenate(base, out, ctx.r4.u32, ctx.r5.u32);
    std::memcpy(native, base + out, 64);
    std::memcpy(base + out, original, 64);  // le recompilé fait foi
    __imp__sub_821C2CC8(ctx, base);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (std::memcmp(native, base + out, 64) != 0) CompterDifference(__LINE__);
    Report();
    return;
  }
  Concatenate(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

REX_EXTERN(__imp__sub_821C74A0);
REX_HOOK_RAW(sub_821C74A0) {
  const int mode = Mode();
  if (mode == 0) {
    __imp__sub_821C74A0(ctx, base);
    return;
  }
  const uint32_t box = ctx.r4.u32;
  const bool visible = BoxVisible(base, ctx.r3.u32, box);
  if (mode == 2) {
    __imp__sub_821C74A0(ctx, base);
    OctreeEvent(ctx, {2u, box, ctx.r3.u32 ? 1u : 0u});
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if ((ctx.r3.u32 != 0) != visible) CompterDifference(__LINE__);
    Report();
    return;
  }
  ctx.r3.u64 = visible ? 1 : 0;
}

// --- Ogre::Node : mise à jour des transformations (os des squelettes et nœuds de scène) ---
// Console, ville : animation des squelettes ~10 % du thread principal, dont Node::_update (récursif, 2,5 %) et
// Node::needUpdate (2,3 %, appelé par chaque translate/rotate/scale/reset d'un os). Petites fonctions faites
// d'appels virtuels : réécrites ensemble, avec dévirtualisation (la case de la table virtuelle est-elle la fonction
// de Node ? sinon appel de la fonction du jeu : SceneNode, TagPoint et Bone surchargent certaines cases).
// Champs de Node (Ogre 1.7, variante Xbox) : +4 mParent, +12 mChildren (hash_map : liste, suivant +0, Node* +36),
// +52 mChildrenToUpdate (std::set MSVC : tête +56, taille +60 ; nœud : gauche +0, parent +4, droite +8, valeur +12,
// isnil +17), +72 mNeedParentUpdate, +73 mNeedChildUpdate, +74 mParentNotified, +104 mOrientation (w x y z),
// +120 mPosition, +132 mScale, +144 mInheritOrientation, +145 mInheritScale, +148 mDerivedOrientation,
// +164 mDerivedPosition, +176 mDerivedScale, +292 mCachedTransformOutOfDate, +296 mListener.
// Cases : [1] _updateFromParent, [2] updateFromParentImpl, [47] _getDerivedOrientation, [48] _getDerivedPosition,
// [49] _getDerivedScale, [51] _update, [63] needUpdate, [64] requestUpdate.
REX_EXTERN(__imp__sub_821C9F08);
REX_EXTERN(__imp__sub_821C9770);
REX_EXTERN(__imp__sub_821C9BB0);
REX_EXTERN(__imp__sub_821C8518);
REX_EXTERN(__imp__sub_821C99D0);
REX_EXTERN(__imp__sub_821C9F68);
REX_EXTERN(__imp__sub_821CA0B8);
REX_EXTERN(__imp__sub_8246CA80);
REX_EXTERN(__imp__sub_821CA140);
REX_EXTERN(__imp__sub_821CB998);
REX_EXTERN(__imp__sub_821C9A18);
REX_EXTERN(__imp__sub_821CB3C0);

namespace {

constexpr uint32_t kUpdateFromParent = 0x821C9F08, kUpdateFromParentImpl = 0x821C9770;
constexpr uint32_t kDerivedOrientation = 0x821C9BB0, kDerivedPosition = 0x821C8518, kDerivedScale = 0x821C99D0;
constexpr uint32_t kNodeUpdate = 0x821C9F68;

// Fonction du jeu à une adresse : table des appels indirects du code généré (REX_LOOKUP_FUNC, constantes de
// switch/generated/default/torchlight_pch.h), sinon le répartiteur du SDK.
PPCFunc* GuestFunc(uint8_t* base, uint32_t addr) {
  constexpr uint64_t kImageEnd = 0x82000000ull + 0x15C0000ull, kCodeBase = 0x82190000ull;
  constexpr uint64_t kCodeSpan = 0xE97448ull + 0x10000ull;
  if (uint64_t(addr) - kCodeBase < kCodeSpan) {
    PPCFunc* f;
    std::memcpy(&f, base + kImageEnd + (uint64_t(addr) - kCodeBase) * 2, sizeof(f));
    if (f) return f;
  }
  return rex::runtime::ResolveIndirectFunction(addr);
}

uint32_t Slot(const uint8_t* base, uint32_t obj, uint32_t offset) { return Load32(base, Load32(base, obj) + offset); }

// Appel virtuel du jeu : r3 = obj, r4, r5 ; renvoie r3.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, uint32_t func, uint32_t obj, uint64_t r4 = 0, uint64_t r5 = 0) {
  ctx.r3.u64 = obj;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  GuestFunc(base, func)(ctx, base);
  return ctx.r3.u32;
}

void StoreFloat(uint8_t* base, uint32_t addr, float f) { Store32(base, addr, FloatBits(f)); }

float FnmsLikeGuest(float a, float b, float c) {  // fnmsubs : -(a * b - c)
  return float(-std::fma(double(a), double(b), -double(c)));
}

void UpdateFromParent(PPCContext& ctx, uint8_t* base, uint32_t node);

uint32_t Derived(PPCContext& ctx, uint8_t* base, uint32_t node, uint32_t slot, uint32_t native, uint32_t field) {
  const uint32_t f = Slot(base, node, slot);
  if (f != native) return CallGuest(ctx, base, f, node);
  if (base[node + 72]) UpdateFromParent(ctx, base, node);
  return node + field;
}

// sub_821C9BF8 : Quaternion * Vector3 (q : w x y z), dans l'ordre des opérations du jeu.
void RotateVector(const uint8_t* base, const float q[4], const float v[3], float out[3]) {
#pragma clang fp contract(off)
  const float two = LoadFloat(base, kConstants + 124);
  const float w = q[0], qx = q[1], qy = q[2], qz = q[3];
  const float uvy = FmsLikeGuest(v[0], qz, v[2] * qx);
  const float uvx = FmsLikeGuest(v[2], qy, v[1] * qz);
  const float uvz = FmsLikeGuest(v[1], qx, v[0] * qy);
  const float w2 = w * two;
  const float uuvx = FmsLikeGuest(uvz, qy, uvy * qz);
  const float uuvz = FmsLikeGuest(uvy, qx, qy * uvx);
  const float uuvy = FmsLikeGuest(uvx, qz, uvz * qx);
  out[0] = (v[0] + uvx * w2) + uuvx * two;
  out[1] = (v[1] + uvy * w2) + uuvy * two;
  out[2] = (v[2] + uvz * w2) + uuvz * two;
}

// sub_821C9770 : Node::updateFromParentImpl, résultat (orientation, position, échelle dérivées) dans out[10].
void UpdateFromParentImplValues(PPCContext& ctx, uint8_t* base, uint32_t node, float out[10]) {
#pragma clang fp contract(off)
  float o[4], p[3], s[3];
  for (int i = 0; i < 4; i++) o[i] = LoadFloat(base, node + 104 + 4 * i);
  for (int i = 0; i < 3; i++) p[i] = LoadFloat(base, node + 120 + 4 * i);
  for (int i = 0; i < 3; i++) s[i] = LoadFloat(base, node + 132 + 4 * i);
  const uint32_t parent = Load32(base, node + 4);
  if (!parent) {
    std::memcpy(out, o, 16);
    std::memcpy(out + 4, p, 12);
    std::memcpy(out + 7, s, 12);
    return;
  }
  const uint32_t pq_addr = Derived(ctx, base, parent, 188, kDerivedOrientation, 148);
  if (base[node + 144]) {
    float pq[4];
    for (int i = 0; i < 4; i++) pq[i] = LoadFloat(base, pq_addr + 4 * i);
    const float pw = pq[0], px = pq[1], py = pq[2], pz = pq[3];
    const float ow = o[0], ox = o[1], oy = o[2], oz = o[3];
    const float t1 = FmsLikeGuest(ow, pw, ox * px);
    const float t2 = FmaLikeGuest(oz, py, ow * px);
    const float t8 = FmaLikeGuest(ow, pz, oy * px);
    const float t10 = FmaLikeGuest(ox, pz, ow * py);
    const float t7 = FmaLikeGuest(ox, pw, t2);
    const float t5 = FnmsLikeGuest(oy, py, t1);
    const float t1b = FmaLikeGuest(oz, pw, t8);
    const float t2b = FmaLikeGuest(oy, pw, t10);
    out[0] = FnmsLikeGuest(oz, pz, t5);
    out[1] = FnmsLikeGuest(oy, pz, t7);
    out[2] = FnmsLikeGuest(oz, px, t2b);
    out[3] = FnmsLikeGuest(ox, py, t1b);
  } else {
    std::memcpy(out, o, 16);
  }
  const uint32_t ps_addr = Derived(ctx, base, parent, 196, kDerivedScale, 176);
  float ps[3];
  for (int i = 0; i < 3; i++) ps[i] = LoadFloat(base, ps_addr + 4 * i);
  for (int i = 0; i < 3; i++) out[7 + i] = base[node + 145] ? s[i] * ps[i] : s[i];
  float scaled[3], pq[4];
  for (int i = 0; i < 3; i++) scaled[i] = ps[i] * p[i];
  for (int i = 0; i < 4; i++) pq[i] = LoadFloat(base, pq_addr + 4 * i);
  RotateVector(base, pq, scaled, out + 4);
  const uint32_t pp_addr = Derived(ctx, base, parent, 192, kDerivedPosition, 164);
  for (int i = 0; i < 3; i++) out[4 + i] = out[4 + i] + LoadFloat(base, pp_addr + 4 * i);
}

void UpdateFromParentImpl(PPCContext& ctx, uint8_t* base, uint32_t node) {
  float out[10];
  UpdateFromParentImplValues(ctx, base, node, out);
  for (int i = 0; i < 10; i++) StoreFloat(base, node + 148 + 4 * i, out[i]);
  base[node + 292] = 1;
  base[node + 72] = 0;
}

// sub_821C9F08 : Node::_updateFromParent (implémentation [2], puis mListener->nodeUpdated [1]).
void UpdateFromParent(PPCContext& ctx, uint8_t* base, uint32_t node) {
  const uint32_t f = Slot(base, node, 4);
  if (f != kUpdateFromParent) {
    CallGuest(ctx, base, f, node);
    return;
  }
  const uint32_t impl = Slot(base, node, 8);
  if (impl == kUpdateFromParentImpl) {
    UpdateFromParentImpl(ctx, base, node);
  } else {
    CallGuest(ctx, base, impl, node);
  }
  const uint32_t listener = Load32(base, node + 296);
  if (listener) CallGuest(ctx, base, Slot(base, listener, 4), listener, node);
}

// sub_8246CA80 sur mChildrenToUpdate : ensemble vide (cas courant) en natif, sinon la fonction du jeu.
void ClearChildrenToUpdate(PPCContext& ctx, uint8_t* base, uint32_t node) {
  const uint32_t set = node + 52, head = Load32(base, set + 4);
  if (!base[Load32(base, head + 4) + 17]) {
    ctx.r3.u64 = set;
    __imp__sub_8246CA80(ctx, base);
    return;
  }
  Store32(base, head + 4, head);
  Store32(base, head + 0, head);
  Store32(base, head + 8, head);
  Store32(base, set + 8, 0);
}

// sub_824C6960 : successeur dans un arbre MSVC (isnil +17).
uint32_t SetNext(const uint8_t* base, uint32_t n) {
  if (base[n + 17]) return n;
  uint32_t r = Load32(base, n + 8);
  if (!base[r + 17]) {
    while (!base[Load32(base, r) + 17]) r = Load32(base, r);
    return r;
  }
  uint32_t p = Load32(base, n + 4);
  while (!base[p + 17] && n == Load32(base, p + 8)) {
    n = p;
    p = Load32(base, p + 4);
  }
  return p;
}

void NodeUpdate(PPCContext& ctx, uint8_t* base, uint32_t node, bool update_children, bool parent_changed);

void ChildUpdate(PPCContext& ctx, uint8_t* base, uint32_t child, bool parent_changed) {
  const uint32_t f = Slot(base, child, 204);
  if (f == kNodeUpdate) {
    NodeUpdate(ctx, base, child, true, parent_changed);
  } else {
    CallGuest(ctx, base, f, child, 1, parent_changed ? 1 : 0);
  }
}

// sub_821C9F68 : Node::_update.
void NodeUpdate(PPCContext& ctx, uint8_t* base, uint32_t node, bool update_children, bool parent_changed) {
  base[node + 74] = 0;
  if (!update_children && !base[node + 72] && !base[node + 73] && !parent_changed) return;
  if (base[node + 72] || parent_changed) UpdateFromParent(ctx, base, node);
  if (!base[node + 73] && !parent_changed) {
    const uint32_t head = Load32(base, node + 56);
    for (uint32_t it = Load32(base, head); it != head; it = SetNext(base, it)) {
      ChildUpdate(ctx, base, Load32(base, it + 12), false);
    }
  } else {
    const uint32_t head = Load32(base, node + 12);
    for (uint32_t it = Load32(base, head); it != head; it = Load32(base, it)) {
      ChildUpdate(ctx, base, Load32(base, it + 36), true);
    }
  }
  ClearChildrenToUpdate(ctx, base, node);
  base[node + 73] = 0;
}

// Node::needUpdate (sub_821CA0B8).
void NeedUpdate(PPCContext& ctx, uint8_t* base, uint32_t node, uint64_t force) {
  base[node + 72] = 1;
  base[node + 73] = 1;
  base[node + 292] = 1;
  const uint32_t parent = Load32(base, node + 4);
  if (parent && (!base[node + 74] || uint8_t(force))) {
    CallGuest(ctx, base, Slot(base, parent, 256), parent, node, force);
    base[node + 74] = 1;
  }
  ClearChildrenToUpdate(ctx, base, node);
}

// Bone::needUpdate (sub_821CA140) : puis, os contrôlé à la main (+310), mCreator (+312)
// ->_notifyManualBoneStateChange [75] (sub_824E8F60 : mManualBonesDirty, octet +264, à vrai).
void BoneNeedUpdate(PPCContext& ctx, uint8_t* base, uint32_t node, uint64_t force) {
  NeedUpdate(ctx, base, node, force);
  if (!base[node + 310]) return;
  const uint32_t creator = Load32(base, node + 312);
  const uint32_t f = Slot(base, creator, 300);
  if (f == 0x824E8F60) {
    base[creator + 264] = 1;
  } else {
    CallGuest(ctx, base, f, creator, node);
  }
}

// Case [63] needUpdate, appelée avec forceParentUpdate faux par translate, rotate, scale.
void NeedUpdateVirtual(PPCContext& ctx, uint8_t* base, uint32_t node) {
  const uint32_t f = Slot(base, node, 252);
  if (f == 0x821CA0B8) {
    NeedUpdate(ctx, base, node, 0);
  } else if (f == 0x821CA140) {
    BoneNeedUpdate(ctx, base, node, 0);
  } else {
    CallGuest(ctx, base, f, node, 0);
  }
}

// sub_821CBC18 : Quaternion::nlerp(t, p, q, shortestPath) (q : w x y z).
void Nlerp(const uint8_t* base, double t, const float p[4], const float q[4], bool shortest, float out[4]) {
#pragma clang fp contract(off)
  const float one = LoadFloat(base, kConstants + 64);
  const float zero = LoadFloat(base, kConstants + 68);
  const float dot = FmaLikeGuest(q[0], p[0], FmaLikeGuest(p[2], q[2], FmaLikeGuest(p[3], q[3], p[1] * q[1])));
  if (dot < zero && shortest) {
    for (int i = 0; i < 4; i++) out[i] = p[i] + float(double((-q[i]) - p[i]) * t);
  } else {
    for (int i = 0; i < 4; i++) out[i] = p[i] + float(double(q[i] - p[i]) * t);
  }
  const float len = float(std::sqrt(double(FmaLikeGuest(
      out[3], out[3], FmaLikeGuest(out[2], out[2], FmaLikeGuest(out[0], out[0], out[1] * out[1]))))));
  const float inv = float(double(one) / double(len));
  for (int i = 0; i < 4; i++) out[i] = out[i] * inv;
}

// sub_821CC1C0, espace local (TS_LOCAL) : mOrientation = mOrientation * q normalisé, puis needUpdate.
void RotateLocal(PPCContext& ctx, uint8_t* base, uint32_t node, const float q[4]) {
#pragma clang fp contract(off)
  const float w = q[0], x = q[1], y = q[2], z = q[3];
  const float len = float(std::sqrt(double(FmaLikeGuest(w, w, FmaLikeGuest(z, z, FmaLikeGuest(x, x, y * y))))));
  const float inv = float(double(LoadFloat(base, kConstants + 64)) / double(len));
  const float nw = w * inv, nx = x * inv, ny = y * inv, nz = z * inv;
  const float ow = LoadFloat(base, node + 104), ox = LoadFloat(base, node + 108);
  const float oy = LoadFloat(base, node + 112), oz = LoadFloat(base, node + 116);
  const float f6 = FmaLikeGuest(nz, oy, ow * nx);
  const float f5 = FmaLikeGuest(nx, oz, ow * ny);
  const float f3 = FmaLikeGuest(ny, ox, ow * nz);
  const float f4 = FmsLikeGuest(ow, nw, nx * ox);
  const float f2 = FmaLikeGuest(ox, nw, f6);
  const float f1 = FmaLikeGuest(oy, nw, f5);
  const float f12 = FmaLikeGuest(oz, nw, f3);
  const float f0 = FnmsLikeGuest(ny, oy, f4);
  StoreFloat(base, node + 108, FnmsLikeGuest(ny, oz, f2));
  StoreFloat(base, node + 112, FnmsLikeGuest(nz, ox, f1));
  StoreFloat(base, node + 116, FnmsLikeGuest(nx, oy, f12));
  StoreFloat(base, node + 104, FnmsLikeGuest(nz, oz, f0));
  NeedUpdateVirtual(ctx, base, node);
}

REX_EXTERN(__imp__sub_8244ED20);  // Quaternion::Slerp
REX_EXTERN(__imp__sub_821CBDC0);  // NodeAnimationTrack::getInterpolatedKeyFrame

// TransformKeyFrame (table 0x82000CE8) : temps +4, piste parente +8, translation +12, échelle +24, rotation +36.
// Accesseurs : [2] setTranslate 0x821CC138, [3] setScale 0x821CBD78, [4] getScale 0x821CC130, [5] setRotation
// 0x821CC178, [6] getRotation 0x821CBDB8 ; chaque setter prévient la piste parente ([9] _keyFrameDataChanged).
uint32_t KeyFrameGet(PPCContext& ctx, uint8_t* base, uint32_t kf, uint32_t slot, uint32_t native, uint32_t field) {
  const uint32_t f = Slot(base, kf, slot);
  return f == native ? kf + field : CallGuest(ctx, base, f, kf);
}

// Setter : valeurs (n mots) copiées en natif, ou par l'appel du jeu depuis scratch (mémoire du jeu).
void KeyFrameSet(PPCContext& ctx, uint8_t* base, uint32_t kf, uint32_t slot, uint32_t native, uint32_t field,
                 const float* v, int n, uint32_t scratch) {
  const uint32_t f = Slot(base, kf, slot);
  if (f != native) {
    for (int i = 0; i < n; i++) StoreFloat(base, scratch + 4 * i, v[i]);
    CallGuest(ctx, base, f, kf, scratch);
    return;
  }
  for (int i = 0; i < n; i++) StoreFloat(base, kf + field + 4 * i, v[i]);
  const uint32_t track = Load32(base, kf + 8);
  if (track) CallGuest(ctx, base, Slot(base, track, 36), track);
}

void LoadFloats(const uint8_t* base, uint32_t addr, float* v, int n) {
  for (int i = 0; i < n; i++) v[i] = LoadFloat(base, addr + 4 * i);
}

// sub_821CBDC0 : NodeAnimationTrack::getInterpolatedKeyFrame(timeIndex, kf), chemins sans écouteur (+28) :
// t nul (copie de l'image clé 1) et interpolation linéaire (mParent +20 : mInterpolationMode +96 nul ; rotation
// nlerp ou slerp selon +100). getKeyFramesAtTime [3] reste celle du jeu (cadre de 304 octets comme l'original :
// image clé 1 en +84, 2 en +88, indice en +80). Faux : l'original est à appeler (écouteur, spline).
bool InterpolatedKeyFrame(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t time, uint32_t kret) {
#pragma clang fp contract(off)
  if (Load32(base, self + 28)) return false;
  const uint32_t parent = Load32(base, self + 20);
  const uint32_t mode = Load32(base, parent + 96), rim = Load32(base, parent + 100);
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 304;
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = self;
  ctx.r4.u64 = time;
  ctx.r5.u64 = sp + 84;
  ctx.r6.u64 = sp + 88;
  ctx.r7.u64 = sp + 80;
  GuestFunc(base, Slot(base, self, 12))(ctx, base);
  const double t = ctx.f1.f64;
  const uint32_t k1 = Load32(base, sp + 84), k2 = Load32(base, sp + 88);
  float rot[4], tr[3], sc[3];
  if (t == double(LoadFloat(base, kConstants + 68))) {
    LoadFloats(base, KeyFrameGet(ctx, base, k1, 24, 0x821CBDB8, 36), rot, 4);
    KeyFrameSet(ctx, base, kret, 20, 0x821CC178, 36, rot, 4, sp + 176);
    LoadFloats(base, k1 + 12, tr, 3);
    KeyFrameSet(ctx, base, kret, 8, 0x821CC138, 12, tr, 3, sp + 96);
    LoadFloats(base, KeyFrameGet(ctx, base, k1, 16, 0x821CC130, 24), sc, 3);
    KeyFrameSet(ctx, base, kret, 12, 0x821CBD78, 24, sc, 3, sp + 112);
    ctx.r1.u64 = old_sp;
    return true;
  }
  if (mode != 0) {  // spline (1) : l'original depuis le début ; au-delà, rien (comme l'original)
    ctx.r1.u64 = old_sp;
    return mode != 1;
  }
  const uint32_t q2 = KeyFrameGet(ctx, base, k2, 24, 0x821CBDB8, 36);
  const uint32_t q1 = KeyFrameGet(ctx, base, k1, 24, 0x821CBDB8, 36);
  const bool shortest = base[self + 57] != 0;
  if (rim == 0) {
    float p[4], q[4];
    LoadFloats(base, q1, p, 4);
    LoadFloats(base, q2, q, 4);
    Nlerp(base, t, p, q, shortest, rot);
  } else {
    ctx.r3.u64 = sp + 192;
    ctx.r5.u64 = q1;
    ctx.r6.u64 = q2;
    ctx.r7.u64 = shortest ? 1 : 0;
    ctx.f1.f64 = t;
    __imp__sub_8244ED20(ctx, base);
    LoadFloats(base, ctx.r3.u32, rot, 4);
  }
  KeyFrameSet(ctx, base, kret, 20, 0x821CC178, 36, rot, 4, sp + 176);
  for (int i = 0; i < 3; i++) {
    const float a = LoadFloat(base, k1 + 12 + 4 * i), b = LoadFloat(base, k2 + 12 + 4 * i);
    tr[i] = float(double(b - a) * t) + a;
  }
  KeyFrameSet(ctx, base, kret, 8, 0x821CC138, 12, tr, 3, sp + 96);
  float s1[3], s2[3];
  LoadFloats(base, KeyFrameGet(ctx, base, k1, 16, 0x821CC130, 24), s1, 3);
  LoadFloats(base, KeyFrameGet(ctx, base, k2, 16, 0x821CC130, 24), s2, 3);
  for (int i = 0; i < 3; i++) sc[i] = float(double(s2[i] - s1[i]) * t) + s1[i];
  KeyFrameSet(ctx, base, kret, 12, 0x821CBD78, 24, sc, 3, sp + 112);
  ctx.r1.u64 = old_sp;
  return true;
}

// sub_821CB998 : NodeAnimationTrack::applyToNode. L'image clé interpolée (TransformKeyFrame : table virtuelle,
// temps +4, piste +8, translation +12, échelle +24, rotation +36) est construite dans un cadre de pile du jeu
// comme l'original (336 octets, image clé en +144) ; getInterpolatedKeyFrame [7] et Slerp restent ceux du jeu.
void ApplyToNode(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t node, uint32_t time, double weight,
                 double scl) {
#pragma clang fp contract(off)
  if (Load32(base, self + 4) == Load32(base, self + 8)) return;  // mKeyFrames vide
  if (weight == double(LoadFloat(base, kConstants + 68)) || !node) return;
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 336;
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  const uint32_t kf = sp + 144;
  constexpr uint32_t kZero = 0x8355A3BC, kUnitScale = 0x8349D51C, kIdentity = 0x8349FF40;
  Store32(base, kf, 0x82000CE8);
  Store32(base, kf + 4, Load32(base, time));
  Store32(base, kf + 8, 0);
  for (int i = 0; i < 3; i++) Store32(base, kf + 12 + 4 * i, Load32(base, kZero + 4 * i));
  for (int i = 0; i < 3; i++) Store32(base, kf + 24 + 4 * i, Load32(base, kUnitScale + 4 * i));
  for (int i = 0; i < 4; i++) Store32(base, kf + 36 + 4 * i, Load32(base, kIdentity + 4 * i));
  const uint32_t interpolate = Slot(base, self, 28);
  if (interpolate != 0x821CBDC0 || !InterpolatedKeyFrame(ctx, base, self, time, kf)) {
    CallGuest(ctx, base, interpolate, self, time, kf);
  }

  // Translation (espace parent).
  float t[3];
  for (int i = 0; i < 3; i++) t[i] = float(double(float(double(LoadFloat(base, kf + 12 + 4 * i)) * weight)) * scl);
  const uint32_t translate = Slot(base, node, 104);
  if (translate == 0x821CB808) {
    for (int i = 0; i < 3; i++) StoreFloat(base, node + 120 + 4 * i, t[i] + LoadFloat(base, node + 120 + 4 * i));
    NeedUpdateVirtual(ctx, base, node);
  } else {
    for (int i = 0; i < 3; i++) StoreFloat(base, sp + 96 + 4 * i, t[i]);
    CallGuest(ctx, base, translate, node, sp + 96, 1);
  }

  // Rotation (espace local) : nlerp (RIM_LINEAR) ou slerp depuis l'identité.
  float q[4];
  const bool shortest = base[self + 57] != 0;
  if (Load32(base, Load32(base, self + 20) + 100) == 0) {
    float p[4], k[4];
    for (int i = 0; i < 4; i++) p[i] = LoadFloat(base, kIdentity + 4 * i);
    for (int i = 0; i < 4; i++) k[i] = LoadFloat(base, kf + 36 + 4 * i);
    Nlerp(base, weight, p, k, shortest, q);
  } else {
    ctx.r3.u64 = sp + 128;
    ctx.r5.u64 = kIdentity;
    ctx.r6.u64 = kf + 36;
    ctx.r7.u64 = shortest ? 1 : 0;
    ctx.f1.f64 = weight;
    __imp__sub_8244ED20(ctx, base);
    for (int i = 0; i < 4; i++) q[i] = LoadFloat(base, ctx.r3.u32 + 4 * i);
  }
  const uint32_t rotate = Slot(base, node, 120);
  if (rotate == 0x821CC1C0) {
    RotateLocal(ctx, base, node, q);
  } else {
    for (int i = 0; i < 4; i++) StoreFloat(base, sp + 112 + 4 * i, q[i]);
    CallGuest(ctx, base, rotate, node, sp + 112, 0);
  }

  // Échelle : (s - 1) * scale + 1 si scale != 1 et s != UNIT_SCALE.
  float sc[3], unit[3];
  for (int i = 0; i < 3; i++) sc[i] = LoadFloat(base, kf + 24 + 4 * i);
  for (int i = 0; i < 3; i++) unit[i] = LoadFloat(base, kUnitScale + 4 * i);
  if (scl != double(LoadFloat(base, kConstants + 64)) &&
      (sc[0] != unit[0] || sc[1] != unit[1] || sc[2] != unit[2])) {
    for (int i = 0; i < 3; i++) sc[i] = float(double(sc[i] - unit[i]) * scl) + unit[i];
  }
  const uint32_t scale = Slot(base, node, 88);
  if (scale == 0x821CCC10) {
    for (int i = 0; i < 3; i++) StoreFloat(base, node + 132 + 4 * i, sc[i] * LoadFloat(base, node + 132 + 4 * i));
    NeedUpdateVirtual(ctx, base, node);
  } else {
    for (int i = 0; i < 3; i++) StoreFloat(base, sp + 80 + 4 * i, sc[i]);
    CallGuest(ctx, base, scale, node, sp + 80);
  }
  ctx.r1.u64 = old_sp;
}

// --- Matrices des os (Skeleton::_getBoneMatrices) ---

// sub_821CB318 : Quaternion::ToRotationMatrix (q : w x y z ; m : 3x3 par lignes).
void QuaternionToMatrix(const uint8_t* base, const float q[4], float m[9]) {
#pragma clang fp contract(off)
  const float two = LoadFloat(base, kConstants + 124), one = LoadFloat(base, kConstants + 64);
  const float w = q[0], x = q[1], y = q[2], z = q[3];
  const float tz = z * two, ty = y * two, tx = x * two;
  const float tzz = z * tz, tyy = y * ty, txx = x * tx;
  const float twy = w * ty, twz = w * tz, txy = x * ty, twx = w * tx, txz = x * tz, tyz = y * tz;
  m[0] = one - (tzz + tyy);
  m[1] = txy - twz;
  m[2] = txz + twy;
  m[3] = txy + twz;
  m[4] = one - (tzz + txx);
  m[5] = tyz - twx;
  m[6] = txz - twy;
  m[7] = tyz + twx;
  m[8] = one - (tyy + txx);
}

// sub_821CB110 : produit de matrices 3x3 (a * b).
void Matrix3Multiply(const float a[9], const float b[9], float out[9]) {
#pragma clang fp contract(off)
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      out[3 * r + c] =
          FmaLikeGuest(a[3 * r + 2], b[6 + c], FmaLikeGuest(a[3 * r], b[c], a[3 * r + 1] * b[3 + c]));
    }
  }
}

// sub_821C9A18 : Bone::_getOffsetTransform(Matrix4& m) : mBindDerivedInverseScale +316, ...Orientation +328
// (w x y z), ...Position +344 ; puis Matrix4::makeTransform (sub_821CB210 ; Matrix3::ZERO en 0x8355A430).
void BoneOffsetTransform(PPCContext& ctx, uint8_t* base, uint32_t bone, float m[16]) {
#pragma clang fp contract(off)
  const uint32_t ds = Derived(ctx, base, bone, 196, kDerivedScale, 176);
  float scale[3];
  for (int i = 0; i < 3; i++) scale[i] = LoadFloat(base, bone + 316 + 4 * i) * LoadFloat(base, ds + 4 * i);
  const uint32_t dq = Derived(ctx, base, bone, 188, kDerivedOrientation, 148);
  const float dw = LoadFloat(base, dq), dx = LoadFloat(base, dq + 4);
  const float dy = LoadFloat(base, dq + 8), dz = LoadFloat(base, dq + 12);
  const float bw = LoadFloat(base, bone + 328), bx = LoadFloat(base, bone + 332);
  const float by = LoadFloat(base, bone + 336), bz = LoadFloat(base, bone + 340);
  float v[3];
  for (int i = 0; i < 3; i++) v[i] = LoadFloat(base, bone + 344 + 4 * i) * scale[i];
  const float f2 = FmaLikeGuest(bz, dy, bx * dw);
  const float f1 = FmsLikeGuest(dw, bw, dx * bx);
  const float f13 = FmaLikeGuest(dz, bx, by * dw);
  const float f11 = FmaLikeGuest(by, dx, bz * dw);
  const float f10 = FmaLikeGuest(dx, bw, f2);
  const float f9 = FnmsLikeGuest(dy, by, f1);
  const float f4 = FmaLikeGuest(dy, bw, f13);
  const float f2b = FmaLikeGuest(dz, bw, f11);
  const float rotation[4] = {FnmsLikeGuest(dz, bz, f9), FnmsLikeGuest(dz, by, f10), FnmsLikeGuest(bz, dx, f4),
                             FnmsLikeGuest(dy, bx, f2b)};
  float t[3];
  RotateVector(base, rotation, v, t);
  const uint32_t dp = Derived(ctx, base, bone, 192, kDerivedPosition, 164);
  for (int i = 0; i < 3; i++) t[i] = t[i] + LoadFloat(base, dp + 4 * i);
  float rot[9], sm[9], r[9];
  QuaternionToMatrix(base, rotation, rot);
  for (int i = 0; i < 9; i++) sm[i] = LoadFloat(base, 0x8355A430 + 4 * i);
  sm[0] = scale[0];
  sm[4] = scale[1];
  sm[8] = scale[2];
  Matrix3Multiply(rot, sm, r);
  for (int row = 0; row < 3; row++) {
    for (int c = 0; c < 3; c++) m[4 * row + c] = r[3 * row + c];
    m[4 * row + 3] = t[row];
  }
  const float zero = LoadFloat(base, kConstants + 68);
  m[12] = m[13] = m[14] = zero;
  m[15] = LoadFloat(base, kConstants + 64);
}

void StoreMatrix(uint8_t* base, uint32_t addr, const float m[16]) {
  for (int i = 0; i < 16; i++) StoreFloat(base, addr + 4 * i, m[i]);
}

// sub_821CB3C0 : Skeleton::_getBoneMatrices(Matrix4* out) : _updateTransforms [70] (sub_821B8C60 : _update(vrai,
// faux) des os racines +224..+228, puis mManualBonesDirty +264 à faux), puis une matrice par os (mBoneList
// +188..+192).
void BoneMatrices(PPCContext& ctx, uint8_t* base, uint32_t skeleton, uint32_t out) {
  const uint32_t update = Slot(base, skeleton, 280);
  if (update == 0x821B8C60) {
    const uint32_t end = Load32(base, skeleton + 228);
    for (uint32_t it = Load32(base, skeleton + 224); it != end; it += 4) {
      ChildUpdate(ctx, base, Load32(base, it), false);
    }
    base[skeleton + 264] = 0;
  } else {
    CallGuest(ctx, base, update, skeleton);
  }
  const uint32_t end = Load32(base, skeleton + 192);
  float m[16];
  for (uint32_t it = Load32(base, skeleton + 188); it != end; it += 4, out += 64) {
    BoneOffsetTransform(ctx, base, Load32(base, it), m);
    StoreMatrix(base, out, m);
  }
}

}  // namespace

REX_HOOK_RAW(sub_821C9770) {
  const int mode = Mode();
  if (mode == 1) {
    UpdateFromParentImpl(ctx, base, ctx.r3.u32);
    return;
  }
  const uint32_t node = ctx.r3.u32;
  __imp__sub_821C9770(ctx, base);
  if (mode != 2) return;
  float out[10];
  UpdateFromParentImplValues(ctx, base, node, out);  // parents à jour : mêmes entrées que l'original
  g_calls.fetch_add(1, std::memory_order_relaxed);
  bool same = true;
  for (int i = 0; i < 10 && same; i++) same = Load32(base, node + 148 + 4 * i) == FloatBits(out[i]);
  if (!same) CompterDifference(__LINE__);
  Report();
}

REX_HOOK_RAW(sub_821C9F08) {
  if (Mode() != 1) {
    __imp__sub_821C9F08(ctx, base);
    return;
  }
  UpdateFromParent(ctx, base, ctx.r3.u32);
}

REX_HOOK_RAW(sub_821C9BB0) {
  if (Mode() != 1) return __imp__sub_821C9BB0(ctx, base);
  const uint32_t node = ctx.r3.u32;
  if (base[node + 72]) UpdateFromParent(ctx, base, node);
  ctx.r3.u64 = node + 148;
}

REX_HOOK_RAW(sub_821C8518) {
  if (Mode() != 1) return __imp__sub_821C8518(ctx, base);
  const uint32_t node = ctx.r3.u32;
  if (base[node + 72]) UpdateFromParent(ctx, base, node);
  ctx.r3.u64 = node + 164;
}

REX_HOOK_RAW(sub_821C99D0) {
  if (Mode() != 1) return __imp__sub_821C99D0(ctx, base);
  const uint32_t node = ctx.r3.u32;
  if (base[node + 72]) UpdateFromParent(ctx, base, node);
  ctx.r3.u64 = node + 176;
}

REX_HOOK_RAW(sub_821C9F68) {
  if (Mode() != 1) return __imp__sub_821C9F68(ctx, base);
  NodeUpdate(ctx, base, ctx.r3.u32, ctx.r4.u8 != 0, ctx.r5.u8 != 0);
}

// sub_821CA0B8 : Node::needUpdate(forceParentUpdate).
REX_HOOK_RAW(sub_821CA0B8) {
  if (Mode() != 1) return __imp__sub_821CA0B8(ctx, base);
  NeedUpdate(ctx, base, ctx.r3.u32, ctx.r4.u64);
}

// sub_821CA140 : Bone::needUpdate.
REX_HOOK_RAW(sub_821CA140) {
  if (Mode() != 1) return __imp__sub_821CA140(ctx, base);
  BoneNeedUpdate(ctx, base, ctx.r3.u32, ctx.r4.u64);
}

// sub_821CB998 : NodeAnimationTrack::applyToNode(node, timeIndex, weight, scale).
REX_HOOK_RAW(sub_821CB998) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821CB998(ctx, base);
  const uint32_t self = ctx.r3.u32, node = ctx.r4.u32, time = ctx.r5.u32;
  const double weight = ctx.f1.f64, scl = ctx.f2.f64;
  if (mode == 1) {
    ApplyToNode(ctx, base, self, node, time, weight, scl);
    return;
  }
  // Validation : état du nœud (orientation, position, échelle) après l'original, puis après le natif depuis le même
  // état de départ (les autres effets, needUpdate et l'image clé interpolée, se répètent sans différence).
  uint8_t before[40], original[40];
  if (node) std::memcpy(before, base + node + 104, 40);
  __imp__sub_821CB998(ctx, base);
  if (!node) return;
  std::memcpy(original, base + node + 104, 40);
  std::memcpy(base + node + 104, before, 40);
  ApplyToNode(ctx, base, self, node, time, weight, scl);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (std::memcmp(original, base + node + 104, 40) != 0) {
    CompterDifference(__LINE__);
    std::memcpy(base + node + 104, original, 40);
  }
  Report();
}

// sub_821C9A18 : Bone::_getOffsetTransform.
REX_HOOK_RAW(sub_821C9A18) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821C9A18(ctx, base);
  const uint32_t bone = ctx.r3.u32, out = ctx.r4.u32;
  float m[16];
  if (mode == 1) {
    BoneOffsetTransform(ctx, base, bone, m);
    StoreMatrix(base, out, m);
    return;
  }
  __imp__sub_821C9A18(ctx, base);
  BoneOffsetTransform(ctx, base, bone, m);  // os à jour : mêmes entrées
  g_calls.fetch_add(1, std::memory_order_relaxed);
  bool same = true;
  for (int i = 0; i < 16 && same; i++) same = Load32(base, out + 4 * i) == FloatBits(m[i]);
  if (!same) CompterDifference(__LINE__);
  Report();
}

// sub_821CB3C0 : Skeleton::_getBoneMatrices.
REX_HOOK_RAW(sub_821CB3C0) {
  if (Mode() != 1) return __imp__sub_821CB3C0(ctx, base);
  BoneMatrices(ctx, base, ctx.r3.u32, ctx.r4.u32);
}

// sub_821CBDC0 : NodeAnimationTrack::getInterpolatedKeyFrame (r4 timeIndex, r5 image clé résultat).
REX_HOOK_RAW(sub_821CBDC0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821CBDC0(ctx, base);
  const uint32_t self = ctx.r3.u32, time = ctx.r4.u32, kret = ctx.r5.u32;
  if (mode == 1) {
    if (!InterpolatedKeyFrame(ctx, base, self, time, kret)) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = time;
      ctx.r5.u64 = kret;
      __imp__sub_821CBDC0(ctx, base);
    }
    return;
  }
  // Validation : translation, échelle, rotation (+12..+52) de l'original puis du natif.
  __imp__sub_821CBDC0(ctx, base);
  uint8_t original[40];
  std::memcpy(original, base + kret + 12, 40);
  if (!InterpolatedKeyFrame(ctx, base, self, time, kret)) return;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (std::memcmp(original, base + kret + 12, 40) != 0) {
    CompterDifference(__LINE__);
    std::memcpy(base + kret + 12, original, 40);
  }
  Report();
}

// --- OptimisedUtilGeneral::concatenateAffineMatrices (sub_8219DF38) ---
// dst[i] = base * src[i] (Matrix4 affines, 3 premières lignes ; dernière ligne 0 0 0 1), pour les matrices des os des
// entités animées à chaque image (Entity::_updateRenderQueue ; 1,1 % du thread principal en ville). Ordre des fma du
// jeu : première ligne fma(s[c], b0, fma(s[4+c], b1, s[8+c] * b2)) ; lignes 1 et 2, colonne 0
// fma(s0, br0, fma(s8, br2, s4 * br1)), colonnes 1 à 3 fma(s[4+c], br1, fma(s[c], br0, s[8+c] * br2)) ; + b[4r+3]
// en colonne 3.
REX_EXTERN(__imp__sub_8219DF38);

namespace {

void ConcatenateAffine(const uint8_t* base, const float b[12], uint32_t src, float out[16]) {
#pragma clang fp contract(off)
  float s[12];
  for (int i = 0; i < 12; i++) s[i] = LoadFloat(base, src + 4 * i);
  for (int c = 0; c < 4; c++) out[c] = FmaLikeGuest(s[c], b[0], FmaLikeGuest(s[4 + c], b[1], s[8 + c] * b[2]));
  out[3] = out[3] + b[3];
  for (int r = 1; r < 3; r++) {
    const float* br = b + 4 * r;
    out[4 * r] = FmaLikeGuest(s[0], br[0], FmaLikeGuest(s[8], br[2], s[4] * br[1]));
    for (int c = 1; c < 4; c++)
      out[4 * r + c] = FmaLikeGuest(s[4 + c], br[1], FmaLikeGuest(s[c], br[0], s[8 + c] * br[2]));
    out[4 * r + 3] = out[4 * r + 3] + br[3];
  }
  out[12] = out[13] = out[14] = LoadFloat(base, kConstants + 68);
  out[15] = LoadFloat(base, kConstants + 64);
}

bool Overlap(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) { return a < b + b_len && b < a + a_len; }

}  // namespace

REX_HOOK_RAW(sub_8219DF38) {
  const int mode = Mode();
  const uint32_t base_m = ctx.r4.u32, src = ctx.r5.u32, dst = ctx.r6.u32, count = ctx.r7.u32;
  // Recouvrements, y compris en place (dst = src) : l'original, qui relit ses entrées au fil des écritures.
  if (mode == 0 || Overlap(dst, count * 64, base_m, 48) || Overlap(dst, count * 64, src, count * 64))
    return __imp__sub_8219DF38(ctx, base);
  float b[12], m[16];
  for (int i = 0; i < 12; i++) b[i] = LoadFloat(base, base_m + 4 * i);
  if (mode == 1) {
    for (uint32_t i = 0; i < count; i++) {
      ConcatenateAffine(base, b, src + 64 * i, m);
      StoreMatrix(base, dst + 64 * i, m);
    }
    return;
  }
  std::vector<float> expected(size_t(count) * 16);
  for (uint32_t i = 0; i < count; i++) ConcatenateAffine(base, b, src + 64 * i, expected.data() + 16 * i);
  __imp__sub_8219DF38(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < count * 16; i++) {
    if (Load32(base, dst + 4 * i) != FloatBits(expected[i])) {
      CompterDifference(__LINE__);
      break;
    }
  }
  Report();
}

// --- GpuProgramParameters::_updateAutoParams (sub_82201EB0) ---
// Appelée pour chaque programme de chaque passe (SceneManager::updateGpuProgramParameters) : boucle sur
// mAutoConstants (+80..+84, entrées de 20 octets : type +0, index physique +4, nombre d'éléments +8, donnée +12,
// variabilité u16 +16) et grand switch de ~2 300 instructions. Le jeu n'en utilise que 10 types (relevé du
// 2026-10-02) : matrices de la source de données (AutoParamDataSource, cases virtuelles), tableau 3x4 des os (4),
// Vector4 (66). Ces types en natif (écritures dans mFloatConstants, +0) ; tout autre type présent : l'original.
// Les accesseurs de la source de données restent ceux du jeu (mêmes appels, même ordre).
REX_EXTERN(__imp__sub_82201EB0);

namespace {

// Type -> case virtuelle de l'accesseur de matrice (0 : pas une matrice simple).
uint32_t AutoMatrixSlot(uint32_t type) {
  switch (type) {
    case 0: return 44;    // ACT_WORLD_MATRIX
    case 1: return 76;    // ACT_INVERSE_WORLD_MATRIX
    case 3: return 88;    // ACT_INVERSE_TRANSPOSE_WORLD_MATRIX
    case 6: return 56;    // ACT_VIEW_MATRIX
    case 14: return 60;   // ACT_VIEWPROJ_MATRIX
    case 21: return 92;   // ACT_INVERSE_TRANSPOSE_WORLDVIEW_MATRIX
    case 22: return 68;   // ACT_WORLDVIEWPROJ_MATRIX
    case 122: return 228; // accesseur avec la donnée de l'entrée en r4
    default: return 0;
  }
}

bool AutoTypeHandled(uint32_t type) { return AutoMatrixSlot(type) || type == 4 || type == 66; }

// sub_821C2868 : _writeRawConstant(index, const Matrix4&, count) : transposée si mTransposeMatrices (+98), au plus
// 16 flottants.
void WriteMatrixConstant(uint8_t* base, uint32_t params, uint32_t index, uint32_t matrix, uint32_t count) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++) m[i] = Load32(base, matrix + 4 * i);
  if (base[params + 98]) {
    uint32_t t[16];
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) t[4 * r + c] = m[4 * c + r];
    std::memcpy(m, t, sizeof(m));
  }
  const uint32_t dst = Load32(base, params) + index * 4;
  for (uint32_t i = 0; i < std::min<uint32_t>(count, 16); i++) Store32(base, dst + 4 * i, m[i]);
}

void UpdateAutoParams(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t source, uint32_t mask) {
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 128;  // Vector4 du cas 66 en +80
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  Store32(base, self + 100, 0xFFFFFFFFu);
  for (uint32_t p = Load32(base, self + 80); p != Load32(base, self + 84); p += 20) {
    if (!((Load32(base, p + 16) >> 16) & mask)) continue;
    const uint32_t type = Load32(base, p), index = Load32(base, p + 4), count = Load32(base, p + 8);
    if (const uint32_t slot = AutoMatrixSlot(type)) {
      const uint32_t m = CallGuest(ctx, base, Slot(base, source, slot), source, type == 122 ? Load32(base, p + 12) : 0);
      WriteMatrixConstant(base, self, index, m, count);
    } else if (type == 4) {  // ACT_WORLD_MATRIX_ARRAY_3x4 : 48 octets par matrice
      const uint32_t array = CallGuest(ctx, base, Slot(base, source, 48), source);
      const uint32_t n = CallGuest(ctx, base, Slot(base, source, 52), source);
      const uint32_t dst = Load32(base, self) + index * 4;
      for (uint32_t i = 0; i < n; i++) std::memmove(base + dst + 48 * i, base + array + 64 * i, 48);
    } else if (type == 66) {  // Vector4 rendu par valeur (r3 : sortie), au plus 4 flottants
      ctx.r3.u64 = sp + 80;
      ctx.r4.u64 = source;
      GuestFunc(base, Slot(base, source, 196))(ctx, base);
      const uint32_t v = ctx.r3.u32, dst = Load32(base, self) + index * 4;
      for (uint32_t i = 0; i < std::min<uint32_t>(count, 4); i++) Store32(base, dst + 4 * i, Load32(base, v + 4 * i));
    }
  }
  ctx.r1.u64 = old_sp;
}

}  // namespace

REX_HOOK_RAW(sub_82201EB0) {
  const int mode = Mode();
  const uint32_t self = ctx.r3.u32, source = ctx.r4.u32, mask = ctx.r5.u32 & 0xFFFF;
  const uint32_t b = Load32(base, self + 80), e = Load32(base, self + 84);
  if (b == e || !((Load32(base, self + 96) >> 16) & mask)) return;
  bool handled = mode != 0;
  for (uint32_t p = b; p != e && handled; p += 20)
    if (((Load32(base, p + 16) >> 16) & mask) && !AutoTypeHandled(Load32(base, p))) handled = false;
  if (!handled) return __imp__sub_82201EB0(ctx, base);
  if (mode == 1) {
    UpdateAutoParams(ctx, base, self, source, mask);
    return;
  }
  // Validation : tampon des constantes (+0..+4) après l'original, puis après le natif (accesseurs mis en cache par
  // le premier passage : mêmes valeurs).
  const uint32_t cb = Load32(base, self), ce = Load32(base, self + 4);
  __imp__sub_82201EB0(ctx, base);
  std::vector<uint8_t> original(base + cb, base + ce);
  UpdateAutoParams(ctx, base, self, source, mask);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (std::memcmp(original.data(), base + cb, original.size()) != 0) {
    CompterDifference(__LINE__);
    std::memcpy(base + cb, original.data(), original.size());
  }
  Report();
}

// --- Parcours de l'octree (sub_821C7158 : OctreeSceneManager::walkOctree, variante Xbox) ---
// Console (2026-10-15, lot 2) : 4 à 7 % du thread principal en temps propre en ville et dans la mine.
// r3 caméra, r4 file de rendu, r5 octant racine. Parcours itératif sur une pile explicite en mémoire du jeu
// (0x83582C00, premiers 128 octets mis à zéro par dcbzl ; entrée = octant | 1 si entièrement visible). Pour chaque
// octant non vide (+96 : nombre de nœuds, enfants compris) : visibilité (sub_821C7000 : limites élargies ;
// sub_821C0E78 : 0 aucune, 1 partielle, 2 complète) sauf si entièrement visible ; nœuds de scène (std::list +80 :
// suivant +0, nœud +8) : dans un octant partiel, boîte du nœud (+356) contre le frustum (BoxVisible, sur
// mCullFrustum +1224 de la caméra s'il existe) ; dans un octant complet, le résultat du dernier test est gardé
// (r18 du jeu, 1 au départ : reproduit tel quel). Objets du nœud (liste +312 : suivant +0, objet +36) :
// isVisible [26], puis _notifyCurrentCamera [18] (caméra) et _updateRenderQueue [23] (file). Puis les 8 enfants
// (+48..+76) empilés dans l'ordre, avec le drapeau si l'octant est entièrement visible.
// Validation (mode 2) : parcours natif « à blanc » (sans _notifyCurrentCamera ni _updateRenderQueue) avant
// l'original ; mêmes visibilités d'octants, mêmes tests de boîtes (adresse, résultat), même contenu de la pile.
REX_EXTERN(__imp__sub_821C7158);
REX_EXTERN(__imp__sub_821C7000);
REX_EXTERN(__imp__sub_821C0E78);
REX_EXTERN(__imp__sub_821C3660);  // MovableObject::isVisible (cas courant)
REX_EXTERN(__imp__sub_821C7088);
REX_EXTERN(__imp__sub_821C0B20);  // MovableObject::_notifyCurrentCamera
REX_EXTERN(__imp__sub_821BE5A8);  // _updateRenderQueue (cas courant)

namespace {

constexpr uint32_t kOctreeStack = 0x83582C00;
constexpr uint32_t kInfinity = 0x8356C9C8;    // lis -31913 ; lfs -13880
constexpr uint32_t kZeroVector = 0x8355A3BC;  // Vector3::ZERO

struct Box {
  float min[3], max[3];
  uint32_t extent;
};

// sub_821C7000(r3 octant, r4 boîte) : Octree::_getCullBounds, mBox (+0 minimum, +12 maximum) élargie de mHalfSize
// (+36), puis sub_821C7698 (setExtents de la variante Xbox : étendue finie, et un minimum plus grand que le maximum
// devient maximum - constante +264).
Box OctantCullBounds(const uint8_t* base, uint32_t node) {
  Box b;
  for (int i = 0; i < 3; i++) {
    const float h = LoadFloat(base, node + 36 + 4 * i);
    b.min[i] = LoadFloat(base, node + 4 * i) - h;
    b.max[i] = LoadFloat(base, node + 12 + 4 * i) + h;
  }
  b.extent = 1;
  const float eps = LoadFloat(base, kConstants + 264);
  for (int i = 0; i < 3; i++)
    if (b.min[i] > b.max[i]) b.min[i] = b.max[i] - eps;
  return b;
}

Box LoadBox(const uint8_t* base, uint32_t box) {
  Box b;
  for (int i = 0; i < 3; i++) {
    b.min[i] = LoadFloat(base, box + 4 * i);
    b.max[i] = LoadFloat(base, box + 12 + 4 * i);
  }
  b.extent = Load32(base, box + 24);
  return b;
}

// sub_821C0E78(r3 caméra, r4 boîte) : OctreeCamera::getVisibility. Plans par getFrustumPlane [91] (le plan 1, le
// lointain, est sauté si mFarDist +228 est nul). Opérations dans l'ordre du jeu (fmuls, fabs, fmadds, fadds).
int Visibility(PPCContext& ctx, uint8_t* base, uint32_t cam, const Box& b) {
  if (b.extent == 0) return 0;
  const float half = LoadFloat(base, kConstants + 104), zero = LoadFloat(base, kConstants + 68);
  const float cy = (b.max[1] + b.min[1]) * half, cz = (b.max[2] + b.min[2]) * half;
  const float cx = (b.min[0] + b.max[0]) * half;
  float hx, hy, hz;
  if (b.extent == 1) {
    hx = (b.max[0] - b.min[0]) * half;
    hy = (b.max[1] - b.min[1]) * half;
    hz = (b.max[2] - b.min[2]) * half;
  } else if (b.extent == 2) {
    hx = hy = hz = LoadFloat(base, kInfinity);
  } else {
    hx = LoadFloat(base, kZeroVector);
    hy = LoadFloat(base, kZeroVector + 4);
    hz = LoadFloat(base, kZeroVector + 8);
  }
  const uint32_t get_plane = Slot(base, cam, 364);
  bool inside = true;
  for (uint32_t i = 0; i < 6; i++) {
    if (i == 1 && LoadFloat(base, cam + 228) == zero) continue;
    const uint32_t p = CallGuest(ctx, base, get_plane, cam, i);
    const float nx = LoadFloat(base, p), ny = LoadFloat(base, p + 4), nz = LoadFloat(base, p + 8);
    const float d = LoadFloat(base, p + 12);
    const float ay = std::fabs(ny * hy), ax = std::fabs(nx * hx), az = std::fabs(nz * hz);
    const float dist = FmaLikeGuest(ny, cy, FmaLikeGuest(nx, cx, nz * cz)) + d;
    const float extent = (ay + ax) + az;
    if (dist < -extent) return 0;
    if (!(dist > extent)) inside = false;
  }
  return inside ? 2 : 1;
}

// Appel virtuel du parcours (r3 objet, r4) ; les cibles courantes appelées directement (comme les appels promus).
uint32_t CallObject(PPCContext& ctx, uint8_t* base, uint32_t func, uint32_t obj, uint32_t r4) {
  ctx.r3.u64 = obj;
  ctx.r4.u64 = r4;
  switch (func) {
    case 0x821C3660: __imp__sub_821C3660(ctx, base); break;
    case 0x821C7088: __imp__sub_821C7088(ctx, base); break;
    case 0x821C0B20: __imp__sub_821C0B20(ctx, base); break;
    case 0x821BE5A8: __imp__sub_821BE5A8(ctx, base); break;
    default: GuestFunc(base, func)(ctx, base); break;
  }
  return ctx.r3.u32;
}

// Parcours ; dry : à blanc (validation) : pile dans `dry_stack`, sans _notifyCurrentCamera ni _updateRenderQueue.
void WalkOctree(PPCContext& ctx, uint8_t* base, uint32_t cam, uint32_t queue, uint32_t root,
                std::vector<uint32_t>* events, std::vector<uint32_t>* dry_stack) {
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 256;
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  const uint32_t cull = Load32(base, cam + 1224);
  const uint32_t planes = cull ? cull : cam;
  auto store = [&](uint32_t i, uint32_t v) {
    if (dry_stack) {
      if (dry_stack->size() <= i) dry_stack->resize(i + 1, 0);
      (*dry_stack)[i] = v;
    } else {
      Store32(base, kOctreeStack + 4 * i, v);
    }
  };
  auto load = [&](uint32_t i) { return dry_stack ? (*dry_stack)[i] : Load32(base, kOctreeStack + 4 * i); };
  if (dry_stack) {
    dry_stack->assign(32, 0);
  } else {
    std::memset(base + kOctreeStack, 0, 128);
  }
  store(0, root);
  uint32_t top = 1;
  bool box_visible = true;  // r18
  while (top) {
    const uint32_t entry = load(--top), node = entry & ~1u;
    if (top) {  // dcbt du jeu : octant suivant
      const uint8_t* next = base + (load(top - 1) & ~1u);
      __builtin_prefetch(next);
      __builtin_prefetch(next + 64);
    }
    if (Load32(base, node + 96) == 0) continue;
    int vis = 2;
    if (!(entry & 1)) {
      vis = Visibility(ctx, base, cam, OctantCullBounds(base, node));
      if (events) events->insert(events->end(), {1u, uint32_t(vis)});
      if (vis == 0) continue;
    }
    const uint32_t flag = vis == 2 ? 1 : 0;
    const uint32_t nodes = Load32(base, node + 80);
    for (uint32_t it = Load32(base, nodes); it != nodes; it = Load32(base, it)) {
      const uint32_t scene_node = Load32(base, it + 8);
      __builtin_prefetch(base + scene_node + 356);
      __builtin_prefetch(base + scene_node + 312);
      if (vis == 1) {
        box_visible = BoxVisible(base, planes, scene_node + 356);
        if (events) events->insert(events->end(), {2u, scene_node + 356, box_visible ? 1u : 0u});
      }
      if (!box_visible) continue;
      const uint32_t objects = Load32(base, scene_node + 312);
      for (uint32_t o = Load32(base, objects); o != objects; o = Load32(base, o)) {
        const uint32_t obj = Load32(base, o + 36);
        __builtin_prefetch(base + Load32(base, o) + 36);
        if (!(CallObject(ctx, base, Slot(base, obj, 104), obj, 0) & 0xFF) || dry_stack) continue;
        CallObject(ctx, base, Slot(base, obj, 72), obj, cam);
        CallObject(ctx, base, Slot(base, obj, 92), obj, queue);
      }
    }
    for (uint32_t k = 0; k < 8; k++) {
      const uint32_t child = Load32(base, node + 48 + 4 * k);
      if (child) store(top++, child | flag);
    }
  }
  ctx.r1.u64 = old_sp;
}

uint64_t g_octree_walks = 0, g_octree_diffs = 0, g_octree_compared = 0;

}  // namespace

REX_HOOK_RAW(sub_821C7158) {
  const int mode = Mode();
  torchlight::particules::BillboardsDebut(ctx, base);
  torchlight::poses::ParcoursDebut();
  // Avant de rendre la main au jeu, sur tous les chemins : plus rien ne doit rester à faire.
  struct FinParcours {
    PPCContext& ctx;
    uint8_t* base;
    ~FinParcours() {
      torchlight::poses::ParcoursFin(ctx, base);
      torchlight::particules::BillboardsFin(ctx, base);
    }
  } fin_parcours{ctx, base};
  if (mode == 0) return __imp__sub_821C7158(ctx, base);
  const uint32_t cam = ctx.r3.u32, queue = ctx.r4.u32, root = ctx.r5.u32;
  if (mode == 1) return WalkOctree(ctx, base, cam, queue, root, nullptr, nullptr);
  std::vector<uint32_t> expected, seen, stack;
  WalkOctree(ctx, base, cam, queue, root, &expected, &stack);
  g_octree_events = &seen;
  g_octree_sp = ctx.r1.u32 - 256;
  ctx.r3.u64 = cam;
  ctx.r4.u64 = queue;
  ctx.r5.u64 = root;
  __imp__sub_821C7158(ctx, base);
  g_octree_events = nullptr;
  bool same = seen == expected;
  g_octree_compared += expected.size() + stack.size();
  for (uint32_t i = 0; same && i < stack.size(); i++) same = Load32(base, kOctreeStack + 4 * i) == stack[i];
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (!same) CompterDifference(__LINE__);
  if (!same && g_octree_diffs++ < 8)
    REXLOG_ERROR("[ogre natif] parcours de l'octree : différence ({} événements natifs, {} de l'original)",
                 expected.size(), seen.size());
  if (++g_octree_walks % 2000 == 0)
    REXLOG_INFO("[ogre natif] parcours de l'octree : {} parcours validés ({} mots comparés), {} différences",
                g_octree_walks, g_octree_compared, g_octree_diffs);
  Report();
}

REX_HOOK_RAW(sub_821C7000) {
  const int mode = Mode();
  const uint32_t node = ctx.r3.u32, box = ctx.r4.u32;
  if (mode == 0) return __imp__sub_821C7000(ctx, base);
  const Box b = OctantCullBounds(base, node);
  uint32_t words[7];
  for (int i = 0; i < 3; i++) {
    words[i] = FloatBits(b.min[i]);
    words[3 + i] = FloatBits(b.max[i]);
  }
  words[6] = b.extent;
  if (mode == 2) {
    __imp__sub_821C7000(ctx, base);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 7; i++) {
      if (Load32(base, box + 4 * i) != words[i]) {
        CompterDifference(__LINE__);
        break;
      }
    }
    Report();
    return;
  }
  for (int i = 0; i < 7; i++) Store32(base, box + 4 * i, words[i]);
  ctx.r3.u64 = box;
}

REX_HOOK_RAW(sub_821C0E78) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821C0E78(ctx, base);
  const uint32_t cam = ctx.r3.u32, box = ctx.r4.u32;
  if (mode == 1) {
    ctx.r3.u64 = uint32_t(Visibility(ctx, base, cam, LoadBox(base, box)));
    return;
  }
  const int vis = Visibility(ctx, base, cam, LoadBox(base, box));
  ctx.r3.u64 = cam;
  ctx.r4.u64 = box;
  __imp__sub_821C0E78(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32 != uint32_t(vis)) CompterDifference(__LINE__);
  OctreeEvent(ctx, {1u, ctx.r3.u32});
  Report();
}

// --- Largeur d'un texte de CEGUI (sub_821B3DA0 : Font::getTextExtent de la variante du jeu) ---
// Console (2026-10-15) : ~40 % du thread principal dans la scène du PNJ donneur de quête (18 images/s), appelée par
// la mise en page des textes (retours à la ligne) pour chaque mot. r3 police, r4 texte (CEGUI::String : longueur +0,
// réserve +4, caractères UTF-32 en +20 si la réserve est d'au plus 32, sinon pointeur en +148), f1 échelle. L'original
// convertit le texte et les deux balises de la police (CEGUI::String +724 et +876) en UTF-8 (c_str, tampon de la
// chaîne), puis en std::string, puis en std::wstring (MultiByteToWideChar, sub_821B36A8 -> sub_821B3780 :
// ConvertUTF8toUTF16 permissif d'Unicode Inc.), à chaque appel. Pour des points de code de 1 à 0xFFFF, l'aller-retour
// est l'identité (un 0 tronquerait la std::string, au-delà de 0xFFFF une paire de substitution changerait les
// indices) : la native lit directement les points de code ; sinon, l'original. Les allocations temporaires et le
// tampon UTF-8 des chaînes (cache reconstruit par chaque c_str) ne sont plus faits.
// Balises (si l'octet +1028 de la police est non nul et que le texte contient un caractère de l'une d'elles :
// sub_821A5CE0, find_first_of) : à l'indice i, balise 1 suivie de 8 caractères sautée, sinon balise 2 sautée
// (comparaison sub_821B39E0, wcsncmp du jeu). Largeur : max(x + (largeur + décalage de l'image) * échelle), x avancé
// de advance * échelle (fmadds).
REX_EXTERN(__imp__sub_821B3DA0);
REX_EXTERN(__imp__sub_821B3C88);

namespace {

// Caractères UTF-32 d'une CEGUI::String ; faux si un point de code est hors de 1..0xFFFF.
bool CeguiChars(const uint8_t* base, uint32_t str, std::vector<uint16_t>& out) {
  const uint32_t len = Load32(base, str);
  const uint32_t data = Load32(base, str + 4) > 32 ? Load32(base, str + 148) : str + 20;
  out.resize(len + 1);
  for (uint32_t i = 0; i < len; i++) {
    const uint32_t c = Load32(base, data + 4 * i);
    if (c - 1 >= 0xFFFF) return false;
    out[i] = uint16_t(c);
  }
  out[len] = 0;
  return true;
}

// sub_821B39E0 : wcsncmp du jeu (n nul : égal).
int GuestWcsncmp(const uint16_t* a, const uint16_t* b, uint32_t n) {
  if (n == 0) return 0;
  for (uint32_t k = n - 1; k && *a && *a == *b; k--) {
    a++;
    b++;
  }
  return int(*a) - int(*b);
}

bool AnyOf(const std::vector<uint16_t>& text, const std::vector<uint16_t>& chars) {
  for (size_t i = 0; i + 1 < text.size(); i++)
    for (size_t k = 0; k + 1 < chars.size(); k++)
      if (text[i] == chars[k]) return true;
  return false;
}

// sub_821B3C88(r3 police, r4 point de code) : Font::getGlyphData. Au-delà de +524 : aucun. Pages de 256 points de
// code chargées à la demande (bits en +720 ; sinon bit posé puis rasterize [3] (police, début, fin) du jeu), puis
// recherche dans la std::map des glyphes (en-tête +24 ; nœud : gauche +0, parent +4, droite +8, clé +12, valeur +16,
// isnil +25). Rend l'adresse du FontGlyph (image +0, avance +4) ou 0.
uint32_t GlyphData(PPCContext& ctx, uint8_t* base, uint32_t font, uint32_t cp) {
  if (cp > Load32(base, font + 524)) return 0;
  if (const uint32_t bits = Load32(base, font + 720)) {
    const uint32_t word = bits + (cp >> 13) * 4, bit = 1u << ((cp >> 8) & 31);
    if (!(Load32(base, word) & bit)) {
      Store32(base, word, Load32(base, word) | bit);
      CallGuest(ctx, base, Slot(base, font, 12), font, cp & ~0xFFu, cp | 0xFFu);
    }
  }
  const uint32_t head = Load32(base, font + 24);
  uint32_t lb = head;
  for (uint32_t n = Load32(base, head + 4); !base[n + 25];) {
    if (Load32(base, n + 12) < cp) {
      n = Load32(base, n + 8);
    } else {
      lb = n;
      n = Load32(base, n);
    }
  }
  if (lb == head || cp < Load32(base, lb + 12)) return 0;
  return lb + 16;
}

thread_local bool g_text_tags = false;  // validation : le dernier texte avait des balises

bool TextExtent(PPCContext& ctx, uint8_t* base, uint32_t font, uint32_t text, double scale, float& result) {
  thread_local std::vector<uint16_t> w, tag1, tag2;
  if (!CeguiChars(base, text, w) || !CeguiChars(base, font + 724, tag1) || !CeguiChars(base, font + 876, tag2))
    return false;
  const uint32_t len = uint32_t(w.size() - 1), len1 = uint32_t(tag1.size() - 1), len2 = uint32_t(tag2.size() - 1);
  const uint32_t data = Load32(base, text + 4) > 32 ? Load32(base, text + 148) : text + 20;
  const bool tags = base[font + 1028] && (AnyOf(w, tag1) || AnyOf(w, tag2));
  g_text_tags = tags;
  float x = LoadFloat(base, kConstants + 68), extent = x;
  for (uint32_t i = 0; i < len; i++) {
    if (tags) {
      if (i + len1 + 8 <= len && GuestWcsncmp(&w[i], tag1.data(), len1) == 0)
        i += len1 + 8;
      else if (i + len2 <= len && GuestWcsncmp(&w[i], tag2.data(), len2) == 0)
        i += len2;
      if (i >= len) continue;
    }
    const uint32_t glyph = GlyphData(ctx, base, font, Load32(base, data + 4 * i));
    if (!glyph) continue;
    const uint32_t image = Load32(base, glyph);
    const float right = float(double(LoadFloat(base, image + 36) + LoadFloat(base, image + 28)) * scale) + x;
    if (right > extent) extent = right;
    x = float(std::fma(double(LoadFloat(base, glyph + 4)), scale, double(x)));
  }
  result = x < extent ? extent : x;
  return true;
}

}  // namespace

REX_HOOK_RAW(sub_821B3DA0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821B3DA0(ctx, base);
  const uint32_t font = ctx.r3.u32, text = ctx.r4.u32;
  const double scale = ctx.f1.f64;
  float result;
  if (mode == 1) {
    if (TextExtent(ctx, base, font, text, scale, result)) {
      ctx.f1.f64 = double(result);
    } else {
      ctx.r3.u64 = font;
      ctx.r4.u64 = text;
      ctx.f1.f64 = scale;
      __imp__sub_821B3DA0(ctx, base);
    }
    return;
  }
  // Validation : l'original d'abord (il charge les pages de glyphes manquantes), puis la native.
  static uint64_t validated = 0, fallback = 0, diffs = 0, tagged = 0;
  __imp__sub_821B3DA0(ctx, base);
  const double original = ctx.f1.f64;
  if (TextExtent(ctx, base, font, text, scale, result)) {
    validated++;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_text_tags) tagged++;
    const double native = double(result);
    if (std::memcmp(&original, &native, 8) != 0) {
      CompterDifference(__LINE__);
      if (diffs++ < 8) REXLOG_ERROR("[ogre natif] largeur de texte : {} au lieu de {}", native, original);
    }
  } else {
    fallback++;
  }
  if ((validated + fallback) % 20000 == 0)
    REXLOG_INFO("[ogre natif] largeur de texte : {} validées ({} avec balises), {} par l'original, {} différences",
                validated, tagged, fallback, diffs);
  ctx.f1.f64 = original;
  Report();
}

REX_HOOK_RAW(sub_821B3C88) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821B3C88(ctx, base);
  const uint32_t font = ctx.r3.u32, cp = ctx.r4.u32;
  if (mode == 1) {
    ctx.r3.u64 = GlyphData(ctx, base, font, cp);
    return;
  }
  __imp__sub_821B3C88(ctx, base);
  const uint32_t original = ctx.r3.u32;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (GlyphData(ctx, base, font, cp) != original) CompterDifference(__LINE__);
  ctx.r3.u64 = original;
  Report();
}

// --- Contrôleurs (sub_821C8C00 : ControllerManager::updateAllControllers) ---
// Console (2026-10-15) : 1,6 % du thread principal en temps propre (7,6 % avec les particules). Une fois par image
// (Root +224 : numéro de l'image suivante, comparé à +56) : pour chaque contrôleur du std::set (en-tête +8 ; nœud :
// valeur +12, isnil +17) actif (+52), valeur = source (+8) getValue [1], puis fonction (+40) calculate [1], puis
// destination (+24) setValue [2]. Cas courants en natif (99,9 % des appels d'après le relevé des appels indirects) :
// source FrameTimeControllerValue (sub_821C8BF8 : +8), fonctions sub_821C8CD8 (getAdjustedInput) et sub_821D2EE0
// (entrée multipliée par +12, puis getAdjustedInput), destination sub_821D2E50 (modificateur de coordonnées de
// texture), ParticleSystemUpdateValue (sub_821C8A58) d'un système arrêté ; les systèmes actifs et les autres restent
// ceux du jeu. Console (2026-10-27, profil de combat) : contrôleurs 17,5 % du thread principal, dont 8,2 % de temps
// propre de la boucle : ~1 700 contrôleurs par image, dont ~1 300 de systèmes arrêtés qui ne font rien. D'où la liste
// plate (--torchlight_controleurs=1, plus bas) : mine 4,0 -> 2,0 ms par image, ville 2,1 -> 1,2 ms. Les préchargements
// par vagues (tous les contrôleurs, ou les seuls systèmes actifs) ralentissaient la console : retirés.
// Validation (mode 2) : chaque calcul natif comparé à la fonction du jeu sur le même état (sauvegardé puis remis),
// successeur dans l'arbre comparé à sub_824C6960 ; liste comparée au set, dormants vérifiés sans effet.
REX_EXTERN(__imp__sub_821C8C00);
REX_EXTERN(__imp__sub_821C8A58);
REX_EXTERN(__imp__sub_821C8CD8);
REX_EXTERN(__imp__sub_821D2EE0);
REX_EXTERN(__imp__sub_821D2E50);
REX_EXTERN(__imp__sub_824C6960);

namespace {

constexpr uint32_t kTwoPi = 0x8356C9CC;  // lis -31913 ; lfs -13876

// sub_821E8398(r3 fonction, f1) : ControllerFunction::getAdjustedInput. Avec mDeltaInput (+4), valeur (+8) cumulée et
// ramenée dans [0, 1) par pas de 1.0 (soustraction en double, arrondie), comme le jeu.
double AdjustedInput(uint8_t* base, uint32_t func, double input) {
  if (!base[func + 4]) return input;
  float v = float(double(LoadFloat(base, func + 8)) + input);
  StoreFloat(base, func + 8, v);
  const float one = LoadFloat(base, kConstants + 64), zero = LoadFloat(base, kConstants + 68);
  const double step = LoadDouble(base, kConstants);
  if (!(v < one)) {
    do {
      v = float(double(LoadFloat(base, func + 8)) - step);
      StoreFloat(base, func + 8, v);
    } while (!(v < one));
  }
  if (LoadFloat(base, func + 8) < zero) {
    do {
      v = float(double(LoadFloat(base, func + 8)) + step);
      StoreFloat(base, func + 8, v);
    } while (v < zero);
  }
  return double(LoadFloat(base, func + 8));
}

// sub_821D2E50(r3 valeur, f1) : translations U/V (+4, +5), échelles U/V (+6, +7), rotation (+8, multipliée par 2π) de
// l'unité de texture (+12 : champs +212..+228, recalcul de la matrice +211).
void TexCoordModifier(uint8_t* base, uint32_t self, double value) {
  for (uint32_t k = 0; k < 4; k++) {
    if (!base[self + 4 + k]) continue;
    const uint32_t tus = Load32(base, self + 12);
    StoreFloat(base, tus + 212 + 4 * k, float(value));
    base[tus + 211] = 1;
  }
  if (base[self + 8]) {
    const uint32_t tus = Load32(base, self + 12);
    StoreFloat(base, tus + 228, float(double(LoadFloat(base, kTwoPi)) * value));
    base[tus + 211] = 1;
  }
}

uint64_t BitsOf(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  return bits;
}

void Diff() { CompterDifference(__LINE__); }

// Fonction du contrôleur ; validation : calcul natif sur l'état sauvegardé, état remis, puis la fonction du jeu.
double ControllerFunction(PPCContext& ctx, uint8_t* base, uint32_t func, uint32_t entry, double input, bool check) {
  auto native = [&]() {
    return entry == 0x821C8CD8 ? AdjustedInput(base, func, input)
                               : AdjustedInput(base, func, double(float(double(LoadFloat(base, func + 12)) * input)));
  };
  if ((entry == 0x821C8CD8 || entry == 0x821D2EE0) && !check) return native();
  const uint32_t saved = Load32(base, func + 8);
  double expected = 0;
  uint32_t expected_state = 0;
  const bool known = entry == 0x821C8CD8 || entry == 0x821D2EE0;
  if (known) {
    expected = native();
    expected_state = Load32(base, func + 8);
    Store32(base, func + 8, saved);
  }
  ctx.r3.u64 = func;
  ctx.f1.f64 = input;
  if (entry == 0x821C8CD8)
    __imp__sub_821C8CD8(ctx, base);
  else if (entry == 0x821D2EE0)
    __imp__sub_821D2EE0(ctx, base);
  else
    GuestFunc(base, entry)(ctx, base);
  if (known) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (BitsOf(ctx.f1.f64) != BitsOf(expected) || Load32(base, func + 8) != expected_state) Diff();
  }
  return ctx.f1.f64;
}

// sub_821C8A58 (ParticleUniverse::ParticleSystemUpdateValue::setValue), système (+4) arrêté ou en pause (état +284 =
// 2 ou 3 ; ~90 % des ~700 systèmes d'une image en ville) : compteur +280 augmenté tant qu'il est sous 900 ; quand il
// atteint 800, l'original prévient aussi le rendu de chaque technique : ce cas (compteur à 799) et les systèmes
// actifs restent au jeu.
bool IdleParticleSystem(uint8_t* base, uint32_t dest) {
  const uint32_t sys = Load32(base, dest + 4);
  const int32_t state = int32_t(Load32(base, sys + 284));
  if (state != 2 && state != 3) return false;
  const int32_t count = int32_t(Load32(base, sys + 280));
  if (count == 799) return false;
  if (count < 900) Store32(base, sys + 280, uint32_t(count + 1));
  return true;
}

// --torchlight_controleurs=0 : systèmes arrêtés laissés au jeu (comme le premier zip de la 1.0.3), pour l'A/B.
int ControllersVariant() {
  static const int variant = REXCVAR_GET(torchlight_controleurs);
  return variant;
}

void ControllerDestination(PPCContext& ctx, uint8_t* base, uint32_t ctrl, uint32_t dest, uint32_t entry, double value,
                           bool check) {
  if (entry == 0x821D2E50 && !check) return TexCoordModifier(base, dest, value);
  if (entry == 0x821C8A58 && ControllersVariant() != 0) {
    if (!check) {
      if (IdleParticleSystem(base, dest)) return;
      // Système actif admis : mis à jour à la fin de la boucle, en parallèle (torchlight_particules.cpp).
      if (torchlight::particules::Prendre(base, ctrl, dest, value)) return;
    } else {
      const uint32_t sys = Load32(base, dest + 4), saved_count = Load32(base, sys + 280);
      const bool handled = IdleParticleSystem(base, dest);
      const uint32_t expected_count = Load32(base, sys + 280);
      Store32(base, sys + 280, saved_count);
      ctx.r3.u64 = dest;
      ctx.f1.f64 = value;
      __imp__sub_821C8A58(ctx, base);
      if (handled) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (Load32(base, sys + 280) != expected_count) Diff();
      }
      return;
    }
  }
  uint8_t saved[24], expected[24];
  const uint32_t tus = Load32(base, dest + 12);
  const bool known = entry == 0x821D2E50 && tus;
  if (known) {
    std::memcpy(saved, base + tus + 208, 24);
    TexCoordModifier(base, dest, value);
    std::memcpy(expected, base + tus + 208, 24);
    std::memcpy(base + tus + 208, saved, 24);
  }
  ctx.r3.u64 = dest;
  ctx.f1.f64 = value;
  if (entry == 0x821D2E50)
    __imp__sub_821D2E50(ctx, base);
  else if (entry == 0x821C8A58)
    __imp__sub_821C8A58(ctx, base);
  else
    GuestFunc(base, entry)(ctx, base);
  if (known) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (std::memcmp(base + tus + 208, expected, 24) != 0) Diff();
  }
}

// Bilan toutes les 10 s : contrôleurs par image, dormants sautés, temps de la boucle (particules actives comprises).
struct CtrlStats {
  uint64_t frames = 0, total = 0, skipped = 0, rebuilds = 0, resumed = 0, inserted = 0, removed = 0;
  double prefetch_ms = 0, loop_ms = 0;
} g_cs;

void ReportControllers() {
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10) || !g_cs.frames) return;
  last = now;
  const double f = double(g_cs.frames);
  if (torchlight::coeurs::JournalStats()) REXLOG_INFO("[contrôleurs] par image : {:.0f} contrôleurs, {:.0f} sautés (dormants) ; ms : liste "
              "{:.3f}, total {:.3f} ; listes refaites {}, reprises dans le set {}, insertions {}, retraits {}",
              (g_cs.total + g_cs.skipped) / f, g_cs.skipped / f, g_cs.prefetch_ms / f, g_cs.loop_ms / f, g_cs.rebuilds,
              g_cs.resumed, g_cs.inserted, g_cs.removed);
  g_cs = CtrlStats{};
}

// Un contrôleur : source (+8) getValue [1], fonction (+40) calculate [1], destination (+24) setValue [2], s'il est
// actif (+52).
void UpdateController(PPCContext& ctx, uint8_t* base, uint32_t ctrl, bool check) {
  g_cs.total++;
  if (!base[ctrl + 52]) return;
  const uint32_t src = Load32(base, ctrl + 8), func = Load32(base, ctrl + 40), dest = Load32(base, ctrl + 24);
  const uint32_t func_vt = Load32(base, func), dest_vt = Load32(base, dest);
  const uint32_t get = Slot(base, src, 4);
  double v;
  if (get == 0x821C8BF8) {
    v = double(LoadFloat(base, src + 8));
  } else {
    ctx.r3.u64 = src;
    GuestFunc(base, get)(ctx, base);
    v = ctx.f1.f64;
  }
  v = ControllerFunction(ctx, base, func, Load32(base, func_vt + 4), v, check);
  ControllerDestination(ctx, base, ctrl, dest, Load32(base, dest_vt + 8), v, check);
}

// Liste plate des contrôleurs dans l'ordre du std::set (pointeurs croissants), tenue à jour à chaque création
// (createController sub_824B4C78 : insertion à sa place) et destruction (~Controller sub_824B5A10 : retrait) faite sur
// le thread de la boucle en dehors de celle-ci ; refaite en entier à l'image suivante sinon (pendant la boucle, autre
// thread, contrôleur inconnu) ou si la taille du set (+12) diffère.
// Dormant : source FrameTimeControllerValue, fonction getAdjustedInput sans mDeltaInput (sans état), destination
// ParticleSystemUpdateValue ; tant que son système est arrêté (état +284 = 2 ou 3) avec le compteur +280 à 900 ou
// plus, toute sa mise à jour ne fait rien (IdleParticleSystem) : seules la ligne du contrôleur (actif, destination
// inchangée) et celle du système sont lues. PC, en jeu : ~1 300 dormants sur ~1 700 contrôleurs.
struct ControllerEntry {
  uint32_t ctrl, dest, sys;  // sys : système d'un contrôleur qui peut dormir, sinon 0
};
std::vector<ControllerEntry> g_controllers;
std::atomic<bool> g_controllers_dirty{true};
thread_local bool t_controllers_owner = false;  // thread qui fait la boucle (le principal)
bool g_controllers_looping = false;

bool CanSleep(const uint8_t* base, uint32_t ctrl) {
  const uint32_t src = Load32(base, ctrl + 8), func = Load32(base, ctrl + 40), dest = Load32(base, ctrl + 24);
  return Slot(base, src, 4) == 0x821C8BF8 && Slot(base, func, 4) == 0x821C8CD8 && !base[func + 4] &&
         Slot(base, dest, 8) == 0x821C8A58;
}

ControllerEntry MakeEntry(const uint8_t* base, uint32_t ctrl) {
  const uint32_t dest = Load32(base, ctrl + 24);
  return {ctrl, dest, CanSleep(base, ctrl) ? Load32(base, dest + 4) : 0u};
}

void RebuildControllers(const uint8_t* base, uint32_t head) {
  g_controllers.clear();
  for (uint32_t n = Load32(base, head); n != head; n = TreeNext<17>(base, n))
    g_controllers.push_back(MakeEntry(base, Load32(base, n + 12)));
}

bool IncrementalAllowed() {
  return t_controllers_owner && !g_controllers_looping && !g_controllers_dirty.load(std::memory_order_relaxed);
}

void ControllerCreated(const uint8_t* base, uint32_t ctrl) {
  if (!IncrementalAllowed()) return g_controllers_dirty.store(true, std::memory_order_relaxed);
  const auto at = std::lower_bound(g_controllers.begin(), g_controllers.end(), ctrl,
                                   [](const ControllerEntry& e, uint32_t c) { return e.ctrl < c; });
  if (at != g_controllers.end() && at->ctrl == ctrl) return g_controllers_dirty.store(true, std::memory_order_relaxed);
  g_controllers.insert(at, MakeEntry(base, ctrl));
  g_cs.inserted++;
}

void ControllerDestroyed(uint32_t ctrl) {
  if (!IncrementalAllowed()) return g_controllers_dirty.store(true, std::memory_order_relaxed);
  const auto at = std::lower_bound(g_controllers.begin(), g_controllers.end(), ctrl,
                                   [](const ControllerEntry& e, uint32_t c) { return e.ctrl < c; });
  if (at == g_controllers.end() || at->ctrl != ctrl) return g_controllers_dirty.store(true, std::memory_order_relaxed);
  g_controllers.erase(at);
  g_cs.removed++;
}

// Premier nœud du std::set<Controller*> (en-tête head : racine +4, valeur +12, isnil +17) de valeur > ctrl.
uint32_t UpperBound(const uint8_t* base, uint32_t head, uint32_t ctrl) {
  uint32_t result = head;
  for (uint32_t x = Load32(base, head + 4); !base[x + 17];) {
    if (ctrl < Load32(base, x + 12)) {
      result = x;
      x = Load32(base, x);
    } else {
      x = Load32(base, x + 8);
    }
  }
  return result;
}

bool Asleep(const uint8_t* base, const ControllerEntry& e) {
  if (!e.sys || !base[e.ctrl + 52] || Load32(base, e.ctrl + 24) != e.dest) return false;
  const int32_t state = int32_t(Load32(base, e.sys + 284)), count = int32_t(Load32(base, e.sys + 280));
  return (state == 2 || state == 3) && count >= 900;
}

// Validation : liste identique au set, et chaque contrôleur sauté vraiment sans effet.
void CheckControllers(const uint8_t* base, uint32_t head) {
  size_t i = 0;
  bool same = true;
  for (uint32_t n = Load32(base, head); n != head; n = TreeNext<17>(base, n), i++)
    same = same && i < g_controllers.size() && g_controllers[i].ctrl == Load32(base, n + 12);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (!same || i != g_controllers.size()) Diff();
  // Reprise dans le set : le successeur de chaque contrôleur est l'entrée suivante de la liste.
  for (size_t k = 0; same && k < g_controllers.size(); k++) {
    const uint32_t n = UpperBound(base, head, g_controllers[k].ctrl);
    const uint32_t expected = k + 1 < g_controllers.size() ? g_controllers[k + 1].ctrl : 0;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if ((n == head ? 0 : Load32(base, n + 12)) != expected) Diff();
  }
  for (const ControllerEntry& e : g_controllers) {
    if (!Asleep(base, e)) continue;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!CanSleep(base, e.ctrl) || Load32(base, e.dest + 4) != e.sys) Diff();
  }
}

void UpdateControllers(PPCContext& ctx, uint8_t* base, uint32_t self, bool check) {
  const int variant = ControllersVariant();
  const uint32_t frame = Load32(base, Load32(base, kRootSingleton) + 224);
  if (frame == Load32(base, self + 56)) return;
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 160;
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  const uint32_t head = Load32(base, self + 8);
  const auto t0 = std::chrono::steady_clock::now();
  g_cs.frames++;
  if (variant != 0) {
    t_controllers_owner = true;
    if (g_controllers_dirty.exchange(false) || Load32(base, self + 12) != g_controllers.size()) {
      RebuildControllers(base, head);
      g_cs.rebuilds++;
    }
    if (check) CheckControllers(base, head);
    g_controllers_looping = true;
    const auto t1 = std::chrono::steady_clock::now();
    g_cs.prefetch_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    constexpr size_t kAhead = 16;
    const size_t count = g_controllers.size();
    for (size_t i = 0; i < count; i++) {
      // Contrôleur créé ou détruit pendant la boucle (par la mise à jour précédente : effet terminé en combat) : la
      // liste n'est plus sûre. Suite dans le std::set à partir du premier contrôleur d'adresse supérieure à la
      // dernière entrée traitée (comparaison d'adresses seulement), comme l'itérateur du jeu ; liste refaite à
      // l'image suivante (drapeau resté levé).
      if (g_controllers_dirty.load(std::memory_order_relaxed)) {
        g_cs.resumed++;
        for (uint32_t n = i ? UpperBound(base, head, g_controllers[i - 1].ctrl) : Load32(base, head); n != head;) {
          UpdateController(ctx, base, Load32(base, n + 12), check);
          n = TreeNext<17>(base, n);
        }
        break;
      }
      if (i + kAhead < count) {
        const ControllerEntry& a = g_controllers[i + kAhead];
        __builtin_prefetch(base + a.ctrl + 24);
        if (a.sys) __builtin_prefetch(base + a.sys + 280);
      }
      const ControllerEntry& e = g_controllers[i];
      if (Asleep(base, e)) {
        g_cs.skipped++;
        continue;
      }
      UpdateController(ctx, base, e.ctrl, check);
    }
    g_controllers_looping = false;
  } else {
    const auto t1 = std::chrono::steady_clock::now();
    g_cs.prefetch_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    for (uint32_t n = Load32(base, head); n != head;) {
      UpdateController(ctx, base, Load32(base, n + 12), check);
      const uint32_t next = TreeNext<17>(base, n);
      if (check) {
        Store32(base, sp + 80, n);
        ctx.r3.u64 = sp + 80;
        __imp__sub_824C6960(ctx, base);
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (Load32(base, sp + 80) != next) Diff();
      }
      n = next;
    }
  }
  torchlight::particules::Executer(ctx, base);
  Store32(base, self + 56, frame);
  ctx.r1.u64 = old_sp;
  g_cs.loop_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ReportControllers();
}

}  // namespace

// ControllerManager::createController (sub_824B4C78, rend le contrôleur) et ~Controller (sub_824B5A10) : liste des
// contrôleurs tenue à jour.
REX_EXTERN(__imp__sub_824B4C78);
REX_EXTERN(__imp__sub_824B5A10);
// Pendant les mises à jour parallèles des particules : en série, et tâche du contrôleur détruit oubliée.
REX_HOOK_RAW(sub_824B4C78) {
  torchlight::particules::Serie serie;
  __imp__sub_824B4C78(ctx, base);
  ControllerCreated(base, ctx.r3.u32);
}
REX_HOOK_RAW(sub_824B5A10) {
  torchlight::particules::Serie serie;
  torchlight::particules::ControleurDetruit(ctx.r3.u32);
  ControllerDestroyed(ctx.r3.u32);
  __imp__sub_824B5A10(ctx, base);
}

REX_HOOK_RAW(sub_821C8C00) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821C8C00(ctx, base);
  UpdateControllers(ctx, base, ctx.r3.u32, mode == 2);
  if (mode == 2) Report();
}

// --- Écouteur par objet du RT Shader System (sub_821C4F50 : SGRenderObjectListener::notifyRenderSingleObject) ---
// D'après torchlight-recomp de YaPeL (https://github.com/YaPeL/torchlight-recomp, GPL-3.0 :
// src/hooks/rtss_listener_hooks.cpp, guest_abi/rtss_listener.h), relu dans notre code généré. Appelé pour chaque
// objet de chaque passe (sub_821C4EE0) : r3 écouteur, r4 Renderable, r5 Pass, r6 AutoParamDataSource, r7 LightList,
// r8 suppressRenderStateChanges. Si le ShaderGenerator (+4) a un viewport actif (+328) : recherche du SGPass de la
// passe dans ses liaisons (Pass +368 : UserObjectBindings, attributs +4, map<String, Any>* +8 ; nœud : clé
// std::string +12, Any +40, isnil +49 ; clé statique 0x8349D2A4), any_cast (holder<SGPass*>, vtable 0x82000B8C,
// valeur +4), puis pour chaque sous-état de son état cible (+20 ; vecteur +8..+12) updateGpuProgramsParams [5]
// (sous-état, objet, mDstPass +12, source, lumières), dans l'ordre. Recherche et cast en natif ; les sous-états
// restent ceux du jeu. Attributs pas encore alloués (getUserAny les alloue) ou autre type : l'original.
// Validation (mode 2) : SGPass trouvé comparé à celui de getUserAny (sub_821C0568) et any_cast (sub_821BFC88).
REX_EXTERN(__imp__sub_821C4F50);
REX_EXTERN(__imp__sub_821C0568);

namespace {

constexpr uint32_t kSgPassKey = 0x8349D2A4, kSgPassHolder = 0x82000B8C;

// std::string MSVC : tampon de 16 octets ou pointeur en +0, taille +0x10, capacité +0x14.
struct GuestString {
  const uint8_t* data;
  uint32_t size;
};

GuestString ViewString(const uint8_t* base, uint32_t s) {
  const uint32_t size = Load32(base, s + 0x10), cap = Load32(base, s + 0x14);
  return {base + (cap >= 16 ? Load32(base, s) : s), size};
}

int CompareStrings(const GuestString& a, const GuestString& b) {
  const uint32_t n = std::min(a.size, b.size);
  const int c = n ? std::memcmp(a.data, b.data, n) : 0;
  if (c) return c < 0 ? -1 : 1;
  return a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
}

enum class SgLookup { kNothing, kGuest, kFound };

// SGPass de la passe ; kGuest : à laisser à l'original.
SgLookup FindSgPass(const uint8_t* base, uint32_t pass, uint32_t& sg_pass) {
  const uint32_t attributes = Load32(base, pass + 368 + 4);
  if (!attributes) return SgLookup::kGuest;
  const uint32_t map = Load32(base, attributes + 8);
  if (!map) return SgLookup::kNothing;
  const GuestString key = ViewString(base, kSgPassKey);
  const uint32_t head = Load32(base, map + 4);
  uint32_t node = Load32(base, head + 4), candidate = head;
  while (!base[node + 49]) {
    if (CompareStrings(ViewString(base, node + 12), key) < 0) {
      node = Load32(base, node + 8);
    } else {
      candidate = node;
      node = Load32(base, node);
    }
  }
  if (candidate == head || CompareStrings(key, ViewString(base, candidate + 12)) < 0) return SgLookup::kNothing;
  const uint32_t content = Load32(base, candidate + 40 + 4);
  if (!content) return SgLookup::kNothing;
  if (Load32(base, content) != kSgPassHolder) return SgLookup::kGuest;
  sg_pass = Load32(base, content + 4);
  return SgLookup::kFound;
}

uint64_t g_rtss_checked = 0, g_rtss_guest = 0, g_rtss_diffs = 0;

}  // namespace

REX_HOOK_RAW(sub_821C4F50) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821C4F50(ctx, base);
  const uint32_t self = ctx.r3.u32, rend = ctx.r4.u32, pass = ctx.r5.u32, source = ctx.r6.u32, lights = ctx.r7.u32;
  const bool suppress = (ctx.r8.u32 & 0xFF) != 0;
  const uint32_t owner = Load32(base, self + 4);
  if (!base[owner + 328]) return;
  uint32_t sg_pass = 0;
  const SgLookup found = FindSgPass(base, pass, sg_pass);
  if (found == SgLookup::kGuest) {
    g_rtss_guest++;
    return __imp__sub_821C4F50(ctx, base);
  }
  if (mode == 2) {
    // getUserAny (attributs déjà alloués : sans effet de bord) puis contenu de l'Any.
    ctx.r3.u64 = pass + 368;
    ctx.r4.u64 = kSgPassKey;
    __imp__sub_821C0568(ctx, base);
    const uint32_t content = Load32(base, ctx.r3.u32 + 4);
    const uint32_t guest = content && Load32(base, content) == kSgPassHolder ? Load32(base, content + 4) : 0;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (guest != (found == SgLookup::kFound ? sg_pass : 0)) {
      CompterDifference(__LINE__);
      g_rtss_diffs++;
    }
    if (++g_rtss_checked % 200000 == 0)
      REXLOG_INFO("[ogre natif] écouteur du RT Shader System : {} recherches comparées, {} différences, {} par "
                  "l'original", g_rtss_checked, g_rtss_diffs, g_rtss_guest);
    Report();
  }
  if (found != SgLookup::kFound || !sg_pass) return;
  const uint32_t state = Load32(base, sg_pass + 20);
  if (!state || suppress) return;
  const uint32_t dst_pass = Load32(base, sg_pass + 12);
  uint32_t it = Load32(base, state + 8);
  if (it == Load32(base, state + 12)) return;
  do {
    const uint32_t sub = Load32(base, it);
    ctx.r3.u64 = sub;
    ctx.r4.u64 = rend;
    ctx.r5.u64 = dst_pass;
    ctx.r6.u64 = source;
    ctx.r7.u64 = lights;
    GuestFunc(base, Slot(base, sub, 20))(ctx, base);
    it += 4;
  } while (it != Load32(base, state + 12));
}

// --- AnimationTrack::getKeyFramesAtTime (sub_821CCC60, fonction feuille) ---
// Console (2026-10-15) : 0,84 % du thread principal en temps propre en jeu. r3 piste (images clés : vecteur de
// pointeurs +4..+8, temps de l'image clé +4 ; index global -> local : tableau de u16 en +32 ; animation +20, durée
// +92), r4 TimeIndex (temps +0, index global +4, -1 sinon), r5 image clé 1, r6 image clé 2, r7 index (u16*, peut être
// nul). Sans index global, le temps est ramené dans la durée (soustractions tant qu'il la dépasse), puis
// lower_bound sur les temps. Rend le facteur d'interpolation (f1). Opérations en float comme le jeu (fsubs, fadds,
// fdivs : arrondis identiques, le recompilé arrondit chaque résultat double en float).
REX_EXTERN(__imp__sub_821CCC60);

namespace {

struct KeyFrames {
  uint32_t kf1, kf2;
  uint16_t index;
  float t;
};

KeyFrames KeyFramesAtTime(const uint8_t* base, uint32_t track, uint32_t time_index) {
#pragma clang fp contract(off)
  float t = LoadFloat(base, time_index);
  const uint32_t key_index = Load32(base, time_index + 4);
  const float zero = LoadFloat(base, kConstants + 68);
  const uint32_t begin = Load32(base, track + 4);
  uint32_t it;
  if (key_index != 0xFFFFFFFFu) {
    const uint32_t map = Load32(base, track + 32);
    const uint32_t local = (uint32_t(base[map + 2 * key_index]) << 8) | base[map + 2 * key_index + 1];
    it = begin + 4 * local;
  } else {
    const float length = LoadFloat(base, Load32(base, track + 20) + 92);
    if (t > length) {
      while (length > zero) {
        t = t - length;
        if (!(t > length)) break;
      }
    }
    uint32_t first = begin;
    int32_t count = int32_t(Load32(base, track + 8) - begin) >> 2;
    while (count > 0) {
      const int32_t half = count / 2;
      const uint32_t mid = first + 4 * uint32_t(half);
      if (LoadFloat(base, Load32(base, mid) + 4) < t) {
        first = mid + 4;
        count -= half + 1;
      } else {
        count = half;
      }
    }
    it = first;
  }
  KeyFrames r{};
  float t2;
  if (it == Load32(base, track + 8)) {
    r.kf2 = Load32(base, begin);
    t2 = LoadFloat(base, Load32(base, track + 20) + 92) + LoadFloat(base, r.kf2 + 4);
    it -= 4;
  } else {
    r.kf2 = Load32(base, it);
    t2 = LoadFloat(base, r.kf2 + 4);
    if (it != Load32(base, track + 4) && t < LoadFloat(base, Load32(base, it) + 4)) it -= 4;
  }
  r.index = uint16_t((it - Load32(base, track + 4)) >> 2);
  r.kf1 = Load32(base, it);
  const float t1 = LoadFloat(base, r.kf1 + 4);
  r.t = zero;
  if (!(t1 == t2)) r.t = (t - t1) / (t2 - t1);
  return r;
}

}  // namespace

REX_HOOK_RAW(sub_821CCC60) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821CCC60(ctx, base);
  const uint32_t track = ctx.r3.u32, ti = ctx.r4.u32, out1 = ctx.r5.u32, out2 = ctx.r6.u32, out_index = ctx.r7.u32;
  const KeyFrames r = KeyFramesAtTime(base, track, ti);
  if (mode == 2) {
    __imp__sub_821CCC60(ctx, base);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t expected = BitsOf(double(r.t));
    bool same = BitsOf(ctx.f1.f64) == expected && Load32(base, out1) == r.kf1 && Load32(base, out2) == r.kf2;
    if (out_index) same = same && ((uint32_t(base[out_index]) << 8 | base[out_index + 1]) == r.index);
    static uint64_t checked = 0, diffs = 0;
    if (!same) {
      CompterDifference(__LINE__);
      diffs++;
    }
    if (++checked % 500000 == 0)
      REXLOG_INFO("[ogre natif] getKeyFramesAtTime : {} appels comparés, {} différences", checked, diffs);
    Report();
    return;
  }
  Store32(base, out2, r.kf2);
  if (out_index) {
    base[out_index] = uint8_t(r.index >> 8);
    base[out_index + 1] = uint8_t(r.index);
  }
  Store32(base, out1, r.kf1);
  ctx.f1.f64 = double(r.t);
}

// --- Recherche d'un élément par nom (sub_821A1510, logique du jeu) ---
// Console (2026-10-27, profil de combat) : 1,9 % du thread principal en temps propre, appelée à chaque mise à jour
// d'unité (sub_821E4588 sous sub_821D3FD8) ; 570 sites d'appel. r3 conteneur (éléments : données +16, nombre +20,
// capacité +24 ; un indice hors de la capacité prend l'élément 0, comme le jeu), r4 std::wstring cherchée (tampon
// de 8 caractères ou pointeur +0, taille +16, capacité +20). Pour chaque élément : nom = valeur de l'identifiant
// (+16) dans la std::map de son objet (+20 ; map +4, en-tête +8, taille +12 ; nœud : clé +12, valeur +16, isnil +45 ;
// sub_821A1608 et sub_821A1670), chaîne par défaut de l'objet (+80) si la map est vide ou la clé absente, chaîne
// statique 0x834240CC sans objet ou d'identifiant -1. Premier élément dont le nom est égal (wstring::compare
// sub_821A1478 nul : même taille, mêmes caractères) rendu, sinon 0.
REX_EXTERN(__imp__sub_821A1510);

namespace {

constexpr uint32_t kEmptyName = 0x834240CC;

uint32_t ElementName(const uint8_t* base, uint32_t item) {
  const uint32_t obj = Load32(base, item + 20), id = Load32(base, item + 16);
  if (!obj || id == 0xFFFFFFFFu) return kEmptyName;
  if (!Load32(base, obj + 12)) return obj + 80;
  const uint32_t head = Load32(base, obj + 8);
  uint32_t found = head;
  for (uint32_t n = Load32(base, head + 4); !base[n + 45];) {
    if (Load32(base, n + 12) < id) {
      n = Load32(base, n + 8);
    } else {
      found = n;
      n = Load32(base, n);
    }
  }
  if (found == head || id < Load32(base, found + 12)) return obj + 80;
  return found + 16;
}

bool SameWString(const uint8_t* base, uint32_t s, const uint8_t* key, uint32_t len) {
  if (Load32(base, s + 16) != len) return false;
  const uint32_t data = Load32(base, s + 20) >= 8 ? Load32(base, s) : s;
  return std::memcmp(base + data, key, 2 * size_t(len)) == 0;
}

uint32_t FindByName(const uint8_t* base, uint32_t cont, uint32_t key) {
  const uint32_t count = Load32(base, cont + 20), data = Load32(base, cont + 16), cap = Load32(base, cont + 24);
  const uint32_t len = Load32(base, key + 16);
  const uint8_t* chars = base + (Load32(base, key + 20) >= 8 ? Load32(base, key) : key);
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t item = Load32(base, i < cap ? data + 4 * i : data);
    if (SameWString(base, ElementName(base, item), chars, len)) return item;
  }
  return 0;
}

}  // namespace

REX_HOOK_RAW(sub_821A1510) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821A1510(ctx, base);
  const uint32_t cont = ctx.r3.u32, key = ctx.r4.u32;
  if (mode == 1) {
    ctx.r3.u64 = FindByName(base, cont, key);
    return;
  }
  const uint32_t expected = FindByName(base, cont, key);
  __imp__sub_821A1510(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  static uint64_t checked = 0, found = 0, diffs = 0;
  if (ctx.r3.u32 != expected) {
    CompterDifference(__LINE__);
    diffs++;
  }
  if (expected) found++;
  if (++checked % 100000 == 0)
    REXLOG_INFO("[ogre natif] recherche par nom : {} appels comparés ({} trouvés), {} différences", checked, found,
                diffs);
  Report();
}

// --- RenderPriorityGroup::clear (sub_821CC998) ---
// Console (2026-10-28, profil de combat avec les particules en parallèle) : 1,25 % du thread principal en temps
// propre, appelée pour chaque groupe de la file de rendu à chaque image. Pour chaque passe du cimetière des passes
// (std::set global 0x8356C990), puis de la liste des passes à re-hacher (0x8356C9A4) : removePassGroup (sub_821CE420)
// dans les six collections du groupe (+8, +48, +128, +88, +168, +208), qui cherche la passe dans la map de la
// collection (en-tête +8 ; nœud : clé +12, isnil +21 ; ordre PassGroupLess : clé de hachage +52, puis adresse,
// sub_821CE4B8) et ne fait rien si elle n'y est pas ; puis vidage des six collections (+8, +88, +48, +128, +168,
// +208 ; sub_821CC8E0). Ici les parcours et les recherches sont natifs ; la fonction du jeu n'est appelée que pour
// une passe présente (retrait du vecteur et du nœud). Validation : chaque recherche comparée à sub_821CE4B8, puis
// l'original.
REX_EXTERN(__imp__sub_821CC998);
REX_EXTERN(__imp__sub_821CE420);
REX_EXTERN(__imp__sub_821CE4B8);

namespace {

constexpr uint32_t kPassGraveyard = 0x8356C990, kPassDirtyHash = 0x8356C9A4;
constexpr uint32_t kGroupCollections[6] = {8, 48, 128, 88, 168, 208};
constexpr uint32_t kGroupClearOrder[6] = {8, 88, 48, 128, 168, 208};

bool PassLess(const uint8_t* base, uint32_t a, uint32_t b) {
  const uint32_t ha = Load32(base, a + 52), hb = Load32(base, b + 52);
  return ha != hb ? ha < hb : a < b;
}

bool CollectionHasPass(const uint8_t* base, uint32_t coll, uint32_t pass) {
  const uint32_t head = Load32(base, coll + 8);
  uint32_t found = head;
  for (uint32_t n = Load32(base, head + 4); !base[n + 21];) {
    if (PassLess(base, Load32(base, n + 12), pass)) {
      n = Load32(base, n + 8);
    } else {
      found = n;
      n = Load32(base, n);
    }
  }
  return found != head && !PassLess(base, pass, Load32(base, found + 12));
}

// Passes des deux ensembles, dans l'ordre de l'original.
template <typename F>
void ForEachClearedPass(const uint8_t* base, F&& f) {
  for (const uint32_t set : {kPassGraveyard, kPassDirtyHash}) {
    const uint32_t head = Load32(base, set + 4);
    for (uint32_t n = Load32(base, head); n != head; n = TreeNext<17>(base, n)) f(Load32(base, n + 12));
  }
}

}  // namespace

REX_HOOK_RAW(sub_821CC998) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821CC998(ctx, base);
  const uint32_t group = ctx.r3.u32;
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 176;
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  if (mode == 2) {
    ForEachClearedPass(base, [&](uint32_t pass) {
      for (const uint32_t off : kGroupCollections) {
        const bool expected = CollectionHasPass(base, group + off, pass);
        Store32(base, sp + 84, pass);
        ctx.r3.u64 = sp + 80;
        ctx.r4.u64 = group + off + 4;
        ctx.r5.u64 = sp + 84;
        __imp__sub_821CE4B8(ctx, base);
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if ((Load32(base, sp + 80) != Load32(base, group + off + 8)) != expected) Diff();
        static uint64_t searched = 0, present = 0;
        searched++;
        if (expected) present++;
        if (searched % 200000 == 0)
          REXLOG_INFO("[ogre natif] RenderPriorityGroup::clear : {} recherches comparées, {} passes présentes", searched,
                      present);
      }
    });
    ctx.r1.u64 = old_sp;
    ctx.r3.u64 = group;
    __imp__sub_821CC998(ctx, base);
    Report();
    return;
  }
  // Le cimetière n'est retiré que de l'ensemble par d'autres fonctions : les retraits ci-dessous ne touchent que les
  // collections du groupe, l'itération reste valable.
  ForEachClearedPass(base, [&](uint32_t pass) {
    for (const uint32_t off : kGroupCollections) {
      if (!CollectionHasPass(base, group + off, pass)) continue;
      ctx.r3.u64 = group + off;
      ctx.r4.u64 = pass;
      __imp__sub_821CE420(ctx, base);
    }
  });
  for (const uint32_t off : kGroupClearOrder) ClearQueuedCollection(base, group + off);
  ctx.r1.u64 = old_sp;
}

// --- Material::getBestTechnique (sub_821CF100) ---
// Console (2026-10-28, profil de combat) : 1,0 % du thread principal en temps propre (setPass, parcours de la scène,
// géométrie statique...). r3 matériau, r4 indice de LOD (u16), r5 renderable. Techniques supportées vides (+200 =
// +204) : 0. Schéma actif = MaterialManager (singleton 0x83506794) case 176 ; l'accesseur sub_821CF0F8 lit +288
// (u16). Map des schémas (en-tête +220 ; nœud : clé u16 +12, map des LOD +16, isnil +21) : schéma absent -> au jeu
// (écouteur case 196 de MaterialManager). Map des LOD (en-tête +4) : entrée de la clé, sinon (après la boucle de
// décrément du jeu) la dernière (LOD non nul, si non nulle) ou la première ; map vide : +16 de l'en-tête.
REX_EXTERN(__imp__sub_821CF100);

namespace {

constexpr uint32_t kMaterialManager = 0x83506794;

uint16_t Load16(const uint8_t* base, uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }

// Nœud de clé u16 (+12) dans une map MSVC (isnil +21), sinon l'en-tête.
uint32_t FindU16(const uint8_t* base, uint32_t head, uint32_t key) {
  uint32_t found = head;
  for (uint32_t n = Load32(base, head + 4); !base[n + 21];) {
    if (Load16(base, n + 12) < key) {
      n = Load32(base, n + 8);
    } else {
      found = n;
      n = Load32(base, n);
    }
  }
  return found != head && !(key < Load16(base, found + 12)) ? found : head;
}

// Vrai si le chemin natif s'applique (sinon : fonction du jeu).
bool BestTechnique(const uint8_t* base, uint32_t mat, uint32_t lod, uint32_t& out) {
  if (Load32(base, mat + 200) == Load32(base, mat + 204)) {
    out = 0;
    return true;
  }
  const uint32_t mm = Load32(base, kMaterialManager);
  if (Slot(base, mm, 176) != 0x821CF0F8) return false;
  const uint32_t schemes = Load32(base, mat + 220);
  const uint32_t si = FindU16(base, schemes, Load16(base, mm + 288));
  if (si == schemes) return false;
  const uint32_t lhead = Load32(base, Load32(base, si + 16) + 4);
  const uint32_t li = FindU16(base, lhead, lod & 0xFFFF);
  if (li != lhead) {
    out = Load32(base, li + 16);
    return true;
  }
  const uint32_t first = Load32(base, lhead);
  if (first != lhead && (lod & 0xFFFF)) {
    out = Load32(base, Load32(base, lhead + 8) + 16);
    if (out) return true;
  }
  out = Load32(base, first + 16);
  return true;
}

}  // namespace

REX_HOOK_RAW(sub_821CF100) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821CF100(ctx, base);
  const uint32_t mat = ctx.r3.u32, lod = ctx.r4.u32;
  uint32_t out;
  const bool native = BestTechnique(base, mat, lod, out);
  if (mode == 1 && native) {
    ctx.r3.u64 = out;
    return;
  }
  __imp__sub_821CF100(ctx, base);
  if (mode != 2) return;
  static uint64_t checked = 0, fallback = 0;
  if (native) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32 != out) Diff();
  } else {
    fallback++;
  }
  if (++checked % 500000 == 0)
    REXLOG_INFO("[ogre natif] getBestTechnique : {} appels ({} laissés au jeu)", checked, fallback);
  Report();
}

// --- Effets encore vivants d'un objet (sub_82312D98, logique du jeu, fonction feuille) ---
// Console (2026-10-27, profil de combat) : 1,2 % du thread principal en temps propre (parcours de pointeurs). r3
// objet (liste d'effets +244 : données +8, nombre +12, capacité +16 ; un indice hors de la capacité prend
// l'élément 0, comme le jeu), r4 octet. Pour chaque système de ParticleUniverse de la liste, sa première technique
// (+348) : si la somme (u32) de +432, +476, +388, +344, +304 est nulle, ou si un émetteur (+216..+220, non nul) a
// +386 vrai, ou (octet r4 nul) +300 = -1, il ne compte pas. Rend le nombre de systèmes comptés.
REX_EXTERN(__imp__sub_82312D98);

namespace {

uint32_t LiveEffects(const uint8_t* base, uint32_t obj, uint32_t flag) {
  const uint32_t list = Load32(base, obj + 244);
  if (!list) return 0;
  const uint32_t count = Load32(base, list + 12), cap = Load32(base, list + 16);
  uint32_t live = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t data = Load32(base, list + 8);
    const uint32_t sys = Load32(base, i < cap ? data + 4 * i : data);
    const uint32_t tech = Load32(base, Load32(base, sys + 348));
    if (!tech) continue;
    if (Load32(base, tech + 432) + Load32(base, tech + 476) + Load32(base, tech + 388) + Load32(base, tech + 344) +
            Load32(base, tech + 304) ==
        0)
      continue;
    const uint32_t n = uint32_t(int32_t(Load32(base, tech + 220) - Load32(base, tech + 216)) >> 2);
    bool stop = false;
    for (uint32_t k = 0; n && k < n; k++) {
      const uint32_t e = Load32(base, Load32(base, tech + 216) + 4 * k);
      if (e && base[e + 386]) {
        stop = true;
        break;
      }
      if (!(flag & 0xFF) && e && Load32(base, e + 300) == 0xFFFFFFFFu) {
        stop = true;
        break;
      }
    }
    if (!stop) live++;
  }
  return live;
}

}  // namespace

REX_HOOK_RAW(sub_82312D98) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_82312D98(ctx, base);
  const uint32_t obj = ctx.r3.u32, flag = ctx.r4.u32;
  if (mode == 1) {
    ctx.r3.u64 = LiveEffects(base, obj, flag);
    return;
  }
  const uint32_t expected = LiveEffects(base, obj, flag);
  __imp__sub_82312D98(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  static uint64_t checked = 0, diffs = 0;
  if (ctx.r3.u32 != expected) {
    CompterDifference(__LINE__);
    diffs++;
  }
  if (++checked % 100000 == 0)
    REXLOG_INFO("[ogre natif] effets vivants : {} appels comparés, {} différences", checked, diffs);
  Report();
}

// --- Valeur dans le vecteur d'une clé d'une std::map (sub_821D7D38, logique du jeu, fonction feuille) ---
// Console (2026-10-27, profil de combat) : 0,7 % du thread principal ; 135 sites d'appel. r3 this (map : en-tête
// +12 ; nœud : gauche +0, droite +8, clé u32 +12, vecteur +16..+20, isnil +33), r4 clé, r5 valeur. Vrai si r4 = r5,
// sinon si la clé est présente et que son vecteur contient r5. L'original écrit l'itérateur trouvé sous r1 (-16 ou
// -12, zone libre de la pile) : sans effet pour l'appelant.
REX_EXTERN(__imp__sub_821D7D38);

namespace {

bool KeyHasValue(const uint8_t* base, uint32_t self, uint32_t key, uint32_t value) {
  if (key == value) return true;
  const uint32_t head = Load32(base, self + 12);
  uint32_t found = head;
  for (uint32_t n = Load32(base, head + 4); !base[n + 33];) {
    if (Load32(base, n + 12) < key) {
      n = Load32(base, n + 8);
    } else {
      found = n;
      n = Load32(base, n);
    }
  }
  if (found == head || key < Load32(base, found + 12)) return false;
  const uint32_t vec = found + 16;
  for (uint32_t i = 0;; i++) {
    const uint32_t begin = Load32(base, vec), n = uint32_t(int32_t(Load32(base, vec + 4) - begin) >> 2);
    if (i == 0 && n == 0) return false;
    if (i >= n) return false;
    if (Load32(base, begin + 4 * i) == value) return true;
  }
}

}  // namespace

REX_HOOK_RAW(sub_821D7D38) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821D7D38(ctx, base);
  const uint32_t self = ctx.r3.u32, key = ctx.r4.u32, value = ctx.r5.u32;
  if (mode == 1) {
    ctx.r3.u64 = KeyHasValue(base, self, key, value) ? 1 : 0;
    return;
  }
  const uint32_t expected = KeyHasValue(base, self, key, value) ? 1 : 0;
  __imp__sub_821D7D38(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  static uint64_t checked = 0, found = 0, diffs = 0;
  if (ctx.r3.u32 != expected) {
    CompterDifference(__LINE__);
    diffs++;
  }
  if (expected) found++;
  if (++checked % 100000 == 0)
    REXLOG_INFO("[ogre natif] clé et valeur : {} appels comparés ({} vrais), {} différences", checked, found, diffs);
  Report();
}

// --- Segment contre les objets d'une liste, intersection la plus proche (sub_822F92A0, logique du jeu) ---
// Console (2026-10-27, profil de combat) : 2,35 % du thread principal avec sub_822815E8 (1,36 % en temps propre,
// fonctions intégrées par le LTO), sous sub_821B60D8 (mise à jour des unités). r3 this, r4 début et r5 fin du
// segment (Vector3), f1 rayon, r7 r8 r9 sorties (Vector3 ; r8 : point touché), r10 code (u32) ; sur la pile de
// l'appelant : +84 objet touché (u32*, peut être nul), octet +95 (seulement les objets dont +342 est vrai). Deux
// listes chaînées (this +84 puis +88, tête rangée en +0 ; nœud : objet +0, suivant +4 ; codes 200 puis 100) : pour
// chaque objet actif (+341 et +356), sub_822815E8 ; distance du début au point touché (fsubs, fmuls, fmadds, fsqrts
// dans l'ordre du jeu), plus proche gardée. Rend vrai si la distance a changé de sa valeur de départ (constante +624).
// sub_822815E8 (r3 objet, mêmes r4 r5 f1, sorties r7 r8 r9) commence par un rejet : case [60] de l'objet, +341 et
// +356, boîte du segment (sub_821BDF30) élargie de rayon * constante +124, contre la boîte de l'objet (cases [67]
// puis [66] ; sub_823A2EC8), et n'écrit ses sorties qu'en cas de touche. Natif : le rejet pour tous les objets ; si
// aucun ne passe (cas courant : PC en ville, 0 sur 20 000 appels), rien n'est écrit et le résultat est 0 comme dans
// le jeu ; sinon la fonction du jeu est appelée en entier (exacte par construction, rejet compris).
REX_EXTERN(__imp__sub_822F92A0);
REX_EXTERN(__imp__sub_822815E8);

namespace {

std::map<std::array<uint32_t, 3>, uint64_t> g_segment_slots;  // validation : cibles des cases [60] [66] [67]

bool SegmentMayHit(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t p0, uint32_t p1, double radius,
                   bool check) {
#pragma clang fp contract(off)
  const uint32_t s60 = Slot(base, obj, 240), s66 = Slot(base, obj, 264), s67 = Slot(base, obj, 268);
  if (check) g_segment_slots[{s60, s66, s67}]++;
  uint32_t active;
  switch (s60) {  // accesseurs du jeu (relevé PC)
    case 0x82282398: active = Load32(base, obj + 484); break;
    case 0x822823A0: active = Load32(base, obj + 488); break;
    case 0x822830E8: active = Load32(base, obj + 1056); break;
    default: active = CallGuest(ctx, base, s60, obj); break;
  }
  if (!active) return false;
  if (!base[obj + 341] || !base[obj + 356]) return false;
  float mn[3], mx[3];
  for (int i = 0; i < 3; i++) mn[i] = mx[i] = LoadFloat(base, p0 + 4 * i);
  for (int i = 0; i < 3; i++) {
    const float v = LoadFloat(base, p1 + 4 * i);
    if (!(v >= mn[i])) mn[i] = v;
    if (!(v <= mx[i])) mx[i] = v;
  }
  const float r = float(radius * double(LoadFloat(base, kConstants + 124)));
  for (int i = 0; i < 3; i++) {
    mn[i] = mn[i] - r;
    mx[i] = r + mx[i];
  }
  // sub_82280380 / sub_82280360 : maximum (+44) / minimum (+32) de la boîte +384, sinon Vector3::ZERO.
  constexpr uint32_t kZero = 0x8355A3BC;
  uint32_t hi, lo;
  if (s67 == 0x82280380) {
    const uint32_t box = Load32(base, obj + 384);
    hi = box ? box + 44 : kZero;
  } else {
    hi = CallGuest(ctx, base, s67, obj);
  }
  if (s66 == 0x82280360) {
    const uint32_t box = Load32(base, obj + 384);
    lo = box ? box + 32 : kZero;
  } else {
    lo = CallGuest(ctx, base, s66, obj);
  }
  if (mx[0] < LoadFloat(base, lo)) return false;
  if (mx[2] < LoadFloat(base, lo + 8)) return false;
  if (mn[0] > LoadFloat(base, hi)) return false;
  if (mn[2] > LoadFloat(base, hi + 8)) return false;
  if (mx[1] < LoadFloat(base, lo + 4)) return false;
  return mn[1] <= LoadFloat(base, hi + 4);
}

// Faux si aucun objet n'est rejeté qu'après son test fin : le jeu ne touche alors à aucune sortie et rend 0. Vrai
// dès qu'un objet passe le rejet : la fonction du jeu est appelée en entier.
bool AnySegmentCandidate(PPCContext& ctx, uint8_t* base, bool check) {
  const uint32_t self = ctx.r3.u32, p0 = ctx.r4.u32, p1 = ctx.r5.u32;
  const double radius = ctx.f1.f64;
  const bool only_342 = base[ctx.r1.u32 + 95] != 0;
  for (const uint32_t list : {84u, 88u}) {
    for (uint32_t n = Load32(base, Load32(base, self + list)); n; n = Load32(base, n + 4)) {
      const uint32_t obj = Load32(base, n);
      if (!base[obj + 341] || !base[obj + 356]) continue;
      if (only_342 && !base[obj + 342]) continue;
      if (SegmentMayHit(ctx, base, obj, p0, p1, radius, check)) return true;
    }
  }
  return false;
}

}  // namespace

REX_HOOK_RAW(sub_822F92A0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_822F92A0(ctx, base);
  const PPCContext entry = ctx;
  const bool candidate = AnySegmentCandidate(ctx, base, mode == 2);
  ctx = entry;
  if (mode == 1) {
    if (candidate) return __imp__sub_822F92A0(ctx, base);
    ctx.r3.u64 = 0;
    return;
  }
  // Validation : sans candidat, le jeu doit rendre 0.
  __imp__sub_822F92A0(ctx, base);
  static uint64_t checked = 0, candidates = 0, hits = 0, diffs = 0;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (candidate) candidates++;
  if (ctx.r3.u8) hits++;
  if (!candidate && ctx.r3.u8) {
    CompterDifference(__LINE__);
    diffs++;
  }
  if (++checked % 20000 == 0) {
    std::string slots;
    for (const auto& [k, v] : g_segment_slots) slots += fmt::format(" {:08X}/{:08X}/{:08X}:{}", k[0], k[1], k[2], v);
    REXLOG_INFO("[ogre natif] segment contre les objets : {} appels comparés ({} avec candidat, {} touchés), {} "
                "différences ; cases{}", checked, candidates, hits, diffs, slots);
  }
  Report();
}

// --- Pilote de réécritures natives (2026-11-03) : petites fonctions chaudes du thread principal en combat, 3,9 % à elles
// sept. Il mesure ce que rapporte une réécriture avant d'en faire davantage (--torchlight_alterner="natives").
// Mode 1 : natif ; mode 2 : comparaison pour les fonctions sans effet de bord (sub_821D7CE0, sub_821CE4B8), original pour les
// autres (boucles d'appels virtuels : mêmes appels dans le même ordre, cibles promues appelées directement) ; mode 0 : original.
REX_EXTERN(__imp__sub_821B7510);
REX_EXTERN(__imp__sub_821BFB00);
REX_EXTERN(__imp__sub_821A7428);
REX_EXTERN(__imp__sub_821D7CE0);
REX_EXTERN(__imp__sub_821C4728);
REX_EXTERN(__imp__sub_821CE4B8);
REX_EXTERN(__imp__sub_821CCE90);
REX_EXTERN(sub_821B7650);
REX_EXTERN(sub_821E4A48);
REX_EXTERN(sub_821A73C0);
REX_EXTERN(sub_821C4798);
REX_EXTERN(sub_821C46D0);
REX_EXTERN(sub_821C3730);

namespace {

#ifdef TORCHLIGHT_NATIVES_FIXES
constexpr bool g_pilote_actif = true;
#else
bool g_pilote_actif = true;
#endif
bool Pilote() { return g_pilote_actif && Mode() == 1; }

// Cadre de l'appelé, comme le stwu de la fonction d'origine.
struct CadreInvite {
  PPCContext& ctx;
  uint32_t ancien;
  CadreInvite(PPCContext& c, uint8_t* base, uint32_t taille) : ctx(c), ancien(c.r1.u32) {
    Store32(base, ancien - taille, ancien);
    ctx.r1.u64 = ancien - taille;
  }
  ~CadreInvite() { ctx.r1.u64 = ancien; }
};

}  // namespace

namespace torchlight::ogre {
void PiloteActif(bool actif) {
#ifdef TORCHLIGHT_NATIVES_FIXES
  (void)actif;
#else
  g_pilote_actif = actif;
#endif
}
}  // namespace torchlight::ogre

// sub_821B7510 (r3, f1) : appel virtuel (case 256) pour chaque élément d'un tableau (+172 données, +176 taille, +180 capacité).
REX_HOOK_RAW(sub_821B7510) {
  if (!Pilote()) return __imp__sub_821B7510(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  if (!Load32(base, self + 176)) return;
  CadreInvite cadre(ctx, base, 128);
  uint32_t i = 0;
  do {
    const uint32_t donnees = Load32(base, self + 172);
    const uint32_t obj = Load32(base, i < Load32(base, self + 180) ? donnees + 4 * i : donnees);
    const uint32_t f = Slot(base, obj, 256);
    ctx.r3.u64 = obj;
    ctx.f1.f64 = dt;
    if (f == 0x821B7650) {
      sub_821B7650(ctx, base);
    } else {
      GuestFunc(base, f)(ctx, base);
    }
    i++;
  } while (i < Load32(base, self + 176));
}

// sub_821BFB00 (r3) : SceneNode::updateFromParentImpl : celle de Node, puis case 68 des objets attachés (liste +312).
REX_HOOK_RAW(sub_821BFB00) {
  if (!Pilote()) return __imp__sub_821BFB00(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 112);
  UpdateFromParentImpl(ctx, base, self);
  uint32_t tete = Load32(base, self + 312);
  for (uint32_t it = Load32(base, tete); it != tete;) {
    const uint32_t obj = Load32(base, it + 36);
    const uint32_t f = Slot(base, obj, 68);
    ctx.r3.u64 = obj;
    if (f == 0x821E4A48) {
      sub_821E4A48(ctx, base);
    } else {
      GuestFunc(base, f)(ctx, base);
    }
    tete = Load32(base, self + 312);
    it = Load32(base, it);
  }
}

// sub_821A7428 (r3 squelette, r4 tous) : case 220 de chaque os (+188..+192), sauf les os manuels (+310) si r4 est nul.
REX_HOOK_RAW(sub_821A7428) {
  if (!Pilote()) return __imp__sub_821A7428(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const bool tous = uint8_t(ctx.r4.u32) != 0;
  const uint32_t debut = Load32(base, self + 188);
  const uint32_t nombre = uint32_t(int32_t(Load32(base, self + 192) - debut) >> 2);
  if (!nombre) return;
  __builtin_prefetch(base + debut);
  CadreInvite cadre(ctx, base, 112);
  for (uint32_t i = 0; i < nombre; i++) {
    const uint32_t os = Load32(base, debut + 4 * i);
    __builtin_prefetch(base + os);
    __builtin_prefetch(base + os + 310);
    if (!tous && base[os + 310]) continue;
    const uint32_t f = Slot(base, os, 220);
    ctx.r3.u64 = os;
    if (f == 0x821A73C0) {
      sub_821A73C0(ctx, base);
    } else {
      GuestFunc(base, f)(ctx, base);
    }
  }
}

// sub_821D7CE0 (r3, r4) : clé (+372) et valeur r4 présentes dans la table du jeu (+96 -> +16) ? Enveloppe de sub_821D7D38.
REX_HOOK_RAW(sub_821D7CE0) {
  const int mode = Mode();
  if (mode == 0 || !g_pilote_actif) return __imp__sub_821D7CE0(ctx, base);
  const uint32_t self = ctx.r3.u32, valeur = ctx.r4.u32;
  const uint32_t m = Load32(base, self + 96);
  const uint32_t table = m ? Load32(base, m + 16) : 0;
  const uint32_t attendu = table && KeyHasValue(base, table, Load32(base, self + 372), valeur) ? 1 : 0;
  if (mode == 1) {
    ctx.r3.u64 = attendu;
    return;
  }
  __imp__sub_821D7CE0(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32 != attendu) CompterDifference(__LINE__);
  Report();
}

// sub_821C4728 (r3) : cases 212 et 208, puis 228 si l'octet +638 est posé.
REX_HOOK_RAW(sub_821C4728) {
  if (!Pilote()) return __imp__sub_821C4728(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 96);
  uint32_t f = Slot(base, self, 212);
  ctx.r3.u64 = self;
  if (f == 0x821C4798) {
    sub_821C4798(ctx, base);
  } else {
    GuestFunc(base, f)(ctx, base);
  }
  f = Slot(base, self, 208);
  ctx.r3.u64 = self;
  if (f == 0x821C46D0) {
    sub_821C46D0(ctx, base);
  } else {
    GuestFunc(base, f)(ctx, base);
  }
  if (base[self + 638]) {
    ctx.r3.u64 = self;
    GuestFunc(base, Slot(base, self, 228))(ctx, base);
  }
}

// sub_821CE4B8 (r3 sortie, r4 arbre, r5 adresse de la clé) : find d'un arbre ordonné par (+52 de l'objet, adresse de l'objet).
namespace {
uint32_t ChercherArbre(const uint8_t* base, uint32_t arbre, uint32_t cle) {
  const uint32_t tete = Load32(base, arbre + 4), valeur = Load32(base, cle + 52);
  uint32_t resultat = tete;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 21];) {
    const uint32_t objet = Load32(base, n + 12), v = Load32(base, objet + 52);
    if (v != valeur ? v < valeur : objet < cle) {
      n = Load32(base, n + 8);
    } else {
      resultat = n;
      n = Load32(base, n);
    }
  }
  if (resultat == tete) return tete;
  const uint32_t objet = Load32(base, resultat + 12), v = Load32(base, objet + 52);
  return (valeur != v ? valeur < v : cle < objet) ? tete : resultat;
}
}  // namespace

REX_HOOK_RAW(sub_821CE4B8) {
  const int mode = Mode();
  if (mode == 0 || !g_pilote_actif) return __imp__sub_821CE4B8(ctx, base);
  const uint32_t sortie = ctx.r3.u32;
  const uint32_t attendu = ChercherArbre(base, ctx.r4.u32, Load32(base, ctx.r5.u32));
  if (mode == 1) {
    Store32(base, sortie, attendu);
    return;
  }
  __imp__sub_821CE4B8(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (Load32(base, sortie) != attendu) CompterDifference(__LINE__);
  Report();
}

// sub_821CCE90 (r3, r4 file) : Region::_updateRenderQueue de la géométrie statique, LOD courant (+268 dans +276) : pour chaque
// matériau (arbre +20, _Isnil en +45), meilleure technique (+52), puis addRenderable de chaque géométrie (+56..+60).
REX_HOOK_RAW(sub_821CCE90) {
  if (!Pilote()) return __imp__sub_821CCE90(ctx, base);
  const uint32_t self = ctx.r3.u32, file = ctx.r4.u32;
  uint16_t lod;
  std::memcpy(&lod, base + self + 268, 2);
  const uint8_t groupe = base[self + 68];
  const uint32_t entree = Load32(base, Load32(base, self + 276) + (uint32_t(__builtin_bswap16(lod)) << 2));
  const uint32_t tete = Load32(base, entree + 20);
  uint32_t it = Load32(base, tete);
  if (it == tete) return;
  CadreInvite cadre(ctx, base, 144);
  do {
    const uint32_t mat = Load32(base, it + 40);
    ctx.r3.u64 = Load32(base, mat + 40);
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0;
    sub_821CF100(ctx, base);
    const uint32_t technique = ctx.r3.u32;
    const uint32_t fin = Load32(base, mat + 60);
    uint32_t p = Load32(base, mat + 56);
    Store32(base, mat + 52, technique);
    for (; p != fin; p += 4) {
      uint16_t priorite;
      std::memcpy(&priorite, base + file + 26, 2);
      ctx.r3.u64 = file;
      ctx.r4.u64 = Load32(base, p);
      ctx.r5.u64 = groupe;
      ctx.r6.u64 = __builtin_bswap16(priorite);
      sub_821C3730(ctx, base);
    }
    it = TreeNext<45>(base, it);
  } while (it != tete);
}

// ======================================================================================================================
// Top 50 du thread principal en temps propre (relevé de la console du 2026-11-03, combat ; demande de l'utilisateur).
// Même règle que le pilote : sous g_pilote_actif pour l'alternance « natives » ; les fonctions pures sont comparées au
// recompilé en mode 2. Ces fonctions peuvent aussi tourner sur les threads de travail (particules, poses) : aucun état
// statique. Une native n'écrit que r3 ou f1 : un appelant qui relirait un autre registre volatil (appelant optimisé par
// le LTCG de Microsoft) est signalé par switch/tools/audit_retours_natifs.py.
// ======================================================================================================================
namespace torchlight::trace {
bool SkipBindings();  // src/torchlight_trace.cpp
}

REX_EXTERN(__imp__KeTlsGetValue);
REX_EXTERN(__imp__sub_821C0E50);
REX_EXTERN(__imp__sub_821AFE28);
REX_EXTERN(__imp__sub_821AFD50);
REX_EXTERN(__imp__sub_821BE510);
REX_EXTERN(__imp__sub_821BE5A8);
REX_EXTERN(__imp__sub_821D3F38);
REX_EXTERN(__imp__sub_8219BE00);
REX_EXTERN(__imp__sub_821C9058);
REX_EXTERN(__imp__sub_821AECC0);
REX_EXTERN(__imp__sub_821DD098);
REX_EXTERN(__imp__sub_821C08B8);
REX_EXTERN(__imp__sub_821CC700);
REX_EXTERN(__imp__sub_821C3E78);
REX_EXTERN(__imp__sub_821C26D0);
REX_EXTERN(__imp__sub_823ECDE8);
REX_EXTERN(__imp__sub_823E7380);
REX_EXTERN(__imp__sub_821CE238);
REX_EXTERN(__imp__sub_823A30D0);
REX_EXTERN(__imp__sub_823A31F8);
REX_EXTERN(__imp__sub_821CEF88);
REX_EXTERN(__imp__sub_821C1EB8);
REX_EXTERN(__imp__sub_821CD7F8);
REX_EXTERN(__imp__sub_821C8A58);
REX_EXTERN(__imp__sub_82194008);
REX_EXTERN(__imp__sub_821AF868);
REX_EXTERN(__imp__sub_8219C6B0);
REX_EXTERN(__imp__sub_821D8770);
REX_EXTERN(sub_821C4570);
REX_EXTERN(sub_821AFCA8);
REX_EXTERN(sub_821C8D88);
REX_EXTERN(sub_821BE358);
REX_EXTERN(sub_821C8518);
REX_EXTERN(sub_8257B878);
REX_EXTERN(sub_821AEC40);
REX_EXTERN(sub_821DCFC8);
REX_EXTERN(sub_821D27F8);
REX_EXTERN(sub_821CCB88);
REX_EXTERN(sub_823497E0);
REX_EXTERN(sub_82480518);
REX_EXTERN(sub_821C3D58);
REX_EXTERN(sub_82582E20);
REX_EXTERN(sub_821A71E8);
REX_EXTERN(sub_82201EB0);
REX_EXTERN(sub_821C2118);
REX_EXTERN(sub_823ECF88);
REX_EXTERN(sub_821CD9A8);
REX_EXTERN(sub_8219C598);
REX_EXTERN(sub_8219C3E8);
REX_EXTERN(sub_823E6ED0);
REX_EXTERN(sub_823F5F60);
REX_EXTERN(sub_821C8618);
REX_EXTERN(sub_821C74A0);
REX_EXTERN(sub_821BB578);
REX_EXTERN(sub_8219B3B8);
REX_EXTERN(sub_8227EEA0);
REX_EXTERN(sub_821C0A50);
REX_EXTERN(sub_821DA090);
REX_EXTERN(sub_821C08B8);

// Appel d'une cible promue (switch/appels_indirects_promus.txt) : directe si c'est elle, sinon par la table.
#define TL_PROMU(f, adresse)             \
  do {                                   \
    if ((f) == 0x##adresse##u) {         \
      sub_##adresse(ctx, base);          \
    } else {                             \
      GuestFunc(base, (f))(ctx, base);   \
    }                                    \
  } while (0)

namespace {

void Store16(uint8_t* base, uint32_t addr, uint32_t value) {
  const uint16_t v = __builtin_bswap16(uint16_t(value));
  std::memcpy(base + addr, &v, 2);
}

bool PiloteOuValidation() { return g_pilote_actif && Mode() != 0; }

#ifndef __SWITCH__
// Couverture des essais sur PC : une ligne de journal au premier passage dans chaque native (grep « [couverture] »,
// à comparer aux crochets du fichier) : une native jamais atteinte n'est validée par aucune session.
#define TL_COUVERTE(actif)                                                                       \
  ([](const char* f, bool a) {                                                                   \
    static std::atomic<bool> vue{false};                                                         \
    if (a && !vue.exchange(true, std::memory_order_relaxed)) REXLOG_INFO("[couverture] {}", f);  \
    return a;                                                                                    \
  }(__func__, (actif)))
#define Pilote() TL_COUVERTE(Pilote())
#define PiloteOuValidation() TL_COUVERTE(PiloteOuValidation())
#endif

// Les premières différences de la validation, pour savoir quelle fonction est en cause.
std::atomic<int> g_differences_vues{0};
void Difference(const char* nom, uint32_t mot, uint32_t natif, uint32_t recompile) {
  if (g_differences_vues.fetch_add(1, std::memory_order_relaxed) < 40) {
    REXLOG_WARN("[ogre natif] différence sub_{} mot {} : natif {:08X}, recompilé {:08X}", nom, mot, natif, recompile);
  }
}

// Fin d'une fonction pure : en mode 1 le résultat natif ; en mode 2 l'original tourne et les résultats sont comparés.
void FinR3(PPCContext& ctx, uint8_t* base, PPCFunc* original, const char* nom, uint32_t valeur) {
  if (Mode() == 1) {
    ctx.r3.u64 = valeur;
    return;
  }
  original(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32 != valeur) {
    CompterDifference(__LINE__);
    Difference(nom, 0, valeur, ctx.r3.u32);
  }
  Report();
}

// Fin d'une fonction pure qui écrit des mots en mémoire : mode 2 comparé aux mots écrits par l'original.
void FinMots(PPCContext& ctx, uint8_t* base, PPCFunc* original, const char* nom, uint32_t sortie, const uint32_t* mots,
             uint32_t nombre) {
  if (Mode() == 1) {
    for (uint32_t i = 0; i < nombre; i++) Store32(base, sortie + 4 * i, mots[i]);
    return;
  }
  original(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < nombre; i++) {
    if (Load32(base, sortie + 4 * i) != mots[i]) {
      CompterDifference(__LINE__);
      Difference(nom, i, mots[i], Load32(base, sortie + 4 * i));
      break;
    }
  }
  Report();
}

// Prédécesseur dans un arbre std::map MSVC (comme sub_824667D8) ; en-tête : le plus grand.
template <uint32_t kIsNil>
uint32_t TreePrev(const uint8_t* base, uint32_t node) {
  if (base[node + kIsNil]) return Load32(base, node + 8);
  uint32_t left = Load32(base, node);
  if (!base[left + kIsNil]) {
    for (uint32_t r = Load32(base, left + 8); !base[r + kIsNil]; r = Load32(base, left + 8)) left = r;
    return left;
  }
  uint32_t parent = Load32(base, node + 4);
  while (!base[parent + kIsNil] && node == Load32(base, parent)) {
    node = parent;
    parent = Load32(base, parent + 4);
  }
  return parent;
}

// lower_bound exact d'une std::map<uint32_t, ...> (sub_82581D98) : en-tête si la clé n'y est pas.
uint32_t ChercherCle(const uint8_t* base, uint32_t tete, uint32_t cle) {
  uint32_t resultat = tete;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 21];) {
    if (Load32(base, n + 12) < cle) {
      n = Load32(base, n + 8);
    } else {
      resultat = n;
      n = Load32(base, n);
    }
  }
  return resultat != tete && !(cle < Load32(base, resultat + 12)) ? resultat : tete;
}

uint32_t DonneesChaine(const uint8_t* base, uint32_t s) { return Load32(base, s + 20) >= 16 ? Load32(base, s) : s; }

// sub_821C0298(a, 0, taille de a, données de b, taille de b) == 0.
bool ChainesEgales(const uint8_t* base, uint32_t a, uint32_t b) {
  const uint32_t n = Load32(base, b + 16);
  return Load32(base, a + 16) == n && std::memcmp(base + DonneesChaine(base, a), base + DonneesChaine(base, b), n) == 0;
}

// sub_821AFE28 : l'objet et tous ses parents (+80) sont visibles (octet +353) ; r4 non nul : l'objet seul.
bool VisibleEnHierarchie(const uint8_t* base, uint32_t objet, uint32_t seul) {
  if (!base[objet + 353]) return false;
  if (seul & 0xFF) return true;
  for (uint32_t p = Load32(base, objet + 80); p; p = Load32(base, p + 80)) {
    if (!base[p + 353]) return false;
  }
  return true;
}

}  // namespace

// sub_821C0E50 (r3) : case 364 de +1224 s'il existe, sinon sub_821C4570.
REX_HOOK_RAW(sub_821C0E50) {
  if (!Pilote()) return __imp__sub_821C0E50(ctx, base);
  const uint32_t autre = Load32(base, ctx.r3.u32 + 1224);
  if (!autre) return sub_821C4570(ctx, base);
  ctx.r3.u64 = autre;
  GuestFunc(base, Slot(base, autre, 364))(ctx, base);
}

REX_HOOK_RAW(sub_821AFE28) {
  if (!PiloteOuValidation()) return __imp__sub_821AFE28(ctx, base);
  FinR3(ctx, base, __imp__sub_821AFE28, "821AFE28", VisibleEnHierarchie(base, ctx.r3.u32, ctx.r4.u32) ? 1 : 0);
}

namespace {
// sub_821AFD50 : enfant le plus haut sous le point (r4), en partant de la fin de la liste (+60..+64), en profondeur.
uint32_t CibleSousPoint(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t point) {
  const uint32_t debut = Load32(base, self + 60);
  uint32_t it = Load32(base, self + 64);
  while (it != debut) {
    it -= 4;
    const uint32_t enfant = Load32(base, it);
    if (!VisibleEnHierarchie(base, enfant, 0)) continue;
    const uint32_t trouve = CibleSousPoint(ctx, base, enfant, point);
    if (trouve) return trouve;
    const uint32_t obj = Load32(base, it);
    if (base[obj + 586]) continue;
    ctx.r3.u64 = obj;
    ctx.r4.u64 = point;
    const uint32_t f = Slot(base, obj, 12);
    TL_PROMU(f, 821AFCA8);
    if (ctx.r3.u32 & 0xFF) return Load32(base, it);
  }
  return 0;
}
}  // namespace

REX_HOOK_RAW(sub_821AFD50) {
  if (!Pilote()) return __imp__sub_821AFD50(ctx, base);
  const uint32_t self = ctx.r3.u32, point = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 112);
  ctx.r3.u64 = CibleSousPoint(ctx, base, self, point);
}

namespace {
void MajTechnique(PPCContext& ctx, uint8_t* base, uint32_t tech, uint32_t arg) {
  const uint32_t rendu = Load32(base, tech + 600);
  if (!rendu || !base[rendu + 66] || !base[tech + 92]) return;
  if (!Load32(base, tech + 304)) {
    ctx.r3.u64 = tech;
    ctx.r4.u64 = arg;
    sub_821C8D88(ctx, base);
  } else {
    ctx.r3.u64 = rendu;
    ctx.r4.u64 = arg;
    ctx.r5.u64 = tech + 280;
    const uint32_t f = Slot(base, rendu, 28);
    TL_PROMU(f, 821DA090);
  }
  ctx.r3.u64 = tech;
  ctx.r4.u64 = arg;
  sub_821BE358(ctx, base);
}
}  // namespace

// sub_821BE510 (r3 technique, r4 file) : mise en file de la technique par son rendu.
REX_HOOK_RAW(sub_821BE510) {
  if (!Pilote()) return __imp__sub_821BE510(ctx, base);
  const uint32_t tech = ctx.r3.u32, arg = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 112);
  MajTechnique(ctx, base, tech, arg);
}

// sub_821BE5A8 (r3 système, r4 file) : sub_821BE510 pour chaque technique (+348..+352) si le système est actif.
REX_HOOK_RAW(sub_821BE5A8) {
  if (!Pilote()) return __imp__sub_821BE5A8(ctx, base);
  const uint32_t self = ctx.r3.u32, arg = ctx.r4.u32;
  if (!Load32(base, self + 40) || !base[self + 45]) return;
  if (!base[self + 437] && LoadFloat(base, self + 488) > LoadFloat(base, kConstants + 68)) return;
  uint32_t it = Load32(base, self + 348);
  if (it == Load32(base, self + 352)) return;
  CadreInvite cadre(ctx, base, 112);
  do {
    MajTechnique(ctx, base, Load32(base, it), arg);
    it += 4;
  } while (it != Load32(base, self + 352));
}

// sub_821D3F38 (r3 sortie, r4 unité, r5 drapeau) : position de l'unité : celle du nœud (+88, case 192) si drapeau et
// nœud attaché (+80), sinon +112.
REX_HOOK_RAW(sub_821D3F38) {
  if (!Pilote()) return __imp__sub_821D3F38(ctx, base);
  const uint32_t sortie = ctx.r3.u32, unite = ctx.r4.u32;
  uint32_t source = unite + 112;
  const uint32_t noeud = Load32(base, unite + 88);
  if ((ctx.r5.u32 & 0xFF) && noeud && Load32(base, unite + 80)) {
    CadreInvite cadre(ctx, base, 96);
    ctx.r3.u64 = noeud;
    const uint32_t f = Slot(base, noeud, 192);
    TL_PROMU(f, 821C8518);
    source = ctx.r3.u32;
  }
  uint32_t mots[3];
  std::memcpy(mots, base + source, 12);
  std::memcpy(base + sortie, mots, 12);
  ctx.r3.u64 = sortie;
}

// sub_8219BE00 (r3, r4 drapeau) : octet +1 de chaque élément (344 octets) de +496..+500, puis case 156 de +92.
REX_HOOK_RAW(sub_8219BE00) {
  if (!Pilote()) return __imp__sub_8219BE00(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const uint8_t drapeau = uint8_t(ctx.r4.u32);
  const uint32_t d = Load32(base, self + 496);
  const int32_t n = int32_t(Load32(base, self + 500) - d) / 344;
  for (int32_t i = 0; i < n; i++) base[d + uint32_t(i) * 344 + 1] = drapeau;
  const uint32_t cible = Load32(base, self + 92);
  ctx.r3.u64 = cible;
  if (!cible) return;
  ctx.r4.u64 = drapeau ? 4 : 1;
  GuestFunc(base, Slot(base, cible, 156))(ctx, base);
}

// sub_821C9058 (r3) : ressource du périphérique courant (map +292, clé 0x8355A2E4) ; créée par l'original si absente.
REX_HOOK_RAW(sub_821C9058) {
  if (!PiloteOuValidation()) return __imp__sub_821C9058(ctx, base);
  const uint32_t tete = Load32(base, ctx.r3.u32 + 296);
  const uint32_t it = ChercherCle(base, tete, Load32(base, 0x8355A2E4));
  if (it == tete) return __imp__sub_821C9058(ctx, base);
  const uint32_t v = Load32(base, it + 16);
  const uint32_t r = v ? Load32(base, v + 12) : 0;
  if (!r) return __imp__sub_821C9058(ctx, base);
  FinR3(ctx, base, __imp__sub_821C9058, "821C9058", r);
}

// sub_821AECC0 (r3, r4, r5) : sub_821AEC40(r3, r4, 0) puis, pour chaque écouteur de l'arbre trouvé, case 4 (r4 = r5) dont
// le résultat est ajouté (ou) à l'octet +4 de r5.
REX_HOOK_RAW(sub_821AECC0) {
  if (!Pilote()) return __imp__sub_821AECC0(ctx, base);
  const uint32_t self = ctx.r3.u32, sortie = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 112);
  ctx.r5.u64 = 0;
  sub_821AEC40(ctx, base);
  const uint32_t r = ctx.r3.u32;
  if (!r || base[self + 20]) return;
  const uint32_t tete = Load32(base, r + 4);
  for (uint32_t it = Load32(base, tete); it != tete; it = TreeNext<25>(base, it)) {
    const uint32_t obj = Load32(base, Load32(base, Load32(base, it + 16) + 4));
    ctx.r3.u64 = obj;
    ctx.r4.u64 = sortie;
    GuestFunc(base, Slot(base, obj, 4))(ctx, base);
    base[sortie + 4] |= uint8_t(ctx.r3.u32);
  }
}

namespace {
constexpr uint32_t kTasRenderables = 0x83582B5C;  // vector statique de sub_821CC700

// std::sort de sub_823497E0 (comparateur sub_82190520 : clé flottante +28) : jusqu'à 32 éléments, le tri par insertion du
// jeu (sub_82349940), mêmes comparaisons, même résultat ; au-delà, le tri du jeu.
void TrierRenderables(PPCContext& ctx, uint8_t* base, uint32_t debut, uint32_t fin) {
  const int32_t n = int32_t(fin - debut) >> 2;
  if (n <= 1) return;
  if (n > 32) {
    ctx.r3.u64 = debut;
    ctx.r4.u64 = fin;
    ctx.r5.u64 = uint32_t(n);
    ctx.r6.u64 = 0x82190520;
    sub_823497E0(ctx, base);
    return;
  }
  uint32_t p[32];
  float cles[32];
  std::memcpy(p, base + debut, size_t(n) * 4);
  for (int32_t i = 0; i < n; i++) cles[i] = LoadFloat(base, __builtin_bswap32(p[i]) + 28);
  for (int32_t i = 1; i < n; i++) {
    const uint32_t v = p[i];
    const float k = cles[i];
    if (k < cles[0]) {
      std::memmove(p + 1, p, size_t(i) * 4);
      std::memmove(cles + 1, cles, size_t(i) * 4);
      p[0] = v;
      cles[0] = k;
    } else {
      int32_t j = i;
      for (; k < cles[j - 1]; j--) {
        p[j] = p[j - 1];
        cles[j] = cles[j - 1];
      }
      p[j] = v;
      cles[j] = k;
    }
  }
  std::memcpy(base + debut, p, size_t(n) * 4);
}
}  // namespace

// sub_821CC700 (r3 collection, r4 visiteur) : QueuedRenderableCollection, parcours groupé par passe et trié : pour chaque
// passe de la map (+8), renderables copiés dans un vector statique, triés (clé +28), case 8 du visiteur à chaque nouvelle
// clé (arrêt du groupe si faux), case 4 pour chacun.
REX_HOOK_RAW(sub_821CC700) {
  if (!Pilote() || !(Load32(base, 0x83582BE4) & 1)) return __imp__sub_821CC700(ctx, base);
  const uint32_t self = ctx.r3.u32, visiteur = ctx.r4.u32;
  const uint32_t tete = Load32(base, self + 8);
  uint32_t it = Load32(base, tete);
  if (it == tete) return;
  CadreInvite cadre(ctx, base, 176);
  const float depart = LoadFloat(base, kConstants + 116);
  do {
    const uint32_t groupe = Load32(base, it + 16);
    const uint32_t g0 = Load32(base, groupe), g1 = Load32(base, groupe + 4);
    if (g0 != g1) {
      const uint32_t vfin = Load32(base, kTasRenderables + 4), octets = g1 - g0;
      if (Load32(base, kTasRenderables + 8) - vfin >= octets && vfin) {
        std::memcpy(base + vfin, base + g0, octets);
        Store32(base, kTasRenderables + 4, vfin + octets);
      } else {
        for (uint32_t p = g0; p != g1; p += 4) {
          ctx.r3.u64 = kTasRenderables;
          ctx.r4.u64 = p;
          sub_82480518(ctx, base);
        }
      }
      TrierRenderables(ctx, base, Load32(base, kTasRenderables), Load32(base, kTasRenderables + 4));

      float courant = depart;
      uint32_t debut = Load32(base, kTasRenderables), fin = Load32(base, kTasRenderables + 4);
      for (uint32_t i = 0; i < uint32_t(int32_t(fin - debut) >> 2);) {
        const float k = LoadFloat(base, Load32(base, debut + 4 * i) + 28);
        if (!(k == courant)) {
          courant = k;
          ctx.r3.u64 = visiteur;
          ctx.r4.u64 = Load32(base, it + 12);
          const uint32_t f = Slot(base, visiteur, 8);
          TL_PROMU(f, 821D27F8);
          debut = Load32(base, kTasRenderables);
          if (!(ctx.r3.u32 & 0xFF)) {
            fin = Load32(base, kTasRenderables + 4);
            break;
          }
        }
        ctx.r3.u64 = visiteur;
        ctx.r4.u64 = Load32(base, debut + 4 * i);
        const uint32_t f = Slot(base, visiteur, 4);
        TL_PROMU(f, 821CCB88);
        i++;
        debut = Load32(base, kTasRenderables);
        fin = Load32(base, kTasRenderables + 4);
      }
      if (debut != fin) Store32(base, kTasRenderables + 4, debut);
    }
    it = TreeNext<21>(base, it);
  } while (it != tete);
}

namespace {
// sub_821C08B8 (r3 collection, r4 visiteur, r5 mode) : selon l'organisation de la collection (octet +0), parcours groupé
// (sub_821CC700) ou liste triée (+24..+28, éléments de 8 octets, case 12 du visiteur). Rend faux pour le cas d'erreur.
bool AccepterVisiteur(PPCContext& ctx, uint8_t* base, uint32_t coll, uint32_t visiteur, uint32_t mode) {
  const uint32_t drapeaux = base[coll];
  uint32_t cas = mode;
  if (!(drapeaux & mode)) {
    if (drapeaux & 1) {
      cas = 1;
    } else if (drapeaux & 6) {
      cas = 6;
    } else {
      return false;
    }
  }
  if (cas == 1) {
    ctx.r3.u64 = coll;
    ctx.r4.u64 = visiteur;
    sub_821CC700(ctx, base);
  } else if (cas == 6) {
    const uint32_t d = Load32(base, coll + 24);
    for (uint32_t p = Load32(base, coll + 28); p != d;) {
      p -= 8;
      ctx.r3.u64 = visiteur;
      ctx.r4.u64 = p;
      const uint32_t f = Slot(base, visiteur, 12);
      TL_PROMU(f, 821C0A50);
    }
  } else if (cas == 2) {
    const uint32_t f1 = Load32(base, coll + 28);
    for (uint32_t p = Load32(base, coll + 24); p != f1; p += 8) {
      ctx.r3.u64 = visiteur;
      ctx.r4.u64 = p;
      const uint32_t f = Slot(base, visiteur, 12);
      TL_PROMU(f, 821C0A50);
    }
  }
  return true;
}
}  // namespace

REX_HOOK_RAW(sub_821C08B8) {
  if (!Pilote()) return __imp__sub_821C08B8(ctx, base);
  const uint32_t coll = ctx.r3.u32, visiteur = ctx.r4.u32, mode = ctx.r5.u32;
  {
    CadreInvite cadre(ctx, base, 176);
    if (AccepterVisiteur(ctx, base, coll, visiteur, mode)) return;
  }
  ctx.r3.u64 = coll;
  ctx.r4.u64 = visiteur;
  ctx.r5.u64 = mode;
  __imp__sub_821C08B8(ctx, base);
}

// sub_821DD098 (r3, r4 groupe, r5 mode) : pour chaque groupe de priorité (map en +12 de r4), tri des 6 collections
// (sub_821DCFC8 si le bit 2 de leur octet +0 est posé), puis 3 d'entre elles visitées par la case 108 (sub_821DD1C0).
REX_HOOK_RAW(sub_821DD098) {
  if (!Pilote()) return __imp__sub_821DD098(ctx, base);
  const uint32_t self = ctx.r3.u32, mode = ctx.r5.u32;
  const uint32_t tete = Load32(base, ctx.r4.u32 + 12);
  uint32_t it = Load32(base, tete);
  if (it == tete) return;
  CadreInvite cadre(ctx, base, 160);
  auto visiter = [&](uint32_t coll, uint32_t m) {
    const uint32_t f = Slot(base, self, 108);
    if (f == 0x821DD1C0u) {
      const uint32_t v = Load32(base, self + 17212);
      base[v + 12] = 1;
      Store32(base, v + 16, 0);
      base[v + 20] = 1;
      if (AccepterVisiteur(ctx, base, coll, v, m)) return;
    }
    ctx.r3.u64 = self;
    ctx.r4.u64 = coll;
    ctx.r5.u64 = m;
    ctx.r6.u64 = 1;
    ctx.r7.u64 = 1;
    ctx.r8.u64 = 0;
    GuestFunc(base, f)(ctx, base);
  };
  do {
    const uint32_t n = Load32(base, it + 16);
    it = TreeNext<21>(base, it);
    const uint32_t camera = Load32(base, self + 164);
    for (const uint32_t decalage : {8u, 88u, 48u, 128u, 168u, 208u}) {
      if (base[n + decalage] & 2) {
        ctx.r3.u64 = n + decalage;
        ctx.r4.u64 = camera;
        sub_821DCFC8(ctx, base);
      }
    }
    visiter(n + 8, mode);
    visiter(n + 168, mode);
    visiter(n + 208, 2);
  } while (it != tete);
}

// sub_821C3E78 (r3 système, r4 liaison) : setVertexBufferBinding : pour chaque flux de la map (nœud : flux +12, tampon +20),
// tampon du périphérique courant (créé si absent, recopié si périmé : sub_82582E20, sub_821A71E8), date, SetStreamSource ;
// flux sans tampon vidés. SetStreamSource est sauté en rendu natif seul (crochet de torchlight_trace.cpp) : pas d'appel.
// Appelée par le crochet de src/torchlight_trace.cpp (qui signale la méthode au relevé du rendu natif).
namespace torchlight::ogre {
void SetVertexBufferBinding(PPCContext& ctx, uint8_t* base) {
  if (!Pilote()) return __imp__sub_821C3E78(ctx, base);
  const uint32_t self = ctx.r3.u32, liaison = ctx.r4.u32;
  const bool sauter = torchlight::trace::SkipBindings();
  CadreInvite cadre(ctx, base, 192);
  auto flux = [&](uint32_t i, uint32_t tampon, uint32_t pas) {
    if (sauter) return;
    ctx.r3.u64 = Load32(base, 0x8355A2E4);
    ctx.r4.u64 = i;
    ctx.r5.u64 = tampon;
    ctx.r6.u64 = 0;
    ctx.r7.u64 = pas;
    ctx.r8.u64 = 0x8000000000000000ull >> (((uint32_t((95 - i) * 21846) >> 16) & 0xFFFF) + 32);
    sub_821C3D58(ctx, base);
  };
  uint32_t carte;
  {
    const uint32_t f = Slot(base, liaison, 16);
    if (f == 0x82590780u) {  // getBindings : r3 + 4
      carte = liaison + 4;
    } else {
      ctx.r3.u64 = liaison;
      GuestFunc(base, f)(ctx, base);
      carte = ctx.r3.u32;
    }
  }
  const uint32_t tete = Load32(base, carte + 4);
  uint32_t i = 0;
  for (uint32_t it = Load32(base, tete); it != tete; it = TreeNext<33>(base, it), i++) {
    for (; i < Load16(base, it + 12); i++) flux(i, 0, 0);
    const uint32_t cle = Load32(base, 0x8355A2E4);
    const uint32_t buf = Load32(base, it + 20);
    const uint32_t pas = Load32(base, buf + 36);
    uint32_t e = ChercherCle(base, Load32(base, buf + 56), cle);
    if (e == Load32(base, buf + 56) || !Load32(base, Load32(base, e + 16))) {
      ctx.r3.u64 = buf;
      ctx.r4.u64 = cle;
      ctx.r5.u64 = Load32(base, buf + 84);
      sub_82582E20(ctx, base);
      e = ChercherCle(base, Load32(base, buf + 56), cle);
    }
    const uint32_t source = Load32(base, buf + 96);
    if (source) {
      const uint32_t v = Load32(base, e + 16);
      if (base[v + 4]) {
        ctx.r3.u64 = buf;
        ctx.r4.u64 = source;
        ctx.r5.u64 = v;
        sub_821A71E8(ctx, base);
      }
    }
    Store32(base, Load32(base, e + 16) + 20, Load32(base, Load32(base, 0x835067D4) + 224));
    flux(i, Load32(base, Load32(base, e + 16)), pas);
  }
  for (uint32_t k = i; k < Load32(base, self + 2188); k++) flux(k, 0, 0);
  Store32(base, self + 2188, i);
}
}  // namespace torchlight::ogre

// sub_821C26D0 (r3 système, r4 passe) : paramètres automatiques (sub_82201EB0) puis liaison (case 364) des programmes de
// sommets +284, de fragments +304, de géométrie +296. La mise à jour reçoit le pointeur brut : pas de copie du pointeur
// partagé. La case 364 le reçoit par valeur et le détruit elle-même (convention de MSVC) : compteur +1 avant l'appel.
REX_HOOK_RAW(sub_821C26D0) {
  if (!Pilote()) return __imp__sub_821C26D0(ctx, base);
  const uint32_t self = ctx.r3.u32, passe = ctx.r4.u32;
  const uint32_t p0 = Load32(base, passe + 284), p1 = Load32(base, passe + 304), p2 = Load32(base, passe + 296);
  if (!p0 && !p1 && !p2) return;
  const uint32_t masque = Load16(base, self + 17332);
  if (!masque) return;
  if ((p0 && !Load32(base, p0 + 32)) || (p1 && !Load32(base, p1 + 32)) || (p2 && !Load32(base, p2 + 32))) {
    return __imp__sub_821C26D0(ctx, base);  // exception du jeu
  }
  CadreInvite cadre(ctx, base, 144);
  const uint32_t source = Load32(base, self + 17128);
  for (const uint32_t p : {p0, p1, p2}) {
    if (!p) continue;
    ctx.r3.u64 = Load32(base, p + 32);
    ctx.r4.u64 = source;
    ctx.r5.u64 = masque;
    sub_82201EB0(ctx, base);
  }
  const uint32_t pointeur = ctx.r1.u32 + 80;
  auto lier = [&](uint32_t p, uint32_t type) {
    if (!p) return;
    Store32(base, pointeur, 0x8200091C);
    std::memcpy(base + pointeur + 4, base + p + 32, 12);
    const uint32_t compteur = Load32(base, p + 36);
    if (compteur) Store32(base, compteur, Load32(base, compteur) + 1);
    const uint32_t systeme = Load32(base, self + 80);
    ctx.r3.u64 = systeme;
    ctx.r4.u64 = type;
    ctx.r5.u64 = pointeur;
    ctx.r6.u64 = Load16(base, self + 17332);
    const uint32_t f = Slot(base, systeme, 364);
    TL_PROMU(f, 821C2118);
  };
  lier(p0, 0);
  lier(p1, 2);
  lier(p2, 1);
  Store16(base, self + 17332, 0);
}

// sub_823ECDE8 (r3 sortie {itérateur, inséré}, r4 std::set, r5 nœud déjà alloué, clé +12) : insertion unique ; nœud libéré
// si la clé existe déjà.
REX_HOOK_RAW(sub_823ECDE8) {
  if (!Pilote()) return __imp__sub_823ECDE8(ctx, base);
  const uint32_t sortie = ctx.r3.u32, arbre = ctx.r4.u32, noeud = ctx.r5.u32;
  const uint32_t cle = Load32(base, noeud + 12);
  const uint32_t tete = Load32(base, arbre + 4);
  uint32_t parent = tete;
  bool gauche = true;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 17];) {
    gauche = cle < Load32(base, n + 12);
    parent = n;
    n = gauche ? Load32(base, n) : Load32(base, n + 8);
  }
  bool inserer;
  uint32_t it = parent;
  if (gauche && parent == Load32(base, tete)) {
    inserer = true;
  } else {
    if (gauche) it = TreePrev<17>(base, parent);
    inserer = Load32(base, it + 12) < cle;
  }
  CadreInvite cadre(ctx, base, 128);
  if (inserer) {
    const uint32_t tmp = ctx.r1.u32 + 80;
    ctx.r3.u64 = tmp;
    ctx.r4.u64 = arbre;
    ctx.r5.u64 = gauche ? 1 : 0;
    ctx.r6.u64 = parent;
    ctx.r7.u64 = noeud;
    sub_823ECF88(ctx, base);
    base[sortie + 4] = 1;
    Store32(base, sortie, Load32(base, ctx.r3.u32));
  } else {
    ctx.r3.u64 = noeud;
    sub_821CD9A8(ctx, base);
    base[sortie + 4] = 0;
    Store32(base, sortie, it);
  }
  ctx.r3.u64 = sortie;
}

// sub_823E7380 (r3, r4 nom) : cherche dans la liste +32..+36 l'élément de ce nom (+4 du détail, std::string en +4) dont
// l'octet +48 est posé, le prépare et le rend (r3 = 1) ; 0 sinon. La comparaison du jeu entre 0x8349D5A0 et elle-même est
// toujours vraie : le second nom n'est jamais comparé.
REX_HOOK_RAW(sub_823E7380) {
  if (!Pilote()) return __imp__sub_823E7380(ctx, base);
  const uint32_t self = ctx.r3.u32, nom = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 144);
  sub_8219C598(ctx, base);
  ctx.r3.u64 = self;
  sub_8219C3E8(ctx, base);
  for (uint32_t it = Load32(base, self + 32); it != Load32(base, self + 36); it += 4) {
    const uint32_t e = Load32(base, it);
    if (!ChainesEgales(base, Load32(base, e + 4) + 4, nom) || !base[e + 48]) continue;
    ctx.r3.u64 = e;
    sub_823E6ED0(ctx, base);
    for (uint32_t p = Load32(base, e + 16); p != Load32(base, e + 20); p += 4) {
      const uint32_t x = Load32(base, p);
      ctx.r3.u64 = Load32(base, 0x835829E8);
      ctx.r4.u64 = Load32(base, x + 12);
      ctx.r5.u64 = Load32(base, x + 20);
      sub_823F5F60(ctx, base);
    }
    base[e + 48] = 0;
    ctx.r3.u64 = 1;
    return;
  }
  ctx.r3.u64 = 0;
}

// sub_821CE238 (r3 affecteur, r5 particule) : couleur d'après l'âge : interpolation dans une map (clé flottante +16, couleur
// +32, _Isnil +49), posée ou multipliée (+192) en +96 de la particule.
REX_HOOK_RAW(sub_821CE238) {
#pragma clang fp contract(off)
  if (!PiloteOuValidation()) return __imp__sub_821CE238(ctx, base);
  const uint32_t self = ctx.r3.u32, part = ctx.r5.u32;
  if (Load32(base, part + 72)) {
    if (Mode() == 2) __imp__sub_821CE238(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  const float total = LoadFloat(base, part + 64);
  const float t = (total - LoadFloat(base, part + 60)) / total;
  const uint32_t tete = Load32(base, self + 180), premier = Load32(base, tete);
  uint32_t it = premier;
  bool reculer = true;
  while (it != tete) {
    if (t < LoadFloat(base, it + 16)) {
      reculer = it != premier;
      break;
    }
    it = TreeNext<49>(base, it);
  }
  if (reculer) it = TreePrev<49>(base, it);
  const uint32_t suivant = TreeNext<49>(base, it);
  uint32_t mots[4];
  if (suivant != tete) {
    const float ka = LoadFloat(base, it + 16);
    const float r = (t - ka) / (LoadFloat(base, suivant + 16) - ka);
    for (int i = 0; i < 4; i++) {
      const float a = LoadFloat(base, it + 32 + 4 * i);
      mots[i] = FloatBits(a + r * (LoadFloat(base, suivant + 32 + 4 * i) - a));
    }
  } else {
    for (int i = 0; i < 4; i++) mots[i] = Load32(base, it + 32 + 4 * i);
  }
  if (int32_t(Load32(base, self + 192)) != 1) {
    for (int i = 0; i < 4; i++) {
      float v;
      std::memcpy(&v, &mots[i], 4);
      mots[i] = FloatBits(v * LoadFloat(base, part + 112 + 4 * i));
    }
  }
  FinMots(ctx, base, __imp__sub_821CE238, "821CE238", part + 96, mots, 4);
}

namespace {
// sub_823A31F8 : point du segment [a, b] le plus proche de p.
void PointSurSegment(const uint8_t* base, uint32_t a, uint32_t b, uint32_t p, float s[3]) {
#pragma clang fp contract(off)
  const float ax = LoadFloat(base, a), ay = LoadFloat(base, a + 4), az = LoadFloat(base, a + 8);
  const float dx = LoadFloat(base, b) - ax, dy = LoadFloat(base, b + 4) - ay, dz = LoadFloat(base, b + 8) - az;
  const float px = LoadFloat(base, p) - ax, py = LoadFloat(base, p + 4) - ay, pz = LoadFloat(base, p + 8) - az;
  const float t = FmaLikeGuest(px, dx, FmaLikeGuest(pz, dz, py * dy)) /
                  FmaLikeGuest(dz, dz, FmaLikeGuest(dx, dx, dy * dy));
  if (t < LoadFloat(base, kConstants + 68)) {
    s[0] = ax;
    s[1] = ay;
    s[2] = az;
  } else if (t > LoadFloat(base, kConstants + 64)) {
    s[0] = LoadFloat(base, b);
    s[1] = LoadFloat(base, b + 4);
    s[2] = LoadFloat(base, b + 8);
  } else {
    s[0] = FmaLikeGuest(dx, t, ax);
    s[1] = FmaLikeGuest(dy, t, ay);
    s[2] = FmaLikeGuest(dz, t, az);
  }
}
}  // namespace

// sub_823A30D0 (r3, r4, r5 sommets, r6 point, r7 sortie) : point le plus proche sur les trois côtés du triangle. Registres
// sortants conservés par le jeu entre ses appels internes seulement (voir l'audit des appelants).
REX_HOOK_RAW(sub_823A30D0) {
#pragma clang fp contract(off)
  if (!PiloteOuValidation()) return __imp__sub_823A30D0(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, c = ctx.r5.u32, p = ctx.r6.u32, sortie = ctx.r7.u32;
  const float px = LoadFloat(base, p), py = LoadFloat(base, p + 4), pz = LoadFloat(base, p + 8);
  float s[3], m[3];
  PointSurSegment(base, a, b, p, s);
  float dx = s[0] - px, dy = s[1] - py, dz = s[2] - pz;
  float meilleure = FmaLikeGuest(dy, dy, FmaLikeGuest(dz, dz, dx * dx));
  PointSurSegment(base, b, c, p, m);
  dx = m[0] - px, dy = m[1] - py, dz = m[2] - pz;
  float d = FmaLikeGuest(dx, dx, FmaLikeGuest(dz, dz, dy * dy));
  if (d < meilleure) {
    s[0] = m[0], s[1] = m[1], s[2] = m[2];
    meilleure = d;
  }
  PointSurSegment(base, c, a, p, m);
  dx = m[0] - px, dy = m[1] - py, dz = m[2] - pz;
  d = FmaLikeGuest(dx, dx, FmaLikeGuest(dz, dz, dy * dy));
  if (d < meilleure) s[0] = m[0], s[1] = m[1], s[2] = m[2];
  const uint32_t mots[3] = {FloatBits(s[0]), FloatBits(s[1]), FloatBits(s[2])};
  FinMots(ctx, base, __imp__sub_823A30D0, "823A30D0", sortie, mots, 3);
}

// sub_821CEF88 (r3 sortie, r4 a, r5 b) : produit de matrices affines (lignes de 4 ; dernière ligne 0 0 0 1), même ordre
// d'opérations que le jeu (fmadds).
REX_HOOK_RAW(sub_821CEF88) {
#pragma clang fp contract(off)
  if (!PiloteOuValidation()) return __imp__sub_821CEF88(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t ma = ctx.r4.u32, mb = ctx.r5.u32;
  float a[12], b[12];
  for (int i = 0; i < 12; i++) {
    a[i] = LoadFloat(base, ma + 4 * i);
    b[i] = LoadFloat(base, mb + 4 * i);
  }
  uint32_t mots[16];
  for (int l = 0; l < 3; l++) {
    const float x = a[4 * l], y = a[4 * l + 1], z = a[4 * l + 2], w = a[4 * l + 3];
    for (int c = 0; c < 4; c++) {
      float v = FmaLikeGuest(z, b[8 + c], FmaLikeGuest(x, b[c], y * b[4 + c]));
      if (c == 3) v = v + w;
      mots[4 * l + c] = FloatBits(v);
    }
  }
  mots[12] = mots[13] = mots[14] = Load32(base, kConstants + 68);
  mots[15] = Load32(base, kConstants + 64);
  FinMots(ctx, base, __imp__sub_821CEF88, "821CEF88", ctx.r3.u32, mots, 16);
}

// sub_821C1EB8 (r3, r5) : paramètres du brouillard : ceux de r5 (+240 couleur, +256, +260, +264) si l'octet +80 est posé,
// sinon ceux de la scène (cases 572 à 588) ; 1 / (fin - début) en +76, couleur et paramètres copiés dans les constantes des
// programmes (+120, +104 : tampon +72, décalage +76).
REX_HOOK_RAW(sub_821C1EB8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821C1EB8(ctx, base);
  const uint32_t self = ctx.r3.u32, autre = ctx.r5.u32;
  if (!Load32(base, self + 44)) return;
  uint8_t couleur[16];
  double debut, fin, densite;
  if (base[self + 80]) {
    std::memcpy(couleur, base + autre + 240, 16);
    debut = LoadFloat(base, autre + 256);
    fin = LoadFloat(base, autre + 260);
    densite = LoadFloat(base, autre + 264);
  } else {
    const uint32_t scene = Load32(base, Load32(base, 0x83506790) + 4);
    const uint32_t vt = Load32(base, scene);
    if (Load32(base, vt + 572) == 0x821D2B88 && Load32(base, vt + 576) == 0x821BEF20 &&
        Load32(base, vt + 580) == 0x821BEF10 && Load32(base, vt + 584) == 0x821BEF18 &&
        Load32(base, vt + 588) == 0x821D2B90) {
      std::memcpy(couleur, base + scene + 352, 16);
      debut = LoadFloat(base, scene + 368);
      fin = LoadFloat(base, scene + 372);
      densite = LoadFloat(base, scene + 376);
    } else {
      CadreInvite cadre(ctx, base, 144);
      ctx.r3.u64 = scene;
      GuestFunc(base, Load32(base, vt + 572))(ctx, base);
      ctx.r3.u64 = scene;
      GuestFunc(base, Slot(base, scene, 576))(ctx, base);
      std::memcpy(couleur, base + ctx.r3.u32, 16);
      ctx.r3.u64 = scene;
      GuestFunc(base, Slot(base, scene, 580))(ctx, base);
      debut = ctx.f1.f64;
      ctx.r3.u64 = scene;
      GuestFunc(base, Slot(base, scene, 584))(ctx, base);
      fin = ctx.f1.f64;
      ctx.r3.u64 = scene;
      GuestFunc(base, Slot(base, scene, 588))(ctx, base);
      densite = ctx.f1.f64;
    }
  }
  ctx.fpscr.disableFlushMode();
  StoreFloat(base, self + 68, float(debut));
  StoreFloat(base, self + 72, float(fin));
  StoreFloat(base, self + 64, float(densite));
  std::memcpy(base + self + 48, couleur, 16);
  const float inverse = fin == debut ? LoadFloat(base, kConstants + 68)
                                     : LoadFloat(base, kConstants + 64) / float(fin - debut);
  StoreFloat(base, self + 76, inverse);
  uint32_t o = Load32(base, self + 120), t = Load32(base, o + 72);
  if (t) std::memcpy(base + (Load32(base, t) + (Load32(base, o + 76) << 2)), base + self + 64, 16);
  o = Load32(base, self + 104);
  t = Load32(base, o + 72);
  if (t) std::memcpy(base + (Load32(base, t) + (Load32(base, o + 76) << 2)), base + self + 48, 16);
}

namespace {
// threadcache_malloc de nedmalloc (sub_8241F6F8), bloc trouvé seulement : sans bloc, rien n'est touché et l'original refait
// tout (son compteur de demandes compte alors une fois, comme le jeu).
uint32_t BlocDuCache(uint8_t* base, uint32_t tc, uint32_t taille) {
  uint32_t indice = 31 - uint32_t(__builtin_clz(taille >> 4));
  uint32_t meilleure = 1u << (indice + 4);
  if (taille > meilleure) {
    meilleure <<= 1;
    indice++;
  }
  const uint32_t demande = taille < meilleure ? meilleure : taille;
  uint32_t case_ = tc + (indice + 3) * 8;
  uint32_t bloc = Load32(base, case_);
  if ((!bloc || Load32(base, bloc + 4) < demande) && indice < 9) {
    case_ += 8;
    bloc = Load32(base, case_);
  }
  if (!bloc) return 0;
  const uint32_t suivant = Load32(base, bloc + 8), octets = Load32(base, bloc + 4);
  if (suivant) Store32(base, suivant + 12, 0);
  Store32(base, case_, suivant);
  if (!suivant) Store32(base, case_ + 4, 0);
  Store32(base, tc + 8, Load32(base, tc + 8) + 1);
  Store32(base, tc + 16, Load32(base, tc + 16) + 1);
  Store32(base, tc + 20, Load32(base, tc + 20) - octets);
  return bloc;
}
}  // namespace

// sub_821CD7F8 (r3 réserve, r4 taille) : nedpmalloc ; chemin court par le cache du thread, le reste par l'original.
REX_HOOK_RAW(sub_821CD7F8) {
  if (!Pilote()) return __imp__sub_821CD7F8(ctx, base);
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  const uint32_t reserve = r3 ? r3 : 0x83558F90;
  const uint32_t taille = r4 < 16 ? 16 : r4;
  if (taille <= 8192 && Load32(base, reserve + 16)) {
    ctx.r3.u64 = Load32(base, reserve + 1044);
    __imp__KeTlsGetValue(ctx, base);
    const int32_t numero = ctx.r3.s32;
    if (numero > 0) {
      const uint32_t bloc = BlocDuCache(base, Load32(base, reserve + (uint32_t(numero) + 4) * 4), taille);
      if (bloc) {
        ctx.r3.u64 = bloc;
        return;
      }
    }
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
  }
  __imp__sub_821CD7F8(ctx, base);
}

// sub_821C8A58 (r3 contrôleur, f1 dt) : avance d'un système de particules (+4) par pas fixes (sub_821C8618), ou décompte du
// système en pause (+280, états 2 et 3 de +284) qui prévient les rendus des techniques à 800.
REX_HOOK_RAW(sub_821C8A58) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821C8A58(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t sys = Load32(base, self + 4);
  const uint32_t etat = Load32(base, sys + 284);
  if (etat == 2 || etat == 3) {
    const uint32_t n = Load32(base, sys + 280);
    if (int32_t(n) < 900) Store32(base, sys + 280, n + 1);
    if (Load32(base, sys + 280) != 800) return;
    uint32_t it = Load32(base, sys + 348);
    if (it == Load32(base, sys + 352)) return;
    CadreInvite cadre(ctx, base, 128);
    do {
      const uint32_t tech = Load32(base, it);
      const uint32_t rendu = Load32(base, tech + 600);
      if (rendu) {
        ctx.r3.u64 = rendu;
        ctx.r4.u64 = tech + 280;
        GuestFunc(base, Slot(base, rendu, 16))(ctx, base);
      }
      it += 4;
    } while (it != Load32(base, sys + 352));
    return;
  }
  Store32(base, sys + 280, 0);
  const uint32_t reglages = Load32(base, 0x83506798);
  CadreInvite cadre(ctx, base, 128);
  if (base[reglages + 260]) {
    ctx.r3.u64 = sys;
    ctx.r4.u64 = 0;
    GuestFunc(base, Slot(base, sys, 212))(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  const float vitesse = LoadFloat(base, reglages + 176);
  const float x = float(double(vitesse) * dt);
  const float y = vitesse * LoadFloat(base, kConstants + 108);
  const float avance = y < x ? y : x;
  const float un = LoadFloat(base, kConstants + 64);
  float pas = un / LoadFloat(base, 0x8306588C);
  const float limite = pas * LoadFloat(base, kConstants + 124);
  const float cumul = avance + LoadFloat(base, self + 8);
  StoreFloat(base, self + 8, cumul);
  if (!(avance < limite)) {
    ctx.r3.u64 = Load32(base, self + 4);
    ctx.f1.f64 = double(avance);
    sub_821C8618(ctx, base);
    StoreFloat(base, self + 8, LoadFloat(base, kConstants + 68));
    return;
  }
  if (!(cumul > pas)) return;
  float reste;
  do {
    ctx.r3.u64 = Load32(base, self + 4);
    ctx.f1.f64 = double(pas);
    sub_821C8618(ctx, base);
    ctx.fpscr.disableFlushMode();
    pas = un / LoadFloat(base, 0x8306588C);
    reste = LoadFloat(base, self + 8) - pas;
    StoreFloat(base, self + 8, reste);
  } while (reste > pas);
}

// sub_82194008 (r3 unité, f1 dt) : animation (sub_821BB578) du modèle +256 seulement si sa boîte, placée à la position de
// l'unité, est vue de la caméra courante.
REX_HOOK_RAW(sub_82194008) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_82194008(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t modele = Load32(base, self + 256);
  if (!modele) return;
  const uint32_t squelette = Load32(base, modele + 460);
  if (!squelette || !Load32(base, squelette + 40)) return;
  CadreInvite cadre(ctx, base, 176);
  const uint32_t sp = ctx.r1.u32;
  ctx.r3.u64 = sp + 80;
  ctx.r4.u64 = self;
  ctx.r5.u64 = 1;
  sub_821D3F38(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t m = Load32(base, self + 256);
  const uint32_t boite = sp + 112;
  const float marge = LoadFloat(base, kConstants + 264);
  for (int i = 0; i < 3; i++) {
    const float pos = LoadFloat(base, sp + 80 + 4 * i);
    const float mn = LoadFloat(base, m + 468 + 4 * i) + pos, mx = LoadFloat(base, m + 480 + 4 * i) + pos;
    StoreFloat(base, boite + 12 + 4 * i, mx);
    StoreFloat(base, boite + 4 * i, mn > mx ? mx - marge : mn);  // sub_821C7698
  }
  Store32(base, boite + 24, 1);
  Store32(base, boite + 28, 0);
  const uint32_t scene = Load32(base, Load32(base, 0x83559498) + 8);
  const uint32_t camera = Load32(base, scene + 1224);
  ctx.r3.u64 = camera ? camera : scene;
  ctx.r4.u64 = boite;
  sub_821C74A0(ctx, base);
  if (ctx.r3.u32) {
    ctx.r3.u64 = Load32(base, self + 256);
    ctx.r5.u64 = 0;
    ctx.f1.f64 = dt;
    sub_821BB578(ctx, base);
  }
  const uint32_t coins = Load32(base, boite + 28);
  if (coins) {
    ctx.r3.u64 = coins;
    sub_821CD9A8(ctx, base);
  }
}

// sub_821AF868 (r3, f1 distance) : alpha et visibilité des éléments (344 octets, +496..+500) d'après la distance.
REX_HOOK_RAW(sub_821AF868) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821AF868(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const uint32_t g = Load32(base, Load32(base, 0x835594CC) + 72);
  const uint32_t indice = Load32(base, 0x83558EEC), d0 = Load32(base, g + 108);
  if (indice < uint32_t(int32_t(Load32(base, g + 112) - d0) >> 2) && Load32(base, d0 + indice * 4) == 1) return;
  const uint32_t d = Load32(base, self + 496);
  const int32_t n = int32_t(Load32(base, self + 500) - d) / 344;
  if (!n) return;
  ctx.fpscr.disableFlushMode();
  const float f1 = float(ctx.f1.f64);
  const float minimum = LoadFloat(base, kConstants + 248);
  const float echelle = LoadFloat(base, kConstants + 584), inverse = LoadFloat(base, kConstants + 588);
  const float seuil = LoadFloat(base, kConstants + 140);
  const float borne = f1 > minimum ? f1 : minimum;
  const double produit = double(borne * echelle);
  const int64_t entier = std::isnan(produit)              ? INT64_MIN
                         : produit > double(INT64_MAX)    ? INT64_MAX
                         : produit < double(INT64_MIN)    ? INT64_MIN
                                                          : int64_t(produit);
  const float alpha = float(uint8_t(entier)) * inverse;
  const float carre = f1 * f1;
  const uint8_t visible = carre * carre > seuil ? 0 : 1;
  for (uint32_t i = 0; i < uint32_t(n); i++) {
    const uint32_t e = d + i * 344;
    StoreFloat(base, Load32(base, e + 340) + 28, alpha);
    if (Load32(base, e + 60)) base[e] = visible;
  }
}

// sub_8219C6B0 (r3, f1 dt) : minuteries (+196) d'une liste d'objets (+8 données, +12 taille, +16 capacité) : objet détruit à
// échéance, remplacé par le dernier ; sub_8219B3B8 et sub_8227EEA0 si la liste a changé.
REX_HOOK_RAW(sub_8219C6B0) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_8219C6B0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  if (!Load32(base, self + 12)) return;
  const uint32_t liste = self + 8;
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68);
  bool change = false;
  uint32_t i = 0, off = 0;
  do {
    uint32_t donnees = Load32(base, liste);
    const uint32_t obj = Load32(base, i < Load32(base, liste + 8) ? donnees + off : donnees);
    float reste = LoadFloat(base, obj + 196);
    bool vivant = true;
    if (!(reste < zero)) {
      reste = float(double(reste) - dt);
      StoreFloat(base, obj + 196, reste);
      if (!(reste > zero)) {
        StoreFloat(base, obj + 196, zero);
        vivant = false;
      }
    }
    if (!vivant) {
      const uint32_t cible = Load32(base, i < Load32(base, liste + 8) ? Load32(base, liste) + off : Load32(base, liste));
      if (cible) {
        {
          CadreInvite cadre(ctx, base, 144);
          ctx.r3.u64 = cible;
          ctx.r4.u64 = 1;
          GuestFunc(base, Slot(base, cible, 0))(ctx, base);
        }
        donnees = Load32(base, liste);
        Store32(base, i < Load32(base, liste + 8) ? donnees + off : donnees, 0);
      }
      const uint32_t taille = Load32(base, liste + 4);
      if (i < taille) {
        const uint32_t nouvelle = taille - 1;
        donnees = Load32(base, liste);
        Store32(base, liste + 4, nouvelle);
        Store32(base, donnees + off, Load32(base, donnees + nouvelle * 4));
      }
      i--;
      off -= 4;
      change = true;
    }
    i++;
    off += 4;
  } while (i < Load32(base, self + 12));
  if (change) {
    CadreInvite cadre(ctx, base, 144);
    ctx.r3.u64 = self;
    sub_8219B3B8(ctx, base);
    ctx.r3.u64 = self;
    sub_8227EEA0(ctx, base);
  }
}

// sub_821D8770 (r3, r4 clé, r5 groupe) : valeur (+16) de la clé dans la map du groupe ; zéro si hors bornes ou absente.
REX_HOOK_RAW(sub_821D8770) {
  if (!PiloteOuValidation()) return __imp__sub_821D8770(ctx, base);
  const int32_t cle = ctx.r4.s32, groupe = ctx.r5.s32;
  uint32_t mot = Load32(base, kConstants + 68);
  if (groupe <= 7 && cle < 146) {
    const uint32_t tete = Load32(base, ctx.r3.u32 + (uint32_t(groupe + 6) << 4) + 4);
    uint32_t resultat = tete;
    for (uint32_t n = Load32(base, tete + 4); !base[n + 21];) {
      if (int32_t(Load32(base, n + 12)) < cle) {
        n = Load32(base, n + 8);
      } else {
        resultat = n;
        n = Load32(base, n);
      }
    }
    if (resultat != tete && !(cle < int32_t(Load32(base, resultat + 12)))) mot = Load32(base, resultat + 16);
  }
  float v;
  std::memcpy(&v, &mot, 4);
  if (Mode() == 1) {
    ctx.f1.f64 = double(v);
    return;
  }
  __imp__sub_821D8770(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (float(ctx.f1.f64) != v && !(std::isnan(v) && std::isnan(ctx.f1.f64))) {
    CompterDifference(__LINE__);
    Difference("821D8770", 0, mot, FloatBits(float(ctx.f1.f64)));
  }
  Report();
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, vague 2. Rappel de la sémantique des comparaisons flottantes du PowerPC (NaN : aucun de lt, gt, eq) : ble = !(a > b),
// bge = !(a < b), bne = !(a == b) : à traduire tels quels, pas par a <= b.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821C93A0);
REX_EXTERN(__imp__sub_821C8E40);
REX_EXTERN(__imp__sub_821CC498);
REX_EXTERN(__imp__sub_822E8828);
REX_EXTERN(__imp__sub_821D9BC8);
REX_EXTERN(__imp__sub_821D9870);
REX_EXTERN(__imp__sub_821C8618);
REX_EXTERN(__imp__sub_821C47F0);
REX_EXTERN(sub_821C5050);
REX_EXTERN(sub_821A58E0);
REX_EXTERN(sub_821CA710);
REX_EXTERN(sub_821C8E40);
REX_EXTERN(sub_821CAD88);
REX_EXTERN(sub_821C9740);
REX_EXTERN(sub_821CA3F0);
REX_EXTERN(sub_821CAD08);
REX_EXTERN(sub_821CA880);
REX_EXTERN(sub_821C9100);
REX_EXTERN(sub_821CA1A8);
REX_EXTERN(sub_821CB0F0);
REX_EXTERN(sub_821CA930);
REX_EXTERN(sub_821CEB60);
REX_EXTERN(sub_821C7E30);
REX_EXTERN(sub_821C08B0);
REX_EXTERN(sub_821A7428);
REX_EXTERN(sub_821DDB00);
REX_EXTERN(sub_821CB998);
REX_EXTERN(sub_821E2D90);
REX_EXTERN(sub_822E3950);
REX_EXTERN(sub_821C3A88);
REX_EXTERN(sub_821C8560);
REX_EXTERN(sub_82593348);
REX_EXTERN(sub_821C81F8);
REX_EXTERN(sub_821DD4F8);
REX_EXTERN(sub_821C9CA8);
REX_EXTERN(sub_821C9BF8);

namespace torchlight::ogre {

// sub_821C93A0 (r3 système, r4 unité, r5 TextureUnitState) : _setTextureUnitSettings : suite d'appels aux méthodes du système.
// Appelée par le crochet de src/torchlight_trace.cpp.
void SetTextureUnitSettings(PPCContext& ctx, uint8_t* base) {
  if (!Pilote()) return __imp__sub_821C93A0(ctx, base);
  const uint32_t self = ctx.r3.u32, unite = ctx.r4.u32, tu = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 144);
  auto appel = [&](uint32_t slot, uint32_t attendu, PPCFunc* direct) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = unite;
    const uint32_t f = Slot(base, self, slot);
    if (f == attendu) {
      direct(ctx, base);
    } else {
      GuestFunc(base, f)(ctx, base);
    }
  };
  uint32_t texture;
  const uint32_t image = Load32(base, tu);
  if (Load32(base, tu + 324)) {  // sub_821C5050, textures chargées
    texture = Load32(base, tu + 348) + (image << 4);
  } else {
    ctx.r3.u64 = tu;
    ctx.r4.u64 = image;
    sub_821C5050(ctx, base);
    texture = ctx.r3.u32;
  }
  constexpr uint32_t kTextureNulle = 0x834A1A78, kReglages = 0x83506794;
  const uint32_t capacites = Load32(base, self + 764);
  const bool sommets = ((Load32(base, capacites + 32) >> 24) & 1) && !base[capacites + 136];
  if (sommets) {
    const bool vers_sommets = int32_t(Load32(base, tu + 320)) == 1;
    ctx.r5.u64 = vers_sommets ? texture : kTextureNulle;
    appel(160, 0x821CA710, sub_821CA710);
    ctx.r5.u64 = 1;
    ctx.r6.u64 = vers_sommets ? kTextureNulle : texture;
  } else {
    ctx.r5.u64 = 1;
    ctx.r6.u64 = texture;
  }
  appel(156, 0x821C8E40, sub_821C8E40);
  ctx.r5.u64 = Load32(base, tu + 24);
  appel(164, 0x821CAD88, sub_821CAD88);
  auto filtre = [&](uint32_t type, uint32_t decalage) -> uint32_t {
    if (!base[tu + 317]) return Load32(base, tu + decalage);
    const uint32_t r = Load32(base, kReglages);
    ctx.r3.u64 = r;
    ctx.r4.u64 = type;
    const uint32_t f = Slot(base, r, 160);
    TL_PROMU(f, 821C9740);
    return ctx.r3.u32;
  };
  const uint32_t mip = filtre(2, 304), mag = filtre(1, 300), min = filtre(0, 296);
  ctx.r5.u64 = min;
  ctx.r6.u64 = mag;
  ctx.r7.u64 = mip;
  appel(180, 0x821CA3F0, sub_821CA3F0);
  ctx.r5.u64 = base[tu + 316] ? Load32(base, Load32(base, kReglages) + 216) : Load32(base, tu + 308);
  appel(184, 0x821CAD08, sub_821CAD08);
  ctx.f1.f64 = double(LoadFloat(base, tu + 312));
  appel(196, 0x821CA880, sub_821CA880);
  ctx.r5.u64 = tu + 64;
  appel(172, 0x821C9100, sub_821C9100);
  ctx.r5.u64 = tu + 144;
  appel(172, 0x821C9100, sub_821C9100);
  ctx.r5.u64 = tu + 28;
  appel(188, 0x821CA1A8, sub_821CA1A8);
  if (int32_t(Load32(base, tu + 28)) == 3 || int32_t(Load32(base, tu + 32)) == 3 || int32_t(Load32(base, tu + 36)) == 3) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = unite;
    ctx.r5.u64 = tu + 48;
    GuestFunc(base, Slot(base, self, 192))(ctx, base);
  }
  bool effet = false;
  const uint32_t tete = Load32(base, tu + 424);
  for (uint32_t it = Load32(base, tete); it != tete; it = TreeNext<61>(base, it)) {
    const uint32_t type = Load32(base, it + 16);
    if (type == 0) {
      const int32_t sorte = int32_t(Load32(base, it + 20));
      const uint32_t calcul = sorte == 1 ? 1 : sorte == 0 ? 2 : sorte == 2 ? 3 : sorte == 3 ? 4 : 0;
      if (!calcul) continue;
      ctx.r5.u64 = calcul;
      ctx.r6.u64 = 0;
    } else if (type == 1) {
      ctx.r5.u64 = 5;
      ctx.r6.u64 = Load32(base, it + 56);
    } else {
      continue;
    }
    appel(168, 0x821CB0F0, sub_821CB0F0);
    effet = true;
  }
  if (!effet) {
    ctx.r5.u64 = 0;
    ctx.r6.u64 = 0;
    appel(168, 0x821CB0F0, sub_821CB0F0);
  }
  if (base[tu + 211]) {
    ctx.r3.u64 = tu;
    sub_821A58E0(ctx, base);
  }
  ctx.r5.u64 = tu + 232;
  appel(200, 0x821CA930, sub_821CA930);
}

// sub_821C8E40 (r3 système, r4 unité, r5 activée, r6 const TexturePtr&) : _setTexture. Le jeu copie le pointeur partagé
// le temps de la fonction (compteur +1 puis -1) : inutile, l'appelant garde le sien (reçu par référence, contrairement à
// la case 364 de sub_821C26D0). Appelée par le crochet de src/torchlight_trace.cpp.
void SetTexture(PPCContext& ctx, uint8_t* base) {
  if (!Pilote()) return __imp__sub_821C8E40(ctx, base);
  const uint32_t self = ctx.r3.u32, unite = ctx.r4.u32, active = ctx.r5.u32;
  const uint32_t texture = Load32(base, ctx.r6.u32 + 4);
  const uint32_t etat = self + unite * 24;
  const uint32_t type_unite = self + (unite + 37) * 24;
  const uint32_t decalage = unite + 32;
  const uint64_t masque = decalage < 64 ? 0x8000000000000000ull >> decalage : 0;
  CadreInvite cadre(ctx, base, 144);
  if ((active & 0xFF) && texture) {
    ctx.r3.u64 = texture;
    ctx.r4.u64 = 1;
    const uint32_t f = Slot(base, texture, 84);
    TL_PROMU(f, 821C7E30);
    ctx.r3.u64 = texture;
    sub_821C9058(ctx, base);
    const uint32_t d3d = ctx.r3.u32;
    if (Load32(base, etat + 904) == d3d) return;
    ctx.r3.u64 = Load32(base, 0x8355A2E4);
    ctx.r4.u64 = unite;
    ctx.r5.u64 = d3d;
    ctx.r6.u64 = masque;
    sub_821CEB60(ctx, base);
    Store32(base, etat + 904, d3d);
    ctx.r3.u64 = texture;
    const uint32_t g = Slot(base, texture, 176);
    TL_PROMU(g, 821C08B0);
    const uint32_t t = ctx.r3.u32;
    Store32(base, type_unite, t == 1 || t == 2 ? 0 : t == 3 ? 2 : t == 4 ? 1 : 3);
    return;
  }
  if (Load32(base, etat + 904)) {
    ctx.r3.u64 = Load32(base, 0x8355A2E4);
    ctx.r4.u64 = unite;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = masque;
    sub_821CEB60(ctx, base);
  }
  Store32(base, etat + 904, 0);
  Store32(base, etat + 896, 0);
  Store32(base, etat + 892, 0);
  Store32(base, type_unite, 0);
}

}  // namespace torchlight::ogre

// sub_821CC498 (r3 squelette, r4 ensemble d'états) : Skeleton::setAnimationState : pour chaque état actif (liste +24), piste
// de chaque os appliquée (case 80) avec son poids, normalisé si le mélange est en moyenne (+184 nul). Tourne aussi sur les
// threads de travail (poses).
REX_HOOK_RAW(sub_821CC498) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821CC498(ctx, base);
  const uint32_t self = ctx.r3.u32, ensemble = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 208);
  const uint32_t sp = ctx.r1.u32;
  ctx.r3.u64 = self;
  ctx.r4.u64 = 0;
  {
    const uint32_t f = Slot(base, self, 220);
    TL_PROMU(f, 821A7428);
  }
  auto animation = [&](uint32_t e, uint32_t lien) -> uint32_t {
    Store32(base, lien, 0);
    ctx.r3.u64 = self;
    ctx.r4.u64 = e + 8;
    ctx.r5.u64 = lien;
    const uint32_t f = Slot(base, self, 240);
    TL_PROMU(f, 821DDB00);
    return ctx.r3.u32;
  };
  const float un = LoadFloat(base, kConstants + 64);
  float facteur = un;
  if (!Load32(base, self + 184)) {
    const uint32_t tete = Load32(base, ensemble + 24);
    float somme = LoadFloat(base, kConstants + 68);
    for (uint32_t it = Load32(base, tete); it != tete;) {
      const uint32_t e = Load32(base, it + 8);
      it = Load32(base, it);
      if (animation(e, sp + 84)) {
        ctx.fpscr.disableFlushMode();
        somme = LoadFloat(base, e + 48) + somme;
      }
    }
    if (somme > un) facteur = un / somme;
  }
  const uint32_t tete = Load32(base, ensemble + 24);
  for (uint32_t it = Load32(base, tete); it != tete;) {
    const uint32_t e = Load32(base, it + 8);
    it = Load32(base, it);
    const uint32_t anim = animation(e, sp + 80);
    if (!anim) continue;
    const uint32_t masque = Load32(base, e + 4), lien = Load32(base, sp + 80);
    const float echelle = lien ? LoadFloat(base, lien + 44) : un;
    const float poids = LoadFloat(base, e + 48);
    const uint32_t temps = masque ? sp + 88 : sp + 96;
    ctx.r3.u64 = temps;
    ctx.r4.u64 = anim;
    ctx.f1.f64 = double(LoadFloat(base, e + 40));
    sub_821E2D90(ctx, base);
    ctx.fpscr.disableFlushMode();
    const float general = poids * facteur;
    const uint32_t pistes = Load32(base, anim + 8);
    for (uint32_t n = Load32(base, pistes); n != pistes; n = TreeNext<21>(base, n)) {
      const uint32_t numero = Load16(base, n + 12);
      const uint32_t g = Slot(base, self, 208);
      uint32_t os;
      if (g == 0x821CCDC0u) {
        os = Load32(base, Load32(base, self + 188) + (numero << 2));
      } else {
        ctx.r3.u64 = self;
        ctx.r4.u64 = numero;
        GuestFunc(base, g)(ctx, base);
        os = ctx.r3.u32;
      }
      float w = general;
      if (masque) {
        ctx.fpscr.disableFlushMode();
        w = LoadFloat(base, Load32(base, masque) + (uint32_t(Load16(base, os + 308)) << 2)) * poids * facteur;
      }
      const uint32_t piste = Load32(base, n + 16);
      ctx.r3.u64 = piste;
      ctx.r4.u64 = os;
      ctx.r5.u64 = temps;
      ctx.f1.f64 = double(w);
      ctx.f2.f64 = double(echelle);
      const uint32_t f = Slot(base, piste, 80);
      TL_PROMU(f, 821CB998);
    }
  }
}

namespace {
void PousserEntier(PPCContext& ctx, uint8_t* base, uint32_t vecteur, uint32_t valeur) {
  const uint32_t fin = Load32(base, vecteur + 4);
  if (fin && fin != Load32(base, vecteur + 8)) {
    Store32(base, fin, valeur);
    Store32(base, vecteur + 4, fin + 4);
    return;
  }
  const uint32_t tmp = ctx.r1.u32 + 80;
  Store32(base, tmp, valeur);
  ctx.r3.u64 = vecteur;
  ctx.r4.u64 = tmp;
  sub_822E3950(ctx, base);
}

// sub_822E8828 : éléments d'un quadtree (nœud : +32 maillage, +36 a des enfants, +40 les 4 enfants, +56..+60 éléments) dans le
// rectangle [min, max] (x, z). Enfant entièrement dedans : tout est pris sans test.
void RequeteQuadtree(PPCContext& ctx, uint8_t* base, uint32_t noeud, float minx, float minz, float maxx, float maxz,
                     bool tester, uint32_t sortie) {
  const uint32_t d = Load32(base, noeud + 56);
  const uint32_t n = uint32_t(int32_t(Load32(base, noeud + 60) - d) >> 2);
  if (!tester) {
    for (uint32_t i = 0; i < n; i++) PousserEntier(ctx, base, sortie, Load32(base, d + 4 * i));
    if (base[noeud + 36]) {
      for (uint32_t k = 0; k < 4; k++) {
        RequeteQuadtree(ctx, base, Load32(base, noeud + 40 + 4 * k), minx, minz, maxx, maxz, false, sortie);
      }
    }
    return;
  }
  const uint32_t maillage = Load32(base, noeud + 32);
  const uint32_t bas = Load32(base, maillage + 32), haut = Load32(base, maillage + 48);
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t v = Load32(base, d + 4 * i), o = v * 12;
    if (LoadFloat(base, bas + o) < minx || LoadFloat(base, bas + o + 8) < minz || LoadFloat(base, haut + o) > maxx ||
        LoadFloat(base, haut + o + 8) > maxz) {
      continue;
    }
    PousserEntier(ctx, base, sortie, v);
  }
  if (!base[noeud + 36]) return;
  for (uint32_t k = 0; k < 4; k++) {
    const uint32_t c = Load32(base, noeud + 40 + 4 * k);
    const float x0 = LoadFloat(base, c + 8), z0 = LoadFloat(base, c + 16);
    const float x1 = LoadFloat(base, c + 20), z1 = LoadFloat(base, c + 28);
    if (!(x0 > maxx) && !(z0 > maxz) && !(x1 < minx) && !(z1 < minz)) {
      RequeteQuadtree(ctx, base, c, minx, minz, maxx, maxz, false, sortie);
    } else if (!(maxx < x1) && !(maxz < z1) && !(minx > x0) && !(minz > z0)) {
      RequeteQuadtree(ctx, base, c, minx, minz, maxx, maxz, true, sortie);
    }
  }
}

int32_t FctiwzLikeGuest(double d) {
  if (std::isnan(d)) return INT32_MIN;
  if (d > double(INT32_MAX)) return INT32_MAX;
  if (d < double(INT32_MIN)) return INT32_MIN;
  return int32_t(d);
}

// sub_821D9BC8 (+1) et sub_821D9870 (-1) (r3 grille, r4 min, r5 max, f1 rayon) : compteurs des cases de la grille (+44 lignes,
// +48 et +52 dimensions) sous un rectangle, ou sous un disque si le rayon n'est pas nul. sub_821D9830 : floor.
template <int kDelta>
void MajGrille(PPCContext& ctx, uint8_t* base) {
#pragma clang fp contract(off)
  ctx.fpscr.disableFlushMode();
  const uint32_t self = ctx.r3.u32, a = ctx.r4.u32, b = ctx.r5.u32;
  const float rayon = float(ctx.f1.f64);
  const float zero = LoadFloat(base, kConstants + 68);
  const bool origine = Load32(base, self + 64) != 0;
  const float ox = origine ? LoadFloat(base, self + 380) : zero, oz = origine ? LoadFloat(base, self + 384) : zero;
  const float c572 = LoadFloat(base, kConstants + 572);
  auto cellule = [&](float v, float o) {
    return FctiwzLikeGuest(double(float(std::floor(double((v - o) * c572)))));
  };
  const float ax = LoadFloat(base, a), az = LoadFloat(base, a + 8), bx = LoadFloat(base, b), bz = LoadFloat(base, b + 8);
  const int32_t l = int32_t(Load32(base, self + 48)), h = int32_t(Load32(base, self + 52));
  const int32_t x0 = std::min(std::max(cellule(ax, ox), 0), l), x1 = std::min(std::max(cellule(bx, ox), 0), l);
  const int32_t z0 = std::min(std::max(cellule(az, oz), 0), h), z1 = std::min(std::max(cellule(bz, oz), 0), h);
  if (x0 >= x1 || z0 >= z1) return;
  const uint32_t lignes = Load32(base, self + 44);
  if (rayon == zero) {
    for (int32_t x = x0; x < x1; x++) {
      const uint32_t ligne = Load32(base, lignes + uint32_t(x) * 4);
      for (int32_t z = z0; z < z1; z++) base[ligne + uint32_t(z)] = uint8_t(base[ligne + uint32_t(z)] + kDelta);
    }
    return;
  }
  const float c124 = LoadFloat(base, kConstants + 124);
  float cx = bx + ax, cz = bz + az;  // sub_821DCF68 : centre
  if (!(c124 == zero)) {
    const float inverse = LoadFloat(base, kConstants + 64) / c124;
    cx *= inverse;
    cz *= inverse;
  }
  const float c140 = LoadFloat(base, kConstants + 140);
  for (int32_t x = x0; x < x1; x++) {
    const uint32_t ligne = Load32(base, lignes + uint32_t(x) * 4);
    const float dx = cx - FmaLikeGuest(float(x), c140, ox);
    const float dx2 = dx * dx;
    for (int32_t z = z0; z < z1; z++) {
      const float dz = cz - FmaLikeGuest(float(z), c140, oz);
      const float d = float(std::sqrt(double(FmaLikeGuest(dz, dz, dx2))));
      if (d > rayon) continue;
      base[ligne + uint32_t(z)] = uint8_t(base[ligne + uint32_t(z)] + kDelta);
    }
  }
}
}  // namespace

REX_HOOK_RAW(sub_822E8828) {
  if (!Pilote()) return __imp__sub_822E8828(ctx, base);
  const uint32_t noeud = ctx.r3.u32, a = ctx.r4.u32, b = ctx.r5.u32, sortie = ctx.r7.u32;
  const bool tester = (ctx.r6.u32 & 0xFF) != 0;
  CadreInvite cadre(ctx, base, 160);
  RequeteQuadtree(ctx, base, noeud, LoadFloat(base, a), LoadFloat(base, a + 8), LoadFloat(base, b), LoadFloat(base, b + 8),
                  tester, sortie);
}

REX_HOOK_RAW(sub_821D9BC8) {
  if (!Pilote()) return __imp__sub_821D9BC8(ctx, base);
  MajGrille<1>(ctx, base);
}

REX_HOOK_RAW(sub_821D9870) {
  if (!Pilote()) return __imp__sub_821D9870(ctx, base);
  MajGrille<-1>(ctx, base);
}

// sub_821C8618 (r3 système, f1 dt) : ParticleSystem::_update : système hors de portée mis en sommeil (+488), durée de vie,
// émission (+424, +428 : pas fixes ; sub_821C81F8), état, puis déplacement (+324 contre la position, case 204). Tourne sur les
// threads de travail (src/torchlight_particules.cpp).
REX_HOOK_RAW(sub_821C8618) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821C8618(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  if (!Load32(base, self + 40) || !base[self + 45]) return;
  ctx.fpscr.disableFlushMode();
  const uint32_t reglages = Load32(base, 0x83506798);
  const float zero = LoadFloat(base, kConstants + 68);
  CadreInvite cadre(ctx, base, 128);
  auto techniques = [&](PPCFunc* f) {
    uint32_t it = Load32(base, self + 348);
    if (it == Load32(base, self + 352)) return;
    do {
      ctx.r3.u64 = Load32(base, it);
      f(ctx, base);
      it += 4;
    } while (it != Load32(base, self + 352));
  };
  auto bouger = [&]() {
    ctx.r3.u64 = self;
    const uint32_t f = Slot(base, self, 204);
    TL_PROMU(f, 821C8560);
    ctx.fpscr.disableFlushMode();
    const uint32_t p = ctx.r3.u32;
    if (LoadFloat(base, self + 324) == LoadFloat(base, p) && LoadFloat(base, self + 328) == LoadFloat(base, p + 4) &&
        LoadFloat(base, self + 332) == LoadFloat(base, p + 8)) {
      return;
    }
    StoreFloat(base, self + 488, zero);
    StoreFloat(base, self + 324, LoadFloat(base, self + 336));
    StoreFloat(base, self + 328, LoadFloat(base, self + 340));
    StoreFloat(base, self + 332, LoadFloat(base, self + 344));
    techniques(sub_821C9CA8);
  };
  if (!base[self + 437]) {
    bool actif = false;
    for (uint32_t it = Load32(base, self + 348); it != Load32(base, self + 352); it += 4) {
      if (base[Load32(base, it) + 801]) {
        actif = true;
        break;
      }
    }
    if (!actif) {
      const float reste = float(double(LoadFloat(base, self + 488)) - dt);
      StoreFloat(base, self + 488, reste);
      if (reste > zero) {
        techniques(sub_821C8D88);
        return;
      }
      const float dz = LoadFloat(base, reglages + 188) - LoadFloat(base, self + 344);
      const float dx = LoadFloat(base, reglages + 180) - LoadFloat(base, self + 336);
      const float portee = LoadFloat(base, reglages + 172);
      const float d = float(std::sqrt(double(FmaLikeGuest(dx, dx, dz * dz))));
      if (d > portee) StoreFloat(base, self + 488, d / portee - LoadFloat(base, kConstants + 64));
    }
  }
  const uint32_t etat = Load32(base, self + 284);
  if (etat == 0) {
    techniques(sub_821DD4F8);
    Store32(base, self + 284, 2);
    return bouger();
  }
  if (etat != 1) return bouger();
  if (base[self + 380]) {
    const int32_t images = int32_t(Load32(base, Load32(base, 0x835067D4) + 224) - Load32(base, self + 372));
    if (images > 1 || images < 0) {
      const float attente = float(double(LoadFloat(base, self + 368)) + dt);
      StoreFloat(base, self + 368, attente);
      if (!(attente < LoadFloat(base, self + 376))) return;
    }
  }
  const float avance = float(double(LoadFloat(base, self + 476)) * dt);
  const float age = LoadFloat(base, self + 384) + avance;
  StoreFloat(base, self + 384, age);
  if (base[self + 436] && !(age < LoadFloat(base, self + 432))) {
    ctx.r3.u64 = self;
    sub_82593348(ctx, base);
    return;
  }
  Store32(base, reglages + 192, Load32(base, reglages + 192) + 1);
  bool emet = false;
  for (uint32_t it = Load32(base, self + 348); it != Load32(base, self + 352); it += 4) {
    if (Load32(base, Load32(base, it) + 304)) {
      emet = true;
      break;
    }
  }
  if (emet) {
    if (Load32(base, self + 484)) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = 0;
      const uint32_t f = Slot(base, self, 212);
      TL_PROMU(f, 821C3A88);
      ctx.fpscr.disableFlushMode();
    }
    if (base[self + 424]) {
      const float cumul = avance + LoadFloat(base, self + 428);
      StoreFloat(base, self + 428, cumul);
      if (cumul < LoadFloat(base, self + 420)) return bouger();
      float pas = LoadFloat(base, self + 420);
      float suite;
      do {
        ctx.r3.u64 = self;
        ctx.f1.f64 = double(pas);
        sub_821C81F8(ctx, base);
        ctx.fpscr.disableFlushMode();
        pas = LoadFloat(base, self + 420);
        StoreFloat(base, self + 428, LoadFloat(base, self + 428) - pas);
        if (!base[self + 437]) {
          const float t = LoadFloat(base, self + 440);
          if (t > zero) StoreFloat(base, self + 440, t - pas);
          if (!(LoadFloat(base, self + 440) > zero)) StoreFloat(base, self + 440, LoadFloat(base, self + 444));
        }
        suite = LoadFloat(base, self + 420);
      } while (!(LoadFloat(base, self + 428) < suite));
      return bouger();
    }
    if (!base[self + 437]) {
      const float t = LoadFloat(base, self + 440);
      if (t > zero) StoreFloat(base, self + 440, t - avance);
    }
  }
  ctx.r3.u64 = self;
  ctx.f1.f64 = double(avance);
  sub_821C81F8(ctx, base);
  ctx.fpscr.disableFlushMode();
  if (!base[self + 437] && !(LoadFloat(base, self + 440) > zero)) {
    StoreFloat(base, self + 440, LoadFloat(base, self + 444));
  }
  for (uint32_t it = Load32(base, self + 348); it != Load32(base, self + 352); it += 4) {
    if (Load32(base, Load32(base, it) + 304) && !emet) StoreFloat(base, self + 440, zero);
  }
  bouger();
}

// sub_821C47F0 (r3 caméra) : Camera::updateView, sans réflexion (+852, laissée à l'original) : recopie la position et
// l'orientation du nœud (+40, cases 188 et 192) si elles ont changé et combine avec les valeurs locales.
REX_HOOK_RAW(sub_821C47F0) {
#pragma clang fp contract(off)
  const uint32_t self = ctx.r3.u32;
  if (!Pilote() || base[self + 852]) return __imp__sub_821C47F0(ctx, base);
  const uint32_t noeud = Load32(base, self + 40);
  if (!noeud) {
    std::memcpy(base + self + 1096, base + self + 1040, 28);
  } else {
    CadreInvite cadre(ctx, base, 256);
    const uint32_t sp = ctx.r1.u32;
    auto orientation = [&]() {
      const uint32_t n = Load32(base, self + 40);
      ctx.r3.u64 = n;
      GuestFunc(base, Slot(base, n, 188))(ctx, base);
      return ctx.r3.u32;
    };
    auto position = [&]() {
      const uint32_t n = Load32(base, self + 40);
      ctx.r3.u64 = n;
      GuestFunc(base, Slot(base, n, 192))(ctx, base);
      return ctx.r3.u32;
    };
    bool refaire = !base[self + 637];
    if (refaire) {
      const uint32_t q = orientation();
      ctx.fpscr.disableFlushMode();
      if (LoadFloat(base, self + 356) == LoadFloat(base, q + 4) && LoadFloat(base, self + 360) == LoadFloat(base, q + 8) &&
          LoadFloat(base, self + 364) == LoadFloat(base, q + 12) && LoadFloat(base, self + 352) == LoadFloat(base, q)) {
        const uint32_t p = position();
        ctx.fpscr.disableFlushMode();
        refaire = !(LoadFloat(base, self + 368) == LoadFloat(base, p) && LoadFloat(base, p + 4) == LoadFloat(base, self + 372) &&
                    LoadFloat(base, p + 8) == LoadFloat(base, self + 376));
      }
    } else {
      refaire = true;
    }
    if (refaire) {
      const uint32_t q = orientation();
      for (int i = 0; i < 4; i++) StoreFloat(base, self + 352 + 4 * i, LoadFloat(base, q + 4 * i));
      const uint32_t p = position();
      for (int i = 0; i < 3; i++) StoreFloat(base, self + 368 + 4 * i, LoadFloat(base, p + 4 * i));
      ctx.fpscr.disableFlushMode();
      const float qw = LoadFloat(base, self + 352), qx = LoadFloat(base, self + 356), qy = LoadFloat(base, self + 360),
                  qz = LoadFloat(base, self + 364);
      const float lw = LoadFloat(base, self + 1040), lx = LoadFloat(base, self + 1044), ly = LoadFloat(base, self + 1048),
                  lz = LoadFloat(base, self + 1052);
      const float f13 = qx * lx, f10 = qz * lw, f12 = qx * lw, f11 = qy * lw;
      const float f9 = FmsLikeGuest(lw, qw, f13);
      const float g13 = FmaLikeGuest(ly, qx, f10), f8 = FmaLikeGuest(lz, qy, f12), g5 = FmaLikeGuest(qz, lx, f11);
      const float g12 = FnmsLikeGuest(qy, ly, f9);
      const float g9 = FmaLikeGuest(lz, qw, g13), g11 = FmaLikeGuest(lx, qw, f8), g10 = FmaLikeGuest(ly, qw, g5);
      StoreFloat(base, self + 1096, FnmsLikeGuest(qz, lz, g12));
      StoreFloat(base, self + 1108, FnmsLikeGuest(qy, lx, g9));
      StoreFloat(base, self + 1100, FnmsLikeGuest(qz, ly, g11));
      StoreFloat(base, self + 1104, FnmsLikeGuest(lz, qx, g10));
      ctx.r3.u64 = sp + 112;
      ctx.r4.u64 = self + 352;
      ctx.r5.u64 = self + 1056;
      sub_821C9BF8(ctx, base);
      ctx.fpscr.disableFlushMode();
      const uint32_t v = ctx.r3.u32;
      for (int i = 0; i < 3; i++) {
        StoreFloat(base, self + 1112 + 4 * i, LoadFloat(base, v + 4 * i) + LoadFloat(base, self + 368 + 4 * i));
      }
      base[self + 637] = 1;
      base[self + 1212] = 1;
    }
  }
  if (base[self + 637]) std::memcpy(base + self + 1068, base + self + 1096, 28);
  ctx.r3.u64 = base[self + 637];
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, collisions et _setPass.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82225988);
REX_EXTERN(__imp__sub_821C5730);
REX_EXTERN(sub_821D2528);
REX_EXTERN(sub_821CF100);
REX_EXTERN(sub_821BF158);
REX_EXTERN(sub_821CB800);
REX_EXTERN(sub_821C14B8);
REX_EXTERN(sub_828AD748);
REX_EXTERN(sub_821D24D8);
REX_EXTERN(sub_821C52B0);
REX_EXTERN(sub_821C56A0);
REX_EXTERN(sub_821D2530);
REX_EXTERN(sub_821BD9E0);
REX_EXTERN(sub_821C93A0);
REX_EXTERN(sub_821BF850);
REX_EXTERN(sub_821C3010);
REX_EXTERN(sub_821C2F38);
REX_EXTERN(sub_821C4E88);
REX_EXTERN(sub_821D14B8);
REX_EXTERN(sub_821C54D8);
REX_EXTERN(sub_821C3520);
REX_EXTERN(sub_821C6CF8);
REX_EXTERN(sub_825BC790);
REX_EXTERN(sub_821C5228);
REX_EXTERN(sub_821BF160);
REX_EXTERN(sub_821BF120);
REX_EXTERN(sub_821C5F18);

namespace {
struct V3 {
  float x, y, z;
};
V3 LireV3(const uint8_t* base, uint32_t a) { return {LoadFloat(base, a), LoadFloat(base, a + 4), LoadFloat(base, a + 8)}; }
void EcrireV3(uint8_t* base, uint32_t a, const V3& v) {
  StoreFloat(base, a, v.x);
  StoreFloat(base, a + 4, v.y);
  StoreFloat(base, a + 8, v.z);
}

// sub_823A2EC8 : recouvrement de deux boîtes.
bool Recouvrement(const uint8_t* base, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  return !(LoadFloat(base, r6) < LoadFloat(base, r3)) && !(LoadFloat(base, r6 + 8) < LoadFloat(base, r3 + 8)) &&
         !(LoadFloat(base, r5) > LoadFloat(base, r4)) && !(LoadFloat(base, r5 + 8) > LoadFloat(base, r4 + 8)) &&
         !(LoadFloat(base, r6 + 4) < LoadFloat(base, r3 + 4)) && !(LoadFloat(base, r5 + 4) > LoadFloat(base, r4 + 4));
}

// sub_823A32E0 (rayon nul) et sub_823A3350 : côté d'un point par rapport au plan d'un triangle : 0 derrière, 1 devant, 2 dedans.
int CotePlan(const uint8_t* base, const V3& p, uint32_t sommet, uint32_t normale, float rayon, bool avec_rayon) {
#pragma clang fp contract(off)
  const V3 v = LireV3(base, sommet), n = LireV3(base, normale);
  const float dx = v.x - p.x, dy = v.y - p.y, dz = v.z - p.z;
  float d = FmaLikeGuest(dx, n.x, FmaLikeGuest(dz, n.z, dy * n.y));
  if (avec_rayon) d = d + rayon;
  if (d < LoadFloat(base, kConstants + 228)) return 0;
  return d > LoadFloat(base, kConstants + 232) ? 1 : 2;
}

// sub_823A34D8 : intersection du segment [a, b] et du plan (sommet, normale). Le jeu y garde ses intermédiaires dans des
// variables globales relues par aucune autre fonction : pas écrites ici.
bool SegmentPlan(const uint8_t* base, const V3& a, const V3& b, uint32_t sommet, const V3& n, V3& sortie) {
#pragma clang fp contract(off)
  const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
  const float denominateur = FmaLikeGuest(n.z, dz, FmaLikeGuest(n.x, dx, n.y * dy));
  if (std::fabs(denominateur) < LoadFloat(base, kConstants + 232)) return false;
  const V3 v = LireV3(base, sommet);
  const float vx = v.x - a.x, vy = v.y - a.y, vz = v.z - a.z;
  const float t = FmaLikeGuest(n.z, vz, FmaLikeGuest(n.x, vx, n.y * vy)) / denominateur;
  sortie = {FmaLikeGuest(t, dx, a.x), FmaLikeGuest(t, dy, a.y), FmaLikeGuest(t, dz, a.z)};
  return true;
}

// sub_822261C0 : point dans le triangle numéro tri du maillage (sommets +128, +144, +160 dans +64, normale +112 dans +96).
bool DansTriangle(const uint8_t* base, uint32_t maillage, const V3& p, uint32_t tri) {
#pragma clang fp contract(off)
  const uint32_t sommets = Load32(base, maillage + 64);
  const V3 v0 = LireV3(base, sommets + Load32(base, Load32(base, maillage + 128) + tri * 4) * 12);
  const V3 v1 = LireV3(base, sommets + Load32(base, Load32(base, maillage + 144) + tri * 4) * 12);
  const V3 v2 = LireV3(base, sommets + Load32(base, Load32(base, maillage + 160) + tri * 4) * 12);
  const V3 n = LireV3(base, Load32(base, maillage + 96) + uint32_t(Load16(base, Load32(base, maillage + 112) + tri * 2)) * 12);
  const float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
  const float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
  const float px = p.x - v0.x, py = p.y - v0.y, pz = p.z - v0.z;
  const float a = FmsLikeGuest(n.z, e2x, n.x * e2z), b = FmsLikeGuest(n.x, e2y, n.y * e2x), c = FmsLikeGuest(n.y, e2z, n.z * e2y);
  const float s = FmaLikeGuest(px, c, FmaLikeGuest(pz, b, py * a));
  const float d = FmaLikeGuest(c, e1x, FmaLikeGuest(b, e1z, a * e1y));
  const float zero = LoadFloat(base, kConstants + 68);
  if (s < zero || s > d) return false;
  const float f12 = FmsLikeGuest(pz, e1x, e1z * px), f9 = FmsLikeGuest(py, e1z, pz * e1y), f10 = FmsLikeGuest(px, e1y, py * e1x);
  const float t = FmaLikeGuest(n.x, f9, FmaLikeGuest(n.z, f10, n.y * f12));
  if (t < zero) return false;
  return !(t + s > d);
}

// sub_823A31F8 : voir PointSurSegment ; sub_823A30D0 : côtés du triangle.
void PlusProcheSurTriangle(const uint8_t* base, uint32_t a, uint32_t b, uint32_t c, uint32_t p, float s[3]) {
#pragma clang fp contract(off)
  const float px = LoadFloat(base, p), py = LoadFloat(base, p + 4), pz = LoadFloat(base, p + 8);
  float m[3];
  PointSurSegment(base, a, b, p, s);
  float dx = s[0] - px, dy = s[1] - py, dz = s[2] - pz;
  float meilleure = FmaLikeGuest(dy, dy, FmaLikeGuest(dz, dz, dx * dx));
  PointSurSegment(base, b, c, p, m);
  dx = m[0] - px, dy = m[1] - py, dz = m[2] - pz;
  float d = FmaLikeGuest(dx, dx, FmaLikeGuest(dz, dz, dy * dy));
  if (d < meilleure) {
    s[0] = m[0], s[1] = m[1], s[2] = m[2];
    meilleure = d;
  }
  PointSurSegment(base, c, a, p, m);
  dx = m[0] - px, dy = m[1] - py, dz = m[2] - pz;
  d = FmaLikeGuest(dx, dx, FmaLikeGuest(dz, dz, dy * dy));
  if (d < meilleure) s[0] = m[0], s[1] = m[1], s[2] = m[2];
}
}  // namespace

// sub_82225988 (r3 maillage, r4 liste de triangles, r5 a, r6 b, r7 et r8 boîte du déplacement, f1 rayon, r10 point de sortie,
// pile : +84 point de contact, +92 normale, +100 matériau, +108 couleur, +116 distance) : déplacement d'une sphère contre
// un maillage : le plus proche contact (face, sinon arête ou sommet). Rend 1 si la distance a été modifiée.
REX_HOOK_RAW(sub_82225988) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_82225988(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t self = ctx.r3.u32, liste = ctx.r4.u32, pa = ctx.r5.u32, pb = ctx.r6.u32;
  const uint32_t boite0 = ctx.r7.u32, boite1 = ctx.r8.u32, sortie = ctx.r10.u32;
  const float rayon = float(ctx.f1.f64);
  const uint32_t sp = ctx.r1.u32;
  const uint32_t contact = Load32(base, sp + 84), normale = Load32(base, sp + 92), matiere = Load32(base, sp + 100);
  const uint32_t couleur = Load32(base, sp + 108), distance = Load32(base, sp + 116);
  const float un = LoadFloat(base, kConstants + 64);
  float meilleur_angle = un;
  const uint32_t debut0 = Load32(base, liste);
  const V3 a = LireV3(base, pa), b = LireV3(base, pb);
  auto materiau_et_couleur = [&](uint32_t tri) {
    const uint32_t c = Load32(base, self + 80);
    const uint32_t n = uint32_t(int32_t(Load32(base, self + 84) - c) / 12);
    const uint32_t i0 = Load32(base, Load32(base, self + 128) + tri * 4);
    Store32(base, matiere, base[Load32(base, self + 176) + tri]);
    if (!(n > i0)) {
      StoreFloat(base, couleur, un);
      StoreFloat(base, couleur + 4, un);
      StoreFloat(base, couleur + 8, un);
      return;
    }
    const V3 c0 = LireV3(base, c + i0 * 12);
    const V3 c1 = LireV3(base, c + Load32(base, Load32(base, self + 144) + tri * 4) * 12);
    const V3 c2 = LireV3(base, c + Load32(base, Load32(base, self + 160) + tri * 4) * 12);
    V3 m = {(c0.x + c1.x) + c2.x, c2.y + (c0.y + c1.y), c2.z + (c0.z + c1.z)};
    const float tiers = LoadFloat(base, kConstants + 492);
    if (!(tiers == LoadFloat(base, kConstants + 68))) {  // sub_821DCF68
      const float inverse = un / tiers;
      m = {m.x * inverse, m.y * inverse, m.z * inverse};
    }
    EcrireV3(base, couleur, m);
  };
  if (Load32(base, liste + 4) != debut0 && (int32_t(Load32(base, liste + 4) - debut0) >> 2) != 0 &&
      Recouvrement(base, self + 20, self + 8, boite0, boite1)) {
    const uint32_t bas = Load32(base, self + 32), haut = Load32(base, self + 48);
    const uint32_t sommets = Load32(base, self + 64), normales = Load32(base, self + 96);
    const uint32_t indices_n = Load32(base, self + 112), indices_s = Load32(base, self + 128);
    uint32_t i = 0;
    do {
      const uint32_t debut = Load32(base, liste);
      if (base[Load32(base, self + 176) + i] == Load32(base, self + 192)) goto suivant;
      {
        const uint32_t tri = Load32(base, debut + i * 4);
        if (!Recouvrement(base, haut + tri * 12, bas + tri * 12, boite0, boite1)) goto suivant;
        const uint32_t adr_n = normales + uint32_t(Load16(base, indices_n + tri * 2)) * 12;
        const uint32_t adr_s = sommets + Load32(base, indices_s + tri * 4) * 12;
        const int cote_b = CotePlan(base, b, adr_s, adr_n, rayon, true);
        if (CotePlan(base, a, adr_s, adr_n, 0, false) == cote_b || cote_b == 0) goto suivant;
        const V3 n = LireV3(base, adr_n);
        const V3 b2 = {b.x - n.x * rayon, b.y - n.y * rayon, b.z - n.z * rayon};
        V3 impact;
        if (!SegmentPlan(base, a, b2, adr_s, n, impact)) goto suivant;
        if (DansTriangle(base, self, impact, tri)) {
          const float dx = impact.x - a.x, dy = impact.y - a.y, dz = impact.z - a.z;
          const float d = float(std::sqrt(double(FmaLikeGuest(dx, dx, FmaLikeGuest(dz, dz, dy * dy)))));
          if (d > LoadFloat(base, distance)) goto suivant;
          EcrireV3(base, contact, impact);
          StoreFloat(base, distance, d);
          EcrireV3(base, normale, n);
          EcrireV3(base, sortie, {impact.x + n.x * rayon, impact.y + n.y * rayon, n.z * rayon + impact.z});
          materiau_et_couleur(tri);
        } else {
          float proche[3];
          PlusProcheSurTriangle(base, sommets + Load32(base, indices_s + tri * 4) * 12,
                                sommets + Load32(base, Load32(base, self + 144) + tri * 4) * 12,
                                sommets + Load32(base, Load32(base, self + 160) + tri * 4) * 12, pa, proche);
          const float rx = b.x - proche[0], ry = b.y - proche[1], rz = b.z - proche[2];
          const float carre = FmaLikeGuest(rx, rx, FmaLikeGuest(rz, rz, ry * ry));
          const float d = float(std::sqrt(double(carre)));
          if (d > rayon || d > LoadFloat(base, distance)) goto suivant;
          // sub_82199360
          float nx = rx, ny = ry, nz = rz;
          const float longueur = float(std::sqrt(double(FmaLikeGuest(rz, rz, FmaLikeGuest(rx, rx, ry * ry)))));
          if (double(longueur) > LoadDouble(base, kConstants + 640)) {
            const float inverse = un / longueur;
            nx = rx * inverse, ny = ry * inverse, nz = rz * inverse;
          }
          // Normale du triangle numéro i, et non tri : c'est ce que fait le jeu.
          const V3 ni = LireV3(base, normales + uint32_t(Load16(base, indices_n + i * 2)) * 12);
          const float angle = FmaLikeGuest(nx, ni.x, FmaLikeGuest(ni.z, nz, ni.y * ny));
          if (!(angle < meilleur_angle)) goto suivant;
          if (CotePlan(base, b, adr_s, adr_n, 0, false) != 0) goto suivant;
          EcrireV3(base, normale, {nx, ny, nz});
          meilleur_angle = angle;
          EcrireV3(base, contact, {proche[0], proche[1], proche[2]});
          StoreFloat(base, distance, d);
          EcrireV3(base, sortie, {nx * rayon + proche[0], ny * rayon + proche[1], nz * rayon + proche[2]});
          materiau_et_couleur(tri);
        }
      }
    suivant:
      i++;
    } while (i < uint32_t(int32_t(Load32(base, liste + 4) - Load32(base, liste)) >> 2));
  }
  ctx.r3.u64 = LoadFloat(base, distance) == LoadFloat(base, kConstants + 624) ? 0 : 1;
}

// sub_821C5730 (r3 SceneManager, r4 passe, r5 même si supprimé) : _setPass : transmet au RenderSystem (+80) et aux paramètres
// automatiques (+17128) tous les états de la passe. Les unités de texture à ombres (+324 == 2, rares) restent à l'original.
REX_HOOK_RAW(sub_821C5730) {
  if (!Pilote()) return __imp__sub_821C5730(ctx, base);
  const uint32_t self = ctx.r3.u32, passe_0 = ctx.r4.u32, meme_si = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 304);
  const uint32_t sp = ctx.r1.u32;
  uint32_t passe = passe_0;
#define TL_M(obj, decalage, adresse)                           \
  do {                                                         \
    const uint32_t objet_ = (obj);                             \
    ctx.r3.u64 = objet_;                                       \
    const uint32_t f_ = Slot(base, objet_, (decalage));        \
    TL_PROMU(f_, adresse);                                     \
  } while (0)
#define TL_V(obj, decalage)                                    \
  do {                                                         \
    const uint32_t objet_ = (obj);                             \
    ctx.r3.u64 = objet_;                                       \
    GuestFunc(base, Slot(base, objet_, (decalage)))(ctx, base); \
  } while (0)
  TL_M(self, 764, 821D2528);
  if (ctx.r3.u32 & 0xFF) {
    ctx.r3.u64 = Load32(base, Load32(base, passe + 16) + 32);
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0;
    sub_821CF100(ctx, base);
    const uint32_t d = Load32(base, ctx.r3.u32);
    const uint32_t n = (uint32_t(int32_t(Load32(base, ctx.r3.u32 + 4) - d) >> 2)) & 0xFFFF;
    const uint32_t indice = Load16(base, passe + 20);
    if (n > indice) passe = Load32(base, d + indice * 4);
  }
  if (base[self + 17177] && !(meme_si & 0xFF)) {
    ctx.r3.u64 = passe;
    return;
  }
  const uint32_t unites0 = Load32(base, passe + 268), unites1 = Load32(base, passe + 272);
  for (uint32_t u = unites0; u != unites1; u += 4) {
    if (int32_t(Load32(base, Load32(base, u) + 324)) == 2) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = passe_0;
      ctx.r5.u64 = meme_si;
      ctx.r1.u64 = cadre.ancien;
      __imp__sub_821C5730(ctx, base);
      return;
    }
  }
  const uint32_t rs = Load32(base, self + 80), autos = Load32(base, self + 17128);
  ctx.r4.u64 = passe;
  TL_M(autos, 40, 821BF158);
  auto lier = [&](uint32_t programme) {
    TL_M(Load32(base, programme + 16), 204, 821CB800);
    ctx.r4.u64 = ctx.r3.u32;
    TL_M(self, 160, 821C14B8);
  };
  auto delier = [&](uint32_t type) {
    ctx.r4.u64 = type;
    TL_M(rs, 376, 821BF120);
    if (ctx.r3.u32 & 0xFF) {
      ctx.r4.u64 = type;
      TL_V(rs, 372);
    }
  };
  bool lumieres = true, brouillard = true;
  uint32_t p = Load32(base, passe + 284);
  if (p) {
    lier(p);
    TL_M(Load32(base, Load32(base, passe + 284) + 16), 268, 828AD748);
    lumieres = (ctx.r3.u32 & 0xFF) != 0;
  } else {
    delier(0);
  }
  p = Load32(base, passe + 304);
  if (p) {
    lier(p);
  } else {
    delier(2);
  }
  if (lumieres) {
    if (base[passe + 204]) {
      ctx.r4.u64 = passe + 64;
      ctx.r5.u64 = passe + 80;
      ctx.r6.u64 = passe + 96;
      ctx.r7.u64 = passe + 112;
      ctx.r9.u64 = Load32(base, passe + 132);
      ctx.f1.f64 = double(LoadFloat(base, passe + 128));
      TL_V(rs, 140);
    }
    ctx.r4.u64 = base[passe + 204];
    TL_V(rs, 56);
  }
  p = Load32(base, passe + 296);
  if (p) {
    lier(p);
    TL_M(Load32(base, Load32(base, passe + 296) + 16), 272, 828AD748);
    brouillard = (ctx.r3.u32 & 0xFF) != 0;
  } else {
    delier(1);
  }
  if (brouillard) {
    const uint32_t couleur = sp + 96;
    if (base[passe + 230]) {
      std::memcpy(base + couleur, base + passe + 240, 16);
      ctx.r4.u64 = Load32(base, passe + 232);
      ctx.f3.f64 = double(LoadFloat(base, passe + 260));
      ctx.f2.f64 = double(LoadFloat(base, passe + 256));
      ctx.f1.f64 = double(LoadFloat(base, passe + 264));
    } else {
      std::memcpy(base + couleur, base + self + 352, 16);
      ctx.f2.f64 = double(LoadFloat(base, self + 368));
      ctx.f3.f64 = double(LoadFloat(base, self + 372));
      ctx.r4.u64 = Load32(base, self + 344);
      ctx.f1.f64 = double(LoadFloat(base, self + 376));
    }
    ctx.r5.u64 = couleur;
    TL_V(rs, 276);
  }
  ctx.r4.u64 = Load32(base, self + 344);
  ctx.r5.u64 = self + 352;
  ctx.f3.f64 = double(LoadFloat(base, self + 372));
  ctx.f2.f64 = double(LoadFloat(base, self + 368));
  ctx.f1.f64 = double(LoadFloat(base, self + 376));
  TL_M(autos, 200, 821D24D8);
  const bool separe = base[passe + 152] != 0, alpha_separe = base[passe + 164] != 0;
  if (separe || alpha_separe) {
    const uint32_t src = Load32(base, passe + 136), dst = Load32(base, passe + 140);
    ctx.r4.u64 = src;
    ctx.r5.u64 = dst;
    ctx.r6.u64 = separe ? Load32(base, passe + 144) : src;
    ctx.r7.u64 = separe ? Load32(base, passe + 148) : dst;
    ctx.r8.u64 = Load32(base, passe + 156);
    ctx.r9.u64 = separe && alpha_separe ? Load32(base, passe + 156) : Load32(base, passe + 160);
    TL_V(rs, 208);
  } else {
    ctx.r4.u64 = Load32(base, passe + 136);
    ctx.r5.u64 = Load32(base, passe + 140);
    ctx.r6.u64 = Load32(base, passe + 156);
    TL_M(rs, 204, 821C52B0);
  }
  ctx.f6.f64 = double(LoadFloat(base, passe + 324));
  ctx.f5.f64 = double(LoadFloat(base, passe + 320));
  ctx.r5.u64 = base[passe + 329];
  ctx.f4.f64 = double(LoadFloat(base, passe + 340));
  ctx.f3.f64 = double(LoadFloat(base, passe + 336));
  ctx.f2.f64 = double(LoadFloat(base, passe + 332));
  ctx.f1.f64 = double(LoadFloat(base, passe + 316));
  TL_M(rs, 148, 821C56A0);
  if ((Load32(base, Load32(base, rs + 764) + 32) >> 22) & 1) {
    ctx.r4.u64 = base[passe + 328];
    TL_M(rs, 144, 821D2530);
  }
  uint32_t numero = 0;
  for (uint32_t u = unites0; u != unites1; u += 4, numero++) {
    const uint32_t unite = Load32(base, u);
    if (!Load32(base, self + 17144) && Load32(base, passe + 284)) {
      Store32(base, sp + 80, 1);
      ctx.r3.u64 = sp + 84;
      ctx.r4.u64 = unite + 420;
      ctx.r5.u64 = sp + 80;
      sub_821C5F18(ctx, base);
      const uint32_t it = Load32(base, sp + 84);
      if (it != Load32(base, unite + 424)) {
        ctx.r4.u64 = Load32(base, it + 56);
        ctx.r5.u64 = numero;
        TL_M(autos, 20, 821BD9E0);
      }
    }
    ctx.r4.u64 = numero;
    ctx.r5.u64 = unite;
    TL_M(rs, 128, 821C93A0);
  }
  ctx.r4.u64 = uint32_t(int32_t(Load32(base, passe + 272) - Load32(base, passe + 268)) >> 2) & 0xFFFF;
  TL_M(rs, 136, 821BF850);
  ctx.r4.u64 = Load32(base, passe + 168);
  TL_M(rs, 264, 821C3010);
  ctx.r4.u64 = base[passe + 165];
  TL_M(rs, 256, 821C2F38);
  ctx.r4.u64 = base[passe + 166];
  TL_M(rs, 260, 821C4E88);
  ctx.f2.f64 = double(LoadFloat(base, passe + 176));
  ctx.f1.f64 = double(LoadFloat(base, passe + 172));
  TL_M(rs, 272, 821D14B8);
  ctx.r6.u64 = base[passe + 193];
  ctx.r5.u64 = base[passe + 192];
  ctx.r4.u64 = Load32(base, passe + 188);
  TL_M(rs, 212, 821C54D8);
  const uint32_t ecriture = base[passe + 184];
  ctx.r4.u64 = ecriture;
  ctx.r5.u64 = ecriture;
  ctx.r6.u64 = ecriture;
  ctx.r7.u64 = ecriture;
  TL_M(rs, 268, 821C3520);
  const uint32_t culling = Load32(base, passe + 196);
  ctx.r4.u64 = culling;
  Store32(base, self + 16800, culling);
  TL_M(rs, 244, 821C6CF8);
  ctx.r4.u64 = Load32(base, passe + 220);
  TL_M(rs, 52, 825BC790);
  ctx.r4.u64 = Load32(base, passe + 224);
  TL_M(rs, 324, 821C5228);
  ctx.r4.u64 = Load16(base, passe + 20);
  TL_M(autos, 428, 821BF160);
  Store16(base, self + 17332, Load16(base, self + 17332) | 1);
  ctx.r3.u64 = passe;
}

REX_EXTERN(__imp__sub_821C5FA0);
REX_EXTERN(sub_828AD7C0);
REX_EXTERN(sub_821A7C98);
REX_EXTERN(sub_821CEE58);
REX_EXTERN(sub_821C07B8);
REX_EXTERN(sub_821C0820);
REX_EXTERN(sub_82204268);
REX_EXTERN(sub_821CEF78);
REX_EXTERN(sub_821CEF80);
REX_EXTERN(sub_821C4EE0);
REX_EXTERN(sub_821C12C0);
REX_EXTERN(sub_821CEF38);
REX_EXTERN(sub_821CEF20);
REX_EXTERN(sub_821C26D0);
REX_EXTERN(sub_821C4058);
REX_EXTERN(sub_821C0700);
REX_EXTERN(sub_8219D110);
REX_EXTERN(sub_8219CE00);

namespace {
// lower_bound exact d'une map à clé entière signée (sub_821C5F18 : _Isnil +61).
uint32_t ChercherEntier61(const uint8_t* base, uint32_t tete, int32_t cle) {
  uint32_t resultat = tete;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 61];) {
    if (int32_t(Load32(base, n + 12)) < cle) {
      n = Load32(base, n + 8);
    } else {
      resultat = n;
      n = Load32(base, n);
    }
  }
  return resultat != tete && !(cle < int32_t(Load32(base, resultat + 12))) ? resultat : tete;
}

// sub_821C0630 : une colonne de la matrice n'est pas de norme 1.
bool AEchelle(const uint8_t* base, uint32_t m) {
#pragma clang fp contract(off)
  const float un = LoadFloat(base, kConstants + 64), tolerance = LoadFloat(base, kConstants + 80);
  for (int c = 0; c < 3; c++) {
    const float a = LoadFloat(base, m + 16 + 4 * c), b = LoadFloat(base, m + 4 * c), d = LoadFloat(base, m + 32 + 4 * c);
    if (std::fabs(un - FmaLikeGuest(d, d, FmaLikeGuest(b, b, a * a))) > tolerance) return true;
  }
  return false;
}

// sub_8244EA10 : mineur 3x3 (lignes 1 à 3) d'une matrice 4x4.
float Mineur(const uint8_t* base, uint32_t m, uint32_t a, uint32_t b, uint32_t c) {
#pragma clang fp contract(off)
  auto e = [&](uint32_t l, uint32_t col) { return LoadFloat(base, m + (l * 4 + col) * 4); };
  const float f0 = e(2, c), f13 = e(3, a), f11 = e(3, c), f10 = e(3, b), f9 = e(2, a), f7 = e(2, b), f6 = e(1, b);
  const float f4 = FmsLikeGuest(f9, f11, f13 * f0);
  const float f2 = FmsLikeGuest(f11, f7, f0 * f10);
  const float g0 = FmsLikeGuest(f9, f10, f13 * f7);
  return FmaLikeGuest(g0, e(1, c), FmsLikeGuest(f2, e(1, a), f4 * f6));
}

float Determinant(const uint8_t* base, uint32_t m) {
#pragma clang fp contract(off)
  const float d1 = Mineur(base, m, 1, 2, 3), m0 = LoadFloat(base, m);
  const float d2 = Mineur(base, m, 0, 2, 3) * LoadFloat(base, m + 4);
  const float s = FmsLikeGuest(d1, m0, d2);
  const float t = FmaLikeGuest(Mineur(base, m, 0, 1, 3), LoadFloat(base, m + 8), s);
  return FnmsLikeGuest(Mineur(base, m, 0, 1, 2), LoadFloat(base, m + 12), t);
}
}  // namespace

// sub_821C5FA0 (r3 SceneManager, r4 renderable, r5 passe, r6 découpe, r7 lumières, r8 liste manuelle) : renderSingleObject,
// chemin courant : ni états supprimés (+17177), ni itération par lumière (+210), ni liste manuelle : ces cas restent à
// l'original. Le guard du vector statique des lumières non posé : première fois, aussi par l'original.
REX_HOOK_RAW(sub_821C5FA0) {
#pragma clang fp contract(off)
  const uint32_t self = ctx.r3.u32, rend = ctx.r4.u32, passe = ctx.r5.u32, decoupe = ctx.r6.u32;
  if (!Pilote() || base[self + 17177] || !(ctx.r7.u32 & 0xFF) || base[passe + 210] || !(Load32(base, 0x83583058) & 1)) {
    return __imp__sub_821C5FA0(ctx, base);
  }
  CadreInvite cadre(ctx, base, 320);
  const uint32_t rs = Load32(base, self + 80), autos = Load32(base, self + 17128);
  constexpr uint32_t kLumieres = 0x83582BB4;
  const uint32_t ro = ctx.r1.u32 + 112;
  Store32(base, ro, 0);
  Store32(base, ro + 4, 4);
  base[ro + 8] = 1;
  Store32(base, ro + 12, 0);
  Store32(base, ro + 16, 0);
  Store32(base, ro + 20, 1);
  ctx.r4.u64 = ro;
  TL_V(rend, 12);
  Store32(base, ro + 16, rend);
  const uint32_t pv = Load32(base, passe + 284);
  const uint32_t programme = pv ? Load32(base, pv + 16) : 0;
  uint32_t vue = 1;
  if (programme) {
    TL_M(programme, 276, 828AD748);
    vue = ctx.r3.u32;
  }
  {
    const uint32_t f = Slot(base, rend, 28);
    ctx.r3.u64 = rend;
    if (f == 0x828AD7C0u) {
      sub_828AD7C0(ctx, base);
    } else if (f == 0x821A7C98u) {
      sub_821A7C98(ctx, base);
    } else {
      GuestFunc(base, f)(ctx, base);
    }
  }
  const uint32_t transformations = ctx.r3.u32, nb = transformations & 0xFFFF;
  if (nb) {
    ctx.r4.u64 = self + 412;
    TL_V(rend, 24);
    if (base[self + 17240] && !base[rend + 26]) {
      ctx.fpscr.disableFlushMode();
      const float cx = LoadFloat(base, self + 17308), cy = LoadFloat(base, self + 17312), cz = LoadFloat(base, self + 17316);
      for (uint32_t i = 0; i < nb; i++) {
        const uint32_t m = self + i * 64;
        StoreFloat(base, m + 424, LoadFloat(base, m + 424) - cx);
        StoreFloat(base, m + 440, LoadFloat(base, m + 440) - cy);
        StoreFloat(base, m + 456, LoadFloat(base, m + 456) - cz);
      }
    }
    if (vue & 0xFF) {
      ctx.r4.u64 = self + 412;
      if (nb > 1) {
        ctx.r5.u64 = transformations;
        TL_V(rs, 116);
      } else {
        TL_V(rs, 112);
      }
    }
  }
  ctx.r4.u64 = rend;
  ctx.r5.u64 = vue;
  TL_M(self, 28, 821CEE58);
  Store16(base, self + 17332, Load16(base, self + 17332) | 2);
  uint32_t reperes = 1;
  if (Load32(base, passe + 284) || Load32(base, passe + 296) || Load32(base, passe + 304)) {
    ctx.r4.u64 = rend;
    TL_M(autos, 4, 821C07B8);
    ctx.r4.u64 = self + 412;
    ctx.r5.u64 = nb;
    TL_M(autos, 8, 821C0820);
    if (programme) {
      TL_M(programme, 268, 828AD748);
      reperes = ctx.r3.u32;
    }
  }
  {
    uint32_t numero = 0;
    const uint32_t fin = Load32(base, passe + 272);
    for (uint32_t u = Load32(base, passe + 268); u != fin; u += 4, numero++) {
      const uint32_t unite = Load32(base, u), tete = Load32(base, unite + 424);
      bool refaire = false;
      for (uint32_t it = ChercherEntier61(base, tete, 0); it != tete; it = TreeNext<61>(base, it)) {
        if (int32_t(Load32(base, it + 20)) == 2) {
          refaire = true;
          break;
        }
      }
      if (!refaire) refaire = ChercherEntier61(base, tete, 1) != tete;
      if (refaire) {
        ctx.r4.u64 = numero;
        ctx.r5.u64 = unite;
        TL_M(rs, 128, 821C93A0);
      }
    }
  }
  ctx.fpscr.disableFlushMode();
  ctx.r4.u64 = (base[passe + 228] || base[self + 16798]) && AEchelle(base, self + 412) ? 1 : 0;
  TL_M(rs, 344, 825BC790);
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68);
  if (base[self + 16799]) {
    uint32_t culling = Load32(base, self + 16800);
    if (Determinant(base, self + 412) < zero) {
      if (int32_t(culling) == 2) {
        culling = 3;
      } else if (int32_t(culling) == 3) {
        culling = 2;
      }
    }
    TL_M(rs, 248, 82204268);
    if (int32_t(culling) != ctx.r3.s32) {
      ctx.r4.u64 = culling;
      TL_V(rs, 244);
    }
  }
  uint32_t lignes = Load32(base, passe + 224);
  TL_M(passe, 8, 821CEF78);
  if (ctx.r3.u32 & 0xFF) {
    TL_M(rend, 52, 821CEF80);
    if (ctx.r3.u32 & 0xFF) {
      const uint32_t maximum = Load32(base, Load32(base, self + 164) + 1140);
      if (int32_t(lignes) > int32_t(maximum)) lignes = maximum;
    }
  }
  ctx.r4.u64 = lignes;
  TL_M(rs, 324, 821C5228);
  TL_V(rend, 36);
  uint32_t lumieres = ctx.r3.u32;
  const uint32_t premier = Load16(base, passe + 208), combien = Load16(base, passe + 206);
  bool faire = true;
  if (premier == 0 && combien == 8) {
    // liste complète
  } else if (premier != 0 && premier >= uint32_t(int32_t(Load32(base, lumieres + 4) - Load32(base, lumieres)) >> 2)) {
    faire = false;
  } else {
    ctx.r3.u64 = kLumieres;
    sub_8219D110(ctx, base);
    const uint32_t d = Load32(base, lumieres);
    const uint32_t reste = uint32_t(int32_t(Load32(base, lumieres + 4) - d) >> 2) - premier;
    const uint32_t debut = d + premier * 4;
    base[kLumieres + 20] = 1;
    ctx.r3.u64 = Load32(base, kLumieres);
    ctx.r4.u64 = debut;
    ctx.r5.u64 = debut + (reste < combien ? reste : combien) * 4;
    sub_8219CE00(ctx, base);
    base[kLumieres + 20] = 1;
    lumieres = kLumieres;
  }
  if (faire) {
    ctx.r4.u64 = rend;
    ctx.r5.u64 = passe;
    ctx.r6.u64 = autos;
    ctx.r7.u64 = lumieres;
    ctx.r8.u64 = base[self + 17177];
    TL_M(self, 52, 821C4EE0);
    if (Load32(base, passe + 284) || Load32(base, passe + 296) || Load32(base, passe + 304)) {
      ctx.r4.u64 = passe;
      ctx.r5.u64 = lumieres;
      TL_M(self, 156, 821C12C0);
    }
    if (base[passe + 204] && (reperes & 0xFF)) {
      ctx.r4.u64 = lumieres;
      ctx.r5.u64 = Load16(base, passe + 206);
      TL_V(self, 148);
    }
    int32_t ciseaux = 0, plans = 0;
    bool dessiner = true;
    if ((decoupe & 0xFF) && (base[passe + 361] || base[passe + 362])) {
      if (Load32(base, lumieres) == Load32(base, lumieres + 4)) {
        dessiner = false;
      } else {
        if (base[passe + 361]) {
          ctx.r4.u64 = lumieres;
          ctx.r5.u64 = Load32(base, self + 164);
          TL_V(self, 120);
          ciseaux = ctx.r3.s32;
        }
        if (base[passe + 362]) {
          ctx.r4.u64 = lumieres;
          TL_V(self, 132);
          plans = ctx.r3.s32;
        }
        if (ciseaux == 2 || plans == 2) dessiner = false;
      }
    }
    if (dessiner) {
      ctx.r4.u64 = Load32(base, passe + 312);
      TL_M(rs, 448, 821CEF38);
      ctx.fpscr.disableFlushMode();
      const float pente = LoadFloat(base, passe + 180);
      if (pente == zero) {
        ctx.r4.u64 = 0;
        ctx.f1.f64 = double(zero);
        ctx.f2.f64 = double(zero);
        ctx.f3.f64 = double(zero);
      } else {
        const float biais = FmaLikeGuest(0.0f, pente, LoadFloat(base, passe + 172));
        ctx.f1.f64 = double(biais);
        ctx.f2.f64 = double(LoadFloat(base, passe + 176));
        TL_V(rs, 272);
        ctx.r4.u64 = 1;
        ctx.f1.f64 = double(biais);
        ctx.f2.f64 = double(LoadFloat(base, passe + 180));
        ctx.f3.f64 = double(LoadFloat(base, passe + 176));
      }
      TL_M(rs, 452, 821CEF20);
      ctx.r4.u64 = passe;
      TL_M(self, 164, 821C26D0);
      ctx.r4.u64 = self;
      ctx.r5.u64 = rs;
      TL_M(rend, 16, 828AD7C0);
      if (ctx.r3.u32 & 0xFF) {
        ctx.r4.u64 = ro;
        TL_M(rs, 348, 821C4058);
      }
      ctx.r4.u64 = self;
      ctx.r5.u64 = rs;
      TL_M(rend, 20, 825BC790);
      if (ciseaux == 1) TL_V(self, 128);
      if (plans == 1) TL_V(self, 140);
    }
  }
  ctx.r4.u64 = vue;
  TL_M(self, 32, 821C0700);
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, unités, interface et chaînes de billboards.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821DC268);
REX_EXTERN(__imp__sub_821FF080);
REX_EXTERN(__imp__sub_821A4470);
REX_EXTERN(__imp__sub_821D2F80);
REX_EXTERN(__imp__sub_821DB5B8);
REX_EXTERN(__imp__sub_822815E8);
REX_EXTERN(__imp__sub_821C1768);
REX_EXTERN(sub_821D7E20);
REX_EXTERN(sub_821D7D38);
REX_EXTERN(sub_821D7CE0);
REX_EXTERN(sub_821DC600);
REX_EXTERN(sub_821BF088);
REX_EXTERN(sub_821BE948);
REX_EXTERN(sub_821C2CC8);
REX_EXTERN(sub_823391F8);
REX_EXTERN(sub_82339248);
REX_EXTERN(sub_8233E1C0);
REX_EXTERN(sub_821BE798);
REX_EXTERN(sub_821BEBE8);
REX_EXTERN(sub_822F25A8);
REX_EXTERN(sub_821B76B8);
REX_EXTERN(sub_8246BBF0);
REX_EXTERN(sub_8245F7A8);
REX_EXTERN(sub_82624758);
REX_EXTERN(sub_821CADB0);
REX_EXTERN(sub_821D2EF0);
REX_EXTERN(sub_821965E8);
REX_EXTERN(sub_8219EFC8);
REX_EXTERN(sub_821BDF30);
REX_EXTERN(sub_822260D0);
REX_EXTERN(sub_821C16A0);
REX_EXTERN(sub_821D2B98);
REX_EXTERN(sub_821DBCD8);

namespace {
uint64_t Load64(const uint8_t* base, uint32_t addr) {
  uint64_t v;
  std::memcpy(&v, base + addr, 8);
  return __builtin_bswap64(v);
}
}  // namespace

// sub_821DC268 (r3 unité, r4 position de la caméra) : visibilité (+353, +354), ombre (+1296) et niveau de détail (+1280) d'une
// unité d'après la distance et son état.
REX_HOOK_RAW(sub_821DC268) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821DC268(ctx, base);
  const uint32_t self = ctx.r3.u32, pos = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 144);
  bool toujours = ((~Load32(base, 0x8355AA10)) >> 1) & 1;
  ctx.r3.u64 = self;
  sub_821D7E20(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    toujours = true;
  } else if (base[self + 1690]) {
    toujours = true;
  } else {
    const uint32_t m = Load32(base, self + 1444);
    if ((m && base[m + 353]) || int32_t(Load32(base, self + 632)) == 5) toujours = true;
  }
  const uint32_t stats = Load32(base, self + 96);
  bool oui = false;
  if (stats && Load32(base, stats + 16)) {
    ctx.r3.u64 = Load32(base, stats + 16);
    ctx.r4.u64 = Load32(base, self + 372);
    ctx.r5.u64 = 128;
    sub_821D7D38(ctx, base);
    oui = (ctx.r3.u32 & 0xFF) != 0;
  }
  if (!oui) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = 170;
    sub_821D7CE0(ctx, base);
    oui = (ctx.r3.u32 & 0xFF) != 0;
  }
  if (oui) toujours = true;
  ctx.r3.u64 = self;
  sub_821DC600(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    toujours = true;
  } else {
    const uint32_t o = Load32(base, self + 388);
    if (o && Load32(base, o + 32)) toujours = true;
  }
  ctx.fpscr.disableFlushMode();
  const uint32_t r = Load32(base, 0x835594C4);
  const float zero = LoadFloat(base, kConstants + 68), c264 = LoadFloat(base, kConstants + 264);
  float echelle = LoadFloat(base, kConstants + 172);
  const float dx = LoadFloat(base, pos) - LoadFloat(base, self + 112), dz = LoadFloat(base, pos + 8) - LoadFloat(base, self + 120);
  float distance = float(std::sqrt(double(FmaLikeGuest(dz, dz, dx * dx))));
  bool visible = true;
  if (!toujours) {
    const uint32_t e = stats ? Load32(base, stats + 12) : 0;
    const uint32_t f = e ? Load32(base, e + 324) : 0;
    float portee = f && !base[f + 2227] ? LoadFloat(base, r + 196) : LoadFloat(base, r + 192);
    const uint32_t o = Load32(base, self + 380);
    echelle = o ? LoadFloat(base, o + 256) : zero;
    if (f) {
      const int32_t k = int32_t(Load32(base, self + 632));
      if (k != 5 && k != 6) echelle = LoadFloat(base, f + 156) + echelle;
    }
    if (!(echelle == zero)) portee = FmaLikeGuest(echelle, c264, LoadFloat(base, kConstants + 64)) * portee;
    if (distance > portee) {
      visible = false;
      base[self + 353] = 0;
      base[self + 354] = distance > LoadFloat(base, r + 184) ? 0 : 1;
    }
  }
  if (visible) {
    base[self + 354] = 1;
    base[self + 353] = 1;
  }
  bool ombre = false;
  if (base[self + 369]) {
    const uint32_t x = stats ? Load32(base, Load32(base, 0x835594CC) + 72) : 0;
    const uint32_t d = Load32(base, x + 108);
    const uint32_t n = uint32_t(int32_t(Load32(base, x + 112) - d) >> 2);
    const uint32_t i = Load32(base, 0x83558F1C);
    if (i < n && int32_t(Load32(base, d + i * 4)) > 0 && base[self + 353] && !(distance > LoadFloat(base, r + 180))) {
      const int32_t k = int32_t(Load32(base, self + 632));
      if (k != 12) {
        const float dy = std::fabs(LoadFloat(base, pos + 4) - LoadFloat(base, self + 116));
        ombre = !(dy > LoadFloat(base, kConstants + 492)) && k != 6;
      }
    }
  }
  base[self + 1296] = ombre ? 1 : 0;
  if (base[self + 353]) {
    if (!(echelle == zero)) distance = distance / (echelle * c264);
    Store32(base, self + 1280, 3);
    for (uint32_t i = 0; i < 4; i++) {
      if (!(distance > LoadFloat(base, 0x820AA1F0 + i * 4))) {
        Store32(base, self + 1280, i);
        break;
      }
    }
  }
  const uint32_t m = Load32(base, self + 1444);
  if (m) {
    const uint32_t s = Load32(base, m + 96);
    if (s && Load32(base, s + 16)) {
      ctx.r3.u64 = Load32(base, s + 16);
      ctx.r4.u64 = Load32(base, m + 372);
      ctx.r5.u64 = 28;
      sub_821D7D38(ctx, base);
      if (ctx.r3.u32 & 0xFF) Store32(base, self + 1280, 0);
    }
  }
}

// sub_821FF080 (r3, r4 caméra, r5 position, r6 forcer) : montre ou cache chaque élément (+444 : données, +448 taille, +452 capacité)
// d'après sa position à l'écran (sub_8233E1C0) et sa distance ; à l'apparition forcée, particules de l'élément avancées.
REX_HOOK_RAW(sub_821FF080) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821FF080(ctx, base);
  const uint32_t self = ctx.r3.u32, camera = ctx.r4.u32, pos = ctx.r5.u32;
  const bool forcer = (ctx.r6.u32 & 0xFF) != 0;
  if (!Load32(base, self + 448)) return;
  const uint32_t vue = Load32(base, Load32(base, self + 368) + 60);
  if (!vue) return;
  ctx.fpscr.disableFlushMode();
  constexpr uint32_t kDernier = 0x8358B8B8, kGarde = 0x8358B8C4;
  uint32_t garde = Load32(base, kGarde);
  float d0, d1, d2;
  if (!(garde & 1)) {
    d0 = d1 = LoadFloat(base, kConstants + 1036);
    d2 = LoadFloat(base, kConstants + 920);
    StoreFloat(base, kDernier, d0);
    Store32(base, kGarde, garde | 1);
    StoreFloat(base, kDernier + 4, d1);
    StoreFloat(base, kDernier + 8, d2);
  } else {
    d2 = LoadFloat(base, kDernier + 8);
    d1 = LoadFloat(base, kDernier + 4);
    d0 = LoadFloat(base, kDernier);
  }
  if (Load32(base, Load32(base, self + 368) + 44)) {
    if (!forcer) {
      const float a = LoadFloat(base, pos + 4) - d1, b = LoadFloat(base, pos + 8) - d2, c = LoadFloat(base, pos) - d0;
      if (float(std::sqrt(double(FmaLikeGuest(c, c, FmaLikeGuest(b, b, a * a))))) < LoadFloat(base, kConstants + 248)) return;
    }
    StoreFloat(base, kDernier, LoadFloat(base, pos));
    StoreFloat(base, kDernier + 4, LoadFloat(base, pos + 4));
    StoreFloat(base, kDernier + 8, LoadFloat(base, pos + 8));
  }
  CadreInvite cadre(ctx, base, 544);
  const uint32_t sp = ctx.r1.u32;
  ctx.r3.u64 = sp + 128;
  ctx.r4.u64 = camera + 1040;
  sub_821BF088(ctx, base);
  ctx.fpscr.disableFlushMode();
  StoreFloat(base, sp + 140, LoadFloat(base, camera + 1056));
  StoreFloat(base, sp + 156, LoadFloat(base, camera + 1060));
  StoreFloat(base, sp + 172, LoadFloat(base, camera + 1064));
  ctx.r3.u64 = sp + 304;
  ctx.r4.u64 = sp + 128;
  sub_821BE948(ctx, base);
  std::memcpy(base + sp + 128, base + ctx.r3.u32, 64);
  TL_V(camera, 332);
  std::memcpy(base + sp + 304, base + ctx.r3.u32, 64);
  ctx.r3.u64 = sp + 192;
  ctx.r4.u64 = sp + 304;
  ctx.r5.u64 = sp + 128;
  sub_821C2CC8(ctx, base);
  ctx.fpscr.disableFlushMode();
  {
    const float moitie = LoadFloat(base, kConstants + 124), un = LoadFloat(base, kConstants + 64);
    const float qx = LoadFloat(base, camera + 1044), qz = LoadFloat(base, camera + 1052), qy = LoadFloat(base, camera + 1048);
    const float qw = LoadFloat(base, camera + 1040);
    const float f10 = qx * moitie, f8 = qz * moitie, f5 = qy * moitie;
    StoreFloat(base, sp + 112, FmsLikeGuest(f5, qx, qw * f8));
    StoreFloat(base, sp + 120, FmaLikeGuest(qw, f10, qy * f8));
    StoreFloat(base, sp + 116, un - FmaLikeGuest(f8, qz, f10 * qx));
  }
  ctx.r3.u64 = vue;
  sub_823391F8(ctx, base);
  const float largeur = float(ctx.f1.f64);
  ctx.r3.u64 = vue;
  sub_82339248(ctx, base);
  const float hauteur = float(ctx.f1.f64);
  ctx.fpscr.disableFlushMode();
  float limite = LoadFloat(base, kConstants + 180);
  {
    const uint32_t g = Load32(base, Load32(base, 0x835594CC) + 72);
    const uint32_t i = Load32(base, 0x83558EEC), d = Load32(base, g + 108);
    if (i < uint32_t(int32_t(Load32(base, g + 112) - d) >> 2) && int32_t(Load32(base, d + i * 4)) == 1) {
      limite = LoadFloat(base, kConstants + 1104);
    }
  }
  const uint64_t m0 = Load64(base, sp + 192), m1 = Load64(base, sp + 200), m2 = Load64(base, sp + 208), m3 = Load64(base, sp + 216);
  const uint32_t liste = self + 444;
  auto element = [&](uint32_t i) {
    const uint32_t d = Load32(base, liste);
    return Load32(base, i < Load32(base, liste + 8) ? d + i * 4 : d);
  };
  for (uint32_t i = 0; i < Load32(base, self + 448); i++) {
    const uint32_t e0 = element(i);
    const uint32_t etat = Load32(base, e0 + 392);
    if (etat) {
      if (base[etat + 291]) continue;
      TL_V(e0, 32);
      if (!(ctx.r3.u32 & 0xFF)) continue;
    }
    ctx.r3.u64 = sp + 272;
    ctx.r4.u64 = element(i);
    ctx.r5.u64 = 1;
    sub_821D3F38(ctx, base);
    const uint32_t ou = ctx.r3.u32;
    std::memcpy(base + sp + 80, base + sp + 224, 32);
    ctx.r3.u64 = sp + 288;
    ctx.r4.u64 = vue;
    ctx.r5.u64 = ou;
    ctx.r6.u64 = sp + 112;
    ctx.r7.u64 = m0;
    ctx.r8.u64 = m1;
    ctx.r9.u64 = m2;
    ctx.r10.u64 = m3;
    sub_8233E1C0(ctx, base);
    const float ex = LoadFloat(base, ctx.r3.u32), ez = LoadFloat(base, ctx.r3.u32 + 8);
    ctx.r3.u64 = sp + 256;
    ctx.r4.u64 = element(i);
    ctx.r5.u64 = 1;
    sub_821D3F38(ctx, base);
    ctx.fpscr.disableFlushMode();
    const float ax = LoadFloat(base, ctx.r3.u32) - LoadFloat(base, pos), az = LoadFloat(base, ctx.r3.u32 + 8) - LoadFloat(base, pos + 8);
    const float distance = float(std::sqrt(double(FmaLikeGuest(az, az, ax * ax))));
    const bool hors_ecran = ex < largeur * LoadFloat(base, kConstants + 120) || ex > largeur * LoadFloat(base, kConstants + 196) ||
                            ez < LoadFloat(base, kConstants + 68) || ez > limite;
    if (hors_ecran && !(distance < hauteur * LoadFloat(base, kConstants + 716)) && !forcer) {
      const uint32_t e = element(i);
      if (base[e + 109] == 1) {
        ctx.r4.u64 = 0;
        TL_V(e, 36);
      }
      continue;
    }
    const uint32_t e = element(i);
    if (base[e + 109] != 0) continue;
    ctx.r4.u64 = 1;
    TL_V(e, 36);
    if (!forcer) continue;
    const uint32_t ee = element(i);
    const uint32_t s = Load32(base, ee + 392);
    if (!s) continue;
    const uint32_t x = Load32(base, s + 244);
    if (!x || !Load32(base, x + 12)) continue;
    for (uint32_t k = 0; k < Load32(base, Load32(base, s + 244) + 12); k++) {
      float reste = LoadFloat(base, kConstants + 64);
      do {
        const uint32_t t = Load32(base, s + 244);
        const uint32_t d = Load32(base, t + 8);
        ctx.r3.u64 = Load32(base, k < Load32(base, t + 16) ? d + k * 4 : d);
        ctx.f1.f64 = double(LoadFloat(base, kConstants + 108));
        sub_821C8618(ctx, base);
        ctx.fpscr.disableFlushMode();
        reste = reste - LoadFloat(base, kConstants + 108);
      } while (reste > LoadFloat(base, kConstants + 68));
    }
  }
}

// sub_821A4470 (r3, r4 matrice de la caméra) : unité sous le curseur ou la plus proche devant : deux passes de la liste (+96) :
// d'abord dans le cône (atan2 sub_821BEBE8), sinon tout près. Rayon de visibilité (sub_822F25A8) : sinon pénalité de distance.
REX_HOOK_RAW(sub_821A4470) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821A4470(ctx, base);
  const uint32_t self = ctx.r3.u32, mat = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 416);
  const uint32_t sp = ctx.r1.u32;
  ctx.r3.u64 = sp + 256;
  ctx.r4.u64 = mat;
  sub_821BE948(ctx, base);
  std::memcpy(base + sp + 192, base + ctx.r3.u32, 64);
  const float infini = LoadFloat(base, kConstants + 116);
  auto candidat = [&](uint32_t noeud) -> bool {
    const uint32_t u = Load32(base, noeud);
    if (!base[u + 412]) return false;
    ctx.r3.u64 = u;
    const uint32_t f = Slot(base, u, 32);
    TL_PROMU(f, 821B76B8);
    if (!(ctx.r3.u32 & 0xFF)) return false;
    const uint32_t w = Load32(base, noeud);
    const uint32_t s = Load32(base, w + 96);
    if (s && Load32(base, s + 16)) {
      ctx.r3.u64 = Load32(base, s + 16);
      ctx.r4.u64 = Load32(base, w + 372);
      ctx.r5.u64 = 29;
      sub_821D7D38(ctx, base);
      if (ctx.r3.u32 & 0xFF) return false;
    }
    return !base[w + 413] && base[w + 353];
  };
  auto dans_repere = [&](uint32_t w, uint32_t tmp1, uint32_t tmp2) -> uint32_t {
    ctx.r3.u64 = tmp1;
    ctx.r4.u64 = w;
    ctx.r5.u64 = 1;
    sub_821D3F38(ctx, base);
    ctx.r5.u64 = ctx.r3.u32;
    ctx.r3.u64 = tmp2;
    ctx.r4.u64 = sp + 192;
    sub_821BE798(ctx, base);
    ctx.fpscr.disableFlushMode();
    return ctx.r3.u32;
  };
  // Rayon de la caméra vers l'unité : sa hauteur + une marge. Vrai si dégagé.
  auto rayon = [&](uint32_t w, uint32_t a, uint32_t b, uint32_t p7, uint32_t p6) -> bool {
    const float x = LoadFloat(base, mat + 12), z = LoadFloat(base, mat + 44), y = LoadFloat(base, mat + 28);
    StoreFloat(base, a, x);
    StoreFloat(base, a + 8, z);
    ctx.r3.u64 = b;
    ctx.r4.u64 = w;
    ctx.r5.u64 = 1;
    sub_821D3F38(ctx, base);
    ctx.fpscr.disableFlushMode();
    const float marge = LoadFloat(base, kConstants + 420);
    StoreFloat(base, a + 4, y + marge);
    StoreFloat(base, b + 4, LoadFloat(base, b + 4) + marge);
    ctx.r3.u64 = self;
    ctx.r4.u64 = a;
    ctx.r5.u64 = b;
    ctx.r6.u64 = p6;
    ctx.r7.u64 = p7;
    ctx.r8.u64 = sp + 108;
    ctx.r9.u64 = sp + 160;
    ctx.r10.u64 = 1;
    sub_822F25A8(ctx, base);
    return (ctx.r3.u32 & 0xFF) != 0;
  };
  uint32_t meilleur = 0;
  float meilleure = infini;
  for (uint32_t n = Load32(base, Load32(base, self + 96)); n; n = Load32(base, n + 4)) {
    if (!candidat(n)) continue;
    const uint32_t v = dans_repere(Load32(base, n), sp + 176, sp + 112);
    const float vz = LoadFloat(base, v + 8), vx = LoadFloat(base, v);
    float d = float(std::sqrt(double(FmaLikeGuest(vx, vx, vz * vz))));
    ctx.f1.f64 = double(vx);
    ctx.f2.f64 = double(vz);
    sub_821BEBE8(ctx, base);
    ctx.fpscr.disableFlushMode();
    const float cone = LoadFloat(base, kConstants + 576), proche = LoadFloat(base, kConstants + 492);
    const float angle = float(ctx.f1.f64) * LoadFloat(base, kConstants + 636);
    bool retenu = !(std::fabs(angle) > cone) && !(d > proche);
    if (!(d > LoadFloat(base, kConstants + 104))) retenu = true;
    const uint32_t w = Load32(base, n);
    ctx.r3.u64 = w;
    ctx.r4.u64 = 122;
    sub_821D7CE0(ctx, base);
    ctx.fpscr.disableFlushMode();
    if (ctx.r3.u32 & 0xFF) d = d - LoadFloat(base, kConstants + 132);
    if (!(std::fabs(angle) > cone) && !(d > proche)) retenu = true;
    if (!retenu) continue;
    if (rayon(w, sp + 80, sp + 96, sp + 128, sp + 144)) d = d + LoadFloat(base, kConstants + 268);
    if (meilleure == infini || d < meilleure) {
      meilleur = Load32(base, n);
      meilleure = d;
    }
  }
  if (!meilleur) {
    meilleure = infini;
    for (uint32_t n = Load32(base, Load32(base, self + 96)); n; n = Load32(base, n + 4)) {
      if (!candidat(n)) continue;
      const uint32_t v = dans_repere(Load32(base, n), sp + 144, sp + 128);
      const float vz = LoadFloat(base, v + 8), vx = LoadFloat(base, v);
      float d = float(std::sqrt(double(FmaLikeGuest(vx, vx, vz * vz))));
      if (d > LoadFloat(base, kConstants + 124)) continue;
      if (rayon(Load32(base, n), sp + 96, sp + 80, sp + 112, sp + 176)) d = d + LoadFloat(base, kConstants + 268);
      if (meilleure == infini || d < meilleure) {
        meilleur = Load32(base, n);
        meilleure = d;
      }
    }
  }
  ctx.r3.u64 = meilleur;
}

// sub_821D2F80 (r3 rendu de l'interface) : doRender de CEGUI : tampon de sommets refait si besoin (6 sommets de 24 octets par
// quadrilatère de la map +204, _Isnil +89), puis un dessin par texture.
REX_HOOK_RAW(sub_821D2F80) {
  if (!Pilote()) return __imp__sub_821D2F80(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 160);
  const uint32_t rs = Load32(base, self + 228);
  TL_V(rs, 240);
  auto paire = [&](uint32_t x) { return x * 24 + self; };
  if (base[ctx.r3.u32 + 93] && Load32(base, self + 208)) {
    if (!base[self + 536]) {
      const uint32_t quads = Load32(base, self + 208), k = Load32(base, self + 332);
      base[self + 536] = 1;
      const uint32_t sommets = quads * 6;
      uint32_t capacite = Load32(base, Load32(base, self + (k + 19) * 16) + 32);
      auto refaire = [&](uint32_t nouvelle) {
        const uint32_t e = paire(k);
        const uint32_t ancien = Load32(base, e + 252);
        if (ancien) {
          ctx.r3.u64 = ancien;
          sub_8246BBF0(ctx, base);
          ctx.r3.u64 = ancien;
          sub_821CD9A8(ctx, base);
        }
        Store32(base, e + 252, 0);
        ctx.r3.u64 = k * 16 + self + 300;
        sub_8245F7A8(ctx, base);
        const uint32_t k2 = Load32(base, self + 332);
        ctx.r3.u64 = paire(k2) + 252;
        ctx.r4.u64 = k2 * 16 + self + 300;
        ctx.r5.u64 = nouvelle;
        sub_82624758(ctx, base);
      };
      if (capacite < sommets) {
        do {
          capacite <<= 1;
        } while (capacite < sommets);
        refaire(capacite);
      } else if (sommets < (capacite >> 1) && Load32(base, self + 336) >= 50000) {
        refaire(capacite >> 1);
        Store32(base, self + 336, 0);
      }
      const uint32_t tampon = Load32(base, self + (Load32(base, self + 332) + 19) * 16);
      ctx.r3.u64 = tampon;
      ctx.r4.u64 = 0;
      ctx.r5.u64 = Load32(base, tampon + 4);
      ctx.r6.u64 = 1;
      TL_V(tampon, 12);
      uint32_t o = ctx.r3.u32;
      const uint32_t tete = Load32(base, self + 204);
      const uint32_t z_fixe = Load32(base, kConstants + 920);
      auto mot = [&](uint32_t dst, uint32_t src) { std::memcpy(base + dst, base + src, 4); };
      for (uint32_t it = Load32(base, tete); it != tete; it = TreeNext<89>(base, it)) {
        const uint32_t n = it + 12;
        const bool plat = base[n + 36] != 0;
        const bool tourne = Load32(base, n + 72) != 0;
        auto sommet = [&](uint32_t x, uint32_t y, uint32_t couleur, uint32_t u, uint32_t v) {
          mot(o, n + x);
          mot(o + 4, n + y);
          if (plat) {
            Store32(base, o + 8, z_fixe);
          } else {
            mot(o + 8, n + 32);
          }
          mot(o + 12, n + couleur);
          mot(o + 16, n + u);
          mot(o + 20, n + v);
          o += 24;
        };
        sommet(24, 20, 56, 48, 44);
        if (!tourne) {
          sommet(28, 20, 60, 52, 44);
        } else {
          sommet(28, 16, 68, 52, 40);
        }
        sommet(24, 16, 64, 48, 40);
        sommet(28, 20, 60, 52, 44);
        sommet(28, 16, 68, 52, 40);
        if (!tourne) {
          sommet(24, 16, 64, 48, 40);
        } else {
          sommet(24, 20, 56, 48, 44);
        }
      }
      const uint32_t t2 = Load32(base, self + (Load32(base, self + 332) + 19) * 16);
      TL_V(t2, 20);
    }
    Store32(base, self + 532, 0);
    bool preparer = true, zecriture = false;
    const uint32_t tete = Load32(base, self + 204);
    uint32_t it = Load32(base, tete);
    while (it != tete) {
      if (preparer) {
        ctx.r3.u64 = self;
        sub_821CADB0(ctx, base);
        preparer = false;
      }
      const bool masque = base[it + 48] != 0;
      if (zecriture != masque) {
        const uint32_t v = masque ? 0 : 1;
        zecriture = masque;
        ctx.r4.u64 = v;
        ctx.r5.u64 = v;
        ctx.r6.u64 = v;
        ctx.r7.u64 = 1;
        TL_V(Load32(base, self + 228), 268);
      }
      ctx.r3.u64 = self + 236;
      ctx.r4.u64 = it + 12;
      sub_821D2EF0(ctx, base);
      const uint32_t o = Load32(base, paire(Load32(base, self + 332)) + 252);
      Store32(base, o + 16, Load32(base, self + 532));
      const uint32_t texture = Load32(base, self + 240);
      while (it != tete) {
        if (int32_t(texture) != int32_t(Load32(base, it + 16))) {
          preparer = true;
          break;
        }
        Store32(base, self + 532, Load32(base, self + 532) + 6);
        it = TreeNext<89>(base, it);
      }
      const uint32_t e = paire(Load32(base, self + 332)) + 252;
      const uint32_t operation = Load32(base, e);
      Store32(base, operation + 20, Load32(base, self + 532) - Load32(base, operation + 16));
      ctx.r4.u64 = 0;
      ctx.r5.u64 = 1;
      ctx.r6.u64 = self + 236;
      TL_M(Load32(base, self + 228), 156, 821C8E40);
      ctx.r4.u64 = paire(Load32(base, self + 332)) + 252;
      TL_M(Load32(base, self + 228), 348, 821C4058);
    }
  }
  const uint32_t tampon = Load32(base, self + (Load32(base, self + 332) + 19) * 16);
  if (Load32(base, self + 532) >= (Load32(base, tampon + 32) >> 1)) {
    Store32(base, self + 336, 0);
  } else {
    Store32(base, self + 336, Load32(base, self + 336) + 1);
  }
}

// sub_821DB5B8 (r3 chaîne, r4 caméra) : BillboardChain::updateVertexBuffer.
REX_HOOK_RAW(sub_821DB5B8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821DB5B8(ctx, base);
  const uint32_t self = ctx.r3.u32, camera = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 304);
  const uint32_t sp = ctx.r1.u32;
  TL_V(self, 304);
  ctx.r4.u64 = 0;
  TL_V(Load32(base, Load32(base, self + 232) + 8), 20);
  ctx.r4.u64 = ctx.r3.u32;
  ctx.r3.u64 = sp + 112;
  sub_821965E8(ctx, base);
  const uint32_t tampon = Load32(base, sp + 116);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = Load32(base, tampon + 4);
  ctx.r6.u64 = 1;
  TL_V(tampon, 12);
  const uint32_t verrou = ctx.r3.u32;
  TL_V(camera, 212);
  const uint32_t parent = Load32(base, self + 40);
  TL_V(parent, 192);
  ctx.fpscr.disableFlushMode();
  const uint32_t pp = ctx.r3.u32;
  StoreFloat(base, sp + 80, LoadFloat(base, camera + 1084) - LoadFloat(base, pp));
  StoreFloat(base, sp + 84, LoadFloat(base, camera + 1088) - LoadFloat(base, pp + 4));
  StoreFloat(base, sp + 88, LoadFloat(base, camera + 1092) - LoadFloat(base, pp + 8));
  TL_V(Load32(base, self + 40), 196);
  const uint32_t echelle = ctx.r3.u32;
  TL_V(Load32(base, self + 40), 188);
  ctx.r4.u64 = ctx.r3.u32;
  ctx.r3.u64 = sp + 128;
  sub_8219EFC8(ctx, base);
  ctx.r4.u64 = ctx.r3.u32;
  ctx.r3.u64 = sp + 96;
  ctx.r5.u64 = sp + 80;
  sub_821C9BF8(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t oeil_v = ctx.r3.u32;
  const float oeil_x = LoadFloat(base, oeil_v) / LoadFloat(base, echelle);
  const float oeil_y = LoadFloat(base, oeil_v + 4) / LoadFloat(base, echelle + 4);
  const float oeil_z = LoadFloat(base, oeil_v + 8) / LoadFloat(base, echelle + 8);
  const double seuil = LoadDouble(base, kConstants + 640);
  const float moitie = LoadFloat(base, kConstants + 104), un = LoadFloat(base, kConstants + 64);
  const uint32_t vide = Load32(base, 0x834A15E8);
  for (uint32_t seg = Load32(base, self + 352); seg != Load32(base, self + 356); seg += 12) {
    uint32_t tete = Load32(base, seg + 4);
    if (tete == vide || tete == Load32(base, seg + 8)) continue;
    uint32_t e_precedent = tete, e_courant = tete;
    for (;;) {
      const uint32_t maximum = Load32(base, self + 220), debut = Load32(base, seg);
      const uint32_t elements = Load32(base, self + 336);
      const uint32_t tete2 = Load32(base, seg + 4), queue = Load32(base, seg + 8);
      const uint32_t i = e_courant != maximum ? e_courant : 0;
      const uint32_t apres = i + 1;
      const uint32_t suivant = apres != maximum ? apres : 0;
      const uint32_t rang = debut + i;
      const uint32_t taille_sommet = Load32(base, Load32(base, sp + 116) + 36);
      uint32_t o = ((rang * 2) & 0xFFFE) * taille_sommet + verrou;
      const uint32_t e = rang * 48 + elements;
      float vx, vy, vz;
      const V3 c = LireV3(base, e);
      if (i == tete2) {
        const V3 n = LireV3(base, (debut + suivant) * 48 + elements);
        vx = n.x - c.x, vy = n.y - c.y, vz = n.z - c.z;
      } else if (i == queue) {
        const V3 p = LireV3(base, (debut + e_precedent) * 48 + elements);
        vx = c.x - p.x, vy = c.y - p.y, vz = c.z - p.z;
      } else {
        const V3 p = LireV3(base, (debut + e_precedent) * 48 + elements);
        const V3 n = LireV3(base, (debut + suivant) * 48 + elements);
        vx = n.x - p.x, vy = n.y - p.y, vz = n.z - p.z;
      }
      const float f9 = oeil_z - c.z, f7 = oeil_y - c.y, f6 = oeil_x - c.x;
      float nz = FmsLikeGuest(vz, f6, f9 * vx);
      float ny = FmsLikeGuest(f7, vx, vy * f6);
      float nx = FmsLikeGuest(f9, vy, f7 * vz);
      const float longueur = float(std::sqrt(double(FmaLikeGuest(nx, nx, FmaLikeGuest(ny, ny, nz * nz)))));
      if (double(longueur) > seuil) {
        const float inverse = un / longueur;
        nx = nx * inverse;
        nz = nz * inverse;
        ny = ny * inverse;
      }
      const float largeur = LoadFloat(base, e + 12) * moitie;
      const float ax = largeur * nx, az = nz * largeur, ay = ny * largeur;
      StoreFloat(base, o, c.x - ax);
      StoreFloat(base, o + 4, c.y - az);
      StoreFloat(base, o + 8, c.z - ay);
      o += 12;
      auto couleur_et_uv = [&](uint32_t decalage_v) {
        if (base[self + 229]) {
          const uint32_t systeme = Load32(base, Load32(base, 0x835067D4) + 20);
          ctx.r4.u64 = e + 32;
          ctx.r5.u64 = o;
          TL_V(systeme, 296);
          ctx.fpscr.disableFlushMode();
          o += 4;
        }
        if (base[self + 228]) {
          if (int32_t(Load32(base, self + 324)) == 0) {
            StoreFloat(base, o, LoadFloat(base, e + 16));
            StoreFloat(base, o + 4, LoadFloat(base, self + decalage_v));
          } else {
            StoreFloat(base, o, LoadFloat(base, self + decalage_v));
            StoreFloat(base, o + 4, LoadFloat(base, e + 16));
          }
          o += 8;
        }
      };
      couleur_et_uv(328);
      StoreFloat(base, o, c.x + ax);
      StoreFloat(base, o + 4, c.y + az);
      StoreFloat(base, o + 8, c.z + ay);
      o += 12;
      couleur_et_uv(332);
      if (i == Load32(base, seg + 8)) break;
      e_precedent = i;
      e_courant = apres;
    }
  }
  TL_V(Load32(base, sp + 116), 20);
  Store32(base, sp + 112, 0x820AE2C0);
  const uint32_t compteur = Load32(base, sp + 120);
  if (compteur) {
    const uint32_t n = Load32(base, compteur) - 1;
    Store32(base, compteur, n);
    if (!n) {
      ctx.r3.u64 = sp + 112;
      GuestFunc(base, Slot(base, sp + 112, 4))(ctx, base);
    }
  }
}

// sub_822815E8 (r3 objet, r4 a, r5 b, f1 rayon, r7, r8, r9 sorties) : déplacement d'une sphère contre le maillage d'un objet (boîte,
// puis passage dans son repère, sub_822260D0, retour).
REX_HOOK_RAW(sub_822815E8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_822815E8(ctx, base);
  const uint32_t self = ctx.r3.u32, pa = ctx.r4.u32, pb = ctx.r5.u32;
  const uint32_t s7 = ctx.r7.u32, s8 = ctx.r8.u32, s9 = ctx.r9.u32;
  const float rayon = float(ctx.f1.f64);
  CadreInvite cadre(ctx, base, 384);
  const uint32_t sp = ctx.r1.u32;
  TL_V(self, 240);
  if (!ctx.r3.u32 || !base[self + 341] || !base[self + 356]) {
    ctx.r3.u64 = 0;
    return;
  }
  std::memcpy(base + sp + 112, base + pa, 12);
  std::memcpy(base + sp + 96, base + pa, 12);
  ctx.fpscr.disableFlushMode();
  StoreFloat(base, sp + 124, LoadFloat(base, kConstants + 624));
  ctx.r3.u64 = sp + 112;
  ctx.r4.u64 = sp + 96;
  ctx.r5.u64 = Load64(base, pb);
  ctx.r6.u64 = uint64_t(Load32(base, pb + 8)) << 32;
  sub_821BDF30(ctx, base);
  ctx.fpscr.disableFlushMode();
  const float marge = rayon * LoadFloat(base, kConstants + 124);
  for (uint32_t i = 0; i < 3; i++) {
    StoreFloat(base, sp + 112 + i * 4, LoadFloat(base, sp + 112 + i * 4) - marge);
    StoreFloat(base, sp + 96 + i * 4, marge + LoadFloat(base, sp + 96 + i * 4));
  }
  TL_V(self, 268);
  const uint32_t haut = ctx.r3.u32;
  TL_V(self, 264);
  ctx.fpscr.disableFlushMode();
  if (!Recouvrement(base, ctx.r3.u32, haut, sp + 112, sp + 96)) {
    ctx.r3.u64 = 0;
    return;
  }
  TL_V(Load32(base, self + 88), 188);
  ctx.r4.u64 = ctx.r3.u32;
  ctx.r3.u64 = sp + 160;
  sub_821BF088(ctx, base);
  ctx.r3.u64 = sp + 224;
  ctx.r4.u64 = sp + 160;
  sub_821BE948(ctx, base);
  TL_V(self, 236);
  uint32_t decalage = 0x8355A3BC;
  if (ctx.r3.u32) {
    TL_V(self, 236);
    ctx.r4.u64 = ctx.r3.u32;
    ctx.r3.u64 = sp + 96;
    ctx.r5.u64 = 0;
    sub_821D3F38(ctx, base);
    decalage = ctx.r3.u32;
  }
  const V3 d = LireV3(base, decalage);
  ctx.r3.u64 = sp + 128;
  ctx.r4.u64 = self;
  ctx.r5.u64 = 1;
  sub_821D3F38(ctx, base);
  ctx.fpscr.disableFlushMode();
  const V3 p = LireV3(base, ctx.r3.u32);
  const float ox = p.x + d.x, oy = p.y + d.y, oz = p.z + d.z;
  const V3 a = LireV3(base, pa), b = LireV3(base, pb);
  EcrireV3(base, sp + 112, {a.x - ox, a.y - oy, a.z - oz});
  EcrireV3(base, sp + 96, {b.x - ox, b.y - oy, b.z - oz});
  ctx.r3.u64 = sp + 144;
  ctx.r4.u64 = sp + 224;
  ctx.r5.u64 = sp + 112;
  sub_821BE798(ctx, base);
  std::memcpy(base + sp + 112, base + ctx.r3.u32, 12);
  ctx.r3.u64 = sp + 128;
  ctx.r4.u64 = sp + 224;
  ctx.r5.u64 = sp + 96;
  sub_821BE798(ctx, base);
  std::memcpy(base + sp + 96, base + ctx.r3.u32, 12);
  TL_V(self, 240);
  const uint32_t maillage = Load32(base, ctx.r3.u32 + 88);
  Store32(base, sp + 84, sp + 124);
  ctx.r6.u64 = sp + 124;
  ctx.r3.u64 = maillage;
  ctx.r4.u64 = sp + 112;
  ctx.r5.u64 = sp + 96;
  ctx.r7.u64 = s7;
  ctx.r8.u64 = s8;
  ctx.r9.u64 = s9;
  ctx.r10.u64 = sp + 156;
  ctx.f1.f64 = double(rayon);
  sub_822260D0(ctx, base);
  if (!(ctx.r3.u32 & 0xFF)) {
    ctx.r3.u64 = 0;
    return;
  }
  auto retour = [&](uint32_t s, uint32_t tmp) {
    ctx.r3.u64 = tmp;
    ctx.r4.u64 = sp + 160;
    ctx.r5.u64 = s;
    sub_821BE798(ctx, base);
    std::memcpy(base + s, base + ctx.r3.u32, 12);
  };
  retour(s8, sp + 144);
  retour(s9, sp + 128);
  retour(s7, sp + 144);
  ctx.fpscr.disableFlushMode();
  EcrireV3(base, s8, {LoadFloat(base, s8) + ox, LoadFloat(base, s8 + 4) + oy, oz + LoadFloat(base, s8 + 8)});
  EcrireV3(base, s7, {LoadFloat(base, s7) + ox, LoadFloat(base, s7 + 4) + oy, LoadFloat(base, s7 + 8) + oz});
  ctx.r3.u64 = 1;
}

namespace {
// Constantes d'un programme : o = paramètre (+72 tampon, +76 décalage).
void CopierConstantes(uint8_t* base, uint32_t o, const void* source, uint32_t octets) {
  const uint32_t t = Load32(base, o + 72);
  if (t) std::memcpy(base + (Load32(base, t) + (Load32(base, o + 76) << 2)), source, octets);
}
}  // namespace

// sub_821C1768 (r3 tableau de paramètres, r5 passe, r6 source, r7 lumières) : paramètres des lumières (entrées de 100 octets de
// +48..+52) : position et direction en repère de vue, atténuation, cône, couleurs.
REX_HOOK_RAW(sub_821C1768) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821C1768(ctx, base);
  const uint32_t self = ctx.r3.u32, passe = ctx.r5.u32, source = ctx.r6.u32, liste = ctx.r7.u32;
  const uint32_t table = self + 48;
  if (!(int32_t(Load32(base, table + 4) - Load32(base, table)) / 100)) return;
  CadreInvite cadre(ctx, base, 512);
  const uint32_t sp = ctx.r1.u32;
  {
    ctx.r3.u64 = source;
    const uint32_t f = Slot(base, source, 56);
    TL_PROMU(f, 821C16A0);
  }
  const uint32_t m = ctx.r3.u32;  // matrice de vue-monde
  int32_t genre = 1;
  uint32_t suivant = 0;
  auto ecrire = [&](uint32_t o, const float v[4], uint32_t n) {
    uint32_t mots[4];
    for (uint32_t i = 0; i < n; i++) mots[i] = __builtin_bswap32(FloatBits(v[i]));
    CopierConstantes(base, o, mots, n * 4);
  };
  for (uint32_t i = 0; i < uint32_t(int32_t(Load32(base, table + 4) - Load32(base, table)) / 100); i++) {
    const uint32_t e = Load32(base, table) + i * 100;
    const int32_t g = int32_t(Load32(base, e));
    if (genre != g) {
      genre = g;
      suivant = 0;
    }
    uint32_t lumiere = 0x8356C810;
    {
      const uint32_t d = Load32(base, liste);
      const uint32_t n = uint32_t(int32_t(Load32(base, liste + 4) - d) >> 2);
      for (uint32_t k = suivant; k < n; k++) {
        const uint32_t l = Load32(base, d + k * 4);
        if (int32_t(Load32(base, l + 176)) == genre) {
          suivant = k + 1;
          if (l) lumiere = l;
          break;
        }
      }
    }
    const uint32_t type = Load32(base, e);
    auto position = [&](uint32_t tmp, float v[4]) {
      ctx.r3.u64 = tmp;
      ctx.r4.u64 = lumiere;
      ctx.r5.u64 = 1;
      sub_821D2B98(ctx, base);
      ctx.fpscr.disableFlushMode();
      const uint32_t p = ctx.r3.u32;
      const float x = LoadFloat(base, p), y = LoadFloat(base, p + 4), z = LoadFloat(base, p + 8), w = LoadFloat(base, p + 12);
      auto mm = [&](uint32_t k) { return LoadFloat(base, m + k * 4); };
      v[0] = FmaLikeGuest(mm(0), x, FmaLikeGuest(mm(3), w, FmaLikeGuest(mm(1), y, mm(2) * z)));
      v[1] = FmaLikeGuest(mm(7), w, FmaLikeGuest(mm(6), z, FmaLikeGuest(mm(4), x, mm(5) * y)));
      v[2] = FmaLikeGuest(mm(11), w, FmaLikeGuest(mm(10), z, FmaLikeGuest(mm(8), x, mm(9) * y)));
      v[3] = w;
    };
    auto diffuse = [&](uint32_t o) {
      float v[4];
      for (uint32_t k = 0; k < 4; k++) v[k] = LoadFloat(base, lumiere + 248 + k * 4);
      ecrire(o, v, 4);
    };
    float v[4];
    if (type == 0) {
      position(sp + 176, v);
      ecrire(Load32(base, e + 8), v, 4);
      diffuse(Load32(base, e + 40));
    } else if (type == 1) {
      position(sp + 160, v);
      ecrire(Load32(base, e + 24), v, 4);
    } else if (type == 2) {
      ctx.r3.u64 = sp + 208;
      ctx.r4.u64 = source;
      GuestFunc(base, Slot(base, source, 276))(ctx, base);
      const uint32_t q = ctx.r3.u32;
      float mv[12];
      for (uint32_t k = 0; k < 12; k++) mv[k] = LoadFloat(base, q + k * 4);
      position(sp + 192, v);
      ecrire(Load32(base, e + 8), v, 4);
      TL_V(lumiere, 216);
      ctx.fpscr.disableFlushMode();
      const float dx = LoadFloat(base, lumiere + 284), dy = LoadFloat(base, lumiere + 288), dz = LoadFloat(base, lumiere + 292);
      const float z = FmaLikeGuest(mv[10], dz, FmaLikeGuest(mv[8], dx, mv[9] * dy));
      float x = FmaLikeGuest(mv[2], dz, FmaLikeGuest(dx, mv[0], mv[1] * dy));
      float y = FmaLikeGuest(mv[6], dz, FmaLikeGuest(mv[4], dx, mv[5] * dy));
      float zz = z;
      const float l = float(std::sqrt(double(FmaLikeGuest(y, y, FmaLikeGuest(x, x, z * z)))));
      if (double(l) > LoadDouble(base, kConstants + 640)) {
        const float inverse = LoadFloat(base, kConstants + 64) / l;
        x = x * inverse;
        y = y * inverse;
        zz = zz * inverse;
      }
      v[0] = -x;
      v[1] = -y;
      v[2] = -zz;
      v[3] = LoadFloat(base, kConstants + 68);
      ecrire(Load32(base, e + 24), v, 4);
      diffuse(Load32(base, e + 40));
      const float demi = LoadFloat(base, kConstants + 104);
      ctx.f1.f64 = double(LoadFloat(base, lumiere + 236) * demi);
      sub_821DBCD8(ctx, base);
      const float c1 = float(ctx.f1.f64);
      ctx.fpscr.disableFlushMode();
      ctx.f1.f64 = double(LoadFloat(base, lumiere + 240) * demi);
      sub_821DBCD8(ctx, base);
      v[1] = c1;
      v[0] = float(ctx.f1.f64);
      ctx.fpscr.disableFlushMode();
      v[2] = LoadFloat(base, lumiere + 244);
      ecrire(Load32(base, e + 56), v, 3);
    }
    ctx.fpscr.disableFlushMode();
    const uint32_t drapeaux = Load32(base, self + 40);
    if (!(drapeaux & 2)) {
      for (uint32_t k = 0; k < 4; k++) v[k] = LoadFloat(base, lumiere + 192 + k * 4) * LoadFloat(base, passe + 80 + k * 4);
      ecrire(Load32(base, e + 72), v, 4);
    } else {
      CopierConstantes(base, Load32(base, e + 72), base + lumiere + 192, 16);
    }
    if (base[self + 44]) {
      if (!(Load32(base, self + 40) & 4)) {
        for (uint32_t k = 0; k < 4; k++) v[k] = LoadFloat(base, passe + 96 + k * 4) * LoadFloat(base, lumiere + 208 + k * 4);
        ecrire(Load32(base, e + 88), v, 4);
      } else {
        CopierConstantes(base, Load32(base, e + 88), base + lumiere + 208, 16);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, mise à jour d'une unité.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821D3FD8);
REX_EXTERN(sub_821BAF08);
REX_EXTERN(sub_821DF998);
REX_EXTERN(sub_821F0210);
REX_EXTERN(sub_8228E580);
REX_EXTERN(sub_821DFF18);
REX_EXTERN(sub_8219C070);
REX_EXTERN(sub_821DC660);
REX_EXTERN(sub_821D5380);
REX_EXTERN(sub_821AF868);
REX_EXTERN(sub_82288068);
REX_EXTERN(sub_822919A8);
REX_EXTERN(sub_8231B6F0);
REX_EXTERN(sub_821AC8F8);
REX_EXTERN(sub_821F1E38);
REX_EXTERN(sub_821D8A50);
REX_EXTERN(sub_821D8F28);
REX_EXTERN(sub_821E2618);
REX_EXTERN(sub_821E20C0);
REX_EXTERN(sub_821A6BE0);
REX_EXTERN(sub_823D8C18);
REX_EXTERN(sub_822A1538);
REX_EXTERN(sub_821E4588);
REX_EXTERN(sub_8219FD08);
REX_EXTERN(sub_8219A258);
REX_EXTERN(sub_821E1D58);
REX_EXTERN(sub_821B60D8);
REX_EXTERN(sub_821D52A8);
REX_EXTERN(sub_821A68B8);

// sub_821D3FD8 (r3 unité, r4 caméra, r5 position de la caméra, f1 dt) : Unit::update.
REX_HOOK_RAW(sub_821D3FD8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821D3FD8(ctx, base);
  const uint32_t self = ctx.r3.u32, camera = ctx.r4.u32, vue = ctx.r5.u32;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 304);
  const uint32_t sp = ctx.r1.u32;
  auto stats = [&]() -> uint32_t {
    const uint32_t s = Load32(base, self + 96);
    return s ? Load32(base, s + 12) : 0;
  };
  auto avec_dt = [&](PPCFunc* f) {
    ctx.r3.u64 = self;
    ctx.f1.f64 = dt;
    f(ctx, base);
  };
  auto octet = [&](PPCFunc* f) -> bool {
    ctx.r3.u64 = self;
    f(ctx, base);
    return (ctx.r3.u32 & 0xFF) != 0;
  };
  auto etat_a = [&](uint32_t u, uint32_t numero) -> bool {
    ctx.r3.u64 = u;
    ctx.r4.u64 = numero;
    sub_821D7CE0(ctx, base);
    return (ctx.r3.u32 & 0xFF) != 0;
  };
  auto position = [&](uint32_t u) -> uint32_t {
    ctx.r3.u64 = sp + 104;
    ctx.r4.u64 = u;
    ctx.r5.u64 = 1;
    sub_821D3F38(ctx, base);
    ctx.fpscr.disableFlushMode();
    return ctx.r3.u32;
  };
  auto vivant = [&](uint32_t u) {
    const int32_t k = int32_t(Load32(base, u + 632));
    return k != 5 && k != 6;
  };
  auto cacher_tout = [&]() {
    Store32(base, self + 1256, Load32(base, self + 1252));
    const float loin = LoadFloat(base, kConstants + 1036);
    StoreFloat(base, self + 1268, loin);
    StoreFloat(base, self + 1272, loin);
    StoreFloat(base, self + 1276, loin);
  };
  auto notifier = [&](uint32_t decalage) {
    ctx.r4.u64 = stats();
    TL_V(self, decalage);
  };
  auto retirer_selection = [&]() {
    ctx.r3.u64 = self;
    ctx.r4.u64 = 0;
    sub_8219C070(ctx, base);
  };
  auto chrono = [&](uint32_t a, float maximum) {
    const float v = float(double(LoadFloat(base, a)) + dt);
    StoreFloat(base, a, v);
    if (v > maximum) StoreFloat(base, a, maximum);
  };
  auto boite_vue = [&]() -> bool {
    const uint32_t o = Load32(base, self + 384);
    ctx.r3.u64 = sp + 128;
    ctx.r4.u64 = o + 32;
    ctx.r5.u64 = o + 44;
    sub_821DC660(ctx, base);
    return true;
  };
  auto boite_visible = [&]() -> bool {
    const uint32_t c = Load32(base, camera + 1224);
    ctx.r3.u64 = c ? c : camera;
    ctx.r4.u64 = sp + 128;
    sub_821C74A0(ctx, base);
    return ctx.r3.u32 != 0;
  };
  auto cacher_ou_non = [&](uint32_t change) {
    if (!base[self + 1605]) {
      ctx.r4.u64 = (change & 0xFF) == 0 ? 1 : 0;
      TL_V(self, 36);
    }
  };
  auto retirer = [&]() {
    ctx.r3.u64 = self;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 1;
    sub_821F0210(ctx, base);
  };

  const uint32_t a = Load32(base, self + 548);
  if (a && Load32(base, self + 552)) {
    ctx.r3.u64 = a;
    ctx.r5.u64 = Load32(base, self + 88);
    ctx.f1.f64 = dt;
    sub_821BAF08(ctx, base);
    ctx.r3.u64 = Load32(base, self + 552);
    ctx.r5.u64 = Load32(base, self + 88);
    ctx.f1.f64 = dt;
    sub_821BAF08(ctx, base);
  }
  ctx.r5.u64 = 0;
  avec_dt(sub_821DF998);
  ctx.fpscr.disableFlushMode();
  StoreFloat(base, self + 700, float(double(LoadFloat(base, self + 700)) + dt));
  const uint32_t monde = stats();
  if (!base[self + 352]) {
    if (base[self + 355]) {
      ctx.r4.u64 = monde;
      TL_V(self, 296);
    }
    ctx.fpscr.disableFlushMode();
    cacher_tout();
    return;
  }
  if (monde && int32_t(Load32(base, self + 632)) == 42 &&
      int32_t(Load32(base, Load32(base, monde + 368) + 5124)) == 1) {
    retirer();
    ctx.r4.u64 = monde;
    TL_V(self, 296);
    ctx.r5.u64 = monde;
    avec_dt(sub_8228E580);
    return;
  }
  ctx.r3.u64 = self;
  ctx.r4.u64 = vue;
  sub_821DC268(ctx, base);
  uint32_t change = 0;
  ctx.r3.u64 = self;
  sub_821DFF18(ctx, base);
  change = ctx.r3.u32;
  if (change & 0xFF) {
    ctx.r4.u64 = 0;
    TL_V(self, 36);
  }
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68), un = LoadFloat(base, kConstants + 64);
  const float maximum = LoadFloat(base, kConstants + 424);
  if (!base[self + 353]) {
    cacher_tout();
    change = 1;
    ctx.r4.u64 = 0;
    TL_V(self, 36);
    ctx.fpscr.disableFlushMode();
    if (int32_t(Load32(base, self + 632)) == 6 && !(LoadFloat(base, self + 1288) > zero)) {
      const uint32_t s = Load32(base, self + 96);
      const uint32_t r9 = Load32(base, 0x835594CC);
      const uint32_t t = s ? Load32(base, r9 + 72) : 0;
      const uint32_t d = Load32(base, t + 108);
      const uint32_t i = Load32(base, 0x83558F08);
      bool marquer = true;
      if (i < uint32_t(int32_t(Load32(base, t + 112) - d) >> 2) && Load32(base, d + i * 4) == 0) {
        const uint32_t j = Load32(base, 0x83558F44);
        const uint32_t d2 = Load32(base, t + 124);
        const uint32_t n2 = uint32_t(int32_t(Load32(base, t + 128) - d2) >> 2);
        const float seuil = LoadFloat(base, kConstants + 716);
        if (j < n2 && LoadFloat(base, d2 + j * 4) > LoadFloat(base, kConstants + 204)) {
          marquer = false;
        } else if (!(LoadFloat(base, self + 520) > seuil) && j < n2 && LoadFloat(base, d2 + j * 4) > seuil) {
          marquer = false;
        }
      }
      if (marquer) base[self + 344] = 1;
    }
    if (!base[self + 109]) {
      chrono(self + 520, maximum);
      if (base[self + 341]) base[self + 356] = 0;
      notifier(296);
      retirer_selection();
      return;
    }
  }
  if (!base[self + 1691] && !base[self + 109]) {
    TL_V(self, 32);
    if (!(ctx.r3.u32 & 0xFF)) {
      ctx.fpscr.disableFlushMode();
      cacher_tout();
      boite_vue();
      bool retire = false;
      if (!boite_visible()) {
        TL_V(self, 312);
        if (!(ctx.r3.u32 & 0xFF)) {
          retirer();
          retire = true;
        }
      }
      if (!retire) cacher_ou_non(change);
      if (base[self + 341]) base[self + 356] = 0;
      notifier(296);
      retirer_selection();
      const uint32_t coins = Load32(base, sp + 156);
      if (coins) {
        ctx.r3.u64 = coins;
        sub_821CD9A8(ctx, base);
      }
      return;
    }
  }
  const uint32_t porte = Load32(base, self + 428);
  if (porte) {
    ctx.fpscr.disableFlushMode();
    float alpha = un;
    if (((Load32(base, 0x8355AA10) >> 1) & 1) && base[Load32(base, monde + 324) + 2227]) {
      const uint32_t r = Load32(base, monde + 324);
      const float dz = LoadFloat(base, self + 120) - LoadFloat(base, vue + 8);
      const float dx = LoadFloat(base, self + 112) - LoadFloat(base, vue);
      float d = float(std::sqrt(double(FmaLikeGuest(dx, dx, dz * dz)))) - LoadFloat(base, r + 2232);
      if (!(d > zero)) d = zero;
      const float etendue = LoadFloat(base, r + 2228) - LoadFloat(base, r + 2232);
      if (!(d < etendue)) d = etendue;
      const float hauteur = std::fabs(LoadFloat(base, self + 116) - LoadFloat(base, vue + 4)) * LoadFloat(base, kConstants + 1288);
      const float attenuation = un - d / etendue;
      const float h = hauteur < un ? hauteur : un;
      alpha = (un - h) * attenuation;
      const uint32_t s = Load32(base, self + 96);
      const uint32_t w = s ? Load32(base, s + 12) : 0;
      if (w && int32_t(Load32(base, Load32(base, w + 368) + 5124)) == 0) alpha = un;
    }
    if (base[self + 109]) {
      auto fondu = [&](uint32_t objet) {
        ctx.r3.u64 = objet;
        ctx.f1.f64 = double(alpha);
        sub_821AF868(ctx, base);
      };
      fondu(porte);
      const uint32_t arme = Load32(base, self + 1000);
      if (arme && Load32(base, arme + 36)) {
        for (uint32_t i = 0; i < 12; i++) {
          ctx.r3.u64 = Load32(base, self + 1000);
          ctx.r4.u64 = i;
          sub_821D5380(ctx, base);
          const uint32_t o = ctx.r3.u32;
          if (!o) continue;
          TL_V(o, 236);
          if (ctx.r3.u32) {
            TL_V(o, 236);
            fondu(ctx.r3.u32);
          }
          TL_V(o, 344);
          if (ctx.r3.u32) {
            TL_V(o, 344);
            fondu(ctx.r3.u32);
          }
        }
      }
    }
    ctx.r3.u64 = sp + 88;
    ctx.r4.u64 = Load32(base, self + 428);
    ctx.r5.u64 = 0;
    sub_821D3F38(ctx, base);
    const uint32_t p1 = ctx.r3.u32;
    const uint32_t p2 = position(self);
    for (uint32_t i = 0; i < 3; i++) {
      StoreFloat(base, self + 436 + i * 4, LoadFloat(base, p1 + i * 4) + LoadFloat(base, p2 + i * 4));
    }
  }
  ctx.fpscr.disableFlushMode();
  chrono(self + 520, maximum);
  chrono(self + 528, maximum);
  chrono(self + 532, maximum);
  chrono(self + 524, maximum);
  {
    const float t = LoadFloat(base, self + 1512);
    if (t > zero && !(t == LoadFloat(base, kConstants + 920))) {
      const float v = float(double(t) - dt);
      StoreFloat(base, self + 1512, v);
      if (!(v > zero)) {
        ctx.r3.u64 = self;
        sub_82288068(ctx, base);
      }
    }
  }
  ctx.fpscr.disableFlushMode();
  {
    const float t = LoadFloat(base, self + 684);
    if (t > zero) {
      const float v = float(double(t) - dt);
      StoreFloat(base, self + 684, v);
      if (!(v > zero)) {
        if (!base[self + 688] && vivant(self)) {
          base[self + 688] = 1;
          ctx.r3.u64 = self;
          ctx.r4.u64 = 0;
          ctx.r5.u64 = 0;
          ctx.r8.u64 = 7;
          ctx.f1.f64 = double(un);
          ctx.f2.f64 = double(un);
          sub_822919A8(ctx, base);
        }
        for (const uint32_t decalage : {1528u, 1532u}) {
          const uint32_t e = Load32(base, self + decalage);
          if (!e) continue;
          ctx.r3.u64 = e;
          ctx.r4.u64 = 0;
          sub_8231B6F0(ctx, base);
          ctx.r4.u64 = 0;
          TL_V(Load32(base, Load32(base, self + decalage) + 48), 96);
        }
        StoreFloat(base, self + 684, zero);
      }
    }
  }
  {
    const uint32_t w = stats();
    if (w && int32_t(Load32(base, Load32(base, w + 368) + 5124)) == 1) avec_dt(sub_821AC8F8);
  }
  if (Load32(base, self + 380)) {
    ctx.r3.u64 = Load32(base, self + 380);
    sub_8219B3B8(ctx, base);
    ctx.r3.u64 = Load32(base, self + 380);
    sub_821F1E38(ctx, base);
  }
  ctx.r4.u64 = stats();
  avec_dt(sub_821D8A50);
  const uint32_t resultat = ctx.r3.u32;
  avec_dt(sub_821D8F28);
  ctx.f1.f64 = dt;
  TL_V(self, 396);
  avec_dt(sub_821E2618);
  notifier(296);
  if (base[self + 341]) base[self + 356] = 0;
  const uint32_t suite = Load32(base, self + 1612);
  if (suite) {
    ctx.r3.u64 = Load32(base, suite + 12);
    ctx.f1.f64 = dt;
    sub_821E20C0(ctx, base);
    ctx.r3.u64 = Load32(base, suite + 16);
    ctx.f1.f64 = dt;
    sub_821A6BE0(ctx, base);
    for (uint32_t i = 0; i < Load32(base, suite + 24); i++) {
      const uint32_t d = Load32(base, suite + 20);
      ctx.r3.u64 = Load32(base, i < Load32(base, suite + 28) ? d + i * 4 : d);
      sub_823D8C18(ctx, base);
    }
  }
  ctx.r5.u64 = 0;
  ctx.f1.f64 = dt;
  TL_V(self, 376);
  ctx.fpscr.disableFlushMode();
  if (dt == double(zero)) {
    if (!vivant(self)) return;
    if (base[self + 341]) base[self + 356] = 1;
    notifier(300);
    return;
  }
  const float f892 = LoadFloat(base, self + 892);
  base[self + 509] = 0;
  if (!(f892 > zero) && vivant(self)) {
    ctx.r4.u64 = resultat;
    ctx.r5.u64 = 0;
    ctx.r7.u64 = 0;
    ctx.f1.f64 = double(zero);
    TL_V(self, 356);
  }
  {
    const uint32_t cible = Load32(base, self + 644);
    if (cible && etat_a(self, 131) && etat_a(cible, 28) && Load32(base, cible + 644) != self) retirer_selection();
  }
  {
    const uint32_t m = Load32(base, self + 1444);
    if (m && !vivant(m) && !etat_a(m, 28)) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = 0;
      sub_822A1538(ctx, base);
    }
  }
  // Déplacement : r4 point visé, f1 vitesse, puis sub_8219FD08.
  auto aller = [&](uint32_t point, double facteur) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = point;
    ctx.f1.f64 = facteur;
    sub_8219FD08(ctx, base);
  };
  [&]() {
    const int32_t etat = int32_t(Load32(base, self + 632));
    if (Load32(base, self + 1444) && (etat == 2 || etat == 4 || etat == 15) && octet(sub_821E4588)) {
      const uint32_t m = Load32(base, self + 1444);
      TL_V(m, 152);
      const uint32_t decalage = ctx.r3.u32;
      const V3 d = LireV3(base, decalage);
      const uint32_t p = position(m);
      EcrireV3(base, sp + 88, {LoadFloat(base, p) + d.x, d.y + LoadFloat(base, p + 4), d.z + LoadFloat(base, p + 8)});
      aller(sp + 88, double(float(dt * double(LoadFloat(base, kConstants + 248)))));
      return;
    }
    const float seuil = LoadFloat(base, kConstants + 108);
    if (octet(sub_821DC600)) {
      const uint32_t c = Load32(base, self + 708);
      uint32_t actif = 1;
      if (c && !(LoadFloat(base, self + 692) < seuil)) actif = base[c + 62];
      if (!(actif & 0xFF) && int32_t(Load32(base, self + 632)) != 40) {
        if (base[self + 508] && (base[self + 510] || octet(sub_8219A258))) {
          const bool garde = octet(sub_821DC600) && base[c + 64];
          if (!garde) {
            base[self + 508] = 0;
            StoreFloat(base, self + 520, LoadFloat(base, kConstants + 268));
          }
        }
        const uint32_t p = position(self);
        EcrireV3(base, sp + 88, {LoadFloat(base, self + 460) + LoadFloat(base, p), LoadFloat(base, self + 464) + LoadFloat(base, p + 4),
                                 LoadFloat(base, self + 468) + LoadFloat(base, p + 8)});
        aller(sp + 88, dt);
        return;
      }
    }
    const uint32_t cible = Load32(base, self + 644);
    if (cible && !base[self + 508] && !octet(sub_821D7E20)) {
      const int32_t k = int32_t(Load32(base, self + 632));
      if (k != 12 && k != 5 && k != 6 && k != 41 && k != 16) {
        aller(position(cible), dt);
        return;
      }
    }
    const uint32_t autre = Load32(base, self + 652);
    if (autre && !base[self + 508] && !octet(sub_821D7E20) && int32_t(Load32(base, self + 632)) != 16) {
      aller(position(autre), dt);
      return;
    }
    const uint8_t retour = base[self + 510];
    if (!retour && !octet(sub_821DC600)) {
      if (!octet(sub_821D7E20) || base[self + 508]) return;
      const uint32_t r704 = Load32(base, self + 704);
      if (r704 && LoadFloat(base, self + 684) > zero) return;
      if (octet(sub_821DC600)) return;
      const uint32_t s = Load32(base, self + 96);
      const uint32_t w = Load32(base, s + 24) ? Load32(base, Load32(base, s + 20)) : 0;
      if (int32_t(Load32(base, w + 5124)) != 1) return;
      const uint32_t p = position(self);
      if (LoadFloat(base, p) == LoadFloat(base, self + 448)) {
        const uint32_t q = position(self);
        if (LoadFloat(base, q + 8) == LoadFloat(base, self + 456)) return;
      }
      aller(self + 448, dt);
      return;
    }
    if (octet(sub_821DC600) && base[Load32(base, self + 708) + 64]) return;
    if (base[self + 508] && (retour || octet(sub_8219A258))) {
      base[self + 508] = 0;
      StoreFloat(base, self + 520, LoadFloat(base, kConstants + 268));
    }
    if (octet(sub_821DC600) && (cible || autre)) {
      const uint32_t c = Load32(base, self + 708);
      uint32_t actif = 1;
      if (c && !(LoadFloat(base, self + 692) < seuil)) actif = base[c + 62];
      if (actif & 0xFF) {
        const uint32_t x = Load32(base, c + 92);
        if (x && LoadFloat(base, x + 100) > zero) {
          const uint32_t p = position(cible ? cible : autre);
          aller(p, double(float(dt * double(LoadFloat(base, kConstants + 944)))));
          return;
        }
      }
    }
    aller(self + 448, dt);
  }();
  avec_dt(sub_821E1D58);
  ctx.r5.u64 = stats();
  avec_dt(sub_821B60D8);
  avec_dt(sub_821D52A8);
  if (vivant(self)) {
    if (base[self + 341]) base[self + 356] = 1;
    notifier(300);
  }
  if (!base[self + 1691]) {
    const bool immobile = etat_a(self, 128) || etat_a(self, 170);
    boite_vue();
    TL_V(self, 312);
    bool retire = false;
    if (!(ctx.r3.u32 & 0xFF) && !immobile && !boite_visible()) {
      retirer();
      retire = true;
    }
    if (!retire) cacher_ou_non(change);
    const uint32_t coins = Load32(base, sp + 156);
    if (coins) {
      ctx.r3.u64 = coins;
      sub_821CD9A8(ctx, base);
    }
  }
  ctx.r3.u64 = self;
  ctx.r4.u64 = camera;
  ctx.r5.u64 = vue;
  ctx.f1.f64 = dt;
  sub_821A68B8(ctx, base);
  ctx.fpscr.disableFlushMode();
  {
    const float t = LoadFloat(base, self + 1600);
    if (t > zero) StoreFloat(base, self + 1600, float(double(t) - dt));
  }
  if (base[self + 1504]) {
    base[self + 1504] = 0;
    ctx.r5.u64 = 1;
    ctx.f1.f64 = double(zero);
    TL_V(self, 376);
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, effets d'une unité.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821AC8F8);
REX_EXTERN(sub_82316838);
REX_EXTERN(sub_821E1B38);
REX_EXTERN(sub_82312FA0);
REX_EXTERN(sub_8227A228);
REX_EXTERN(sub_8227A590);
REX_EXTERN(sub_82284BA0);
REX_EXTERN(sub_821D86E0);
REX_EXTERN(sub_822853F0);

namespace {
// Effets dont le traitement est long et rare (fin d'effet, changements d'état, téléportations...) : l'unité passe alors entière par
// l'original, avant toute écriture. Les chemins natifs n'ajoutent pas d'effet : la liste relevée reste valable jusqu'à la fin.
bool EffetLong(int32_t t) {
  switch (t) {
    case 47: case 74: case 76: case 81: case 105: case 75: case 61: case 80: case 64:
    case 107: case 106: case 109: case 108: case 59:
      return true;
    default:
      return false;
  }
}
#ifndef __SWITCH__
std::atomic<uint64_t> g_effets_natifs{0}, g_effets_originaux{0};
#endif

// Chaîne large MSVC (en ligne jusqu'à 7 caractères).
uint32_t DonneesChaineLarge(const uint8_t* base, uint32_t s) { return Load32(base, s + 20) >= 8 ? Load32(base, s) : s; }
}  // namespace

// sub_821AC8F8 (r3 unité, f1 dt) : effets de l'unité (deux groupes de 16 octets à partir de +380 ; vector en +24) : temps, particule de
// l'effet, effets 52, 7, 124 (+1293), 43 et 145 (vecteur cumulé +472), fin d'effet ; puis vitesse cumulée bornée.
REX_HOOK_RAW(sub_821AC8F8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821AC8F8(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t groupes = Load32(base, self + 380);
  if (!groupes) return;
  for (uint32_t j = 0; j < 2; j++) {
    const uint32_t g = groupes + j * 16, d = Load32(base, g + 24), n = Load32(base, g + 28), cap = Load32(base, g + 32);
    for (uint32_t i = 0; i < n; i++) {
      if (EffetLong(int32_t(Load32(base, Load32(base, i < cap ? d + i * 4 : d) + 20)))) {
#ifndef __SWITCH__
        g_effets_originaux.fetch_add(1, std::memory_order_relaxed);
#endif
        return __imp__sub_821AC8F8(ctx, base);
      }
    }
  }
#ifndef __SWITCH__
  const uint64_t compte = g_effets_natifs.fetch_add(1, std::memory_order_relaxed);
  if (compte % 100000 == 0) {
    REXLOG_INFO("[ogre natif] effets d'une unité : {} natifs, {} par l'original", compte, g_effets_originaux.load());
  }
#endif
  CadreInvite cadre(ctx, base, 656);
  const uint32_t sp = ctx.r1.u32;
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68), un = LoadFloat(base, kConstants + 64);
  const float c272 = LoadFloat(base, kConstants + 272);
  const uint32_t cumul = self + 472;
  base[self + 1293] = 0;
  StoreFloat(base, cumul, zero);
  StoreFloat(base, cumul + 4, zero);
  StoreFloat(base, cumul + 8, zero);
  constexpr uint32_t kNomVide = 0x831B4210;
  for (uint32_t j = 0; j < 2; j++) {
    const uint32_t v = Load32(base, self + 380) + j * 16 + 24;
    const uint32_t n = Load32(base, v + 4);
    auto element = [&](uint32_t i) {
      const uint32_t d = Load32(base, v);
      return Load32(base, i < Load32(base, v + 8) ? d + i * 4 : d);
    };
    auto valeur = [&](uint32_t e) -> double {
      ctx.r3.u64 = e;
      ctx.r4.u64 = 0;
      sub_8227A228(ctx, base);
      ctx.fpscr.disableFlushMode();
      return ctx.f1.f64;
    };
    auto amortir = [&](uint32_t e) {
      const double x = valeur(e);
      ctx.f1.f64 = dt;
      sub_82284BA0(ctx, base);
      ctx.fpscr.disableFlushMode();
      StoreFloat(base, e + 224, float(x * ctx.f1.f64));
      ctx.r3.u64 = e;
      ctx.r4.u64 = 0;
      sub_8227A590(ctx, base);
      ctx.fpscr.disableFlushMode();
    };
    for (uint32_t i = 0; i < n; i++) {
      uint32_t e = element(i);
      if (base[e + 32]) StoreFloat(base, e + 16, float(double(LoadFloat(base, e + 16)) + dt));
      const uint32_t particule = Load32(base, e + 184);
      if (!particule || !Load32(base, particule + 244)) {
        const uint32_t nom = e + 188;
        const uint32_t longueur = Load32(base, nom + 16);
        const bool vide = longueur == Load32(base, kNomVide + 16) &&
                          std::memcmp(base + DonneesChaineLarge(base, nom), base + DonneesChaineLarge(base, kNomVide),
                                      size_t(longueur) * 2) == 0;
        if (!vide) {
          const float pas = LoadFloat(base, kConstants + 108);
          const float ox = LoadFloat(base, 0x8349D4E0) * pas, oy = LoadFloat(base, 0x8349D4E4) * pas;
          const float oz = LoadFloat(base, 0x8349D4E8) * pas;
          ctx.r3.u64 = sp + 448;
          ctx.r4.u64 = self;
          ctx.r5.u64 = 1;
          sub_821D3F38(ctx, base);
          ctx.fpscr.disableFlushMode();
          const uint32_t p = ctx.r3.u32;
          EcrireV3(base, sp + 208, {LoadFloat(base, p) + ox, LoadFloat(base, p + 4) + oy, LoadFloat(base, p + 8) + oz});
          const uint32_t proprietaire = Load32(base, self + 96);
          const uint32_t e2 = element(i);
          if (!Load32(base, e2 + 184)) {
            const uint32_t gestionnaire = Load32(base, Load32(base, 0x835594CC) + 124);
            uint32_t nouvelle = 0;
            if (gestionnaire) {
              ctx.r3.u64 = gestionnaire;
              ctx.r4.u64 = proprietaire;
              ctx.r5.u64 = DonneesChaineLarge(base, e2 + 188);
              sub_82316838(ctx, base);
              nouvelle = ctx.r3.u32;
            }
            Store32(base, e2 + 184, nouvelle);
            if (nouvelle) {
              ctx.r3.u64 = nouvelle;
              ctx.r4.u64 = sp + 208;
              sub_821E1B38(ctx, base);
              ctx.r3.u64 = Load32(base, e2 + 184);
              sub_82312FA0(ctx, base);
            }
            ctx.fpscr.disableFlushMode();
          }
        }
      }
      e = element(i);
      const int32_t t = int32_t(Load32(base, e + 20));
      if (t == 52 || t == 7 || t == 124) {
        if (valeur(e) < double(zero) && !(LoadFloat(base, element(i) + 28) == c272)) base[self + 1293] = 1;
      } else if (t == 43 && int32_t(Load32(base, e + 24)) == 1) {
        ctx.r3.u64 = self;
        ctx.r4.u64 = 53;
        ctx.r5.u64 = 7;
        sub_821D86E0(ctx, base);
        ctx.fpscr.disableFlushMode();
        float x = float(ctx.f1.f64 * double(LoadFloat(base, kConstants + 264)));
        if (!(x < un)) x = un;
        if (!(x > zero)) x = zero;
        const float reste = un - x;
        const uint32_t e2 = element(i);
        const float w = float(valeur(e2) * double(reste));
        StoreFloat(base, cumul, LoadFloat(base, cumul) + w * LoadFloat(base, e2 + 172));
        StoreFloat(base, cumul + 4, LoadFloat(base, cumul + 4) + LoadFloat(base, e2 + 176) * w);
        StoreFloat(base, cumul + 8, LoadFloat(base, cumul + 8) + LoadFloat(base, e2 + 180) * w);
        amortir(element(i));
      } else if (t == 145 && int32_t(Load32(base, e + 24)) == 1) {
        const uint32_t e2 = element(i);
        const double w = valeur(e2);
        StoreFloat(base, cumul + 4, float(double(LoadFloat(base, cumul + 4)) + double(float(double(LoadFloat(base, e2 + 176)) * w))));
        StoreFloat(base, cumul + 8, float(double(LoadFloat(base, cumul + 8)) + double(float(double(LoadFloat(base, e2 + 180)) * w))));
        StoreFloat(base, cumul, float(double(LoadFloat(base, cumul)) + double(float(double(LoadFloat(base, e2 + 172)) * w))));
        amortir(element(i));
      }
      e = element(i);
      if (LoadFloat(base, e + 28) == c272) {
        ctx.r3.u64 = self;
        ctx.r4.u64 = e;
        sub_822853F0(ctx, base);
        ctx.fpscr.disableFlushMode();
      }
    }
  }
  StoreFloat(base, cumul + 4, zero);
  const float cx = LoadFloat(base, cumul), cz = LoadFloat(base, cumul + 8), cy = LoadFloat(base, cumul + 4);
  const float longueur = float(std::sqrt(double(FmaLikeGuest(cz, cz, FmaLikeGuest(cx, cx, cy * cy)))));
  bool borne = false;
  const uint32_t s = Load32(base, self + 96);
  if (s && Load32(base, s + 16)) {
    ctx.r3.u64 = Load32(base, s + 16);
    ctx.r4.u64 = Load32(base, self + 372);
    ctx.r5.u64 = 28;
    sub_821D7D38(ctx, base);
    ctx.fpscr.disableFlushMode();
    borne = (ctx.r3.u32 & 0xFF) != 0;
  }
  const float petit = LoadFloat(base, kConstants + 268), grand = LoadFloat(base, kConstants + 944);
  float maximum = 0;
  if (borne && longueur > petit) {
    maximum = petit;
  } else if (longueur > grand) {
    maximum = grand;
  } else {
    return;
  }
  // sub_82199360, puis mise à l'échelle.
  const float x = LoadFloat(base, cumul), y = LoadFloat(base, cumul + 4), z = LoadFloat(base, cumul + 8);
  const float l = float(std::sqrt(double(FmaLikeGuest(z, z, FmaLikeGuest(x, x, y * y)))));
  float nx = x, ny = y, nz = z;
  if (double(l) > LoadDouble(base, kConstants + 640)) {
    const float inverse = un / l;
    nx = x * inverse;
    ny = y * inverse;
    nz = z * inverse;
  }
  EcrireV3(base, cumul, {nx * maximum, ny * maximum, nz * maximum});
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, couleurs et brouillard de guerre.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821D3638);
REX_EXTERN(__imp__sub_821EF7B8);
REX_EXTERN(sub_821D3A98);
REX_EXTERN(sub_82625758);
REX_EXTERN(sub_821D3990);
REX_EXTERN(sub_821DC6D8);
REX_EXTERN(sub_8245BB48);
REX_EXTERN(sub_822E6C40);

namespace {
// vmaddfp x, 0x820DC010 (échelle), 0x820DC020 (3.0), puis vpkd3d128 en D3DCOLOR : octet de poids faible du flottant borné à
// [3.0, 0x404000FF]. Mots : r, g, b, a en mémoire ; sortie 0xAARRGGBB.
uint32_t OctetCouleur(float x, float echelle) {
#pragma clang fp contract(off)
  const float v = x * echelle + 3.0f;
  const uint32_t haut_bits = 0x404000FF;
  float haut;
  std::memcpy(&haut, &haut_bits, 4);
  const float borne = !(v >= 3.0f) ? 3.0f : (v > haut ? haut : v);
  return FloatBits(borne) & 0xFF;
}

uint32_t CouleurD3D(const uint8_t* base, float r, float g, float b, float a) {
  const float echelle = LoadFloat(base, 0x820DC010);
  return (OctetCouleur(a, echelle) << 24) | (OctetCouleur(r, echelle) << 16) | (OctetCouleur(g, echelle) << 8) | OctetCouleur(b, echelle);
}
}  // namespace

// sub_821D3638 (r3 rendu CEGUI, r4 rectangle, r6 texture, r7 uv, r8 couleurs, r9 mode, f1 z) : addQuad : quadrilatère en coordonnées
// écran, map triée sur z (+200, _Isnil +89, clé +44). Tampon direct (+216 nul) : sub_82624FE0 par l'original.
REX_HOOK_RAW(sub_821D3638) {
#pragma clang fp contract(off)
  const uint32_t self = ctx.r3.u32;
  if (!Pilote() || !base[self + 216]) return __imp__sub_821D3638(ctx, base);
  const uint32_t rect = ctx.r4.u32, texture = ctx.r6.u32, uv = ctx.r7.u32, couleurs = ctx.r8.u32, mode = ctx.r9.u32;
  const double z = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 256);
  const uint32_t sp = ctx.r1.u32, q = sp + 128;
  ctx.fpscr.disableFlushMode();
  base[self + 536] = 0;
  const float x0 = LoadFloat(base, rect), y0 = LoadFloat(base, rect + 4), x1 = LoadFloat(base, rect + 8), y1 = LoadFloat(base, rect + 12);
  const float h = LoadFloat(base, self + 188) - LoadFloat(base, self + 184);
  const float l = LoadFloat(base, self + 196) - LoadFloat(base, self + 192);
  const float dx = LoadFloat(base, self + 540), dy = LoadFloat(base, self + 544);
  const float demi = LoadFloat(base, kConstants + 104), un = LoadFloat(base, kConstants + 64);
  const float demi_h = h * demi, demi_l = l * demi;
  const uint32_t mots[] = {FloatBits((dy + (h - x0)) / demi_h - un), FloatBits((dy + (h - y0)) / demi_h - un),
                           FloatBits((dx + x1) / demi_l - un), FloatBits((dx + y1) / demi_l - un)};
  Store32(base, q, 0x820009AC);
  Store32(base, q + 4, 0);
  Store32(base, q + 8, 0);
  Store32(base, q + 12, 0);
  for (uint32_t i = 0; i < 4; i++) Store32(base, q + 16 + i * 4, mots[i]);
  StoreFloat(base, q + 32, float(z - double(LoadFloat(base, kConstants + 124))));
  base[q + 36] = base[texture + 29] ? 1 : 0;
  ctx.r3.u64 = sp + 112;
  ctx.r4.u64 = texture + 8;
  sub_821D3A98(ctx, base);
  ctx.r3.u64 = q;
  ctx.r4.u64 = sp + 112;
  sub_821D2EF0(ctx, base);
  ctx.r3.u64 = sp + 112;
  sub_82625758(ctx, base);
  ctx.fpscr.disableFlushMode();
  for (uint32_t i = 0; i < 4; i++) Store32(base, q + 40 + i * 4, Load32(base, uv + i * 4));
  auto couleur = [&](uint32_t c) {
    return CouleurD3D(base, LoadFloat(base, c + 4), LoadFloat(base, c + 8), LoadFloat(base, c + 12), LoadFloat(base, c));
  };
  Store32(base, q + 56, couleur(couleurs + 48));
  Store32(base, q + 60, couleur(couleurs + 72));
  Store32(base, q + 64, couleur(couleurs));
  Store32(base, q + 68, couleur(couleurs + 24));
  Store32(base, q + 72, mode);
  ctx.r3.u64 = self + 200;
  ctx.r4.u64 = q;
  sub_821D3990(ctx, base);
  const uint32_t noeud = ctx.r3.u32;
  ctx.fpscr.disableFlushMode();
  const uint32_t tete = Load32(base, self + 204);
  uint32_t parent = tete, gauche = 1;
  uint32_t n = Load32(base, tete + 4);
  if (!base[n + 89]) {
    const float cle = LoadFloat(base, noeud + 44);
    do {
      parent = n;
      gauche = cle > LoadFloat(base, n + 44) ? 1 : 0;
      n = gauche ? Load32(base, n) : Load32(base, n + 8);
    } while (!base[n + 89]);
  }
  ctx.r3.u64 = sp + 80;
  ctx.r4.u64 = self + 200;
  ctx.r5.u64 = gauche;
  ctx.r6.u64 = parent;
  ctx.r7.u64 = noeud;
  sub_821DC6D8(ctx, base);
  Store32(base, q, 0x820AE31C);
  const uint32_t compteur = Load32(base, q + 8);
  if (compteur) {
    const uint32_t c = Load32(base, compteur) - 1;
    Store32(base, compteur, c);
    if (!c) {
      ctx.r3.u64 = q;
      GuestFunc(base, Slot(base, q, 4))(ctx, base);
    }
  }
}

// sub_821EF7B8 (r3, r4 position, f1 dt) : brouillard de guerre : caméra de la carte, cases découvertes (+128 cibles, +132 valeurs, +112 et
// +116 dimensions) qui évoluent à vitesse fixe et vont dans une texture, opacité des marqueurs (+140 et +172), position des icônes.
REX_HOOK_RAW(sub_821EF7B8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821EF7B8(ctx, base);
  const uint32_t self = ctx.r3.u32, pos = ctx.r4.u32;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 288);
  const uint32_t sp = ctx.r1.u32;
  ctx.fpscr.disableFlushMode();
  {
    const uint32_t camera = Load32(base, self + 64);
    const float k = LoadFloat(base, self + 76);
    const float kx = k * LoadFloat(base, 0x8349D4E0), ky = k * LoadFloat(base, 0x8349D4E4), kz = k * LoadFloat(base, 0x8349D4E8);
    const float ox = LoadFloat(base, pos) + LoadFloat(base, self + 120), oy = LoadFloat(base, pos + 4);
    const float oz = LoadFloat(base, pos + 8) + LoadFloat(base, self + 124);
    if (base[self + 137]) {
      const float c = LoadFloat(base, kConstants + 420);
      EcrireV3(base, camera + 1056, {ox + kx * c, oy + ky * c, oz + kz * c});
    } else {
      EcrireV3(base, camera + 1056, {ox + kx, oy + ky, oz + kz});
    }
    TL_V(camera, 256);
  }
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68), un = LoadFloat(base, kConstants + 64);
  const float vitesse = LoadFloat(base, kConstants + 124), jamais = LoadFloat(base, kConstants + 424);
  auto cible = [&](int32_t x, int32_t y) -> float {
    const int32_t l = int32_t(Load32(base, self + 112));
    const int32_t i = y * l + x;
    if (i < l * int32_t(Load32(base, self + 116)) && x >= 0 && y >= 0) return LoadFloat(base, Load32(base, self + 128) + uint32_t(i) * 4);
    return zero;
  };
  auto avancer = [&](float v) { return float(std::fma(dt, double(vitesse), double(v))); };
  auto couleur_marqueur = [&](uint32_t objet, float a, float b) {
    StoreFloat(base, objet + 64, a);
    StoreFloat(base, objet + 68, a);
    StoreFloat(base, objet + 72, a);
    StoreFloat(base, objet + 76, b);
  };
  if (base[self + 136] && base[self + 92]) {
    const bool brouillard_seul = base[self + 138] != 0;
    base[self + 136] = 0;
    if (!brouillard_seul) {
      bool change = false;
      for (uint32_t y = 0; y < Load32(base, self + 116); y++) {
        for (uint32_t x = 0; x < Load32(base, self + 112); x++) {
          const uint32_t a = (Load32(base, self + 112) * y + x) * 4;
          const uint32_t valeurs = Load32(base, self + 132);
          const float v = LoadFloat(base, valeurs + a);
          if (v == jamais) continue;
          if (v == cible(int32_t(x), int32_t(y))) continue;
          change = true;
          StoreFloat(base, valeurs + a, avancer(v));
          const float t = cible(int32_t(x), int32_t(y));
          const uint32_t p = Load32(base, self + 132) + a;
          const float w = LoadFloat(base, p);
          StoreFloat(base, p, w < t ? w : t);
          if (!(LoadFloat(base, Load32(base, self + 132) + a) == cible(int32_t(x), int32_t(y)))) base[self + 136] = 1;
        }
      }
      if (change) {
        ctx.r3.u64 = Load32(base, self + 36);
        ctx.r4.u64 = 0;
        sub_8245BB48(ctx, base);
        ctx.r4.u64 = 1;
        TL_V(Load32(base, Load32(base, ctx.r3.u32 + 8) + 8), 20);
        ctx.r4.u64 = ctx.r3.u32;
        ctx.r3.u64 = sp + 112;
        sub_821965E8(ctx, base);
        const uint32_t tampon = Load32(base, sp + 116);
        ctx.r4.u64 = 0;
        ctx.r5.u64 = Load32(base, tampon + 4);
        ctx.r6.u64 = 1;
        TL_V(tampon, 12);
        uint32_t o = ctx.r3.u32;
        ctx.fpscr.disableFlushMode();
        for (uint32_t y = 0; y < Load32(base, self + 116); y++) {
          for (uint32_t x = 0; x < Load32(base, self + 112); x++) {
            const float v = LoadFloat(base, Load32(base, self + 132) + (y * Load32(base, self + 112) + x) * 4);
            Store32(base, o, CouleurD3D(base, v, v, v, un));
            o += 4;
          }
        }
        TL_V(Load32(base, sp + 116), 20);
        Store32(base, sp + 112, 0x820AE2C0);
        const uint32_t compteur = Load32(base, sp + 120);
        if (compteur) {
          const uint32_t c = Load32(base, compteur) - 1;
          Store32(base, compteur, c);
          if (!c) {
            ctx.r3.u64 = sp + 112;
            GuestFunc(base, Slot(base, sp + 112, 4))(ctx, base);
          }
        }
        ctx.fpscr.disableFlushMode();
      }
    }
    if (!base[self + 138]) {
      const uint32_t nombre = uint32_t(int32_t(Load32(base, self + 144) - Load32(base, self + 140)) >> 2);
      const float echelle = LoadFloat(base, kConstants + 248), hors = LoadFloat(base, kConstants + 1152);
      for (uint32_t k = 0, off = 0; k < nombre; k++, off += 4) {
        const float v = LoadFloat(base, Load32(base, self + 156) + off);
        if (v == jamais) continue;
        const uint32_t objets = Load32(base, self + 140);
        const uint32_t o = Load32(base, objets + off);
        const float fx = (LoadFloat(base, o + 28) - LoadFloat(base, self + 104)) * echelle;
        const float fz = (LoadFloat(base, o + 36) - LoadFloat(base, self + 108)) * echelle;
        const int32_t ix = FctiwzLikeGuest(double(fx)), iz = FctiwzLikeGuest(double(fz));
        if (!base[self + 138] && int32_t(Load32(base, self + 112)) >= 4) {
          ctx.r3.u64 = self;
          ctx.r4.u64 = uint32_t(ix);
          ctx.r5.u64 = uint32_t(iz);
          sub_822E6C40(ctx, base);
          ctx.fpscr.disableFlushMode();
          if (!(ctx.f1.f64 > double(zero))) {
            if (v == zero) continue;
            StoreFloat(base, o + 36, LoadFloat(base, o + 36));
            StoreFloat(base, o + 28, LoadFloat(base, o + 28));
            StoreFloat(base, o + 32, hors);
            couleur_marqueur(Load32(base, Load32(base, self + 140) + off), zero, zero);
            StoreFloat(base, Load32(base, self + 156) + off, zero);
            continue;
          }
        }
        if (v == cible(ix, iz)) continue;
        StoreFloat(base, o + 32, zero);
        const uint32_t valeurs = Load32(base, self + 156);
        StoreFloat(base, valeurs + off, avancer(LoadFloat(base, valeurs + off)));
        const float t = cible(ix, iz);
        const uint32_t p = Load32(base, self + 156) + off;
        const float w = LoadFloat(base, p);
        StoreFloat(base, p, w < t ? w : t);
        const float t3 = cible(ix, iz);
        const float final_ = LoadFloat(base, Load32(base, self + 156) + off);
        if (!(final_ == t3)) base[self + 136] = 1;
        couleur_marqueur(Load32(base, Load32(base, self + 140) + off), un, final_);
      }
      const uint32_t icones = uint32_t(int32_t(Load32(base, self + 176) - Load32(base, self + 172)) >> 2);
      for (uint32_t k = 0, off = 0, off12 = 0; k < icones; k++, off += 4, off12 += 12) {
        const uint32_t p = Load32(base, self + 204) + off12;
        const float fx = (LoadFloat(base, p) - LoadFloat(base, self + 104)) * echelle;
        const float fz = (LoadFloat(base, p + 8) - LoadFloat(base, self + 108)) * echelle;
        const uint32_t nombre_liste = Load32(base, self + 224);
        bool montrer = true;
        if (nombre_liste) {
          const uint32_t liste = Load32(base, self + 220);
          const uint32_t objet = Load32(base, Load32(base, self + 172) + off);
          for (uint32_t i = 0; i < nombre_liste; i++) {
            if (Load32(base, liste + i * 4) == objet) {
              montrer = false;
              break;
            }
          }
        }
        if (montrer && !base[self + 138] && int32_t(Load32(base, self + 112)) >= 4) {
          const int32_t iz = FctiwzLikeGuest(double(fz)), ix = FctiwzLikeGuest(double(fx));
          const int32_t l = int32_t(Load32(base, self + 112));
          const int32_t i = l * iz + ix;
          if (!(i < int32_t(Load32(base, self + 116)) * l) || ix < 0 || iz < 0 ||
              LoadFloat(base, Load32(base, self + 128) + uint32_t(i) * 4) == zero) {
            montrer = false;
          }
        }
        if (montrer) {
          const float v = LoadFloat(base, Load32(base, self + 188) + off);
          if (v == un) continue;
          const uint32_t a = Load32(base, self + 188) + off;
          StoreFloat(base, a, avancer(v));
          const float w = LoadFloat(base, Load32(base, self + 188) + off);
          StoreFloat(base, Load32(base, self + 188) + off, w < un ? w : un);
          const float f = LoadFloat(base, Load32(base, self + 188) + off);
          if (!(f == un)) base[self + 136] = 1;
          couleur_marqueur(Load32(base, Load32(base, self + 172) + off), un, f);
        } else {
          if (LoadFloat(base, Load32(base, self + 188) + off) == zero) continue;
          couleur_marqueur(Load32(base, Load32(base, self + 172) + off), zero, zero);
          StoreFloat(base, Load32(base, self + 188) + off, zero);
        }
      }
    }
  }
  const uint32_t icones = uint32_t(int32_t(Load32(base, self + 176) - Load32(base, self + 172)) >> 2);
  if (!icones) return;
  const double seuil = LoadDouble(base, kConstants + 640);
  const float cachee = LoadFloat(base, kConstants + 256), loin = LoadFloat(base, kConstants + 172), rayon = LoadFloat(base, kConstants + 132);
  for (uint32_t k = 0, off = 0, off12 = 0; k < icones; k++, off += 4, off12 += 12) {
    if (!base[self + 137]) {
      const uint32_t p = Load32(base, self + 204) + off12;
      const bool nulle = LoadFloat(base, p) == loin && LoadFloat(base, p + 4) == cachee && LoadFloat(base, p + 8) == loin;
      if (!nulle && !(LoadFloat(base, Load32(base, self + 188) + off) == zero)) {
        const float px = LoadFloat(base, pos), pz = LoadFloat(base, pos + 8);
        float dx = LoadFloat(base, p) - px, dz = LoadFloat(base, p + 8) - pz;
        const float limite = LoadFloat(base, self + 76) * rayon;
        const float carre = FmaLikeGuest(dz, dz, dx * dx);
        const float d = float(std::sqrt(double(carre)));
        const float r = limite < d ? limite : d;
        float dy = zero;
        if (double(d) > seuil) {
          const float inverse = un / d;
          dx = inverse * dx;
          dy = inverse * zero;
          dz = dz * inverse;
        }
        EcrireV3(base, Load32(base, Load32(base, self + 172) + off) + 28,
                 {px + dx * r, LoadFloat(base, pos + 4) + r * dy, dz * r + pz});
        continue;
      }
    }
    const uint32_t o = Load32(base, Load32(base, self + 172) + off);
    if (LoadFloat(base, Load32(base, self + 188) + off) == zero) {
      EcrireV3(base, o + 28, {loin, cachee, loin});
    } else {
      const uint32_t p = Load32(base, self + 204) + off12;
      EcrireV3(base, o + 28, {LoadFloat(base, p), LoadFloat(base, p + 4), LoadFloat(base, p + 8)});
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, traînées.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821E30A0);
REX_EXTERN(sub_821D9830);
REX_EXTERN(sub_821BDDA8);
REX_EXTERN(sub_821DBBF8);

// sub_821E30A0 (r3 traînée, r4 chaîne, r5 nœud suivi) : RibbonTrail::updateTrail ; +440 posé : éclair (la chaîne est refaite entre la tête et le
// nœud, avec du bruit) ; sinon la tête suit le nœud et un élément est ajouté à chaque longueur (+532, carré +536). Éléments de 48 octets
// (+336), segments de 12 octets (+352 : début, tête, queue).
REX_HOOK_RAW(sub_821E30A0) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821E30A0(ctx, base);
  const uint32_t self = ctx.r3.u32, chaine = ctx.r4.u32, noeud = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 512);
  const uint32_t sp = ctx.r1.u32;
  ctx.fpscr.disableFlushMode();
  const int32_t tuiles_h = int32_t(Load32(base, self + 456)), tuiles_v = int32_t(Load32(base, self + 460));
  const float zero = LoadFloat(base, kConstants + 68), un = LoadFloat(base, kConstants + 64);
  const float pas_v = un / float(double(tuiles_v)), pas_h = un / float(double(tuiles_h));
  float u0 = zero, v0 = zero;
  if (!(tuiles_h == 1 && tuiles_v == 1)) {
    const int32_t image = int32_t(Load32(base, Load32(base, self + 572) + chaine * 4));
    const float rang = float(std::floor(double(float(double(image)) / float(double(tuiles_h)))));
    const int32_t ligne = FctiwzLikeGuest(double(rang));
    const int32_t colonne = int32_t(Load32(base, Load32(base, self + 572) + chaine * 4)) - ligne * int32_t(Load32(base, self + 456));
    u0 = float(double(colonne)) * pas_h;
    v0 = float(double(ligne)) * pas_v;
  }
  ctx.f1.f64 = double(u0);
  ctx.f2.f64 = double(u0 + pas_h);
  TL_V(self, 236);
  ctx.fpscr.disableFlushMode();
  const uint32_t decalage_seg = chaine * 12;
  auto element = [&](uint32_t debut, uint32_t i) { return (debut + i) * 48 + Load32(base, self + 336); };
  auto bouts = [&](uint32_t e, uint32_t a, uint32_t b, uint32_t i, uint32_t tete, uint32_t queue) {
    // Couleur de l'élément : celle de la chaîne, multipliée par +496 en tête, +512 en queue.
    const uint32_t c = Load32(base, self + 540) + chaine * 16;
    (void)a, (void)b;
    if (i == tete || i == queue) {
      const uint32_t f = i == tete ? self + 496 : self + 512;
      for (uint32_t k = 0; k < 4; k++) StoreFloat(base, e + 32 + k * 4, LoadFloat(base, f + k * 4) * LoadFloat(base, c + k * 4));
    } else {
      std::memcpy(base + e + 32, base + c, 16);
    }
  };
  auto finir = [&]() {
    const uint32_t r40 = Load32(base, self + 40);
    base[self + 242] = 1;
    if (!r40) return;
    TL_V(self, 44);
    const uint32_t objet = ctx.r3.u32;
    Store32(base, sp + 80, objet);
    if (!base[objet + 75]) {
      base[objet + 75] = 1;
      ctx.r3.u64 = 0x8349FFAC;
      ctx.r4.u64 = sp + 80;
      sub_82480518(ctx, base);
    }
  };
  if (base[self + 440]) {
    const int32_t maximum = int32_t(Load32(base, self + 220));
    const int32_t nombre = maximum > 2 ? maximum : 2;
    TL_V(noeud, 52);
    ctx.fpscr.disableFlushMode();
    const uint32_t pp = ctx.r3.u32;
    uint32_t brut[3];
    std::memcpy(brut, base + pp, 12);
    const V3 p = LireV3(base, pp);
    const float dy = LoadFloat(base, self + 480) - p.y, dx = LoadFloat(base, self + 476) - p.x, dz = LoadFloat(base, self + 484) - p.z;
    const float parties = float(double(nombre)) - un;
    float px = dx, py = dy, pz = dz;
    if (!(parties == zero)) {  // sub_821DCF68
      const float inverse = un / parties;
      px = dx * inverse, py = dy * inverse, pz = dz * inverse;
    }
    uint32_t seg = decalage_seg + Load32(base, self + 352);
    int32_t restants = nombre - int32_t(Load32(base, seg + 8)) + int32_t(Load32(base, seg + 4)) - 1;
    for (; restants > 0; restants--) {
      const uint32_t e = sp + 176;
      std::memcpy(base + e, brut, 12);
      StoreFloat(base, e + 12, LoadFloat(base, Load32(base, self + 588) + chaine * 4));
      StoreFloat(base, e + 16, zero);
      std::memcpy(base + e + 32, base + Load32(base, self + 540) + chaine * 16, 16);
      ctx.r4.u64 = chaine;
      ctx.r5.u64 = e;
      TL_V(self, 260);
    }
    ctx.fpscr.disableFlushMode();
    const float inverse_parties = un / parties;
    const uint32_t source = Load32(base, self + 352) + decalage_seg;
    std::memcpy(base + seg, base + source, 12);
    const float longueur = float(std::sqrt(double(FmaLikeGuest(dy, dy, FmaLikeGuest(dx, dx, dz * dz)))));
    if (longueur > zero) {
      const float petit = LoadFloat(base, kConstants + 480);
      const uint32_t vecteur_nul = 0x8355A3BC;
      float t = zero;
      for (uint32_t i = Load32(base, seg + 4);;) {
        const uint32_t w = i == uint32_t(nombre) ? 0 : i;
        float bruit = LoadFloat(base, self + 452);
        const float mini = LoadFloat(base, self + 448);
        if (!(mini == bruit)) {
          sub_821BDDA8(ctx, base);
          ctx.fpscr.disableFlushMode();
          bruit = FmaLikeGuest(float(double(ctx.r3.s32)) * (bruit - mini), petit, mini);
        }
        const float f = inverse_parties * t;
        ctx.f1.f64 = double(FmaLikeGuest(f, LoadFloat(base, 0x8356C9CC), LoadFloat(base, self + 444)) + bruit);
        sub_821DBBF8(ctx, base);
        ctx.fpscr.disableFlushMode();
        const float sinus = float(ctx.f1.f64);
        V3 d = LireV3(base, vecteur_nul);
        if (w != Load32(base, seg + 4) && w != Load32(base, seg + 8)) {
          d = {LoadFloat(base, self + 464) * sinus, LoadFloat(base, self + 468) * sinus, LoadFloat(base, self + 472) * sinus};
        }
        const uint32_t e = element(Load32(base, seg), w);
        StoreFloat(base, e + 16, FmaLikeGuest(f, pas_v, v0));
        bouts(e, 0, 0, w, Load32(base, seg + 4), Load32(base, seg + 8));
        StoreFloat(base, e + 8, d.z + (pz * t + p.z));
        StoreFloat(base, e + 4, d.y + (py * t + p.y));
        StoreFloat(base, e, (px * t + p.x) + d.x);
        StoreFloat(base, e + 12, LoadFloat(base, Load32(base, self + 588) + chaine * 4));
        if (w == Load32(base, seg + 8)) break;
        t = t + un;
        i = w + 1;
      }
    }
    finir();
    return;
  }
  const double tolerance = LoadDouble(base, kConstants + 848);
  bool fini = false;
  while (!fini) {
    const uint32_t seg = Load32(base, self + 352) + decalage_seg;
    const uint32_t total = Load32(base, self + 220);
    const uint32_t debut = Load32(base, seg), tete = Load32(base, seg + 4);
    const uint32_t apres = tete + 1 != total ? tete + 1 : 0;
    const uint32_t e_tete = element(debut, tete), e_suite = element(debut, apres);
    TL_V(noeud, 52);
    ctx.fpscr.disableFlushMode();
    const uint32_t pp = ctx.r3.u32;
    uint32_t brut[3];
    std::memcpy(brut, base + pp, 12);
    const V3 p = LireV3(base, pp);
    V3 s = LireV3(base, e_suite);
    float dy = p.y - s.y, dx = p.x - s.x, dz = p.z - s.z;
    float cx = dx * dx, cy = dy * dy, cz = dz * dz;
    float d2 = (cx + cy) + cz;
    // Nœud très loin de la traînée (effet qui démarre ou qui saute) : la boucle ajoutait un élément par longueur, jusqu'à des
    // dizaines de milliers de fois (blocage de ~170 ms en combat, 2026-11-04), alors que l'anneau n'en garde que total. Tout
    // ce qui précède est écrasé : on saute le long de la droite pour ne faire que 2 x total tours. Mêmes éléments, à
    // l'arrondi près ; seul le rang de la tête dans l'anneau change (sans effet sur l'image).
    const float pas_chaine = LoadFloat(base, self + 532);
    if (pas_chaine > zero && d2 > pas_chaine * pas_chaine * float(total) * float(total) * 16.0f) {
      const float reste = pas_chaine * float(total) * 2.0f;
      const float saut = 1.0f - reste / float(std::sqrt(double(d2)));
      s.x += dx * saut, s.y += dy * saut, s.z += dz * saut;
      dx = p.x - s.x, dy = p.y - s.y, dz = p.z - s.z;
      cx = dx * dx, cy = dy * dy, cz = dz * dz;
      d2 = (cx + cy) + cz;
    }
    if (d2 < LoadFloat(base, self + 536)) {
      EcrireV3(base, e_tete, p);
      fini = true;
    } else {
      const float k = pas_chaine / float(std::sqrt(double(d2)));
      EcrireV3(base, e_tete, {s.x + dx * k, s.y + dy * k, s.z + dz * k});
      const uint32_t e = sp + 224;
      std::memcpy(base + e, brut, 12);
      StoreFloat(base, e + 12, LoadFloat(base, Load32(base, self + 588) + chaine * 4));
      StoreFloat(base, e + 16, zero);
      std::memcpy(base + e + 32, base + Load32(base, self + 540) + chaine * 16, 16);
      ctx.r4.u64 = chaine;
      ctx.r5.u64 = e;
      TL_V(self, 260);
      ctx.fpscr.disableFlushMode();
      const V3 t = LireV3(base, e_tete);
      const float ax = p.x - t.x, ay = p.y - t.y, az = p.z - t.z;
      cx = ax * ax, cy = ay * ay, cz = az * az;
      if (!(((cx + cy) + cz) > LoadFloat(base, self + 536))) fini = true;
    }
    const uint32_t queue = Load32(base, seg + 8), capacite = Load32(base, self + 220);
    if ((queue + 1) % capacite == Load32(base, seg + 4)) {
      const uint32_t dep = Load32(base, seg);
      const uint32_t e_q = element(dep, queue);
      const uint32_t precedent = queue ? queue - 1 : capacite - 1;
      const uint32_t e_p = element(dep, precedent);
      const V3 q = LireV3(base, e_q), r = LireV3(base, e_p);
      const float qy = q.y - r.y, qz = q.z - r.z, qx = q.x - r.x;
      const float d = float(std::sqrt(double(FmaLikeGuest(qx, qx, FmaLikeGuest(qz, qz, qy * qy)))));
      if (double(d) > tolerance) {
        const float w = (LoadFloat(base, self + 532) - float(std::sqrt(double((cx + cy) + cz)))) / d;
        EcrireV3(base, e_q, {r.x + w * qx, r.y + qy * w, r.z + qz * w});
      }
    }
  }
  const uint32_t source = Load32(base, self + 352) + decalage_seg;
  float cumul = zero;
  uint32_t prec = element(Load32(base, source), Load32(base, source + 4));
  for (uint32_t i = Load32(base, source + 4);;) {
    const uint32_t w = i == Load32(base, self + 220) ? 0 : i;
    const uint32_t e = element(Load32(base, source), w);
    if (w == Load32(base, source + 4)) {
      StoreFloat(base, e + 16, zero);
    } else {
      const V3 a = LireV3(base, e), b = LireV3(base, prec);
      const float ey = a.y - b.y, ez = a.z - b.z, ex = a.x - b.x;
      cumul = float(std::sqrt(double(FmaLikeGuest(ex, ex, FmaLikeGuest(ez, ez, ey * ey))))) + cumul;
      StoreFloat(base, e + 16, cumul);
    }
    prec = e;
    if (w == Load32(base, source + 8)) break;
    i = w + 1;
  }
  if (cumul > zero) {
    const float inverse = un / cumul;
    for (uint32_t i = Load32(base, source + 4);;) {
      const uint32_t w = i == Load32(base, self + 220) ? 0 : i;
      const uint32_t e = element(Load32(base, source), w);
      StoreFloat(base, e + 16, FmaLikeGuest(inverse * LoadFloat(base, e + 16), pas_v, v0));
      bouts(e, 0, 0, w, Load32(base, source + 4), Load32(base, source + 8));
      if (w == Load32(base, source + 8)) break;
      i = w + 1;
    }
  }
  finir();
}

// ---------------------------------------------------------------------------------------------------------------------
// Top 50, texte.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821B8F00);
REX_EXTERN(sub_821B3B70);
REX_EXTERN(sub_821BE290);
REX_EXTERN(sub_821B35A0);
REX_EXTERN(sub_821B4118);
REX_EXTERN(sub_821AC4F8);
REX_EXTERN(sub_821A5CE0);
REX_EXTERN(sub_821B39E0);
REX_EXTERN(sub_821AC490);
REX_EXTERN(sub_825B4530);
REX_EXTERN(sub_821B3988);
REX_EXTERN(sub_825B41C8);
REX_EXTERN(sub_825AB530);
REX_EXTERN(sub_825D56F8);
REX_EXTERN(sub_825B4600);
REX_EXTERN(sub_821B1EE8);
REX_EXTERN(sub_821AF228);
REX_EXTERN(sub_821B3C88);
REX_EXTERN(sub_821BA438);

// sub_821B8F00 (r3 police, r4 texte, r5 position, r6, f1 et f2 échelles) : dessin d'un texte : balises de couleur (ouverture +724, fermeture +876)
// si la police les traite (+1028), puis un quadrilatère par caractère (sub_821BA438).
REX_HOOK_RAW(sub_821B8F00) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821B8F00(ctx, base);
  const uint32_t self = ctx.r3.u32, texte = ctx.r4.u32, pos = ctx.r5.u32, r6 = ctx.r6.u32;
  const double ex = ctx.f1.f64, ey = ctx.f2.f64;
  CadreInvite cadre(ctx, base, 672);
  const uint32_t sp = ctx.r1.u32;
  const float y0 = LoadFloat(base, pos + 4);
  const uint32_t lg_ouvre = Load32(base, self + 724), lg_ferme = Load32(base, self + 876);
  std::memcpy(base + sp + 96, base + pos, 12);
  auto copie_large = [&](uint32_t chaine, uint32_t sortie) {
    ctx.r3.u64 = chaine;
    sub_821B3B70(ctx, base);
    ctx.r4.u64 = ctx.r3.u32;
    ctx.r3.u64 = sp + 112;
    sub_821BE290(ctx, base);
    ctx.r3.u64 = sortie;
    ctx.r4.u64 = sp + 112;
    sub_821B35A0(ctx, base);
    ctx.r3.u64 = sp + 112;
    sub_821B4118(ctx, base);
  };
  auto donnees = [&](uint32_t s) { return Load32(base, s + 20) >= 8 ? Load32(base, s) : s; };
  copie_large(self + 724, sp + 208);
  copie_large(self + 876, sp + 144);
  const uint32_t ouvre = donnees(sp + 208), ferme = donnees(sp + 144);
  std::memcpy(base + sp + 240, base + 0x82000B60, 18);
  copie_large(texte, sp + 176);
  uint32_t curseur = donnees(sp + 176);
  ctx.r3.u64 = curseur;
  sub_821AC4F8(ctx, base);
  const uint32_t longueur = ctx.r3.u32;
  bool balises = false, sans_couleur = false;
  ctx.fpscr.disableFlushMode();
  if (base[self + 1028]) {
    const float zero = LoadFloat(base, kConstants + 68);
    sans_couleur = LoadFloat(base, self + 580) == zero && LoadFloat(base, self + 588) == zero && LoadFloat(base, self + 584) == zero;
    ctx.r3.u64 = texte;
    ctx.r4.u64 = self + 724;
    ctx.r5.u64 = 0;
    sub_821A5CE0(ctx, base);
    balises = ctx.r3.s32 != -1;
    if (!balises) {
      ctx.r3.u64 = texte;
      ctx.r4.u64 = self + 876;
      ctx.r5.u64 = 0;
      sub_821A5CE0(ctx, base);
      balises = ctx.r3.s32 != -1;
    }
  }
  if (longueur) {
    const float z = LoadFloat(base, sp + 104);
    float x = LoadFloat(base, sp + 96);
    uint32_t i = 0, i_ferme = lg_ferme, rang = 0;
    do {
      if (balises) {
        bool pris = false;
        if (i + lg_ouvre + 8 <= longueur) {
          ctx.r3.u64 = curseur;
          ctx.r4.u64 = ouvre;
          ctx.r5.u64 = lg_ouvre;
          sub_821B39E0(ctx, base);
          if (!ctx.r3.u32) {
            i += lg_ouvre;
            i_ferme += lg_ouvre;
            curseur += lg_ouvre * 2;
            rang += lg_ouvre * 4;
            if (!sans_couleur) {
              std::memcpy(base + sp + 240, base + curseur, 16);
              Store32(base, sp + 80, 0);
              ctx.r3.u64 = sp + 272;
              ctx.r4.u64 = sp + 240;
              sub_821AC490(ctx, base);
              ctx.r3.u64 = sp + 304;
              ctx.r4.u64 = sp + 272;
              sub_825B4530(ctx, base);
              ctx.r3.u64 = sp + 272;
              sub_821B3988(ctx, base);
              const uint32_t flux = sp + 304 + Load32(base, Load32(base, sp + 304) + 4);
              Store32(base, flux + 20, (Load32(base, flux + 20) & ~0xE00u) | 0x800u);
              ctx.r3.u64 = sp + 304;
              ctx.r4.u64 = sp + 80;
              sub_825B41C8(ctx, base);
              ctx.r3.u64 = sp + 112;
              ctx.r4.u64 = Load32(base, sp + 80);
              sub_825AB530(ctx, base);
              ctx.fpscr.disableFlushMode();
              const float c = LoadFloat(base, self + 576);
              ctx.r3.u64 = self + 528;
              ctx.r4.u64 = sp + 112;
              sub_825D56F8(ctx, base);
              const uint32_t q = ctx.r3.u32;
              for (uint32_t k = 0; k < 4; k++) {
                StoreFloat(base, q + k * 24, c);
                base[q + k * 24 + 20] = 0;
              }
              ctx.r3.u64 = sp + 392;
              sub_825B4600(ctx, base);
              Store32(base, sp + 392, 0x820AE1D0);
              ctx.r3.u64 = sp + 392;
              sub_821B1EE8(ctx, base);
            }
            i += 8;
            i_ferme += 8;
            curseur += 16;
            rang += 32;
            pris = true;
          }
        }
        if (!pris && i_ferme <= longueur) {
          ctx.r3.u64 = curseur;
          ctx.r4.u64 = ferme;
          ctx.r5.u64 = lg_ferme;
          sub_821B39E0(ctx, base);
          if (!ctx.r3.u32) {
            i += lg_ferme;
            i_ferme += lg_ferme;
            curseur += lg_ferme * 2;
            rang += lg_ferme * 4;
            if (!sans_couleur) {
              ctx.r3.u64 = self + 528;
              ctx.r4.u64 = self + 624;
              sub_821AF228(ctx, base);
            }
          }
        }
      }
      if (i < longueur) {
        const uint32_t t = Load32(base, texte + 4) > 32 ? Load32(base, texte + 148) : texte + 20;
        ctx.r3.u64 = self;
        ctx.r4.u64 = Load32(base, t + rang);
        sub_821B3C88(ctx, base);
        const uint32_t glyphe = ctx.r3.u32;
        if (glyphe) {
          ctx.fpscr.disableFlushMode();
          const uint32_t g = Load32(base, glyphe);
          StoreFloat(base, sp + 104, x);
          const float h = LoadFloat(base, g + 40);
          const float py = y0 - FnmsLikeGuest(h, float(ey), h);
          StoreFloat(base, sp + 96, py);
          StoreFloat(base, sp + 108, float(double(LoadFloat(base, g + 28)) * ex) + x);
          StoreFloat(base, sp + 100, float(double(LoadFloat(base, g + 32)) * ey) + py);
          ctx.r3.u64 = g;
          ctx.r4.u64 = sp + 96;
          ctx.r6.u64 = r6;
          ctx.r7.u64 = self + 528;
          ctx.f1.f64 = double(z);
          sub_821BA438(ctx, base);
          ctx.fpscr.disableFlushMode();
          x = float(std::fma(double(LoadFloat(base, glyphe + 4)), ex, double(x)));
        }
      }
      i++;
      i_ferme++;
      curseur += 2;
      rang += 4;
    } while (i < longueur);
  }
  auto liberer = [&](uint32_t s) {
    const uint32_t capacite = Load32(base, s + 20);
    Store32(base, s + 16, 0);
    Store32(base, s + 20, 7);
    if (capacite >= 8 && Load32(base, s)) {
      ctx.r3.u64 = Load32(base, s);
      sub_821CD9A8(ctx, base);
    }
    Store16(base, s, 0);
  };
  liberer(sp + 176);
  liberer(sp + 144);
  const uint32_t capacite = Load32(base, sp + 228);
  if (capacite >= 8 && Load32(base, sp + 208)) {
    ctx.r3.u64 = Load32(base, sp + 208);
    sub_821CD9A8(ctx, base);
  }
}

// ======================================================================================================================
// Les 50 suivantes du thread principal (mêmes règles). Laissées de côté : sub_82883C50, sub_8287F408, sub_8287F090 et sub_82867A10
// (attente et fichiers de la bibliothèque C du jeu : le temps est dans le noyau), sub_821D5410 (1 400 instructions d'événements
// d'animation froids, que les sessions d'essai ne couvrent presque pas).
// ======================================================================================================================
REX_EXTERN(__imp__sub_821B93B0);
REX_EXTERN(__imp__sub_821C09C8);
REX_EXTERN(__imp__sub_821C14B8);
REX_EXTERN(__imp__sub_821A7AF0);
REX_EXTERN(__imp__sub_821AEB70);
REX_EXTERN(__imp__sub_821CF3C0);
REX_EXTERN(__imp__sub_821CF690);
REX_EXTERN(__imp__sub_821C5128);
REX_EXTERN(__imp__sub_821D1610);
REX_EXTERN(__imp__sub_821DA848);
REX_EXTERN(__imp__sub_821C4DF8);
REX_EXTERN(__imp__sub_821CD9A8);
REX_EXTERN(__imp__sub_8241F820);
REX_EXTERN(__imp__sub_821A5058);
REX_EXTERN(__imp__sub_821C2C38);
REX_EXTERN(__imp__sub_821CEB60);
REX_EXTERN(__imp__sub_821D2620);
REX_EXTERN(__imp__sub_821A71E8);
REX_EXTERN(__imp__sub_82446FB0);
REX_EXTERN(__imp__sub_8246FD00);
REX_EXTERN(__imp__sub_823E74E8);
REX_EXTERN(__imp__sub_821BA660);
REX_EXTERN(__imp__sub_821C12C0);
REX_EXTERN(__imp__sub_821C3198);
REX_EXTERN(__imp__sub_821BEF28);
REX_EXTERN(__imp__sub_821E1CC0);
REX_EXTERN(__imp__sub_821CCAB0);
REX_EXTERN(__imp__sub_821C78E8);
REX_EXTERN(sub_821CC498);
REX_EXTERN(sub_821CB3C0);
REX_EXTERN(sub_821BE630);
REX_EXTERN(sub_821C1338);
REX_EXTERN(sub_821C1398);
REX_EXTERN(sub_821A7B30);
REX_EXTERN(sub_821AEAF8);
REX_EXTERN(sub_821AEE40);
REX_EXTERN(sub_821C9F68);
REX_EXTERN(sub_82469B00);
REX_EXTERN(sub_821FE150);
REX_EXTERN(sub_8242FEF0);
REX_EXTERN(sub_821C7E30);
REX_EXTERN(sub_82446EE8);
REX_EXTERN(sub_821D2900);
REX_EXTERN(sub_821D1840);
REX_EXTERN(sub_821D29D8);
REX_EXTERN(sub_821D18C0);
REX_EXTERN(sub_821DE398);
REX_EXTERN(sub_821DA9E8);
REX_EXTERN(sub_821DAB40);
REX_EXTERN(sub_821C4798);
REX_EXTERN(sub_821A4FD0);
REX_EXTERN(sub_8241F7D8);
REX_EXTERN(sub_821CCF30);
REX_EXTERN(sub_821D2698);
REX_EXTERN(sub_821A70D8);
REX_EXTERN(sub_821A7138);
REX_EXTERN(sub_821A7128);
REX_EXTERN(sub_821BA5C0);
REX_EXTERN(sub_821BA610);
REX_EXTERN(sub_821C1260);
REX_EXTERN(sub_821C1608);
REX_EXTERN(sub_8244C798);
REX_EXTERN(sub_821CC998);
REX_EXTERN(sub_82582F58);
REX_EXTERN(sub_821B17D0);
REX_EXTERN(sub_821E1828);
REX_EXTERN(sub_821E1BC0);
REX_EXTERN(sub_821C7EA0);
REX_EXTERN(sub_821C7698);

// sub_821B93B0 (r3 entité) : Entity::cacheBoneMatrices : pose (case 252) puis matrices (case 264), une fois par image (repère +396).
REX_HOOK_RAW(sub_821B93B0) {
  if (!Pilote()) return __imp__sub_821B93B0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const uint32_t image = Load32(base, Load32(base, 0x835067D4) + 224);
  CadreInvite cadre(ctx, base, 112);
  if (Load32(base, Load32(base, self + 396)) == image) {
    const uint32_t s = Load32(base, self + 444);
    if (!s) return;
    TL_V(s, 308);
    if (!(ctx.r3.u32 & 0xFF)) return;
  }
  if (!base[self + 440] && Load32(base, Load32(base, self + 396)) != image) {
    ctx.r4.u64 = Load32(base, self + 208);
    TL_M(Load32(base, self + 444), 252, 821CC498);
  }
  ctx.r4.u64 = Load32(base, self + 384);
  TL_M(Load32(base, self + 444), 264, 821CB3C0);
  Store32(base, Load32(base, self + 396), image);
}

REX_HOOK_RAW(sub_821C09C8) {
  if (!Pilote()) return __imp__sub_821C09C8(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 96);
  sub_821C9F68(ctx, base);
  TL_M(self, 316, 821BE630);
}

// sub_821C14B8 (r3 système, r4 programme) : bindGpuProgram : oublie les constantes (+17328, masque +17332) et lie.
REX_HOOK_RAW(sub_821C14B8) {
  if (!Pilote()) return __imp__sub_821C14B8(ctx, base);
  const uint32_t self = ctx.r3.u32, programme = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 112);
  Store32(base, self + 17328, 0);
  ctx.r4.u64 = 0;
  TL_M(Load32(base, self + 17128), 16, 821C1338);
  Store16(base, self + 17332, 0xFFFF);
  ctx.r4.u64 = programme;
  TL_M(Load32(base, self + 80), 360, 821C1398);
}

REX_HOOK_RAW(sub_821A7AF0) {
  if (!Pilote()) return __imp__sub_821A7AF0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 96);
  TL_M(self, 4, 821A7B30);
  base[self + 12] = 0;
}

namespace {
// sub_821AEB70 : mise à jour d'une fenêtre de CEGUI (case 228), événement, puis ses enfants (+44).
void MajFenetre(PPCContext& ctx, uint8_t* base, uint32_t self, double dt) {
  ctx.f1.f64 = dt;
  TL_M(self, 228, 821AEAF8);
  const uint32_t e = ctx.r1.u32 + 80;
  Store32(base, e, 0x821012DC);
  base[e + 4] = 0;
  base[e + 5] = 1;
  Store32(base, e + 8, self);
  StoreFloat(base, e + 12, float(dt));
  ctx.r3.u64 = self + 20;
  ctx.r4.u64 = 0x834B1670;
  ctx.r5.u64 = e;
  ctx.r6.u64 = 0x834B15D8;
  const uint32_t f = Slot(base, self + 20, 20);
  TL_PROMU(f, 821AEE40);
  for (uint32_t i = 0; i < uint32_t(int32_t(Load32(base, self + 48) - Load32(base, self + 44)) >> 2); i++) {
    MajFenetre(ctx, base, Load32(base, Load32(base, self + 44) + i * 4), dt);
  }
}

// sub_821CF690 : collection de la file : liste triée (bit 2 : couple renderable, passe en +24) et groupes par passe (bit 1 : map en +4).
void AjouterDansCollection(PPCContext& ctx, uint8_t* base, uint32_t coll, uint32_t passe, uint32_t rend) {
  const uint32_t sp = ctx.r1.u32;
  if (base[coll] & 2) {
    Store32(base, sp + 80, rend);
    Store32(base, sp + 84, passe);
    ctx.r3.u64 = coll + 24;
    ctx.r4.u64 = sp + 80;
    sub_82469B00(ctx, base);
  }
  if (!(base[coll] & 1)) return;
  uint32_t it = ChercherArbre(base, coll + 4, passe);
  if (it == Load32(base, coll + 8)) {
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 16;
    sub_821CD7F8(ctx, base);
    const uint32_t v = ctx.r3.u32;
    if (v) {
      Store32(base, v + 12, 0x820DD088);
      Store32(base, v, 0);
      Store32(base, v + 4, 0);
      Store32(base, v + 8, 0);
    }
    Store32(base, sp + 80, passe);
    Store32(base, sp + 84, v);
    ctx.r3.u64 = sp + 88;
    ctx.r4.u64 = coll + 4;
    ctx.r5.u64 = sp + 80;
    sub_821FE150(ctx, base);
    it = Load32(base, ctx.r3.u32);
  }
  const uint32_t vecteur = Load32(base, it + 16);
  const uint32_t fin = Load32(base, vecteur + 4);
  if (fin && fin != Load32(base, vecteur + 8)) {
    Store32(base, fin, rend);
    Store32(base, vecteur + 4, fin + 4);
  } else {
    Store32(base, sp + 164, rend);
    ctx.r3.u64 = vecteur;
    ctx.r4.u64 = sp + 164;
    sub_82480518(ctx, base);
  }
}
}  // namespace

REX_HOOK_RAW(sub_821AEB70) {
  if (!Pilote()) return __imp__sub_821AEB70(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 144);
  MajFenetre(ctx, base, self, dt);
}

REX_HOOK_RAW(sub_821CF690) {
  if (!Pilote()) return __imp__sub_821CF690(ctx, base);
  const uint32_t coll = ctx.r3.u32, passe = ctx.r4.u32, rend = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 128);
  AjouterDansCollection(ctx, base, coll, passe, rend);
}

// sub_821CF3C0 (r3 groupe de priorité, r4 renderable, r5 technique) : addRenderable : solide, décalque ou transparent, par passe.
REX_HOOK_RAW(sub_821CF3C0) {
  if (!Pilote()) return __imp__sub_821CF3C0(ctx, base);
  const uint32_t self = ctx.r3.u32, rend = ctx.r4.u32, tech = ctx.r5.u32;
  const uint32_t p0 = Load32(base, tech), p1 = Load32(base, tech + 4);
  const uint32_t premiere = p0 != p1 ? Load32(base, p0) : 0;
  CadreInvite cadre(ctx, base, 128);
  auto toutes = [&](uint32_t coll) {
    for (uint32_t it = p0; it != p1; it += 4) AjouterDansCollection(ctx, base, coll, Load32(base, it), rend);
  };
  bool transparent = premiere && base[premiere + 195];
  if (!transparent && premiere) {
    const int32_t k = int32_t(Load32(base, premiere + 136));
    if (int32_t(Load32(base, premiere + 140)) != 1 || k == 2 || k == 4 || k == 6 || k == 8) {
      transparent = !base[premiere + 166] || !base[premiere + 165] || !base[premiere + 184];
    }
  }
  if (transparent) {
    toutes(!premiere || base[premiere + 194] ? self + 208 : self + 168);
    return;
  }
  const uint32_t reglages = Load32(base, self);
  if (base[self + 5] && base[reglages + 28]) {
    toutes(self + 128);
    return;
  }
  if (base[self + 4] && base[reglages + 28]) {
    if (int32_t(Load32(base, tech + 40)) != 1) {
      Store32(base, tech + 40, 0xFFFFFFFF);
      ctx.r3.u64 = tech;
      sub_8242FEF0(ctx, base);
      Store32(base, tech + 40, 1);
    }
    const uint32_t f = Load32(base, tech + 20);
    for (uint32_t it = Load32(base, tech + 16); it != f; it += 4) {
      const uint32_t e = Load32(base, it);
      const uint32_t k = Load32(base, e);
      const uint32_t coll = k < 1 ? self + 8 : k == 1 ? self + 48 : k < 3 ? self + 88 : 0;
      AjouterDansCollection(ctx, base, coll, Load32(base, e + 4), rend);
    }
    return;
  }
  toutes(self + 8);
}

// sub_821C5128 (r3 unité, r4 image) : charge la texture : déjà créée, simple load (case 60) ; sinon l'original.
REX_HOOK_RAW(sub_821C5128) {
  if (!Pilote()) return __imp__sub_821C5128(ctx, base);
  const uint32_t self = ctx.r3.u32, image = ctx.r4.u32;
  if (!Load32(base, Load32(base, self + 332) + image * 28 + 16) || base[self + 208]) return;
  const uint32_t t = Load32(base, Load32(base, self + 348) + (image << 4) + 4);
  if (!t) return __imp__sub_821C5128(ctx, base);
  CadreInvite cadre(ctx, base, 160);
  ctx.r4.u64 = 0;
  TL_M(t, 60, 821C7EA0);
}

// sub_821D1610 (r3 scène, r4 position, f1 rayon, r6 liste, r7 masque) : findLightsAffectingFrustum d'un objet : distance de chaque lumière
// (+172), tri stable, index (+268).
REX_HOOK_RAW(sub_821D1610) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821D1610(ctx, base);
  const uint32_t self = ctx.r3.u32, pos = ctx.r4.u32, liste = ctx.r6.u32, masque = ctx.r7.u32;
  const double rayon = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 208);
  const uint32_t sp = ctx.r1.u32;
  TL_M(self, 240, 821D18C0);
  const uint32_t lumieres = ctx.r3.u32;
  Store32(base, liste + 4, Load32(base, liste));
  Store32(base, liste + 16, 0);
  base[liste + 20] = 0;
  ctx.r3.u64 = liste;
  ctx.r4.u64 = uint32_t(int32_t(Load32(base, lumieres + 4) - Load32(base, lumieres)) >> 2);
  sub_82446EE8(ctx, base);
  for (uint32_t it = Load32(base, lumieres); it != Load32(base, lumieres + 4); it += 4) {
    const uint32_t l = Load32(base, it);
    Store32(base, sp + 84, l);
    TL_V(l, 184);
    if (!(ctx.r3.u32 & masque)) continue;
    const bool directionnelle = int32_t(Load32(base, l + 176)) == 1;
    if (directionnelle) {
      Store32(base, l + 172, Load32(base, kConstants + 68));
    } else {
      TL_V(l, 216);
      ctx.fpscr.disableFlushMode();
      const float dy = LoadFloat(base, pos + 4) - LoadFloat(base, l + 276);
      const float dz = LoadFloat(base, pos + 8) - LoadFloat(base, l + 280);
      const float dx = LoadFloat(base, pos) - LoadFloat(base, l + 272);
      const float d2 = FmaLikeGuest(dx, dx, FmaLikeGuest(dz, dz, dy * dy));
      StoreFloat(base, l + 172, d2);
      const float portee = float(double(LoadFloat(base, l + 248)) + rayon);
      if (d2 > portee * portee) continue;
    }
    ctx.r3.u64 = liste;
    ctx.r4.u64 = sp + 84;
    sub_821D2900(ctx, base);
    if (!base[liste + 20]) {
      ctx.r3.u64 = liste;
      ctx.r4.u64 = sp + 84;
      sub_821D1840(ctx, base);
    }
  }
  const uint32_t a = Load32(base, liste), b = Load32(base, liste + 4);
  base[liste + 20] = 1;
  if (a != b) {
    const int32_t n = int32_t(b - a) >> 2;
    Store32(base, sp + 96, 0);
    Store32(base, sp + 100, 0);
    Store32(base, sp + 104, 0);
    Store32(base, sp + 108, uint32_t((n + 1) / 2));
    Store32(base, sp + 112, sp + 96);
    ctx.r3.u64 = a;
    ctx.r4.u64 = b;
    ctx.r5.u64 = uint32_t(n);
    ctx.r6.u64 = sp + 96;
    ctx.r7.u64 = 0;
    sub_821D29D8(ctx, base);
    const uint32_t t = Load32(base, sp + 96);
    if (t) {
      ctx.r3.u64 = t;
      sub_821CD9A8(ctx, base);
    }
  }
  base[liste + 20] = 1;
  uint32_t numero = 0;
  for (uint32_t it = Load32(base, liste); it != Load32(base, liste + 4); it += 4) Store32(base, Load32(base, it) + 268, numero++);
}

// sub_821DA848 (r3 ensemble, r4 billboard) : injectBillboard ; coins par sub_821DA788, sommets par sub_821DAB40. Sur les threads de travail
// avec --torchlight_billboards.
REX_HOOK_RAW(sub_821DA848) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821DA848(ctx, base);
  const uint32_t self = ctx.r3.u32, bb = ctx.r4.u32;
  if (Load16(base, self + 580) == Load32(base, self + 584)) return;
  CadreInvite cadre(ctx, base, 176);
  const uint32_t sp = ctx.r1.u32;
  if (base[self + 532]) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = Load32(base, self + 444);
    ctx.r5.u64 = bb;
    sub_821DA9E8(ctx, base);
    if (!(ctx.r3.u32 & 0xFF)) return;
  }
  auto coins = [&](float l, float h, uint32_t sortie) {
    ctx.fpscr.disableFlushMode();
    const float a = LoadFloat(base, self + 448) * l, b = LoadFloat(base, self + 452) * l;
    const float c = LoadFloat(base, self + 456) * h, d = LoadFloat(base, self + 460) * h;
    const V3 x = LireV3(base, self + 464), y = LireV3(base, self + 476);
    const float cs[2] = {c, d}, ab[2] = {a, b};
    uint32_t o = sortie;
    for (int j = 0; j < 2; j++) {
      for (int i = 0; i < 2; i++) {
        EcrireV3(base, o, {y.x * cs[j] + x.x * ab[i], y.y * cs[j] + x.y * ab[i], y.z * cs[j] + x.z * ab[i]});
        o += 12;
      }
    }
  };
  const bool propre = !base[self + 582];
  const int32_t k = int32_t(Load32(base, self + 552));
  const bool axes = k == 2 || k == 4 || (base[self + 319] && k != 3);
  if (propre && axes) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = self + 464;
    ctx.r5.u64 = self + 476;
    ctx.r6.u64 = bb;
    sub_821DE398(ctx, base);
  }
  uint32_t decalages = self + 396;
  if (!base[self + 316] && propre) {
    if (axes || base[bb]) {
      decalages = sp + 96;
      coins(LoadFloat(base, bb + 20), LoadFloat(base, bb + 24), decalages);
    }
  } else if (propre && axes) {
    coins(LoadFloat(base, self + 264), LoadFloat(base, self + 268), self + 396);
  }
  ctx.r3.u64 = self;
  ctx.r4.u64 = decalages;
  ctx.r5.u64 = bb;
  sub_821DAB40(ctx, base);
  Store16(base, self + 580, Load16(base, self + 580) + 1);
}

// sub_821C4DF8 (r3 objet, r4 caméra) : getSquaredViewDepth.
REX_HOOK_RAW(sub_821C4DF8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821C4DF8(ctx, base);
  const uint32_t self = ctx.r3.u32, camera = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 112);
  TL_M(camera, 212, 821C4798);
  TL_M(self, 192, 821C8518);
  ctx.fpscr.disableFlushMode();
  const uint32_t p = ctx.r3.u32;
  const float dz = LoadFloat(base, p + 8) - LoadFloat(base, camera + 1092);
  const float dx = LoadFloat(base, p) - LoadFloat(base, camera + 1084);
  const float dy = LoadFloat(base, p + 4) - LoadFloat(base, camera + 1088);
  ctx.f1.f64 = double(FmaLikeGuest(dy, dy, FmaLikeGuest(dx, dx, dz * dz)));
}

// sub_821CD9A8 (r3 bloc) : nedpfree : cache du thread (sub_8241F820) ; le reste par l'original.
REX_HOOK_RAW(sub_821CD9A8) {
  if (!Pilote()) return __imp__sub_821CD9A8(ctx, base);
  const uint32_t bloc = ctx.r3.u32;
  constexpr uint32_t kReserve = 0x83558F90;
  if (!bloc || !Load32(base, kReserve + 16)) return __imp__sub_821CD9A8(ctx, base);
  ctx.r3.u64 = Load32(base, kReserve + 1044);
  __imp__KeTlsGetValue(ctx, base);
  const int32_t numero = ctx.r3.s32;
  ctx.r3.u64 = bloc;
  if (numero <= 0) return __imp__sub_821CD9A8(ctx, base);
  const uint32_t tc = Load32(base, kReserve + 16 + uint32_t(numero) * 4);
  const uint32_t entete = Load32(base, bloc - 4);
  const uint32_t taille = (entete & 2) ? (entete & ~7u) - 8 : 0;
  if (!tc || taille > 8200) return __imp__sub_821CD9A8(ctx, base);
  const uint32_t indice = 31 - uint32_t(__builtin_clz(taille >> 4));
  const uint32_t meilleure = 1u << (indice + 4);
  const uint32_t case_ = tc + (indice + 3) * 8;
  const uint32_t tete = Load32(base, case_);
  if (bloc == tete) return __imp__sub_821CD9A8(ctx, base);  // double libération : message du jeu
  const uint32_t frees = Load32(base, tc + 12) + 1;
  Store32(base, tc + 12, frees);
  Store32(base, bloc, frees);
  Store32(base, bloc + 4, meilleure);
  Store32(base, bloc + 8, tete);
  Store32(base, bloc + 12, 0);
  if (tete) {
    Store32(base, tete + 12, bloc);
  } else {
    Store32(base, case_ + 4, bloc);
  }
  Store32(base, case_, bloc);
  const uint32_t cumul = Load32(base, tc + 20) + meilleure;
  Store32(base, tc + 20, cumul);
  if (cumul >= 0x80000) {
    CadreInvite cadre(ctx, base, 128);
    ctx.r3.u64 = tc;
    sub_8241F7D8(ctx, base);
  }
}

// sub_821A5058 (r3 caméra, r4 et r5 éléments) : comparateur du tri des renderables transparents : par profondeur, puis par adresse de passe.
REX_HOOK_RAW(sub_821A5058) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821A5058(ctx, base);
  const uint32_t self = ctx.r3.u32, ea = ctx.r4.u32, eb = ctx.r5.u32;
  const uint32_t a = Load32(base, ea), b = Load32(base, eb);
  if (a == b) {
    ctx.r3.u64 = Load32(base, Load32(base, ea + 4) + 52) < Load32(base, Load32(base, eb + 4) + 52) ? 1 : 0;
    return;
  }
  CadreInvite cadre(ctx, base, 128);
  ctx.r4.u64 = Load32(base, self);
  TL_M(a, 32, 821A4FD0);
  const double da = ctx.f1.f64;
  ctx.r4.u64 = Load32(base, self);
  TL_M(b, 32, 821A4FD0);
  ctx.fpscr.disableFlushMode();
  const double db = ctx.f1.f64;
  if (!(std::fabs(float(db - da)) > LoadFloat(base, kConstants + 224))) {
    ctx.r3.u64 = Load32(base, ea + 4) < Load32(base, eb + 4) ? 1 : 0;
  } else {
    ctx.r3.u64 = da > db ? 1 : 0;
  }
}

REX_HOOK_RAW(sub_821C2C38) {
  if (!Pilote() || base[ctx.r3.u32 + 19169]) return __imp__sub_821C2C38(ctx, base);
  ctx.r3.u64 = ctx.r3.u32 + 16536;
}

// sub_821CEB60 (r3 périphérique, r4 étage, r5 texture, r6 masque) : SetTexture du Direct3D Xbox (constantes de lecture de 24 octets à partir de
// (étage + 48) * 24). La fence de la texture remplacée, si elle doit être notée, reste à l'original.
REX_HOOK_RAW(sub_821CEB60) {
  if (!Pilote()) return __imp__sub_821CEB60(ctx, base);
  const uint32_t dev = ctx.r3.u32, etage = ctx.r4.u32, tex = ctx.r5.u32;
  const uint32_t lien = dev + (etage + 3198) * 4;
  const uint32_t precedente = Load32(base, lien);
  const uint32_t en_cours = Load32(base, dev + 11036);
  if (precedente && !en_cours && (Load32(base, dev + 11040) & Load32(base, precedente))) return __imp__sub_821CEB60(ctx, base);
  const uint32_t f = dev + (etage + 48) * 24;
  if (tex) {
    const uint32_t w48 = Load32(base, tex + 48), w32 = Load32(base, tex + 32), w44 = Load32(base, tex + 44);
    const uint32_t a0 = (Load32(base, tex + 28) & ~0x003FFC00u) | (Load32(base, f) & 0x003FFC00u);
    const uint32_t a1 = ((((((w32 >> 20) & 0xFFF) + 512) & 0x1000) + (w32 & 0x1FFFFFFF)) & ~0x800u) | (Load32(base, f + 4) & 0x800u);
    const uint32_t a3 = (Load32(base, tex + 40) & ~0x7FF80000u) | (Load32(base, f + 12) & 0x7FF80000u);
    const uint32_t a5 = ((((((w48 >> 20) & 0xFFF) + 512) & 0x1000) + (w48 & 0x1FFFFE00)) & ~0x1FFu) | (Load32(base, f + 20) & 0x1FFu);
    uint32_t a4 = (w44 & 0x3FCu) | (Load32(base, f + 16) & 0xFFFFFC03u);
    const uint32_t mip_max = base[dev + etage + 12228], mip_min = base[dev + etage + 12254];
    const uint32_t x = (w44 >> 2) & 0xF;
    const uint32_t m1 = x > mip_max ? x : mip_max;
    const uint32_t y = (w44 >> 6) & 0xF;
    const uint32_t m2 = y < mip_min ? y : mip_min;
    a4 = (a4 & ~0x3Cu) | ((m1 << 2) & 0x3Cu);
    a4 = (a4 & ~0x3C0u) | ((m2 << 6) & 0x3C0u);
    Store32(base, f + 8, Load32(base, tex + 36));
    Store32(base, f, a0);
    Store32(base, f + 4, a1);
    Store32(base, f + 12, a3);
    Store32(base, f + 16, a4);
    Store32(base, f + 20, a5);
    const uint64_t etats = Load64(base, dev + 24) | ctx.r6.u64;
    const uint64_t be = __builtin_bswap64(etats);
    std::memcpy(base + dev + 24, &be, 8);
  } else {
    Store32(base, f, Load32(base, f) & 0xFFFFFFFCu);
  }
  Store32(base, lien, tex);
  if (precedente && en_cours) Store32(base, precedente + 8, en_cours);
}

REX_HOOK_RAW(sub_821D2620) {
  if (!Pilote()) return __imp__sub_821D2620(ctx, base);
  const uint32_t self = ctx.r3.u32, sortie = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 96);
  TL_M(Load32(base, Load32(base, Load32(base, self + 68) + 4) + 4), 136, 821D2698);
  std::memcpy(base + sortie, base + ctx.r3.u32, 64);
}

// sub_821A71E8 (r3 tampon d'Ogre, r4 source, r5 tampon du périphérique) : recopie paresseuse (Lock, copie, Unlock).
REX_HOOK_RAW(sub_821A71E8) {
  if (!Pilote()) return __imp__sub_821A71E8(ctx, base);
  const uint32_t self = ctx.r3.u32, source = ctx.r4.u32, v = ctx.r5.u32;
  const int32_t usage = int32_t(Load32(base, v + 16));
  const uint32_t drapeaux = Load32(base, self + 8);
  uint32_t r6 = 0;
  if (usage == 2) {
    if (!(drapeaux & 4)) r6 = 16;
  } else if (usage == 3 && (drapeaux & 2) && !(drapeaux & 8)) {
    r6 = 4096;
  }
  CadreInvite cadre(ctx, base, 128);
  ctx.r3.u64 = Load32(base, v);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = r6;
  sub_821A70D8(ctx, base);
  const uint32_t d = Load32(base, v + 8);
  ctx.r3.u64 = uint32_t(d + ctx.r3.u32);
  ctx.r4.u64 = uint32_t(d + source);
  ctx.r5.u64 = Load32(base, v + 12);
  sub_821A7138(ctx, base);
  ctx.r3.u64 = Load32(base, v);
  sub_821A7128(ctx, base);
  base[v + 4] = 0;
  Store32(base, v + 8, Load32(base, self + 4));
  Store32(base, v + 12, 0);
  Store32(base, v + 16, 0);
  ctx.r3.u64 = 1;
}

namespace {
// sub_821C0298 de deux std::string : ordre.
int ComparerChaines(const uint8_t* base, uint32_t a, uint32_t b) {
  const uint32_t na = Load32(base, a + 16), nb = Load32(base, b + 16);
  const int r = std::memcmp(base + DonneesChaine(base, a), base + DonneesChaine(base, b), na < nb ? na : nb);
  if (r) return r;
  return na < nb ? -1 : (na != nb ? 1 : 0);
}

// lower_bound d'une map<string, ...> (nœud : chaîne +12, _Isnil +45).
uint32_t BorneChaine(const uint8_t* base, uint32_t tete, uint32_t cle) {
  uint32_t resultat = tete;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 45];) {
    if (ComparerChaines(base, n + 12, cle) < 0) {
      n = Load32(base, n + 8);
    } else {
      resultat = n;
      n = Load32(base, n);
    }
  }
  return resultat;
}
}  // namespace

REX_HOOK_RAW(sub_82446FB0) {
  if (!PiloteOuValidation()) return __imp__sub_82446FB0(ctx, base);
  const uint32_t mots[1] = {BorneChaine(base, Load32(base, ctx.r4.u32 + 4), ctx.r5.u32)};
  const uint32_t sortie = ctx.r3.u32;
  FinMots(ctx, base, __imp__sub_82446FB0, "82446FB0", sortie, mots, 1);
  ctx.r3.u64 = sortie;
}

REX_HOOK_RAW(sub_8246FD00) {
  if (!PiloteOuValidation()) return __imp__sub_8246FD00(ctx, base);
  const uint32_t tete = Load32(base, ctx.r4.u32 + 4), cle = ctx.r5.u32, sortie = ctx.r3.u32;
  uint32_t it = BorneChaine(base, tete, cle);
  if (it != tete && ComparerChaines(base, cle, it + 12) < 0) it = tete;
  const uint32_t mots[1] = {it};
  FinMots(ctx, base, __imp__sub_8246FD00, "8246FD00", sortie, mots, 1);
  ctx.r3.u64 = sortie;
}

// sub_823E74E8 (r3, r4 nom) : marque +48 de l'élément du nom et de l'objet.
REX_HOOK_RAW(sub_823E74E8) {
  if (!Pilote()) return __imp__sub_823E74E8(ctx, base);
  const uint32_t self = ctx.r3.u32, nom = ctx.r4.u32;
  const uint32_t fin = Load32(base, self + 36);
  for (uint32_t it = Load32(base, self + 32); it != fin; it += 4) {
    const uint32_t e = Load32(base, it);
    if (ChainesEgales(base, Load32(base, e + 4) + 4, nom)) {
      base[e + 48] = 1;
      break;
    }
  }
  base[self + 48] = 1;
}

// sub_821BA660 (r3 image, r4 uv, r5 rectangle, r7 découpe, r8 couleurs, f1 z) : Image::draw : rectangle coupé, uv recalculés, arrondi au pixel.
REX_HOOK_RAW(sub_821BA660) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821BA660(ctx, base);
  const uint32_t self = ctx.r3.u32, uv = ctx.r4.u32, rect = ctx.r5.u32, decoupe = ctx.r7.u32, couleurs = ctx.r8.u32;
  const double z = ctx.f1.f64;
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68);
  const float dr = LoadFloat(base, rect + 12), dl = LoadFloat(base, rect + 8), db = LoadFloat(base, rect + 4), dt_ = LoadFloat(base, rect);
  const float cr = LoadFloat(base, decoupe + 12), cl = LoadFloat(base, decoupe + 8), cb = LoadFloat(base, decoupe + 4), ct = LoadFloat(base, decoupe);
  if (!(dr > cl) || !(dl < cr) || !(db > ct) || !(dt_ < cb)) return;  // sub_821BA4C8 : rien à dessiner
  const float x0 = dl > cl ? dl : cl, x1 = dr < cr ? dr : cr, y0 = dt_ > ct ? dt_ : ct, y1 = db < cb ? db : cb;
  if (x1 - x0 == zero) return;
  CadreInvite cadre(ctx, base, 240);
  const uint32_t sp = ctx.r1.u32;
  const uint32_t texture = Load32(base, self + 168);
  TL_M(texture, 8, 821BA5C0);
  const double largeur = ctx.f1.f64;
  TL_M(Load32(base, self + 168), 20, 821BA610);
  ctx.fpscr.disableFlushMode();
  const double hauteur = ctx.f1.f64;
  const float u1 = LoadFloat(base, uv + 12), u0 = LoadFloat(base, uv + 8), v1 = LoadFloat(base, uv + 4), v0 = LoadFloat(base, uv);
  const float pu = (u1 - u0) / (dr - dl), pv = (v1 - v0) / (db - dt_);
  StoreFloat(base, sp + 120, float(double(FmaLikeGuest(x0 - dl, pu, u0)) * largeur));
  StoreFloat(base, sp + 124, float(double(FmaLikeGuest(x1 - dr, pu, u1)) * largeur));
  StoreFloat(base, sp + 116, float(double(FmaLikeGuest(y1 - db, pv, v1)) * hauteur));
  StoreFloat(base, sp + 112, float(double(FmaLikeGuest(y0 - dt_, pv, v0)) * hauteur));
  const float plus = LoadFloat(base, kConstants + 104), moins = LoadFloat(base, kConstants + 120);
  auto arrondi = [&](float v) { return float(double(FctiwzLikeGuest(double((v > zero ? plus : moins) + v)))); };
  StoreFloat(base, sp + 104, arrondi(x0));
  StoreFloat(base, sp + 108, arrondi(x1));
  StoreFloat(base, sp + 96, arrondi(y0));
  StoreFloat(base, sp + 100, arrondi(y1));
  const uint32_t r = Load32(base, Load32(base, self + 168) + 4);
  ctx.r4.u64 = sp + 96;
  ctx.r5.u64 = Load32(base, r);
  ctx.r6.u64 = Load32(base, self + 168);
  ctx.r7.u64 = sp + 112;
  ctx.r8.u64 = couleurs;
  ctx.r9.u64 = 0;
  ctx.f1.f64 = z;
  TL_M(r, 24, 821D3638);
}

REX_HOOK_RAW(sub_821C12C0) {
  if (!Pilote()) return __imp__sub_821C12C0(ctx, base);
  const uint32_t self = ctx.r3.u32, liste = ctx.r5.u32;
  // sub_821C1260 relu après : somme de contrôle de la liste.
  CadreInvite cadre(ctx, base, 112);
  ctx.r3.u64 = liste;
  sub_821C1260(ctx, base);
  if (ctx.r3.u32 == Load32(base, self + 17328)) return;
  ctx.r4.u64 = liste;
  TL_M(Load32(base, self + 17128), 16, 821C1338);
  Store16(base, self + 17332, Load16(base, self + 17332) | 4);
  ctx.r3.u64 = liste;
  sub_821C1260(ctx, base);
  Store32(base, self + 17328, ctx.r3.u32);
}

REX_HOOK_RAW(sub_821C3198) {
  if (!Pilote() || base[ctx.r3.u32 + 19166]) return __imp__sub_821C3198(ctx, base);
  ctx.r3.u64 = ctx.r3.u32 + 19064;
}

// sub_821BEF28 (r3 sortie, r4 matrice affine) : inverse.
REX_HOOK_RAW(sub_821BEF28) {
#pragma clang fp contract(off)
  if (!PiloteOuValidation()) return __imp__sub_821BEF28(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t m = ctx.r4.u32, sortie = ctx.r3.u32;
  auto e = [&](uint32_t i) { return LoadFloat(base, m + i * 4); };
  const float m0 = e(0), m1 = e(1), m2 = e(2), m3 = e(3), m4 = e(4), m5 = e(5), m6 = e(6), m7 = e(7), m8 = e(8), m9 = e(9), m10 = e(10), m11 = e(11);
  const float c0 = FmsLikeGuest(m8, m6, m10 * m4);   // f10
  const float c1 = FmsLikeGuest(m9, m4, m8 * m5);    // f6
  const float c2 = FmsLikeGuest(m10, m5, m9 * m6);   // f3
  const float det = FmaLikeGuest(m0, c2, FmaLikeGuest(m2, c1, m1 * c0));
  const float k = LoadFloat(base, kConstants + 64) / det;
  const float a0 = k * m0, a1 = k * m1, a2 = k * m2;
  const float r4 = k * c0, r0 = k * c2, r8 = k * c1;
  const float r9 = FmsLikeGuest(a1, m8, a0 * m9);
  const float r1 = FmsLikeGuest(a2, m9, a1 * m10);
  const float r5 = FmsLikeGuest(a0, m10, a2 * m8);
  const float r2 = FmsLikeGuest(a1, m6, a2 * m5);
  const float r6 = FmsLikeGuest(a2, m4, a0 * m6);
  const float r10 = FmsLikeGuest(a0, m5, a1 * m4);
  const float t9 = FmaLikeGuest(m11, r10, m7 * r9);
  const float t1 = FmaLikeGuest(m11, r2, m7 * r1);
  const float t5 = FmaLikeGuest(m11, r6, m7 * r5);
  const float t11 = -FmaLikeGuest(m3, r8, t9);
  const float t3 = -FmaLikeGuest(m3, r0, t1);
  const float t7 = -FmaLikeGuest(m3, r4, t5);
  const uint32_t z = Load32(base, kConstants + 68);
  const uint32_t mots[16] = {FloatBits(r0), FloatBits(r1), FloatBits(r2), FloatBits(t3), FloatBits(r4), FloatBits(r5), FloatBits(r6), FloatBits(t7),
                             FloatBits(r8), FloatBits(r9), FloatBits(r10), FloatBits(t11), z, z, z, Load32(base, kConstants + 64)};
  FinMots(ctx, base, __imp__sub_821BEF28, "821BEF28", sortie, mots, 16);
}

// sub_821E1CC0 (r3 système de particules à effets, f1 dt).
REX_HOOK_RAW(sub_821E1CC0) {
  if (!Pilote()) return __imp__sub_821E1CC0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 144);
  for (uint32_t i = 0; i < Load32(base, self + 176); i++) {
    const uint32_t d = Load32(base, self + 172);
    ctx.r3.u64 = Load32(base, i < Load32(base, self + 180) ? d + i * 4 : d);
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0x83038B88;
    ctx.r6.u64 = 0x83038AF8;
    ctx.r7.u64 = 0;
    sub_821E1828(ctx, base);
    if (ctx.r3.u32) {
      ctx.f1.f64 = dt;
      sub_821E1BC0(ctx, base);
    }
  }
}

// sub_821C78E8 (r3 Frustum..., f1 dt) : Node::_update des attachements : voir le jeu.
REX_HOOK_RAW(sub_821C78E8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821C78E8(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 128);
  base[self + 716] = 0;
  if (base[self + 744]) {
    ctx.fpscr.disableFlushMode();
    for (uint32_t i = 0; i < 3; i++) {
      const float mx = LoadFloat(base, self + 732 + i * 4), mn = LoadFloat(base, self + 720 + i * 4);
      StoreFloat(base, self + 720 + i * 4, mx < mn ? mx : mn);
    }
    for (uint32_t i = 0; i < 3; i++) {
      const float mn = LoadFloat(base, self + 720 + i * 4), mx = LoadFloat(base, self + 732 + i * 4);
      StoreFloat(base, self + 732 + i * 4, mn < mx ? mx : mn);
    }
    ctx.r3.u64 = self + 684;
    ctx.r4.u64 = self + 720;
    ctx.r5.u64 = self + 732;
    sub_821C7698(ctx, base);
  }
  auto tous = [&](uint32_t debut, uint32_t decalage) {
    const uint32_t f = Load32(base, debut + 4);
    for (uint32_t it = Load32(base, debut); it != f; it += 4) {
      ctx.r4.u64 = self;
      ctx.f1.f64 = dt;
      TL_V(Load32(base, it), decalage);
    }
  };
  auto actifs = [&](uint32_t debut, uint32_t octet, uint32_t decalage) {
    const uint32_t f = Load32(base, debut + 4);
    for (uint32_t it = Load32(base, debut); it != f; it += 4) {
      const uint32_t o = Load32(base, it);
      if (base[o + octet]) continue;
      TL_V(o, 12);
      if (!(ctx.r3.u32 & 0xFF)) continue;
      ctx.r4.u64 = self;
      ctx.f1.f64 = dt;
      TL_V(Load32(base, it), decalage);
    }
  };
  tous(self + 264, 40);
  actifs(self + 216, 164, 72);
  actifs(self + 232, 156, 92);
  tous(self + 584, 92);
  tous(self + 248, 36);
}

// ---------------------------------------------------------------------------------------------------------------------
// Les 50 suivantes, deuxième tranche.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821BD670);
REX_EXTERN(__imp__sub_821B5EF0);
REX_EXTERN(__imp__sub_821D26D0);
REX_EXTERN(__imp__sub_821C3660);
REX_EXTERN(__imp__sub_821A1608);
REX_EXTERN(__imp__sub_821BC5A0);
REX_EXTERN(__imp__sub_821B01B8);
REX_EXTERN(__imp__sub_821BDFA8);
REX_EXTERN(__imp__sub_821CCAB0);
REX_EXTERN(sub_821BD608);
REX_EXTERN(sub_821BD510);
REX_EXTERN(sub_821BD5A0);
REX_EXTERN(sub_821CF0F8);
REX_EXTERN(sub_821D26C8);
REX_EXTERN(sub_821D27C0);
REX_EXTERN(sub_821C35C0);
REX_EXTERN(sub_821C35B8);
REX_EXTERN(sub_821A1670);
REX_EXTERN(sub_821BC4D0);
REX_EXTERN(sub_821BC468);
REX_EXTERN(sub_821BC3F0);
REX_EXTERN(sub_821BC400);
REX_EXTERN(sub_821B0488);
REX_EXTERN(sub_821AF2F0);

// sub_821BD670 (r3 sortie, r4 lumière) : couleur = cases 184 et 192 additionnées, alpha de la case 176.
REX_HOOK_RAW(sub_821BD670) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821BD670(ctx, base);
  const uint32_t sortie = ctx.r3.u32, l = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 128);
  const uint32_t sp = ctx.r1.u32;
  TL_M(l, 184, 821BD608);
  const uint32_t a = ctx.r3.u32;
  ctx.r3.u64 = sp + 80;
  ctx.r4.u64 = l;
  {
    const uint32_t f = Slot(base, l, 192);
    TL_PROMU(f, 821BD510);
  }
  const uint32_t b = ctx.r3.u32;
  ctx.fpscr.disableFlushMode();
  for (uint32_t k = 0; k < 4; k++) StoreFloat(base, sortie + k * 4, LoadFloat(base, b + k * 4) + LoadFloat(base, a + k * 4));
  TL_M(l, 176, 821BD5A0);
  StoreFloat(base, sortie + 12, LoadFloat(base, ctx.r3.u32 + 12));
  ctx.r3.u64 = sortie;
}

// sub_821B5EF0 (r3) : map à clé 16 bits (+404, _Isnil +17) du schéma courant ; absent : l'original crée l'entrée.
REX_HOOK_RAW(sub_821B5EF0) {
  if (!Pilote()) return __imp__sub_821B5EF0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  {
    CadreInvite cadre(ctx, base, 128);
    TL_M(Load32(base, 0x83506794), 176, 821CF0F8);
  }
  const uint32_t cle = ctx.r3.u32 & 0xFFFF;
  const uint32_t tete = Load32(base, self + 408);
  uint32_t resultat = tete;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 17];) {
    if (Load16(base, n + 12) < cle) {
      n = Load32(base, n + 8);
    } else {
      resultat = n;
      n = Load32(base, n);
    }
  }
  if (resultat == tete || cle < Load16(base, resultat + 12)) {
    ctx.r3.u64 = self;
    return __imp__sub_821B5EF0(ctx, base);
  }
  ctx.r3.u64 = base[resultat + 14];
}

// sub_821D26D0 (r3 objet) : getLights : liste de la scène, refaite si elle a changé (+164).
REX_HOOK_RAW(sub_821D26D0) {
  if (!Pilote()) return __imp__sub_821D26D0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 128);
  const uint32_t ecouteur = Load32(base, self + 136);
  if (ecouteur) {
    ctx.r4.u64 = self;
    TL_V(ecouteur, 24);
    if (ctx.r3.u32) return;
  }
  if (base[self + 44]) {
    TL_V(Load32(base, Load32(base, self + 40) + 356), 180);
    return;
  }
  const uint32_t parent = Load32(base, self + 40);
  if (!parent) {
    ctx.r3.u64 = self + 140;
    sub_8219D110(ctx, base);
    ctx.r3.u64 = self + 140;
    return;
  }
  const uint32_t version = Load32(base, Load32(base, parent + 352) + 16928);
  if (Load32(base, self + 164) != version) {
    Store32(base, self + 164, version);
    TL_M(self, 184, 821D26C8);
    const uint32_t masque = ctx.r3.u32;
    TL_V(self, 80);
    ctx.r4.u64 = self + 140;
    ctx.r6.u64 = masque;
    TL_M(parent, 352, 821D27C0);
  }
  ctx.r3.u64 = self + 140;
}

// sub_821C3660 (r3 objet) : isVisible (+45, +56, +132) et filtre de visibilité (case 168) de la scène en cours (sub_821C35C0).
REX_HOOK_RAW(sub_821C3660) {
  if (!Pilote()) return __imp__sub_821C3660(ctx, base);
  const uint32_t self = ctx.r3.u32;
  if (!base[self + 45] || base[self + 56] || base[self + 132]) {
    ctx.r3.u64 = 0;
    return;
  }
  CadreInvite cadre(ctx, base, 96);
  ctx.r3.u64 = Load32(base, 0x835067D4);
  sub_821C35C0(ctx, base);
  const uint32_t scene = ctx.r3.u32;
  if (!scene) {
    ctx.r3.u64 = 1;
    return;
  }
  const uint32_t vp = Load32(base, scene + 168);
  const uint32_t masque = vp ? Load32(base, scene + 17172) & Load32(base, vp + 96) : Load32(base, scene + 17172);
  TL_M(self, 168, 821C35B8);
  ctx.r3.u64 = (ctx.r3.u32 & masque) ? 1 : 0;
}

// sub_821A1608 (r3, r4 clé) : valeur de la clé (map +4), sinon +80.
REX_HOOK_RAW(sub_821A1608) {
  if (!Pilote()) return __imp__sub_821A1608(ctx, base);
  const uint32_t self = ctx.r3.u32, cle = ctx.r4.u32;
  if (int32_t(cle) != -1 && Load32(base, self + 12)) {
    CadreInvite cadre(ctx, base, 112);
    const uint32_t sp = ctx.r1.u32;
    Store32(base, sp + 140, cle);
    ctx.r3.u64 = sp + 80;
    ctx.r4.u64 = self + 4;
    ctx.r5.u64 = sp + 140;
    sub_821A1670(ctx, base);
    const uint32_t it = Load32(base, sp + 80);
    if (it != Load32(base, self + 8)) {
      ctx.r3.u64 = it + 16;
      return;
    }
  }
  ctx.r3.u64 = self + 80;
}

// sub_821BC5A0 (r3 fenêtre) : entrées : pour chaque touche de la map (clé 8 bits), événements en attente.
REX_HOOK_RAW(sub_821BC5A0) {
  if (!Pilote()) return __imp__sub_821BC5A0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 144);
  TL_V(self, 36);
  TL_V(self, 676);
  const uint32_t tete = Load32(base, ctx.r3.u32 + 8);
  constexpr uint32_t kEvenement = 0x8349D4B8;
  for (uint32_t it = Load32(base, tete); it != tete;) {
    const uint32_t cle = base[it + 12], valeur = Load32(base, it + 16);
    it = TreeNext<21>(base, it);
    ctx.r4.u64 = cle;
    TL_M(self, 716, 821BC4D0);
    if (!(ctx.r3.u32 & 0xFF)) continue;
    for (;;) {
      ctx.r4.u64 = cle;
      ctx.r5.u64 = kEvenement;
      TL_M(self, 44, 821BC468);
      if (ctx.r3.u32 & 0xFF) break;
      ctx.r4.u64 = valeur;
      ctx.r5.u64 = 1;
      TL_M(self, 924, 821BC3F0);
      ctx.r4.u64 = cle;
      ctx.r5.u64 = kEvenement;
      TL_M(self, 48, 821BC400);
      if (!(ctx.r3.u32 & 0xFF)) break;
    }
  }
  TL_V(self, 40);
}

// sub_821B01B8 (r3 liste, r4 décalage, f1 z, r6 découpe) : éléments d'interface : images (140 octets) et textes (296 octets), découpés.
REX_HOOK_RAW(sub_821B01B8) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821B01B8(ctx, base);
  const uint32_t self = ctx.r3.u32, dec = ctx.r4.u32, decoupe = ctx.r6.u32;
  const double z = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 256);
  const uint32_t sp = ctx.r1.u32;
  ctx.r3.u64 = sp + 128;
  ctx.r4.u64 = Load32(base, Load32(base, 0x835067CC) + 28);
  {
    const uint32_t f = Slot(base, ctx.r4.u32, 76);
    TL_PROMU(f, 821B0488);
  }
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68);
  // sub_821BA4C8 : r5 (rectangle) coupé par r4.
  auto couper = [&](uint32_t c, uint32_t e) -> uint32_t {
    const float x = LoadFloat(base, dec), y = LoadFloat(base, dec + 4);
    const float h0 = y + LoadFloat(base, e - 12), h1 = y + LoadFloat(base, e - 8);
    const float l0 = x + LoadFloat(base, e - 4), l1 = x + LoadFloat(base, e);
    uint32_t mots[4] = {FloatBits(zero), FloatBits(zero), FloatBits(zero), FloatBits(zero)};
    const float cl = LoadFloat(base, c + 8), cr = LoadFloat(base, c + 12), ch0 = LoadFloat(base, c), ch1 = LoadFloat(base, c + 4);
    if (l1 > cl && l0 < cr && h1 > ch0 && h0 < ch1) {
      mots[2] = FloatBits(l0 > cl ? l0 : cl);
      mots[3] = FloatBits(l1 < cr ? l1 : cr);
      mots[0] = FloatBits(h0 > ch0 ? h0 : ch0);
      mots[1] = FloatBits(h1 < ch1 ? h1 : ch1);
    }
    for (uint32_t k = 0; k < 4; k++) Store32(base, sp + 96 + k * 4, mots[k]);
    return sp + 96;
  };
  auto rectangle = [&](uint32_t e) {
    const float x = LoadFloat(base, dec), y = LoadFloat(base, dec + 4);
    StoreFloat(base, sp + 120, x + LoadFloat(base, e - 120));
    StoreFloat(base, sp + 112, y + LoadFloat(base, e - 128));
    StoreFloat(base, sp + 124, x + LoadFloat(base, e - 116));
    StoreFloat(base, sp + 116, y + LoadFloat(base, e - 124));
  };
  auto clip = [&](uint32_t e) -> uint32_t {
    const uint32_t c = base[e + 5] ? sp + 128 : decoupe;
    return base[e + 4] ? couper(c, e) : c;
  };
  {
    const uint32_t fin = Load32(base, self + 4);
    (void)fin;
    for (uint32_t it = Load32(base, self); it != Load32(base, self + 4); it += 140) {
      const uint32_t e = it + 132;
      ctx.fpscr.disableFlushMode();
      const uint32_t c = clip(e);
      rectangle(e);
      ctx.r3.u64 = Load32(base, it);
      ctx.r4.u64 = sp + 112;
      ctx.r6.u64 = c;
      ctx.r7.u64 = e - 108;
      ctx.f1.f64 = double(LoadFloat(base, e - 112) + float(z));
      sub_821BA438(ctx, base);
    }
  }
  const float un = LoadFloat(base, kConstants + 64);
  for (uint32_t it = Load32(base, self + 16); it != Load32(base, self + 20); it += 296) {
    const uint32_t e = it + 288;
    ctx.fpscr.disableFlushMode();
    const uint32_t c = clip(e);
    rectangle(e);
    base[sp + 95] = 1;
    ctx.r3.u64 = Load32(base, e - 136);
    ctx.r4.u64 = it;
    ctx.r5.u64 = sp + 112;
    ctx.r7.u64 = c;
    ctx.r8.u64 = Load32(base, e - 132);
    ctx.r9.u64 = e - 108;
    ctx.f1.f64 = double(LoadFloat(base, e - 112) + float(z));
    ctx.f2.f64 = double(un);
    ctx.f3.f64 = double(un);
    sub_821AF2F0(ctx, base);
  }
}

// sub_821BDFA8 (r3 boîte orientée, r4 matrice) : boîte englobante (+32, +44) des 8 coins transformés.
REX_HOOK_RAW(sub_821BDFA8) {
  if (!Pilote()) return __imp__sub_821BDFA8(ctx, base);
  const uint32_t self = ctx.r3.u32, m = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 176);
  const uint32_t sp = ctx.r1.u32;
  ctx.fpscr.disableFlushMode();
  auto transformer = [&](uint32_t point, uint32_t tmp) {
    ctx.r3.u64 = tmp;
    ctx.r4.u64 = m;
    ctx.r5.u64 = point;
    sub_821BE798(ctx, base);
    return ctx.r3.u32;
  };
  auto ajouter = [&](uint32_t p) {
    ctx.r3.u64 = self + 32;
    ctx.r4.u64 = self + 44;
    ctx.r5.u64 = Load64(base, p);
    ctx.r6.u64 = uint64_t(Load32(base, p + 8)) << 32;
    sub_821BDF30(ctx, base);
  };
  const uint32_t p0 = transformer(self + 8, sp + 96);
  std::memcpy(base + self + 32, base + p0, 12);
  std::memcpy(base + self + 44, base + p0, 12);
  ajouter(p0);
  ajouter(transformer(self + 20, sp + 96));
  // Coins mélangés : (x, y, z) pris dans +8..+16 ou +20..+28.
  static constexpr uint8_t kCoins[6][3] = {{8, 12, 28}, {8, 24, 28}, {8, 24, 16}, {20, 12, 16}, {20, 24, 16}, {20, 12, 28}};
  for (const auto& c : kCoins) {
    for (uint32_t k = 0; k < 3; k++) Store32(base, sp + 96 + k * 4, Load32(base, self + c[k]));
    ajouter(transformer(sp + 96, sp + 112));
  }
}

// sub_821CCAB0 (r3, r4 détruire) : vidage de toutes les files de rendu.
REX_HOOK_RAW(sub_821CCAB0) {
  if (!Pilote()) return __imp__sub_821CCAB0(ctx, base);
  const uint32_t self = ctx.r3.u32, detruire = ctx.r4.u32 & 0xFF;
  CadreInvite cadre(ctx, base, 144);
  const uint32_t tete = Load32(base, self + 8);
  for (uint32_t it = Load32(base, tete); it != tete; it = TreeNext<21>(base, it)) {
    const uint32_t file = Load32(base, it + 16);
    const uint32_t t2 = Load32(base, file + 12);
    for (uint32_t n = Load32(base, t2); n != t2; n = TreeNext<21>(base, n)) {
      const uint32_t g = Load32(base, n + 16);
      if (!detruire) {
        ctx.r3.u64 = g;
        sub_821CC998(ctx, base);
        continue;
      }
      if (!g) continue;
      for (const uint32_t d : {208u, 168u, 128u, 88u, 48u, 8u}) {
        ctx.r3.u64 = g + d;
        sub_8244C798(ctx, base);
      }
      ctx.r3.u64 = g;
      sub_821CD9A8(ctx, base);
    }
    if (detruire) {
      ctx.r3.u64 = file + 8;
      sub_82582F58(ctx, base);
    }
  }
  sub_821B17D0(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------------
// Les 50 suivantes, troisième tranche.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_821AEE40);
REX_EXTERN(__imp__sub_821AFEA0);
REX_EXTERN(__imp__sub_821B7650);
REX_EXTERN(__imp__sub_821C8D88);
REX_EXTERN(__imp__sub_821E0D18);
REX_EXTERN(__imp__sub_821D8238);
REX_EXTERN(__imp__sub_821DDBB0);
REX_EXTERN(__imp__sub_822E1090);
REX_EXTERN(__imp__sub_821CB418);
REX_EXTERN(__imp__sub_821CADB0);
REX_EXTERN(__imp__sub_823ECF88);
REX_EXTERN(__imp__sub_823E7248);
REX_EXTERN(sub_821AED70);
REX_EXTERN(sub_821B00A8);
REX_EXTERN(sub_821AFF88);
REX_EXTERN(sub_821B00D0);
REX_EXTERN(sub_821B7588);
REX_EXTERN(sub_8259B148);
REX_EXTERN(sub_825A4C68);
REX_EXTERN(sub_825A5BF0);
REX_EXTERN(sub_823586C8);
REX_EXTERN(sub_821E8630);
REX_EXTERN(sub_82458640);
REX_EXTERN(sub_821C3730);
REX_EXTERN(sub_821E0EA8);
REX_EXTERN(sub_824DBBB8);
REX_EXTERN(sub_821DDF58);
REX_EXTERN(sub_822F6E38);
REX_EXTERN(sub_8219BEB8);
REX_EXTERN(sub_8247A650);
REX_EXTERN(sub_821A5740);
REX_EXTERN(sub_821A7928);
REX_EXTERN(sub_82282158);
REX_EXTERN(sub_821D85A0);
REX_EXTERN(sub_8227E058);
REX_EXTERN(sub_821DD310);
REX_EXTERN(sub_821C0B00);
REX_EXTERN(sub_8219F2E0);
REX_EXTERN(sub_821C77A8);
REX_EXTERN(sub_821BF568);
REX_EXTERN(sub_82198CD8);
REX_EXTERN(sub_821C7B88);
REX_EXTERN(sub_821BD3E0);
REX_EXTERN(sub_821DEE98);
REX_EXTERN(sub_821CE030);
REX_EXTERN(sub_821CDE10);
REX_EXTERN(sub_821DD450);
REX_EXTERN(sub_821BAC40);
REX_EXTERN(sub_821AC278);
REX_EXTERN(sub_821ABAE8);
REX_EXTERN(sub_821BDC50);
REX_EXTERN(sub_821BE2F8);
REX_EXTERN(sub_821BE470);
REX_EXTERN(sub_8246CB00);
REX_EXTERN(sub_82447398);
REX_EXTERN(sub_823E70B0);

REX_HOOK_RAW(sub_821AEE40) {
  if (!Pilote()) return __imp__sub_821AEE40(ctx, base);
  const uint32_t self = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 112);
  TL_M(Load32(base, 0x835067DC), 20, 821AED70);
  ctx.r3.u64 = self;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  sub_821AECC0(ctx, base);
}

namespace {
void DessinerFenetre(PPCContext& ctx, uint8_t* base, uint32_t self) {
  if (!VisibleEnHierarchie(base, self, 0)) return;
  CadreInvite cadre(ctx, base, 128);
  const uint32_t e = ctx.r1.u32 + 80;
  Store32(base, e, 0x820CEF98);
  base[e + 4] = 0;
  base[e + 5] = 1;
  Store32(base, e + 8, self);
  ctx.r4.u64 = e;
  TL_M(self, 108, 821B00A8);
  const uint32_t rendu = Load32(base, Load32(base, 0x835067CC) + 28);
  ctx.f1.f64 = double(LoadFloat(base, rendu + 24));
  TL_M(self, 232, 821AFF88);
  ctx.fpscr.disableFlushMode();
  StoreFloat(base, rendu + 24, LoadFloat(base, rendu + 24) - LoadFloat(base, kConstants + 84));
  const uint32_t nombre = uint32_t(int32_t(Load32(base, self + 48) - Load32(base, self + 44)) >> 2);
  for (uint32_t k = 0; k < nombre; k++) DessinerFenetre(ctx, base, Load32(base, Load32(base, self + 60) + k * 4));
  ctx.r4.u64 = e;
  TL_M(self, 112, 821B00D0);
}
}  // namespace

// sub_821AFEA0 (r3 fenêtre) : dessin de CEGUI, enfants compris, de l'avant vers l'arrière.
REX_HOOK_RAW(sub_821AFEA0) {
  if (!Pilote()) return __imp__sub_821AFEA0(ctx, base);
  DessinerFenetre(ctx, base, ctx.r3.u32);
}

REX_HOOK_RAW(sub_821B7650) {
  if (!Pilote()) return __imp__sub_821B7650(ctx, base);
  const uint32_t self = ctx.r3.u32;
  if (!base[self + 109]) return;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 112);
  TL_M(self, 32, 821B76B8);
  if (ctx.r3.u32 & 0xFF) {
    ctx.r3.u64 = self;
    ctx.f1.f64 = dt;
    sub_821B7588(ctx, base);
  }
}

// sub_821C8D88 (r3 technique) : mise à jour du rendu de la technique.
REX_HOOK_RAW(sub_821C8D88) {
  if (!Pilote()) return __imp__sub_821C8D88(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const uint32_t rendu = Load32(base, self + 600);
  CadreInvite cadre(ctx, base, 112);
  if (!base[self + 188]) {
    if (rendu) {
      ctx.r4.u64 = self + 280;
      TL_M(rendu, 32, 8259B148);
    }
    return;
  }
  if (rendu) {
    ctx.r4.u64 = 0;
    TL_V(rendu, 20);
  }
  const uint32_t r2 = Load32(base, self + 600);
  if (r2) {
    ctx.r4.u64 = self + 280;
    TL_M(r2, 32, 8259B148);
  }
  ctx.r3.u64 = self + 280;
  sub_825A4C68(ctx, base);
  ctx.r3.u64 = self + 280;
  sub_825A5BF0(ctx, base);
  base[self + 188] = 0;
}

// sub_821E0D18 (r3 entité, r4 file) : Entity::_updateRenderQueue.
REX_HOOK_RAW(sub_821E0D18) {
  if (!Pilote()) return __imp__sub_821E0D18(ctx, base);
  const uint32_t self = ctx.r3.u32, file = ctx.r4.u32;
  if (!base[self + 448]) return;
  CadreInvite cadre(ctx, base, 128);
  const uint32_t sp = ctx.r1.u32;
  TL_M(Load32(base, self + 180), 152, 823586C8);
  if (ctx.r3.u32 != Load32(base, self + 516)) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = 1;
    sub_82458640(ctx, base);
  }
  const uint32_t fin = Load32(base, self + 196);
  for (uint32_t it = Load32(base, self + 192); it != fin; it += 4) {
    TL_M(Load32(base, it), 76, 821E8630);
    if (!(ctx.r3.u32 & 0xFF)) continue;
    uint32_t groupe, priorite;
    if (base[self + 72]) {
      priorite = Load16(base, self + 70);
      groupe = base[self + 68];
    } else {
      priorite = Load16(base, file + 26);
      groupe = base[self + 69] ? base[self + 68] : base[file + 24];
    }
    ctx.r3.u64 = file;
    ctx.r4.u64 = Load32(base, it);
    ctx.r5.u64 = groupe;
    ctx.r6.u64 = priorite;
    sub_821C3730(ctx, base);
  }
  if (base[self + 441] && Load32(base, self + 444)) {
    ctx.r3.u64 = self;
    sub_821B93B0(ctx, base);
    TL_V(Load32(base, self + 444), 280);
  }
  if (!Load32(base, self + 444) && !Load32(base, Load32(base, self + 180) + 412)) return;
  ctx.r3.u64 = self;
  sub_821E0EA8(ctx, base);
  const uint32_t tete = Load32(base, self + 524);
  Store32(base, sp + 80, Load32(base, tete));
  while (Load32(base, sp + 80) != tete) {
    const uint32_t o = Load32(base, Load32(base, sp + 80) + 40);
    TL_V(o, 104);
    if (ctx.r3.u32 & 0xFF) {
      ctx.r4.u64 = file;
      TL_V(o, 92);
    }
    ctx.r3.u64 = sp + 80;
    sub_824DBBB8(ctx, base);
  }
}

// sub_821D8238 (r3 monde, r4, r5, f1 dt, f2 vitesse) : mise à jour de toutes les unités du niveau.
REX_HOOK_RAW(sub_821D8238) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821D8238(ctx, base);
  const uint32_t self = ctx.r3.u32, camera = ctx.r4.u32, vue = ctx.r5.u32;
  const double dt = ctx.f1.f64;
  const double acc = double(float(dt * ctx.f2.f64));
  CadreInvite cadre(ctx, base, 208);
  const uint32_t sp = ctx.r1.u32;
  auto nettoyer = [&]() {
    ctx.r3.u64 = self;
    sub_821DDF58(ctx, base);
  };
  nettoyer();
  uint32_t n = Load32(base, Load32(base, self + 84));
  Store32(base, self + 376, 0);
  auto etat28 = [&](uint32_t u) -> bool {
    const uint32_t s = Load32(base, u + 96);
    if (!s || !Load32(base, s + 16)) return false;
    ctx.r3.u64 = Load32(base, s + 16);
    ctx.r4.u64 = Load32(base, u + 372);
    ctx.r5.u64 = 28;
    sub_821D7D38(ctx, base);
    return (ctx.r3.u32 & 0xFF) != 0;
  };
  while (n) {
    const uint32_t u = Load32(base, n);
    n = Load32(base, n + 4);
    if (base[u + 344]) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = u;
      sub_822F6E38(ctx, base);
      nettoyer();
      continue;
    }
    const bool e = etat28(u);
    ctx.r4.u64 = camera;
    ctx.r5.u64 = vue;
    ctx.f1.f64 = e ? dt : acc;
    TL_V(u, 252);
    if (!base[u + 353]) continue;
    const uint32_t ombre = Load32(base, self + 328);
    if (ombre) {
      if (etat28(u)) {
        ctx.r3.u64 = sp + 80;
        ctx.r4.u64 = u;
        ctx.r5.u64 = 1;
        sub_821D3F38(ctx, base);
        ctx.r4.u64 = ctx.r3.u32;
        ctx.r3.u64 = Load32(base, self + 328);
        sub_8219BEB8(ctx, base);
      } else {
        const uint32_t m = Load32(base, u + 1444);
        if (m) {
          ctx.r3.u64 = m;
          ctx.r4.u64 = 28;
          sub_821D7CE0(ctx, base);
          if (ctx.r3.u32 & 0xFF) {
            const uint32_t a = Load32(base, m + 1452);
            const uint32_t premier = (int32_t(Load32(base, m + 1456) - a) & ~3) == 0 ? 0 : Load32(base, Load32(base, a));
            if (u == premier) {
              if (int32_t(Load32(base, u + 632)) == 42) {
                const uint32_t o = Load32(base, ombre + 288);
                ctx.fpscr.disableFlushMode();
                const float loin = LoadFloat(base, kConstants + 172);
                EcrireV3(base, o + 28, {loin, LoadFloat(base, kConstants + 256), loin});
              } else {
                ctx.r3.u64 = sp + 96;
                ctx.r4.u64 = u;
                ctx.r5.u64 = 1;
                sub_821D3F38(ctx, base);
                std::memcpy(base + Load32(base, Load32(base, self + 328) + 288) + 28, base + ctx.r3.u32, 12);
              }
            }
          }
        }
      }
    }
    nettoyer();
    for (uint32_t c = Load32(base, Load32(base, self + 124)); c;) {
      const uint32_t o = Load32(base, c);
      c = Load32(base, c + 4);
      ctx.r5.u64 = u;
      ctx.f1.f64 = dt;
      TL_V(o, 4);
    }
    Store32(base, self + 376, Load32(base, self + 376) + 1);
  }
  nettoyer();
}

// sub_821DDBB0 (r3 ensemble, r4 nombre) : beginBillboards.
REX_HOOK_RAW(sub_821DDBB0) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821DDBB0(ctx, base);
  const uint32_t self = ctx.r3.u32, nombre = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 160);
  if (!base[self + 583]) sub_8247A650(ctx, base);
  if (!base[self + 582]) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = self + 448;
    ctx.r5.u64 = self + 452;
    ctx.r6.u64 = self + 456;
    ctx.r7.u64 = self + 460;
    sub_821A5740(ctx, base);
    const int32_t k = int32_t(Load32(base, self + 552));
    if (!(k == 2 || k == 4 || (base[self + 319] && k != 3))) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = self + 464;
      ctx.r5.u64 = self + 476;
      ctx.r6.u64 = 0;
      sub_821DE398(ctx, base);
      ctx.fpscr.disableFlushMode();
      const float l = LoadFloat(base, self + 264), h = LoadFloat(base, self + 268);
      const float a = LoadFloat(base, self + 448) * l, b = LoadFloat(base, self + 452) * l;
      const float c = LoadFloat(base, self + 456) * h, d = LoadFloat(base, self + 460) * h;
      const V3 x = LireV3(base, self + 464), y = LireV3(base, self + 476);
      const float cs[2] = {c, d}, ab[2] = {a, b};
      uint32_t o = self + 396;
      for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
          EcrireV3(base, o, {y.x * cs[j] + x.x * ab[i], y.y * cs[j] + x.y * ab[i], y.z * cs[j] + x.z * ab[i]});
          o += 12;
        }
      }
    }
  }
  Store16(base, self + 580, 0);
  const uint32_t tampon = Load32(base, self + 380);
  if (nombre) {
    const uint32_t maximum = Load32(base, self + 584);
    const uint32_t m = nombre < maximum ? nombre : maximum;
    uint32_t pas = Load32(base, tampon + 36);
    if (!base[self + 582]) pas <<= 2;
    ctx.r5.u64 = pas * m;
  } else {
    ctx.r5.u64 = Load32(base, tampon + 4);
  }
  ctx.r4.u64 = 0;
  ctx.r6.u64 = (Load32(base, tampon + 8) >> 1) & 1;
  TL_M(tampon, 12, 821A7928);
  Store32(base, self + 392, ctx.r3.u32);
}

// sub_822E1090 (r3 équipement, r4, r5) : somme d'une propriété sur les 12 emplacements et leurs gemmes.
REX_HOOK_RAW(sub_822E1090) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_822E1090(ctx, base);
  const uint32_t self = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  CadreInvite cadre(ctx, base, 160);
  ctx.fpscr.disableFlushMode();
  const double zero = double(LoadFloat(base, kConstants + 68));
  float somme = float(zero);
  auto valeur = [&](uint32_t o) {
    ctx.r4.u64 = 0;
    ctx.r5.u64 = r4;
    ctx.r7.u64 = r5;
    ctx.f1.f64 = zero;
    TL_M(o, 288, 82282158);
    sub_821D85A0(ctx, base);
    ctx.fpscr.disableFlushMode();
    somme = float(ctx.f1.f64) + somme;
  };
  for (uint32_t i = 0; i < 12; i++) {
    ctx.r3.u64 = self;
    ctx.r4.u64 = i;
    sub_821D5380(ctx, base);
    const uint32_t o = ctx.r3.u32;
    if (!o) continue;
    valeur(o);
    const uint32_t v = o + 920;
    for (uint32_t k = 0; k < Load32(base, v + 4); k++) {
      const uint32_t d = Load32(base, v);
      valeur(Load32(base, k < Load32(base, v + 8) ? d + k * 4 : d));
    }
  }
  const uint32_t r16 = Load32(base, self + 16);
  if (r16) {
    ctx.r3.u64 = r16;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = r4;
    ctx.r6.u64 = r5;
    sub_8227E058(ctx, base);
    sub_821D85A0(ctx, base);
    ctx.fpscr.disableFlushMode();
    ctx.f1.f64 = double(float(ctx.f1.f64) + somme);
  } else {
    ctx.f1.f64 = double(somme);
  }
}

// sub_821CB418 (r3 technique, f1 dt) : ParticleTechnique::_processParticles : boucle des particules actives (affecteurs, durée de vie).
REX_HOOK_RAW(sub_821CB418) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821CB418(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t pool = self + 280;
  if (!Load32(base, self + 304) && !Load32(base, pool + 64) && !Load32(base, pool + 196) && !Load32(base, pool + 108) && !Load32(base, pool + 152)) return;
  CadreInvite cadre(ctx, base, 176);
  ctx.r3.u64 = pool;
  sub_821DD310(ctx, base);
  ctx.fpscr.disableFlushMode();
  const float zero = LoadFloat(base, kConstants + 68);
  uint32_t premier1 = 1, premier2 = 1;
  for (;;) {
    const uint32_t p = ctx.r3.u32;
    const uint32_t s = pool + 8;
    if (Load32(base, s + 4) == Load32(base, s + 12) && Load32(base, pool + 48) == Load32(base, pool + 60) &&
        Load32(base, pool + 180) == Load32(base, pool + 192) && Load32(base, pool + 92) == Load32(base, pool + 104) &&
        Load32(base, pool + 136) == Load32(base, pool + 148)) {
      return;
    }
    if (p) {
      auto tous = [&](uint32_t debut, uint32_t decalage, uint32_t drapeau) {
        const uint32_t f = Load32(base, debut + 4);
        for (uint32_t it = Load32(base, debut); it != f; it += 4) {
          ctx.r4.u64 = self;
          ctx.r5.u64 = p;
          ctx.r7.u64 = drapeau;
          ctx.f1.f64 = dt;
          TL_V(Load32(base, it), decalage);
        }
      };
      bool expire = false;
      const uint32_t emetteur = Load32(base, p + 4);
      if (!(emetteur && base[emetteur + 386])) {
        expire = LoadFloat(base, p + 60) < float(dt);
        if (expire) {
          ctx.r4.u64 = 1;
          TL_V(p, 24);
        }
      }
      ctx.r4.u64 = self;
      ctx.f1.f64 = dt;
      if (!expire) {
        TL_M(p, 40, 821C0B00);
        if (base[self + 716]) {
          ctx.r3.u64 = self;
          ctx.r4.u64 = p;
          sub_8219F2E0(ctx, base);
        }
        ctx.r3.u64 = self;
        ctx.r4.u64 = p;
        ctx.r6.u64 = premier2;
        ctx.f1.f64 = dt;
        sub_821C77A8(ctx, base);
        tous(self + 264, 32, premier2);
        if (base[self + 92]) {
          const int32_t k = int32_t(Load32(base, p + 72));
          if (k == 2) {
            ctx.f1.f64 = dt;
            TL_V(p, 100);
            ctx.r5.u64 = ctx.r3.u32 & 0xFFFF;
            ctx.r3.u64 = self;
            ctx.r4.u64 = p;
            ctx.f1.f64 = dt;
            sub_821BF568(ctx, base);
          } else if (k == 1) {
            if (base[self + 716]) {
              ctx.r3.u64 = p;
              sub_82198CD8(ctx, base);
            }
            ctx.r3.u64 = p;
            ctx.f1.f64 = dt;
            sub_821C7B88(ctx, base);
            if (base[self + 716]) {
              ctx.r3.u64 = self + 684;
              ctx.r4.u64 = p + 684;
              sub_821BD3E0(ctx, base);
            }
          }
        }
        premier2 = 0;
      } else {
        TL_V(p, 8);
        const uint32_t f = Load32(base, self + 776);
        for (uint32_t it = Load32(base, self + 772); it != f; it += 4) {
          ctx.r4.u64 = self;
          ctx.r5.u64 = p;
          TL_V(Load32(base, it), 8);
        }
        ctx.r3.u64 = pool;
        sub_821DEE98(ctx, base);
      }
      tous(self + 248, 28, premier1);
      ctx.r3.u64 = self;
      ctx.r4.u64 = p;
      ctx.f1.f64 = dt;
      sub_821CE030(ctx, base);
      ctx.r4.u64 = 1;
      TL_V(p, 36);
      const bool b = (ctx.r3.u32 & 0xFF) != 0;
      ctx.r4.u64 = 0;
      TL_V(p, 20);
      if (b) {
        ctx.r4.u64 = 1;
        TL_V(p, 24);
      }
      ctx.fpscr.disableFlushMode();
      const float reste = float(double(LoadFloat(base, p + 60)) - dt);
      StoreFloat(base, p + 60, reste);
      const uint32_t em = Load32(base, p + 4);
      if (em && base[em + 386] && !(reste > zero)) {
        const float vie = LoadFloat(base, p + 64);
        float v;
        do {
          v = vie + LoadFloat(base, p + 60);
          StoreFloat(base, p + 60, v);
        } while (!(v > zero));
      }
    }
    ctx.r3.u64 = pool;
    premier1 = 0;
    sub_821CDE10(ctx, base);
  }
}

// sub_821CADB0 (r3 rendu de CEGUI) : états du dessin 2D.
REX_HOOK_RAW(sub_821CADB0) {
  if (!Pilote()) return __imp__sub_821CADB0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 272);
  const uint32_t sp = ctx.r1.u32;
  const uint32_t rs = Load32(base, self + 228);
  constexpr uint32_t kIdentite = 0x8349FEC0;
  ctx.r4.u64 = kIdentite;
  TL_M(rs, 112, 821DD450);
  ctx.r4.u64 = kIdentite;
  TL_M(rs, 120, 821BAC40);
  ctx.f1.f64 = LoadDouble(base, kConstants + 608);
  sub_821AC278(ctx, base);
  ctx.fpscr.disableFlushMode();
  StoreFloat(base, sp + 80, LoadFloat(base, 0x8356C9DC) * LoadFloat(base, kConstants + 180));
  ctx.r4.u64 = sp + 80;
  ctx.r8.u64 = sp + 160;
  ctx.r9.u64 = 0;
  ctx.f1.f64 = double(LoadFloat(base, kConstants + 596));
  ctx.f2.f64 = double(LoadFloat(base, kConstants + 264));
  ctx.f3.f64 = double(LoadFloat(base, kConstants + 172));
  TL_M(rs, 316, 821ABAE8);
  std::memcpy(base + sp + 96, base + kIdentite, 64);
  std::memcpy(base + sp + 136, base + sp + 200, 8);
  ctx.r4.u64 = sp + 96;
  TL_M(rs, 124, 821BDC50);
  ctx.r4.u64 = 0;
  TL_M(rs, 56, 825BC790);
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 1;
  ctx.r6.u64 = 1;
  TL_M(rs, 252, 821BE2F8);
  const double zero = double(LoadFloat(base, kConstants + 68));
  ctx.f1.f64 = zero;
  ctx.f2.f64 = zero;
  TL_M(rs, 272, 821D14B8);
  ctx.r4.u64 = 1;
  TL_M(rs, 244, 821C6CF8);
  const double un = double(LoadFloat(base, kConstants + 64));
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0x8349D5E0;
  ctx.f1.f64 = un;
  ctx.f2.f64 = zero;
  ctx.f3.f64 = un;
  TL_M(rs, 276, 825BC790);
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 1;
  ctx.r6.u64 = 1;
  ctx.r7.u64 = 1;
  TL_M(rs, 268, 821C3520);
  ctx.r4.u64 = 1;
  TL_M(rs, 372, 821BE470);
  ctx.r4.u64 = 0;
  TL_M(rs, 372, 821BE470);
  ctx.r4.u64 = 1;
  TL_M(rs, 52, 825BC790);
  ctx.r4.u64 = Load32(base, self + 220);
  TL_M(rs, 324, 821C5228);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  TL_M(rs, 168, 821CB0F0);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  TL_M(rs, 164, 821CAD88);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 2;
  ctx.r6.u64 = 2;
  ctx.r7.u64 = 0;
  TL_M(rs, 180, 821CA3F0);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = self + 512;
  TL_M(rs, 188, 821CA1A8);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = kIdentite;
  TL_M(rs, 200, 821CA930);
  ctx.r4.u64 = 7;
  ctx.r5.u64 = 5;
  ctx.r6.u64 = 0;
  TL_M(rs, 212, 821C54D8);
  ctx.r4.u64 = 1;
  TL_M(rs, 260, 821C4E88);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = self + 384;
  TL_M(rs, 172, 821C9100);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = self + 448;
  TL_M(rs, 172, 821C9100);
  ctx.r4.u64 = 1;
  TL_M(rs, 136, 821BF850);
  ctx.r4.u64 = 7;
  ctx.r5.u64 = 9;
  ctx.r6.u64 = 0;
  TL_M(rs, 204, 821C52B0);
}

// sub_823ECF88 (r3 sortie, r4 arbre, r5 à gauche, r6 parent, r7 nœud) : insertion et rééquilibrage rouge-noir (couleur +16, 1 : noir).
REX_HOOK_RAW(sub_823ECF88) {
  if (!Pilote()) return __imp__sub_823ECF88(ctx, base);
  const uint32_t sortie = ctx.r3.u32, arbre = ctx.r4.u32, gauche = ctx.r5.u32, parent = ctx.r6.u32, noeud = ctx.r7.u32;
  const uint32_t taille = Load32(base, arbre + 8);
  if (taille >= 0xFFFFFFFEu) return __imp__sub_823ECF88(ctx, base);  // longueur maximale : exception du jeu
  Store32(base, arbre + 8, taille + 1);
  Store32(base, noeud + 4, parent);
  const uint32_t tete = Load32(base, arbre + 4);
  if (parent == tete) {
    Store32(base, tete + 4, noeud);
    Store32(base, tete, noeud);
    Store32(base, tete + 8, noeud);
  } else if (gauche & 0xFF) {
    Store32(base, parent, noeud);
    if (parent == Load32(base, tete)) Store32(base, tete, noeud);
  } else {
    Store32(base, parent + 8, noeud);
    if (parent == Load32(base, tete + 8)) Store32(base, tete + 8, noeud);
  }
  CadreInvite cadre(ctx, base, 144);
  auto tourner = [&](PPCFunc* f, uint32_t n) {
    ctx.r3.u64 = arbre;
    ctx.r4.u64 = n;
    f(ctx, base);
  };
  uint32_t x = noeud;
  while (!base[Load32(base, x + 4) + 16]) {
    const uint32_t p = Load32(base, x + 4), g = Load32(base, p + 4);
    const bool cote_gauche = p == Load32(base, g);
    const uint32_t oncle = Load32(base, cote_gauche ? g + 8 : g);
    if (!base[oncle + 16]) {
      base[p + 16] = 1;
      base[oncle + 16] = 1;
      base[Load32(base, Load32(base, x + 4) + 4) + 16] = 0;
      x = Load32(base, Load32(base, x + 4) + 4);
    } else {
      if (x == Load32(base, cote_gauche ? p + 8 : p)) {
        x = p;
        tourner(cote_gauche ? sub_8246CB00 : sub_82447398, p);
      }
      base[Load32(base, x + 4) + 16] = 1;
      const uint32_t g2 = Load32(base, Load32(base, x + 4) + 4);
      base[g2 + 16] = 0;
      tourner(cote_gauche ? sub_82447398 : sub_8246CB00, g2);
    }
  }
  Store32(base, sortie, noeud);
  base[Load32(base, Load32(base, arbre + 4) + 4) + 16] = 1;
  ctx.r3.u64 = sortie;
}

// sub_823E7248 (r3, r4 nom, r5 second nom, r6) : élément dont le matériau a ce nom ; la comparaison du second nom seulement s'il n'est pas vide.
REX_HOOK_RAW(sub_823E7248) {
  if (!Pilote()) return __imp__sub_823E7248(ctx, base);
  const uint32_t self = ctx.r3.u32, nom = ctx.r4.u32, second = ctx.r5.u32, r6 = ctx.r6.u32;
  CadreInvite cadre(ctx, base, 160);
  const bool vide = ChainesEgales(base, second, 0x8349D5A0);
  for (uint32_t it = Load32(base, self + 32); it != Load32(base, self + 36); it += 4) {
    const uint32_t e = Load32(base, it);
    const uint32_t m = Load32(base, Load32(base, e + 8) + 32);
    TL_V(m, 88);
    if (!ChainesEgales(base, ctx.r3.u32, nom)) continue;
    if (!vide) {
      TL_V(m, 132);
      if (!ChainesEgales(base, ctx.r3.u32, second)) continue;
    }
    ctx.r3.u64 = e;
    ctx.r4.u64 = r6;
    sub_823E70B0(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Dernière tranche : l'oubliée du top 50 (recherche de chemin), le free du tas, largeur de fenêtre de CEGUI, cartes de
// chaînes larges. Laissées de côté, voir le journal : sub_821D5410, sub_821F3F28, sub_821A6D90, sub_8247F888, sub_821BA040,
// sub_821DA090, bibliothèque C, inflate.
// ---------------------------------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8221FEB0);
REX_EXTERN(__imp__sub_82220110);
REX_EXTERN(__imp__sub_8231F078);
REX_EXTERN(__imp__sub_823C67E0);
REX_EXTERN(__imp__sub_825EBAA0);
REX_EXTERN(__imp__sub_821A4AC8);
REX_EXTERN(__imp__sub_821A4A50);
REX_EXTERN(__imp__sub_821A4E48);
REX_EXTERN(__imp__sub_821A4020);
REX_EXTERN(__imp__sub_821A4B78);
REX_EXTERN(__imp__sub_821C00C0);
REX_EXTERN(__imp__sub_821CCF30);
REX_EXTERN(sub_821E4E90);
REX_EXTERN(sub_821E9F98);
REX_EXTERN(sub_821AEDE0);
REX_EXTERN(sub_821A4EC8);
REX_EXTERN(sub_8285C900);
REX_EXTERN(sub_8287D840);
REX_EXTERN(sub_8241DA58);
REX_EXTERN(sub_8241D610);

namespace {
constexpr uint32_t kCoutVoisin = 0x820AA300;  // coût d'un pas de la recherche de chemin

// Pour une case voisine déjà connue : le parent la retient (première case libre, débordement compris, comme le jeu).
void RetenirVoisin(uint8_t* base, uint32_t parent, uint32_t noeud) {
  uint32_t i = 0;
  while (i < 8 && Load32(base, parent + 28 + i * 4)) i++;
  Store32(base, parent + 28 + i * 4, noeud);
}

// Le coût d'un nœud baisse : +0 total, +4 heuristique, +8 parcouru, +24 parent.
void BaisserCout(uint8_t* base, uint32_t noeud, uint32_t parent, float cout) {
  const float h = LoadFloat(base, noeud + 4);
  Store32(base, noeud + 24, parent);
  StoreFloat(base, noeud + 8, cout);
  StoreFloat(base, noeud + 0, h + cout);
}

// sub_82220250 : pile des nœuds à propager, en tête de la liste de self+24.
void EmpilerNoeud(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t noeud) {
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 8;
  sub_821CD7F8(ctx, base);
  const uint32_t e = ctx.r3.u32;
  Store32(base, e, noeud);
  Store32(base, e + 4, Load32(base, Load32(base, self + 24) + 4));
  Store32(base, Load32(base, self + 24) + 4, e);
}
}  // namespace

// sub_82220110 (r3, r4 nœud) : propage un coût plus bas aux voisins, en profondeur par une pile.
REX_HOOK_RAW(sub_82220110) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_82220110(ctx, base);
  const uint32_t self = ctx.r3.u32, noeud = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 160);
  ctx.fpscr.disableFlushMode();
  const float pas = LoadFloat(base, kCoutVoisin), parcouru = LoadFloat(base, noeud + 8);
  for (uint32_t i = 0; i < 8; i++) {
    const uint32_t v = Load32(base, noeud + 28 + i * 4);
    if (!v) break;
    const float c = parcouru + pas;
    if (!(c < LoadFloat(base, v + 8))) continue;
    BaisserCout(base, v, noeud, c);
    EmpilerNoeud(ctx, base, self, v);
    ctx.fpscr.disableFlushMode();
  }
  for (;;) {
    const uint32_t liste = Load32(base, self + 24);
    const uint32_t e = Load32(base, liste + 4);
    if (!e) break;
    const uint32_t courant = Load32(base, e);
    Store32(base, liste + 4, Load32(base, e + 4));
    ctx.r3.u64 = e;
    sub_821CD9A8(ctx, base);
    ctx.fpscr.disableFlushMode();
    for (uint32_t i = 0; i < 8; i++) {
      const uint32_t v = Load32(base, courant + 28 + i * 4);
      if (!v) break;
      const float c = LoadFloat(base, courant + 8) + pas;
      if (!(c < LoadFloat(base, v + 8))) continue;
      BaisserCout(base, v, courant, c);
      EmpilerNoeud(ctx, base, self, v);
      ctx.fpscr.disableFlushMode();
    }
  }
}

// sub_8221FEB0 (r3, r4 parent, r5 x, r6 y, r7 x but, r8 y but) : un voisin de la recherche de chemin. Déjà dans la liste
// ouverte ou fermée : coût baissé s'il le faut ; sinon, nouveau nœud rangé par coût total dans la liste ouverte.
REX_HOOK_RAW(sub_8221FEB0) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_8221FEB0(ctx, base);
  const uint32_t self = ctx.r3.u32, parent = ctx.r4.u32, x = ctx.r5.u32, y = ctx.r6.u32;
  const uint32_t but_x = ctx.r7.u32, but_y = ctx.r8.u32;
  ctx.fpscr.disableFlushMode();
  const float cout = LoadFloat(base, parent + 8) + LoadFloat(base, kCoutVoisin);
  const uint32_t cle = y * Load32(base, self + 36) + x;

  for (uint32_t n = Load32(base, Load32(base, self + 12) + 60); n; n = Load32(base, n + 60)) {
    if (Load32(base, n + 20) != cle) continue;
    RetenirVoisin(base, parent, n);
    if (cout < LoadFloat(base, n + 8)) BaisserCout(base, n, parent, cout);
    return;
  }
  for (uint32_t n = Load32(base, Load32(base, self + 16) + 60); n; n = Load32(base, n + 60)) {
    if (Load32(base, n + 20) != cle) continue;
    RetenirVoisin(base, parent, n);
    if (cout < LoadFloat(base, n + 8)) {
      BaisserCout(base, n, parent, cout);
      ctx.r3.u64 = self;
      ctx.r4.u64 = n;
      CadreInvite cadre(ctx, base, 176);
      sub_82220110(ctx, base);
    }
    return;
  }

  CadreInvite cadre(ctx, base, 176);
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 64;
  sub_821CD7F8(ctx, base);
  ctx.fpscr.disableFlushMode();
  const uint32_t nouveau = ctx.r3.u32;
  std::memset(base + nouveau, 0, 64);
  const float dy = float(int32_t(y - but_y)), dx = float(int32_t(x - but_x));
  const float h = FmaLikeGuest(dy, dy, dx * dx);
  const float total = h + cout;
  StoreFloat(base, nouveau + 8, cout);
  Store32(base, nouveau + 24, parent);
  Store32(base, nouveau + 12, x);
  Store32(base, nouveau + 16, y);
  Store32(base, nouveau + 20, cle);
  StoreFloat(base, nouveau + 4, h);
  StoreFloat(base, nouveau + 0, total);
  uint32_t avant = Load32(base, self + 12);
  uint32_t suivant = Load32(base, avant + 60);
  while (suivant && LoadFloat(base, suivant) < total) {
    avant = suivant;
    suivant = Load32(base, suivant + 60);
  }
  Store32(base, nouveau + 60, suivant);
  Store32(base, avant + 60, nouveau);
  uint32_t i = 0;
  while (i < 8 && Load32(base, parent + 28 + i * 4)) i++;
  const uint32_t meilleur = Load32(base, self + 8);
  if (!meilleur || LoadFloat(base, meilleur + 4) > h) Store32(base, self + 8, nouveau);
  Store32(base, parent + 28 + i * 4, nouveau);
}

namespace {
// Chaînes larges (sub_821A1478) : mots de 16 bits en gros-boutiste, memcmp donne le même ordre.
int ComparerLarges(const uint8_t* base, uint32_t a, uint32_t b) {
  const uint32_t na = Load32(base, a + 16), nb = Load32(base, b + 16);
  const int r = std::memcmp(base + DonneesChaineLarge(base, a), base + DonneesChaineLarge(base, b),
                            size_t(na < nb ? na : nb) * 2);
  if (r) return r;
  return na < nb ? -1 : (na != nb ? 1 : 0);
}

uint32_t BorneLarge(const uint8_t* base, uint32_t tete, uint32_t cle) {
  uint32_t resultat = tete;
  for (uint32_t n = Load32(base, tete + 4); !base[n + 45];) {
    if (ComparerLarges(base, n + 12, cle) < 0) {
      n = Load32(base, n + 8);
    } else {
      resultat = n;
      n = Load32(base, n);
    }
  }
  return resultat;
}
}  // namespace

// sub_8231F078 (r3 sortie, r4 carte, r5 clé) : lower_bound d'une map<wstring, ...>.
REX_HOOK_RAW(sub_8231F078) {
  if (!PiloteOuValidation()) return __imp__sub_8231F078(ctx, base);
  const uint32_t sortie = ctx.r3.u32;
  const uint32_t mots[1] = {BorneLarge(base, Load32(base, ctx.r4.u32 + 4), ctx.r5.u32)};
  FinMots(ctx, base, __imp__sub_8231F078, "8231F078", sortie, mots, 1);
  ctx.r3.u64 = sortie;
}

// sub_823C67E0 (r3 sortie, r4 carte, r5 clé) : find de la même carte.
REX_HOOK_RAW(sub_823C67E0) {
  if (!PiloteOuValidation()) return __imp__sub_823C67E0(ctx, base);
  const uint32_t sortie = ctx.r3.u32, cle = ctx.r5.u32;
  const uint32_t tete = Load32(base, ctx.r4.u32 + 4);
  const uint32_t it = BorneLarge(base, tete, cle);
  const uint32_t mots[1] = {it != tete && ComparerLarges(base, cle, it + 12) >= 0 ? it : tete};
  FinMots(ctx, base, __imp__sub_823C67E0, "823C67E0", sortie, mots, 1);
  ctx.r3.u64 = sortie;
}

// sub_825EBAA0 (r3) : objet local construit, passé à la case 12, détruit.
REX_HOOK_RAW(sub_825EBAA0) {
  if (!Pilote()) return __imp__sub_825EBAA0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 256);
  const uint32_t o = ctx.r1.u32 + 80;
  Store32(base, o, 0);
  Store32(base, o + 4, 32);
  Store32(base, o + 8, 0);
  Store32(base, o + 12, 0);
  Store32(base, o + 16, 0);
  Store32(base, o + 20, 0);
  Store32(base, o + 148, 0);
  ctx.r3.u64 = o;
  sub_821E9F98(ctx, base);
  ctx.r4.u64 = o;
  TL_V(self, 12);
  ctx.r3.u64 = o;
  sub_821AEDE0(ctx, base);
}

namespace {
// sub_821A4AC8 : UVector2 de CEGUI en pixels (échelle * taille arrondie, plus décalage).
void UVecAbsolu(const uint8_t* base, const uint32_t uv, const uint32_t taille, float sortie[2]) {
#pragma clang fp contract(off)
  const float zero = LoadFloat(base, 0x820AA2C0 + 68), plus = LoadFloat(base, 0x820AA2C0 + 104);
  const float moins = LoadFloat(base, 0x820AA2C0 + 120);
  const float vy = LoadFloat(base, uv + 8) * LoadFloat(base, taille + 4);
  const float vx = LoadFloat(base, uv) * LoadFloat(base, taille);
  const float dy = vy > zero ? plus : moins;
  const float dx = vx > zero ? plus : moins;
  const float ay = vy + dy, ax = vx + dx;
  const float ox = LoadFloat(base, uv + 4), oy = LoadFloat(base, uv + 12);
  sortie[0] = float(FctiwzLikeGuest(double(ax))) + ox;
  sortie[1] = float(FctiwzLikeGuest(double(ay))) + oy;
}

// sub_821A4A50 : taille du parent, ou de l'écran (case 72 du moteur de rendu, écrit dans tampon).
uint32_t TailleParent(PPCContext& ctx, uint8_t* base, uint32_t fenetre, uint32_t tampon) {
  const uint32_t parent = Load32(base, fenetre + 80);
  if (parent) return parent + 280;
  const uint32_t rendu = Load32(base, Load32(base, 0x835067CC) + 28);
  ctx.r3.u64 = tampon;
  ctx.r4.u64 = rendu;
  GuestFunc(base, Slot(base, rendu, 72))(ctx, base);
  return ctx.r3.u32;
}

uint32_t TailleEcran(PPCContext& ctx, uint8_t* base, uint32_t tampon) {
  const uint32_t rendu = Load32(base, Load32(base, 0x835067CC) + 28);
  ctx.r3.u64 = tampon;
  ctx.r4.u64 = rendu;
  GuestFunc(base, Slot(base, rendu, 72))(ctx, base);
  return ctx.r3.u32;
}

bool RectsEgaux(const uint8_t* base, uint32_t a, uint32_t b) {
  return LoadFloat(base, a) == LoadFloat(base, b) && LoadFloat(base, a + 4) == LoadFloat(base, b + 4) &&
         LoadFloat(base, a + 8) == LoadFloat(base, b + 8) && LoadFloat(base, a + 12) == LoadFloat(base, b + 12);
}

// sub_821A4020 : nouvelle position (4 flottants) et même écart pour le rectangle qui suit.
void DeplacerRect(uint8_t* base, uint32_t r, uint32_t aire) {
#pragma clang fp contract(off)
  float ecart[4];
  for (uint32_t k = 0; k < 4; k++) ecart[k] = LoadFloat(base, r + 16 + k * 4) - LoadFloat(base, r + k * 4);
  for (uint32_t k = 0; k < 4; k++) Store32(base, r + k * 4, Load32(base, aire + k * 4));
  for (uint32_t k = 0; k < 4; k++) StoreFloat(base, r + 16 + k * 4, LoadFloat(base, r + k * 4) + ecart[k]);
}
}  // namespace

REX_HOOK_RAW(sub_821A4AC8) {
  if (!PiloteOuValidation()) return __imp__sub_821A4AC8(ctx, base);
  const uint32_t sortie = ctx.r3.u32;
  ctx.fpscr.disableFlushMode();
  float v[2];
  UVecAbsolu(base, ctx.r4.u32, ctx.r5.u32, v);
  const uint32_t mots[2] = {FloatBits(v[0]), FloatBits(v[1])};
  FinMots(ctx, base, __imp__sub_821A4AC8, "821A4AC8", sortie, mots, 2);
  ctx.r3.u64 = sortie;
}

REX_HOOK_RAW(sub_821A4A50) {
  if (!Pilote()) return __imp__sub_821A4A50(ctx, base);
  const uint32_t sortie = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 112);
  const uint32_t t = TailleParent(ctx, base, ctx.r4.u32, ctx.r1.u32 + 80);
  Store32(base, sortie, Load32(base, t));
  Store32(base, sortie + 4, Load32(base, t + 4));
  ctx.r3.u64 = sortie;
}

// sub_821A4E48 (r3, r4 rectangles) : différents ?
REX_HOOK_RAW(sub_821A4E48) {
  if (!PiloteOuValidation()) return __imp__sub_821A4E48(ctx, base);
  FinR3(ctx, base, __imp__sub_821A4E48, "821A4E48", RectsEgaux(base, ctx.r3.u32, ctx.r4.u32) ? 0 : 1);
}

REX_HOOK_RAW(sub_821A4020) {
  if (!Pilote()) return __imp__sub_821A4020(ctx, base);
  ctx.fpscr.disableFlushMode();
  DeplacerRect(base, ctx.r3.u32, ctx.r4.u32);
}

// sub_821A4B78 (r3 fenêtre, r4 position, r5 taille, r6, r7 événements) : Window::setArea_impl de CEGUI, avec bornes.
REX_HOOK_RAW(sub_821A4B78) {
#pragma clang fp contract(off)
  if (!Pilote()) return __imp__sub_821A4B78(ctx, base);
  const uint32_t self = ctx.r3.u32, position = ctx.r4.u32, aire = ctx.r5.u32;
  const bool garder = (ctx.r6.u32 & 0xFF) != 0, evenements = (ctx.r7.u32 & 0xFF) != 0;
  CadreInvite cadre(ctx, base, 224);
  const uint32_t sp = ctx.r1.u32;
  const float ancien_x = LoadFloat(base, self + 280), ancien_y = LoadFloat(base, self + 284);
  base[self + 624] = 0;
  base[self + 644] = 0;
  base[self + 664] = 0;
  base[self + 684] = 0;

  float maxi[2], mini[2], v[2];
  const uint32_t e1 = TailleEcran(ctx, base, sp + 88);
  ctx.fpscr.disableFlushMode();
  UVecAbsolu(base, self + 336, e1, maxi);
  const uint32_t e2 = TailleEcran(ctx, base, sp + 80);
  ctx.fpscr.disableFlushMode();
  UVecAbsolu(base, self + 320, e2, mini);
  const uint32_t pere = TailleParent(ctx, base, self, sp + 80);
  ctx.fpscr.disableFlushMode();
  UVecAbsolu(base, aire, pere, v);
  // Le jeu relit le flottant écrit : bornes appliquées dans le même ordre (NaN : aucune borne).
  for (int k = 0; k < 2; k++) {
    if (v[k] < mini[k]) {
      v[k] = mini[k];
    } else if (v[k] > maxi[k]) {
      v[k] = maxi[k];
    }
    StoreFloat(base, self + 280 + k * 4, v[k]);
  }
  // Rectangle self+264 : position + aire (x0, y0, x1, y1).
  for (uint32_t k = 0; k < 4; k++) {
    StoreFloat(base, self + 264 + k * 4, LoadFloat(base, aire + k * 4) + LoadFloat(base, self + 248 + k * 4));
  }
  const bool change = !(v[0] == ancien_x && v[1] == ancien_y);
  bool bouge = false;
  if ((!garder || change) && !RectsEgaux(base, position, self + 248)) {
    DeplacerRect(base, self + 248, position);
    bouge = true;
  }
  if (evenements) {
    const uint32_t a = sp + 128;
    Store32(base, a, 0x820CEF98);
    base[a + 4] = 0;
    base[a + 5] = 1;
    Store32(base, a + 8, self);
    if (bouge) {
      ctx.r4.u64 = a;
      TL_V(self, 48);
      base[a + 4] = 0;
    }
    if (change) {
      ctx.r4.u64 = a;
      TL_V(self, 44);
    }
    Store32(base, a, 0x820CEF90);
  }
  if (bouge || change) {
    ctx.r3.u64 = Load32(base, 0x835067CC);
    sub_821A4EC8(ctx, base);
  }
}

// sub_821C00C0 (r3, r4 préfixe) : animation dont le nom en majuscules commence par ce préfixe, tirée au sort entre celles
// qui conviennent. Sans les deux copies de chaînes de l'original (donc sans allocation).
REX_HOOK_RAW(sub_821C00C0) {
  if (!PiloteOuValidation()) return __imp__sub_821C00C0(ctx, base);
  const uint32_t self = ctx.r3.u32, cle = ctx.r4.u32;
  const uint32_t table = Load32(base, self + 460);
  const uint32_t n = table ? Load32(base, table + 40) : 0;
  const uint32_t taille = Load32(base, cle + 16);
  const uint8_t* octets = base + DonneesChaine(base, cle);
  const auto convient = [&](uint32_t i) {
    const uint32_t s = Load32(base, table + 44) + i * 28;
    if (Load32(base, s + 16) < taille) return false;
    const uint8_t* c = base + DonneesChaine(base, s);
    for (uint32_t k = 0; k < taille; k++) {
      const uint8_t u = c[k] >= 'a' && c[k] <= 'z' ? uint8_t(c[k] - 32) : c[k];
      if (u != octets[k]) return false;
    }
    return true;
  };
  if (Mode() == 2) {
    int32_t premier = -1;
    uint32_t vus = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (!convient(i)) continue;
      if (!vus++) premier = int32_t(i);
    }
    if (vus >= 2) return __imp__sub_821C00C0(ctx, base);  // le tirage avance : pas de second calcul
    FinR3(ctx, base, __imp__sub_821C00C0, "821C00C0", uint32_t(premier));
    return;
  }
  const float seuil = LoadFloat(base, 0x820AA2C0 + 780), haut = LoadFloat(base, 0x820AA2C0 + 172);
  const float bas = LoadFloat(base, 0x820AA2C0 + 68);
  int32_t choix = -1;
  for (uint32_t i = 0; i < n; i++) {
    if (!convient(i)) continue;
    if (choix != -1) {
      ctx.f2.f64 = double(haut);
      ctx.f1.f64 = double(bas);
      sub_821E4E90(ctx, base);
      if (!(ctx.f1.f64 < double(seuil))) continue;
    }
    choix = int32_t(i);
  }
  ctx.r3.s64 = choix;
}

// ---------------------------------------------------------------------------------------------------------------------
// sub_821CCF30 : mspace_free de dlmalloc (nedmalloc du jeu), appelé par tous les threads, sous le verrou récursif du tas.
// Le mot du verrou reste compatible avec l'original (échange atomique), sans la section critique globale du recompilé.
// Blocs mmappés : original. Tas corrompu : abort du jeu, comme l'original.
//
// Validation (--torchlight_ogre_natif=2) : verrou tenu, le calcul natif écrit dans un journal, puis l'original s'exécute ;
// chaque mot du journal doit être en mémoire, et aucun mot de l'en-tête du tas ne doit changer hors du journal.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
constexpr uint32_t kMagieTas = 0x835593EC;

struct TasDirect {
  uint8_t* base;
  uint32_t L(uint32_t a) const { return Load32(base, a); }
  void S(uint32_t a, uint32_t v) const { Store32(base, a, v); }
};

struct TasJournal {
  uint8_t* base;
  struct Entree {
    uint32_t a, v;
  };
  Entree e[96];
  uint32_t n = 0;
  bool deborde = false;
  uint32_t L(uint32_t a) const {
    for (uint32_t i = 0; i < n; i++) {
      if (e[i].a == a) return e[i].v;
    }
    return Load32(base, a);
  }
  void S(uint32_t a, uint32_t v) {
    for (uint32_t i = 0; i < n; i++) {
      if (e[i].a == a) {
        e[i].v = v;
        return;
      }
    }
    if (n == 96) {
      deborde = true;
      return;
    }
    e[n++] = {a, v};
  }
  bool Ecrit(uint32_t a) const {
    for (uint32_t i = 0; i < n; i++) {
      if (e[i].a == a) return true;
    }
    return false;
  }
};

enum class FinTas { kFini, kOriginal, kErreur, kReduire, kSegments };

template <class M>
bool DetacherPetit(M& m, uint32_t ms, uint32_t c, uint32_t idx) {
  const uint32_t f = m.L(c + 8), b = m.L(c + 12);
  if (f == b) {
    m.S(ms, m.L(ms) & ~(1u << idx));
    return true;
  }
  const uint32_t min = m.L(ms + 16), casier = ms + (idx + 5) * 8;
  if (!((f == casier || f >= min) && (b == casier || b >= min))) return false;
  m.S(f + 12, b);
  m.S(b + 8, f);
  return true;
}

template <class M>
bool DetacherGrand(M& m, uint32_t ms, uint32_t tp) {
  const uint32_t min = m.L(ms + 16);
  const uint32_t pere = m.L(tp + 24), x = m.L(tp + 12);
  uint32_t r = x;
  if (x != tp) {
    const uint32_t f = m.L(tp + 8);
    if (f < min) return false;
    m.S(f + 12, x);
    m.S(x + 8, f);
  } else {
    uint32_t rp = tp + 20;
    r = m.L(rp);
    if (!r) {
      rp = tp + 16;
      r = m.L(rp);
    }
    if (r) {
      for (;;) {
        uint32_t cp = r + 20;
        if (!m.L(cp)) {
          cp = r + 16;
          if (!m.L(cp)) break;
        }
        rp = cp;
        r = m.L(cp);
      }
      if (rp < min) return false;
      m.S(rp, 0);
    }
  }
  if (!pere) return true;
  const uint32_t idx = m.L(tp + 28), h = ms + (idx + 76) * 4;
  if (tp == m.L(h)) {
    m.S(h, r);
    if (!r) {
      m.S(ms + 4, m.L(ms + 4) & ~(1u << idx));
      return true;
    }
  } else {
    if (pere < min) return false;
    m.S(m.L(pere + 16) == tp ? pere + 16 : pere + 20, r);
    if (!r) return true;
  }
  if (r < min) return false;
  m.S(r + 24, pere);
  const uint32_t c0 = m.L(tp + 16);
  if (c0) {
    if (c0 < min) return false;
    m.S(r + 16, c0);
    m.S(c0 + 24, r);
  }
  const uint32_t c1 = m.L(tp + 20);
  if (c1) {
    if (c1 < min) return false;
    m.S(r + 20, c1);
    m.S(c1 + 24, r);
  }
  return true;
}

template <class M>
FinTas LibererBloc(M& m, uint32_t ms, uint32_t p) {
  const uint32_t min = m.L(ms + 16);
  if (p < min) return FinTas::kErreur;
  const uint32_t tete = m.L(p + 4);
  if (!(tete & 2)) return FinTas::kErreur;
  uint32_t taille = tete & ~7u;
  const uint32_t suivant = p + taille;
  if (!(tete & 1)) {
    const uint32_t avant = m.L(p);
    if (avant & 1) return FinTas::kOriginal;  // bloc mmappé
    taille += avant;
    p -= avant;
    if (p < min) return FinTas::kErreur;
    if (p == m.L(ms + 20)) {
      const uint32_t ts = m.L(suivant + 4);
      if ((ts & 3) == 3) {
        m.S(ms + 8, taille);
        m.S(suivant + 4, ts & ~1u);
        m.S(p + 4, taille | 1);
        m.S(p + taille, taille);
        return FinTas::kFini;
      }
    } else if ((avant >> 3) < 32) {
      if (!DetacherPetit(m, ms, p, avant >> 3)) return FinTas::kErreur;
    } else if (!DetacherGrand(m, ms, p)) {
      return FinTas::kErreur;
    }
  }
  if (p >= suivant) return FinTas::kErreur;
  const uint32_t ts = m.L(suivant + 4);
  if (!(ts & 1)) return FinTas::kErreur;
  if (ts & 2) {
    m.S(suivant + 4, ts & ~1u);
    m.S(p + 4, taille | 1);
    m.S(p + taille, taille);
  } else {
    if (suivant == m.L(ms + 24)) {
      const uint32_t t = m.L(ms + 12) + taille;
      m.S(ms + 24, p);
      m.S(ms + 12, t);
      m.S(p + 4, t | 1);
      if (p == m.L(ms + 20)) {
        m.S(ms + 20, 0);
        m.S(ms + 8, 0);
      }
      return t > m.L(ms + 28) ? FinTas::kReduire : FinTas::kFini;
    }
    if (suivant == m.L(ms + 20)) {
      const uint32_t d = m.L(ms + 8) + taille;
      m.S(ms + 20, p);
      m.S(ms + 8, d);
      m.S(p + 4, d | 1);
      m.S(p + d, d);
      return FinTas::kFini;
    }
    taille += ts & ~7u;
    if ((ts >> 3) < 32) {
      if (!DetacherPetit(m, ms, suivant, ts >> 3)) return FinTas::kErreur;
    } else if (!DetacherGrand(m, ms, suivant)) {
      return FinTas::kErreur;
    }
    m.S(p + 4, taille | 1);
    m.S(p + taille, taille);
    if (p == m.L(ms + 20)) {
      m.S(ms + 8, taille);
      return FinTas::kFini;
    }
  }
  // Rangé dans un casier : petit, ou arbre des grands.
  const uint32_t idx = taille >> 3;
  if (idx < 32) {
    const uint32_t b = ms + (idx + 5) * 8, carte = m.L(ms), bit = 1u << idx;
    if (!(carte & bit)) {
      m.S(ms, carte | bit);
      m.S(b + 8, p);
      m.S(b + 12, p);
      m.S(p + 8, b);
      m.S(p + 12, b);
      return FinTas::kFini;
    }
    const uint32_t f = m.L(b + 8);
    if (f < m.L(ms + 16)) return FinTas::kErreur;
    m.S(b + 8, p);
    m.S(f + 12, p);
    m.S(p + 8, f);
    m.S(p + 12, b);
    return FinTas::kFini;
  }
  const uint32_t x = taille >> 8;
  uint32_t ti = 0;
  if (x > 0xFFFF) {
    ti = 31;
  } else if (x) {
    const uint32_t k = 31 - uint32_t(__builtin_clz(x));
    ti = (k << 1) + ((taille >> (k + 7)) & 1);
  }
  m.S(p + 28, ti);
  m.S(p + 20, 0);
  m.S(p + 16, 0);
  const uint32_t h = ms + (ti + 76) * 4, carte = m.L(ms + 4), bit = 1u << ti;
  if (!(carte & bit)) {
    m.S(ms + 4, carte | bit);
    m.S(h, p);
    m.S(p + 24, h);
    m.S(p + 12, p);
    m.S(p + 8, p);
  } else {
    uint32_t t = m.L(h);
    uint32_t k = taille << (ti == 31 ? 0 : 25 - (ti >> 1));
    for (;;) {
      if ((m.L(t + 4) & ~7u) == taille) {
        const uint32_t mini = m.L(ms + 16), f = m.L(t + 8);
        if (t < mini || f < mini) return FinTas::kErreur;
        m.S(f + 12, p);
        m.S(t + 8, p);
        m.S(p + 8, f);
        m.S(p + 12, t);
        m.S(p + 24, 0);
        break;
      }
      const uint32_t cp = t + 16 + (k >> 31) * 4;
      k <<= 1;
      const uint32_t c = m.L(cp);
      if (!c) {
        if (cp < m.L(ms + 16)) return FinTas::kErreur;
        m.S(cp, p);
        m.S(p + 24, t);
        m.S(p + 12, p);
        m.S(p + 8, p);
        break;
      }
      t = c;
    }
  }
  const uint32_t reste = m.L(ms + 32) - 1;
  m.S(ms + 32, reste);
  return reste ? FinTas::kFini : FinTas::kSegments;
}

std::atomic<uint32_t>* MotVerrouTas(uint8_t* base, uint32_t ms) {
  return reinterpret_cast<std::atomic<uint32_t>*>(base + ms + 444);
}

// Verrou récursif du tas : +444 le mot, +448 le compte, +452 le propriétaire.
void PrendreVerrouTas(PPCContext& ctx, uint8_t* base, uint32_t ms) {
  const uint32_t moi = Load32(base, Load32(base, ctx.r13.u32 + 256) + 332);
  std::atomic<uint32_t>* mot = MotVerrouTas(base, ms);
  for (uint32_t essais = 0;;) {
    if (mot->load(std::memory_order_relaxed) != 0) {
      if (Load32(base, ms + 452) == moi) {
        Store32(base, ms + 448, Load32(base, ms + 448) + 1);
        return;
      }
    } else if (mot->exchange(__builtin_bswap32(1u), std::memory_order_seq_cst) == 0) {
      Store32(base, ms + 448, moi);
      Store32(base, ms + 452, moi);
      Store32(base, ms + 448, 1);
      return;
    }
    if ((++essais & 63) == 0) {
      CadreInvite cadre(ctx, base, 192);
      ctx.r3.u64 = 0;
      ctx.r4.u64 = 0;
      sub_8287D840(ctx, base);
    }
  }
}

void RendreVerrouTas(uint8_t* base, uint32_t ms) {
  const uint32_t reste = Load32(base, ms + 448) - 1;
  Store32(base, ms + 448, reste);
  if (reste) return;
  Store32(base, ms + 452, 0);
  MotVerrouTas(base, ms)->exchange(0, std::memory_order_seq_cst);
}
}  // namespace

REX_HOOK_RAW(sub_821CCF30) {
  if (!PiloteOuValidation()) return __imp__sub_821CCF30(ctx, base);
  const uint32_t mem = ctx.r3.u32, bloc = mem - 8;
  const uint32_t magie = Load32(base, kMagieTas);
  const uint32_t ms = Load32(base, bloc + (Load32(base, mem - 4) & ~7u)) ^ magie;
  if (Load32(base, ms + 36) != magie) return __imp__sub_821CCF30(ctx, base);  // message d'erreur du jeu
  const bool verrou = (Load32(base, ms + 440) & 2) != 0;
  if (verrou) PrendreVerrouTas(ctx, base, ms);

  if (Mode() == 2) {
    TasJournal j{base};
    const FinTas fin = LibererBloc(j, ms, bloc);
    uint32_t avant[114];
    for (uint32_t i = 0; i < 114; i++) avant[i] = Load32(base, ms + i * 4);
    ctx.r3.u64 = mem;
    __imp__sub_821CCF30(ctx, base);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (fin == FinTas::kFini && !j.deborde) {
      static std::atomic<uint64_t> compares{0};
      const uint64_t vus = compares.fetch_add(1, std::memory_order_relaxed);
      if (vus % 20000 == 0) REXLOG_INFO("[ogre natif] free du tas : {} blocs comparés", vus);
      for (uint32_t i = 0; i < j.n; i++) {
        const uint32_t reel = Load32(base, j.e[i].a);
        if (reel != j.e[i].v) {
          CompterDifference(__LINE__);
          Difference("821CCF30", j.e[i].a - ms, j.e[i].v, reel);
        }
      }
      // Le compte et le propriétaire du verrou (+448, +452) reviennent à leur valeur ; le mot aussi.
      for (uint32_t i = 0; i < 114; i++) {
        const uint32_t a = ms + i * 4;
        if (Load32(base, a) != avant[i] && !j.Ecrit(a)) {
          CompterDifference(__LINE__);
          Difference("821CCF30", i * 4, avant[i], Load32(base, a));
        }
      }
    }
    if (verrou) RendreVerrouTas(base, ms);
    Report();
    return;
  }

  TasDirect m{base};
  const FinTas fin = LibererBloc(m, ms, bloc);
  switch (fin) {
    case FinTas::kFini:
      break;
    case FinTas::kOriginal:
      if (verrou) RendreVerrouTas(base, ms);
      ctx.r3.u64 = mem;
      return __imp__sub_821CCF30(ctx, base);
    case FinTas::kErreur: {
      CadreInvite cadre(ctx, base, 192);
      sub_8285C900(ctx, base);
      break;
    }
    case FinTas::kReduire: {
      CadreInvite cadre(ctx, base, 192);
      ctx.r3.u64 = ms;
      sub_8241DA58(ctx, base);
      break;
    }
    case FinTas::kSegments: {
      CadreInvite cadre(ctx, base, 192);
      ctx.r3.u64 = ms;
      sub_8241D610(ctx, base);
      break;
    }
  }
  if (Load32(base, ms + 440) & 2) RendreVerrouTas(base, ms);
}

// --- Natives du relevé du 2026-11-04 (combat, GPU à 460,8 MHz : switch/RENDU_NATIF.md, fin du journal) ---
REX_EXTERN(__imp__sub_821C1608);
REX_EXTERN(__imp__sub_821C1098);
REX_EXTERN(__imp__sub_821C16A0);
REX_EXTERN(__imp__sub_821C46D0);
REX_EXTERN(__imp__sub_821BE428);
REX_EXTERN(__imp__sub_82194138);
REX_EXTERN(__imp__sub_821C30B0);
REX_EXTERN(__imp__sub_821C1E50);
REX_EXTERN(__imp__sub_821A5150);
REX_EXTERN(__imp__sub_821A7B30);
REX_EXTERN(__imp__sub_821E4588);
REX_EXTERN(__imp__sub_82284BA0);
REX_EXTERN(sub_821C1E50);
REX_EXTERN(sub_824639B8);
REX_EXTERN(sub_821C28D8);
REX_EXTERN(sub_82194008);
REX_EXTERN(sub_821A4FD0);
REX_EXTERN(sub_824FF600);
REX_EXTERN(sub_821C32A0);
REX_EXTERN(sub_821C2EC0);

// AutoParamDataSource : matrices recalculées seulement si leur drapeau est levé ; le chemin courant ne fait que rendre l'adresse.
REX_HOOK_RAW(sub_821C1608) {
  const uint32_t self = ctx.r3.u32;
  if (!Pilote() || base[self + 19167]) return __imp__sub_821C1608(ctx, base);
  ctx.r3.u64 = self + 16408;
}
REX_HOOK_RAW(sub_821C1098) {
  const uint32_t self = ctx.r3.u32;
  if (!Pilote() || base[self + 19174]) return __imp__sub_821C1098(ctx, base);
  ctx.r3.u64 = self + 16856;
}
REX_HOOK_RAW(sub_821C16A0) {
  const uint32_t self = ctx.r3.u32;
  if (!Pilote() || base[self + 19165]) return __imp__sub_821C16A0(ctx, base);
  ctx.r3.u64 = self + 19000;
}

// sub_821C46D0 (r3) : case 248, puis case 216 si elle répond vrai.
REX_HOOK_RAW(sub_821C46D0) {
  if (!Pilote()) return __imp__sub_821C46D0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 96);
  TL_V(self, 248);
  if (!(ctx.r3.u32 & 0xFF)) return;
  TL_V(self, 216);
}

// sub_821BE428 (r3) : getBestTechnique du matériau de l'objet (case 4, puis son +4).
REX_HOOK_RAW(sub_821BE428) {
  if (!Pilote()) return __imp__sub_821BE428(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 96);
  TL_M(self, 4, 824FF600);
  ctx.r3.u64 = Load32(base, ctx.r3.u32 + 4);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = self;
  sub_821CF100(ctx, base);
}

// sub_82194138 (r3, f1) : sub_82194008 pour chaque élément de la liste +172 (données +0, nombre +4, capacité +8).
REX_HOOK_RAW(sub_82194138) {
  if (!Pilote()) return __imp__sub_82194138(ctx, base);
  const uint32_t liste = ctx.r3.u32 + 172;
  const double dt = ctx.f1.f64;
  CadreInvite cadre(ctx, base, 128);
  for (uint32_t i = 0; i < Load32(base, liste + 4); i++) {
    const uint32_t d = Load32(base, liste);
    ctx.r3.u64 = Load32(base, i < Load32(base, liste + 8) ? d + i * 4 : d);
    ctx.f1.f64 = dt;
    sub_82194008(ctx, base);
  }
}

// sub_821C1E50 (r3, r4 matrice) : écriture d'une matrice dans les constantes (+72, indice +76), convertie si +98 du tampon.
REX_HOOK_RAW(sub_821C1E50) {
  if (!Pilote()) return __imp__sub_821C1E50(ctx, base);
  const uint32_t tampon = Load32(base, ctx.r3.u32 + 72);
  if (!tampon) return;
  const uint32_t indice = Load32(base, ctx.r3.u32 + 76);
  uint32_t source = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 176);
  if (base[tampon + 98]) {
    ctx.r3.u64 = ctx.r1.u32 + 80;
    sub_824639B8(ctx, base);
    source = ctx.r1.u32 + 80;
  }
  ctx.r3.u64 = Load32(base, tampon) + (indice << 2);
  ctx.r4.u64 = source;
  ctx.r5.u64 = 64;
  sub_821C28D8(ctx, base);
}

// sub_821C30B0 (r3) : pour chaque entrée (124 octets, vecteur +40) qui a un frustum (+4) et une cible (+48) : matrice fixe
// 0x8349FF00 x case 328 x case 336, écrite dans la cible. La copie intermédiaire de l'original (sur sa pile) est sautée.
REX_HOOK_RAW(sub_821C30B0) {
  if (!Pilote()) return __imp__sub_821C30B0(ctx, base);
  const uint32_t self = ctx.r3.u32;
  const auto nombre = [&] { return uint32_t(int32_t(Load32(base, self + 44) - Load32(base, self + 40)) / 124); };
  if (!nombre()) return;
  CadreInvite cadre(ctx, base, 336);
  const uint32_t sp = ctx.r1.u32;
  for (uint32_t i = 0; i < nombre(); i++) {
    const uint32_t e = Load32(base, self + 40) + i * 124;
    if (!Load32(base, e + 4) || !Load32(base, e + 48)) continue;
    TL_M(Load32(base, e + 4), 328, 821C32A0);
    ctx.r5.u64 = ctx.r3.u32;
    ctx.r4.u64 = 0x8349FF00;
    ctx.r3.u64 = sp + 144;
    sub_821C2CC8(ctx, base);
    TL_M(Load32(base, e + 4), 336, 821C2EC0);
    ctx.r5.u64 = ctx.r3.u32;
    ctx.r4.u64 = sp + 144;
    ctx.r3.u64 = sp + 80;
    sub_821C2CC8(ctx, base);
    ctx.r4.u64 = sp + 80;
    ctx.r3.u64 = Load32(base, e + 48);
    sub_821C1E50(ctx, base);
  }
}

namespace {
// Comparateur du tri des objets transparents (même logique que le crochet de sub_821A5058, plus haut), sur des éléments déjà lus :
// a (a0 objet, a1 passe) avant b ? f : foncteur.
// Attention : ble et bgt de l'original traduits tels quels (NaN).
bool ProfondeurAvant(PPCContext& ctx, uint8_t* base, uint32_t f, uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
  if (a0 == b0) return Load32(base, a1 + 52) < Load32(base, b1 + 52);
  ctx.r4.u64 = Load32(base, f);
  TL_M(a0, 32, 821A4FD0);
  const double fa = ctx.f1.f64;
  ctx.r4.u64 = Load32(base, f);
  TL_M(b0, 32, 821A4FD0);
  const double fb = ctx.f1.f64;
  ctx.fpscr.disableFlushMode();
  if (!(std::fabs(float(fb - fa)) > LoadFloat(base, kConstants + 224))) return a1 < b1;
  return fa > fb;
}
}  // namespace

// sub_821A5150 (r3 début, r4 fin, r5 foncteur) : tri par insertion d'éléments de 8 octets (std::_Insertion_sort).
REX_HOOK_RAW(sub_821A5150) {
  if (!Pilote()) return __imp__sub_821A5150(ctx, base);
  const uint32_t debut = ctx.r3.u32, fin = ctx.r4.u32, f = ctx.r5.u32;
  if (debut + 8 == fin) return;
  CadreInvite cadre(ctx, base, 160);
  for (uint32_t cur = debut + 8; cur != fin; cur += 8) {
    const uint32_t v0 = Load32(base, cur), v1 = Load32(base, cur + 4);
    uint32_t trou = cur;
    if (ProfondeurAvant(ctx, base, f, v0, v1, Load32(base, debut), Load32(base, debut + 4))) {
      for (; trou != debut; trou -= 8) {
        Store32(base, trou, Load32(base, trou - 8));
        Store32(base, trou + 4, Load32(base, trou - 4));
      }
    } else {
      for (uint32_t p = cur - 8; ProfondeurAvant(ctx, base, f, v0, v1, Load32(base, p), Load32(base, p + 4)); p -= 8) {
        Store32(base, trou, Load32(base, p));
        Store32(base, trou + 4, Load32(base, p + 4));
        trou = p;
      }
    }
    Store32(base, trou, v0);
    Store32(base, trou + 4, v1);
  }
}

// sub_821A7B30 (r3) : sans +96, sub_821A7128 sur la première entrée de la map +56 ; sinon sub_821A71E8 sur chaque entrée active
// et récente (image de +20 à au plus 1 image de celle de Root).
REX_HOOK_RAW(sub_821A7B30) {
  if (!Pilote()) return __imp__sub_821A7B30(ctx, base);
  const uint32_t self = ctx.r3.u32;
  CadreInvite cadre(ctx, base, 112);
  if (!Load32(base, self + 96)) {
    ctx.r3.u64 = Load32(base, Load32(base, Load32(base, Load32(base, self + 56)) + 16));
    sub_821A7128(ctx, base);
    return;
  }
  const uint32_t tete = Load32(base, self + 56), image = Load32(base, Load32(base, 0x835067D4) + 224);
  uint32_t it = Load32(base, tete);
  if (it == tete) return;
  do {
    const uint32_t e = Load32(base, it + 16);
    if (base[e + 4] && Load32(base, e) && image - Load32(base, e + 20) <= 1) {
      ctx.r3.u64 = self;
      ctx.r4.u64 = Load32(base, self + 96);
      ctx.r5.u64 = e;
      sub_821A71E8(ctx, base);
    }
    it = TreeNext(base, it);
  } while (it != Load32(base, self + 56));
}

// sub_821E4588 (r3 unité) : propriété L"ATTACHES_TO_MASTER" de l'unité (recherche par nom dans +376), octet +8. L'original
// construit à chaque appel une std::wstring de 18 caractères, donc une allocation et une libération : une clé posée sur la pile,
// qui montre le texte de l'image, suffit à la recherche (FindByName), qui ne fait que la lire.
REX_HOOK_RAW(sub_821E4588) {
  if (!PiloteOuValidation()) return __imp__sub_821E4588(ctx, base);
  const uint32_t cle = ctx.r1.u32 - 48;
  Store32(base, cle, 0x82000F50);
  Store32(base, cle + 16, 18);
  Store32(base, cle + 20, 18);
  const uint32_t r = FindByName(base, Load32(base, ctx.r3.u32 + 376), cle);
  FinR3(ctx, base, __imp__sub_821E4588, "821E4588", r ? base[r + 8] : 0);
}

// sub_82284BA0 (f1 x) : 0,001 puissance x, par des instructions vectorielles sur quatre valeurs dont une seule sert (log2f quatre
// fois à chaque appel). Mêmes formules que vlogefp et vexptefp du générateur (sdk/src/codegen/builders/vector.cpp) sur cette
// valeur. x nul, très petit, NaN ou résultat hors de la plage normale : laissés à l'original.
REX_HOOK_RAW(sub_82284BA0) {
  if (!PiloteOuValidation()) return __imp__sub_82284BA0(ctx, base);
  const float x = float(ctx.f1.f64);
  static const float k = std::log2(LoadFloat(base, 0x82114E30));
  const float m = k * x;
  if (x == 0.0f || !(std::fabs(x) >= 1.0e-30f) || !(std::fabs(m) < 100.0f)) return __imp__sub_82284BA0(ctx, base);
  ctx.fpscr.disableFlushMode();
  const float n = std::floor(m), f = m - n;
  float p = 1.8775767e-3f;
  p = p * f + 8.9893397e-3f;
  p = p * f + 5.5826318e-2f;
  p = p * f + 2.4015361e-1f;
  p = p * f + 6.9315308e-1f;
  p = p * f + 1.0f;
  const uint32_t bits = uint32_t(int32_t(n) + 127) << 23;
  float deux;
  std::memcpy(&deux, &bits, 4);
  const float r = p * deux;
  if (Mode() == 1) {
    ctx.f1.f64 = double(r);
    return;
  }
  __imp__sub_82284BA0(ctx, base);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (float(ctx.f1.f64) != r) {
    CompterDifference(__LINE__);
    uint32_t natif, original;
    const float o = float(ctx.f1.f64);
    std::memcpy(&natif, &r, 4);
    std::memcpy(&original, &o, 4);
    Difference("82284BA0", 0, natif, original);
  }
  Report();
}

// --- Interface : pas de reconstruction à chaque image pour des fenêtres qui ne sont pas à l'écran (2026-11-04) ---
// System::renderGUI de CEGUI (sub_821FBC60) ne reconstruit la liste des quadrilatères que si son drapeau +40 est levé. Traçage
// sur PC (gdb, point d'observation) : levé à chaque image, toujours par le même chemin : sub_821A40E0 -> sub_821A41A0 ->
// sub_821A4B78 -> sub_821A4938 (Window::onMoved). Ce sont 14 petits boutons animés (<page>_lefttriggerbutton,
// <page>_righttriggerbutton) de pages de menus chargées, qui oscillent en permanence alors que leur page n'est pas sous la feuille
// active : jamais dessinés, ils coûtaient tout de même le parcours de l'arbre (console : 5,9 % du thread principal en combat, 18,4 %
// menu ouvert), la liste et le remplissage du tampon de sommets. Une fenêtre ne lève plus le drapeau en bougeant si renderGUI ne
// la dessine pas (cachée, ou hors de la feuille active). Tout le reste est conservé (enfants prévenus, événement).
// CEGUI garde les quadrilatères de chaque fenêtre en cache et ne les refait que sur requestRedraw : un changement de contenu lève
// donc déjà le drapeau. Runic aurait pu écrire des widgets qui s'en dispensent, jamais remarqués puisque tout était refait à chaque
// image.
// Sur la console, en vrai jeu : encore 85 % d'images avec une demande (96 % sur la fin de la session), par d'autres chemins : les
// 107 modificateurs de CEGUI lèvent le drapeau en ligne, fenêtres cachées comprises. Mode 3 : quel que soit le demandeur, une
// demande n'est suivie que si une fenêtre dessinée a son besoin de redessin levé (+424, requestRedraw) ou a bougé, ou si la liste
// des fenêtres dessinées a changé. Mode 2 : valide cette règle : liste refaite à chaque image, comparée à l'octet à celle d'avant
// quand la règle n'aurait rien refait ; avec --torchlight_journal_stats, nomme les fenêtres responsables.
// Console, vrai combat (2026-11-04) : 14 400 images, 0 différence ; 27 % d'images sans reconstruction sur la session, peu en combat :
// barre de mana, chiffres de dégâts (des fenêtres de CEGUI) changent vraiment à presque chaque image.
REXCVAR_DEFINE_INT32(torchlight_interface, 3, "Torchlight",
                     "Interface : 0 = comme le jeu ; 1 = les fenêtres qui ne sont pas dessinées ne demandent plus de "
                     "reconstruction en bougeant ; 2 = validation (reconstruite à chaque image, comparée à l'octet) ; 3 = "
                     "reconstruction seulement si une fenêtre dessinée a changé")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__sub_821A4938);
REX_EXTERN(__imp__sub_821FBC60);
REX_EXTERN(sub_825B1AF8);

namespace {
constexpr uint32_t kSystemeCegui = 0x835067CC;  // CEGUI::System::ms_Singleton

int ModeInterface() {
  static const int mode = REXCVAR_GET(torchlight_interface);
  return mode;
}

// La fenêtre, et ses parents jusqu'à la feuille active, sont visibles : DessinerFenetre l'atteint depuis cette feuille.
bool FenetreDessinee(const uint8_t* base, uint32_t fenetre, uint32_t feuille) {
  for (uint32_t p = fenetre; p; p = Load32(base, p + 80)) {
    if (!base[p + 353]) return false;
    if (p == feuille) return true;
  }
  return false;
}

// Suivi des fenêtres dessinées (thread principal seulement, comme tout CEGUI) : ce que DessinerFenetre parcourt, dans le même ordre.
struct SuiviInterface {
  std::vector<uint32_t> liste, avant;
  std::map<uint32_t, uint32_t> responsables;  // diagnostic : fenêtre -> images
  bool bougee = false;                        // une fenêtre dessinée a bougé depuis le dernier renderGUI
  uint64_t images = 0, demandes = 0, utiles = 0;
};
SuiviInterface g_suivi;

void ParcourirDessinees(const uint8_t* base, uint32_t w, bool& besoin, bool noter) {
  if (!base[w + 353]) return;
  g_suivi.liste.push_back(w);
  if (base[w + 424]) {
    besoin = true;
    if (noter) g_suivi.responsables[w]++;
  }
  const uint32_t nombre = uint32_t(int32_t(Load32(base, w + 48) - Load32(base, w + 44)) >> 2);
  const uint32_t ordre = Load32(base, w + 60);
  for (uint32_t k = 0; k < nombre; k++) ParcourirDessinees(base, Load32(base, ordre + k * 4), besoin, noter);
}

// Une fenêtre dessinée a-t-elle changé ?
bool ReconstructionUtile(const uint8_t* base, uint32_t systeme, bool noter) {
  bool besoin = g_suivi.bougee;
  g_suivi.bougee = false;
  g_suivi.liste.clear();
  const uint32_t feuille = Load32(base, systeme + 48);
  if (feuille) ParcourirDessinees(base, feuille, besoin, noter);
  if (g_suivi.liste != g_suivi.avant) besoin = true;
  g_suivi.avant.swap(g_suivi.liste);
  return besoin;
}

// Diagnostic : nom d'une fenêtre : textes UTF-32 de l'objet (type, apparence, nom de CEGUI).
std::string NomFenetre(const uint8_t* base, uint32_t w) {
  std::string r;
  std::string cur;
  for (uint32_t o = 0; o < 0x400; o += 4) {
    const uint32_t c = Load32(base, w + o);
    if (c >= 0x21 && c < 0x7F) {
      cur.push_back(char(c));
    } else {
      if (cur.size() >= 3) r += (r.empty() ? "" : "|") + cur;
      cur.clear();
    }
  }
  return r;
}

void BilanInterface(const uint8_t* base) {
  std::vector<std::pair<uint32_t, uint32_t>> v;
  for (const auto& [w, n] : g_suivi.responsables) v.emplace_back(n, w);
  std::sort(v.rbegin(), v.rend());
  std::string texte;
  for (size_t i = 0; i < v.size() && i < 12; i++) texte += fmt::format(" {} x{} ;", NomFenetre(base, v[i].second), v[i].first);
  REXLOG_INFO("[interface] {} images, {} demandes, {} utiles ; fenêtres :{}", g_suivi.images, g_suivi.demandes, g_suivi.utiles, texte);
  g_suivi.responsables.clear();
}

// Validation : liste des quadrilatères du rendu de l'interface (map +204, _Isnil +89, quadrilatère de 76 octets après l'en-tête du
// nœud), comparée à celle de l'image d'avant quand la règle n'aurait rien reconstruit.
struct ValidationInterface {
  std::vector<uint8_t> avant, liste;
  bool avant_valide = false;
  uint64_t images = 0, utiles = 0, differences = 0;
};

void ComparerInterface(const uint8_t* base, uint32_t rendu, bool utile) {
  static ValidationInterface v;
  constexpr uint32_t kQuad = 76;
  v.liste.clear();
  const uint32_t tete = Load32(base, rendu + 204);
  for (uint32_t it = Load32(base, tete); it != tete; it = TreeNext<89>(base, it)) {
    v.liste.insert(v.liste.end(), base + it + 12, base + it + 12 + kQuad);
    // Booléen +36 suivi de 3 octets de remplissage, jamais initialisés et jamais lus (sub_821D2F80) : valeurs de la pile du jeu.
    std::memset(v.liste.data() + v.liste.size() - kQuad + 37, 0, 3);
  }
  v.images++;
  if (utile) v.utiles++;
  if (!utile && v.avant_valide && v.liste != v.avant) {
    v.differences++;
    size_t i = 0;
    const size_t n = std::min(v.liste.size(), v.avant.size());
    while (i < n && v.liste[i] == v.avant[i]) i++;
    if (v.differences <= 40)
      REXLOG_WARN("[interface] différence sans reconstruction utile : {} -> {} quadrilatères, premier écart : quadrilatère {}, octet {}",
                  v.avant.size() / kQuad, v.liste.size() / kQuad, i / kQuad, i % kQuad);
  }
  v.avant.swap(v.liste);
  v.avant_valide = true;
  if (v.images % 3600 == 0)
    REXLOG_INFO("[interface] validation : {} images, {} avec reconstruction utile, {} différences", v.images, v.utiles, v.differences);
}
}  // namespace

// sub_821A4938 (r3 fenêtre, r4 arguments de l'événement) : Window::onMoved : enfants prévenus (sub_825B1AF8), drapeau de
// reconstruction, événement « Moved » (case 20 de l'EventSet +20).
REX_HOOK_RAW(sub_821A4938) {
  if (ModeInterface() == 0) return __imp__sub_821A4938(ctx, base);
  const uint32_t self = ctx.r3.u32, args = ctx.r4.u32;
  CadreInvite cadre(ctx, base, 96);
  const uint32_t nombre = uint32_t(int32_t(Load32(base, self + 48) - Load32(base, self + 44)) >> 2);
  for (uint32_t k = 0; k < nombre; k++) {
    ctx.r3.u64 = Load32(base, Load32(base, self + 44) + k * 4);
    ctx.r4.u64 = args;
    ctx.r5.u64 = args;
    sub_825B1AF8(ctx, base);
  }
  const uint32_t systeme = Load32(base, kSystemeCegui);
  if (FenetreDessinee(base, self, Load32(base, systeme + 48))) {
    base[systeme + 40] = 1;
    g_suivi.bougee = true;
    if (torchlight::coeurs::JournalStats()) g_suivi.responsables[self]++;
  }
  ctx.r4.u64 = 0x834B1838;
  ctx.r5.u64 = args;
  ctx.r6.u64 = 0x834B15D8;
  TL_V(self + 20, 20);
}

// sub_821FBC60 (r3 System) : renderGUI. Mode 3 : drapeau baissé si rien de dessiné n'a changé. Mode 2 : liste refaite à chaque
// image, comparée à l'octet quand le mode 3 ne l'aurait pas refaite.
REX_HOOK_RAW(sub_821FBC60) {
  const int mode = ModeInterface();
  if (mode == 0) return __imp__sub_821FBC60(ctx, base);
  const uint32_t systeme = ctx.r3.u32;
  const bool demande = base[systeme + 40] != 0;
  const bool stats = torchlight::coeurs::JournalStats();
  bool utile = demande;
  if (demande && mode >= 2) utile = ReconstructionUtile(base, systeme, stats);
  if (stats) {
    g_suivi.images++;
    if (demande) g_suivi.demandes++;
    if (utile) g_suivi.utiles++;
    if (g_suivi.images % 3600 == 0) BilanInterface(base);
  }
  if (mode == 1) return __imp__sub_821FBC60(ctx, base);
  if (mode == 3) {
    if (!utile) base[systeme + 40] = 0;
    return __imp__sub_821FBC60(ctx, base);
  }
  base[systeme + 40] = 1;
  __imp__sub_821FBC60(ctx, base);
  ComparerInterface(base, Load32(base, systeme + 28), utile);
}
