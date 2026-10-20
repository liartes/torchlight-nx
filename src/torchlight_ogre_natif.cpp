// torchlight - fonctions d'Ogre les plus chaudes du thread principal réécrites en C++ natif (méthode de nfsmw-nx),
// choisies d'après le profil des piles de la console (switch/RENDU_NATIF.md, 2026-10-02).
//
// --torchlight_ogre_natif : 0 = code recompilé ; 1 = natif (défaut) ; 2 = valider : le recompilé fait foi, le natif
// est calculé à côté et les différences sont comptées (bilan dans le journal toutes les 10 s).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(torchlight_ogre_natif, 1, "Torchlight",
                     "Fonctions chaudes d'Ogre en C++ natif : 0 = recompilé, 1 = natif, 2 = valider contre le recompilé")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace torchlight::ogre {
// Aussi pour les interceptions de torchlight_trace.cpp (bindGpuProgramParameters).
int Mode() {
  static const int mode = REXCVAR_GET(torchlight_ogre_natif);
  return mode;
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
    if (!same) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
    if (std::memcmp(native, base + out, 64) != 0) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
    if ((ctx.r3.u32 != 0) != visible) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
  if (!same) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
    g_diffs.fetch_add(1, std::memory_order_relaxed);
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
  if (!same) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
    g_diffs.fetch_add(1, std::memory_order_relaxed);
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
      g_diffs.fetch_add(1, std::memory_order_relaxed);
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
    g_diffs.fetch_add(1, std::memory_order_relaxed);
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
  if (!same) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
        g_diffs.fetch_add(1, std::memory_order_relaxed);
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
  if (ctx.r3.u32 != uint32_t(vis)) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
      g_diffs.fetch_add(1, std::memory_order_relaxed);
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
  if (GlyphData(ctx, base, font, cp) != original) g_diffs.fetch_add(1, std::memory_order_relaxed);
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
// texture) ; ParticleSystemUpdateValue (sub_821C8A58) et les autres restent ceux du jeu.
// Validation (mode 2) : chaque calcul natif comparé à la fonction du jeu sur le même état (sauvegardé puis remis),
// successeur dans l'arbre comparé à sub_824C6960.
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

void Diff() { g_diffs.fetch_add(1, std::memory_order_relaxed); }

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

void ControllerDestination(PPCContext& ctx, uint8_t* base, uint32_t dest, uint32_t entry, double value, bool check) {
  if (entry == 0x821D2E50 && !check) return TexCoordModifier(base, dest, value);
  if (entry == 0x821C8A58) {
    if (!check) {
      if (IdleParticleSystem(base, dest)) return;
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

// Préchargement avant le parcours (console, 2026-10-20 : dans les donjons, contrôleurs et particules ~16 % du thread
// principal, surtout des attentes de la mémoire, un objet après l'autre : l'état +284 de chaque système de particules
// coûte à lui seul la plupart du temps propre de sub_821C8A58). Les objets sont demandés par vagues : contrôleurs,
// destinations, systèmes de ParticleUniverse (destination ParticleSystemUpdateValue, sub_821C8A58 en [2] : système
// +4), puis, pour les systèmes qui se mettent à jour (état +284 ni 2 ni 3, nœud +40, +45), leurs lignes lues par
// ParticleSystem::_update (sub_821C8618) et celles de leurs techniques (vecteur +348..+352) lues par
// ParticleTechnique::_update (sub_821C7B88) et ses appels. Lectures seulement, des mêmes adresses que le parcours.
void PrefetchControllers(const uint8_t* base, uint32_t head) {
  static std::vector<uint32_t> ctrls, systems;
  ctrls.clear();
  systems.clear();
  for (uint32_t n = Load32(base, head); n != head; n = TreeNext<17>(base, n)) {
    const uint32_t c = Load32(base, n + 12);
    __builtin_prefetch(base + c + 8);
    __builtin_prefetch(base + c + 52);
    ctrls.push_back(c);
  }
  for (uint32_t c : ctrls) __builtin_prefetch(base + Load32(base, c + 24));
  for (uint32_t c : ctrls) {
    if (!base[c + 52]) continue;
    const uint32_t dest = Load32(base, c + 24);
    if (Load32(base, Load32(base, dest) + 8) != 0x821C8A58) continue;
    const uint32_t sys = Load32(base, dest + 4);
    __builtin_prefetch(base + sys + 280);
    systems.push_back(sys);
  }
  size_t running = 0;
  for (uint32_t sys : systems) {
    const uint32_t state = Load32(base, sys + 284);
    if (state == 2 || state == 3) continue;
    systems[running++] = sys;
    __builtin_prefetch(base + sys + 40);
    __builtin_prefetch(base + sys + 348);
    __builtin_prefetch(base + sys + 437);
    __builtin_prefetch(base + sys + 488);
  }
  systems.resize(running);
  for (uint32_t sys : systems) {
    if (!Load32(base, sys + 40) || !base[sys + 45]) continue;
    const uint32_t end = Load32(base, sys + 352);
    for (uint32_t t = Load32(base, sys + 348); t != end; t += 4) {
      const uint8_t* tech = base + Load32(base, t);
      for (uint32_t off : {92u, 140u, 216u, 264u, 304u, 584u, 684u, 720u, 768u}) __builtin_prefetch(tech + off);
    }
  }
}

void UpdateControllers(PPCContext& ctx, uint8_t* base, uint32_t self, bool check) {
  const uint32_t frame = Load32(base, Load32(base, kRootSingleton) + 224);
  if (frame == Load32(base, self + 56)) return;
  const uint32_t old_sp = ctx.r1.u32, sp = old_sp - 160;
  Store32(base, sp, old_sp);
  ctx.r1.u64 = sp;
  const uint32_t head = Load32(base, self + 8);
  PrefetchControllers(base, head);
  for (uint32_t n = Load32(base, head); n != head;) {
    const uint32_t ctrl = Load32(base, n + 12);
    if (base[ctrl + 52]) {
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
      ControllerDestination(ctx, base, dest, Load32(base, dest_vt + 8), v, check);
    }
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
  Store32(base, self + 56, frame);
  ctx.r1.u64 = old_sp;
}

}  // namespace

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
      g_diffs.fetch_add(1, std::memory_order_relaxed);
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
      g_diffs.fetch_add(1, std::memory_order_relaxed);
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
