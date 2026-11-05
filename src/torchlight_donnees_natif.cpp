// torchlight - primitives du chargement des données en C++ natif.
//
// Ces fonctions ne ressortent pas d'un profil moyen : elles ne tournent presque qu'au chargement d'un monstre, d'un objet ou
// d'une zone, sur le thread principal, et c'est là qu'elles font les saccades. Relevé du chien de garde (2026-11-04, 560
// échantillons pris pendant ces blocages) : profil plat, le coût est dans les feuilles appelées en masse, pas dans les
// parcours (sub_822574F0, sub_8224F068 : < 0,5 % de temps propre). Réécrites ici :
//   - PixelUtil::unpackColour et packColour d'Ogre (recoloration des textures, pixel par pixel) ;
//   - png_read_filter_row de libpng ;
//   - le décodeur IMA ADPCM de FMOD ;
//   - std::wstring (construction, assign, croissance, destruction, comparaison) ;
//   - AnimationTrack::createKeyFrame d'Ogre.
// Le lecteur zip est dans src/torchlight_inflate.cpp.
//
// --torchlight_donnees_natif : 0 = recompilé, 1 = natif, 2 = chaque appel fait des deux côtés et comparé (lignes
// « [donnees natif] » du journal). En mode 2, les chaînes sont refaites sur une copie allouée dans le tas du jeu.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_INT32(torchlight_donnees_natif, 1, "Torchlight",
                     "Primitives du chargement des données en C++ natif : 0 = recompilé, 1 = natif, 2 = valider")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__sub_82474560);  // PixelUtil::unpackColour(r, g, b, a, format, source)
REX_EXTERN(__imp__sub_82473FA0);  // PixelUtil::packColour(r, g, b, a, format, destination)
REX_EXTERN(__imp__sub_82650028);  // png_read_filter_row(png, infos, ligne, ligne précédente, filtre)
REX_EXTERN(__imp__sub_826FF2E8);  // IMA ADPCM(source, destination, blocs, taille d'un bloc, échantillons, canaux)
REX_EXTERN(__imp__sub_821AC4F8);  // wcslen
REX_EXTERN(__imp__sub_821AC490);  // wstring(const wchar_t*)
REX_EXTERN(__imp__sub_821B1C28);  // wstring::assign(ptr, n)
REX_EXTERN(__imp__sub_821EDB78);  // wstring::assign(chaîne, début, n)
REX_EXTERN(__imp__sub_821B3350);  // wstring::_Grow(n, trim)
REX_EXTERN(__imp__sub_82482528);  // wstring::_Copy(nouvelle taille, taille à garder)
REX_EXTERN(__imp__sub_821B3988);  // ~wstring
REX_EXTERN(__imp__sub_821A1478);  // wstring::compare(0, n0, ptr, n)
REX_EXTERN(__imp__sub_82226B50);  // allocateur des wstring (lève une exception si la mémoire manque)
REX_EXTERN(__imp__sub_824EE860);  // AnimationTrack::createKeyFrame(temps)
REX_EXTERN(sub_821CD7F8);         // malloc du jeu (r3 réserve, r4 taille)
REX_EXTERN(sub_821CD9A8);         // free du jeu
REX_EXTERN(sub_82480CF0);         // croissance d'un vecteur de pointeurs
REX_EXTERN(sub_825BE488);         // std::rotate sur des pointeurs
REX_EXTERN(sub_824EF810);         // NodeAnimationTrack::createKeyFrameImpl
REX_EXTERN(sub_824EF3B0);         // NodeAnimationTrack::_keyFrameDataChanged

namespace {

int Mode() {
  static const int mode = REXCVAR_GET(torchlight_donnees_natif);
  return mode;
}

inline uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

inline void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

inline uint32_t L16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}

inline void S16(uint8_t* base, uint32_t a, uint32_t v) {
  const uint16_t w = __builtin_bswap16(uint16_t(v));
  std::memcpy(base + a, &w, 2);
}

inline float LF(const uint8_t* base, uint32_t a) {
  const uint32_t v = L32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// slw et srw prennent 6 bits de décalage : 32 et plus donnent 0.
inline uint32_t Slw(uint32_t x, uint32_t s) { return (s & 0x20) ? 0 : x << (s & 0x1F); }
inline uint32_t Srw(uint32_t x, uint32_t s) { return (s & 0x20) ? 0 : x >> (s & 0x1F); }

PPCFunc* FonctionJeu(uint8_t* base, uint32_t addr) {
  constexpr uint64_t kImageEnd = 0x82000000ull + 0x15C0000ull, kCodeBase = 0x82190000ull;
  constexpr uint64_t kCodeSpan = 0xE97448ull + 0x10000ull;
  if (uint64_t(addr) - kCodeBase < kCodeSpan) {
    PPCFunc* f;
    std::memcpy(&f, base + kImageEnd + (uint64_t(addr) - kCodeBase) * 2, sizeof(f));
    if (f) return f;
  }
  return rex::runtime::ResolveIndirectFunction(addr);
}

// Compteurs de la validation.
struct Compteur {
  const char* nom;
  std::atomic<uint64_t> appels{0}, differences{0};
  explicit Compteur(const char* n) : nom(n) {}
  void Noter(bool egal) {
    const uint64_t n = appels.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t d = differences.load(std::memory_order_relaxed);
    if (!egal) {
      d = differences.fetch_add(1, std::memory_order_relaxed) + 1;
      if (d <= 8) REXLOG_WARN("[donnees natif] {} : différence (appel {})", nom, n);
    }
    if (n == 1 || n == 100 || n == 1000 || n == 10000 || n % 100000 == 0) REXLOG_INFO("[donnees natif] {} : {} comparaisons, {} différences", nom, n, d);
  }
};

Compteur g_unpack("unpackColour"), g_pack("packColour"), g_png("filtre PNG"), g_adpcm("ADPCM"), g_wcslen("wcslen"),
    g_ctor("wstring()"), g_assign("assign(ptr)"), g_assign2("assign(chaîne)"), g_grow("_Grow"), g_copy("_Copy"),
    g_cmp("compare"), g_kf("createKeyFrame");

// --- PixelUtil d'Ogre ---
// Table des formats : 44 octets par format. +4 octets par élément, +8 drapeaux (0x10 PFF_NATIVEENDIAN, 0x20 PFF_LUMINANCE,
// 1 PFF_HASALPHA), +17..+20 bits RGBA, +24..+36 masques, +40..+43 décalages. Les formats flottants et compressés (sans
// PFF_NATIVEENDIAN) restent à l'original.
constexpr uint32_t kFormats = 0x83065EB0;

// Bitwise::fixedToFixed (sub_824732C8)
inline uint32_t FixedToFixed(uint32_t v, uint32_t n, uint32_t p) {
  if (n > p) return Srw(v, n - p);
  if (n == p || v == 0) return v;
  const uint32_t masque = Slw(1, n) - 1;
  if (v == masque) return Slw(1, p) - 1;
  return masque ? (Slw(1, p) * v) / masque : 0;  // masque nul : v nul, déjà rendu (le jeu a une trappe ici)
}

// Bitwise::intRead (sub_82473388) : mots de la mémoire du jeu, gros-boutistes.
inline uint32_t IntRead(const uint8_t* base, uint32_t a, uint32_t n) {
  switch (n) {
    case 1: return base[a];
    case 2: return L16(base, a);
    case 3: return (uint32_t(base[a]) << 16) | (uint32_t(base[a + 1]) << 8) | base[a + 2];
    case 4: return L32(base, a);
    default: return 0;
  }
}

void Deballer(const uint8_t* base, uint32_t d, uint32_t drapeaux, uint32_t source, uint8_t sortie[4]) {
  const uint32_t v = IntRead(base, source, base[d + 4]);
  sortie[0] = uint8_t(FixedToFixed(Srw(v & L32(base, d + 24), base[d + 40]), base[d + 17], 8));
  if (drapeaux & 0x20) {
    sortie[1] = sortie[2] = sortie[0];
  } else {
    sortie[1] = uint8_t(FixedToFixed(Srw(v & L32(base, d + 28), base[d + 41]), base[d + 18], 8));
    sortie[2] = uint8_t(FixedToFixed(Srw(v & L32(base, d + 32), base[d + 42]), base[d + 19], 8));
  }
  sortie[3] = (drapeaux & 1) ? uint8_t(FixedToFixed(Srw(v & L32(base, d + 36), base[d + 43]), base[d + 20], 8)) : 255;
}

uint32_t Emballer(const uint8_t* base, uint32_t d, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  return (Slw(FixedToFixed(r, 8, base[d + 17]), base[d + 40]) & L32(base, d + 24)) |
         (Slw(FixedToFixed(g, 8, base[d + 18]), base[d + 41]) & L32(base, d + 28)) |
         (Slw(FixedToFixed(b, 8, base[d + 19]), base[d + 42]) & L32(base, d + 32)) |
         (Slw(FixedToFixed(a, 8, base[d + 20]), base[d + 43]) & L32(base, d + 36));
}

// Bitwise::intWrite
inline void IntWrite(uint8_t* base, uint32_t a, uint32_t n, uint32_t v) {
  switch (n) {
    case 1: base[a] = uint8_t(v); break;
    case 2: S16(base, a, v); break;
    case 3:
      base[a] = uint8_t(v >> 16);
      base[a + 1] = uint8_t(v >> 8);
      base[a + 2] = uint8_t(v);
      break;
    case 4: S32(base, a, v); break;
    default: break;
  }
}

// --- png_read_filter_row ---
// Rend faux pour un cas laissé à l'original (filtre inconnu : avertissement de libpng ; ligne plus courte qu'un pixel).
bool FiltrePng(uint8_t* ligne, const uint8_t* avant, uint32_t octets, uint32_t bpp, uint32_t filtre) {
  switch (filtre) {
    case 0:
      return true;
    case 1:
      for (uint32_t i = bpp; i < octets; i++) ligne[i] = uint8_t(ligne[i] + ligne[i - bpp]);
      return true;
    case 2:
      for (uint32_t i = 0; i < octets; i++) ligne[i] = uint8_t(ligne[i] + avant[i]);
      return true;
    case 3:
      if (octets < bpp) return false;
      for (uint32_t i = 0; i < bpp; i++) ligne[i] = uint8_t(ligne[i] + (avant[i] >> 1));
      for (uint32_t i = bpp; i < octets; i++)
        ligne[i] = uint8_t(ligne[i] + ((uint32_t(avant[i]) + ligne[i - bpp]) >> 1));
      return true;
    case 4:
      if (octets < bpp) return false;
      for (uint32_t i = 0; i < bpp; i++) ligne[i] = uint8_t(ligne[i] + avant[i]);
      for (uint32_t i = bpp; i < octets; i++) {
        const int a = ligne[i - bpp], b = avant[i], c = avant[i - bpp];
        const int p = b - c, q = a - c;
        const int pa = p < 0 ? -p : p, pb = q < 0 ? -q : q, pc = (p + q) < 0 ? -(p + q) : p + q;
        const int choix = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
        ligne[i] = uint8_t(ligne[i] + choix);
      }
      return true;
    default:
      return false;
  }
}

// --- IMA ADPCM (FMOD) ---
// Blocs : en-tête de 4 octets (prédicteur 16 bits petit-boutiste, indice du pas sur un octet), puis deux échantillons par
// octet, le quartet du bas d'abord. Tables du jeu en 0x8200CB98 : 16 ajustements d'indice (mots), puis 89 pas (demi-mots).
constexpr uint32_t kAdpcmTables = 0x8200CB98;

inline int32_t AdpcmEchantillon(const uint8_t* base, uint32_t quartet, int32_t predicteur, int32_t& indice) {
  const int32_t pas = int16_t(L16(base, kAdpcmTables + 64 + uint32_t(indice) * 2));
  int32_t delta = pas >> 3;
  if (quartet & 4) delta += pas;
  if (quartet & 2) delta += pas >> 1;
  if (quartet & 1) delta += pas >> 2;
  if (quartet & 8) delta = -delta;
  int32_t p = predicteur + delta;
  if (p != int16_t(p)) p = p < -32768 ? -32768 : 32767;
  int32_t i = int32_t(L32(base, kAdpcmTables + quartet * 4)) + indice;
  indice = i < 0 ? 0 : (i >= 89 ? 88 : i);
  return p;
}

// Décode vers sortie + destination (la mémoire du jeu, ou un tampon de validation). Rend le nombre de blocs décodés :
// moins que demandé si un indice est invalide, cas que l'original reprend là où on s'est arrêté.
uint32_t Adpcm(const uint8_t* base, uint8_t* sortie, uint32_t source, uint32_t destination, uint32_t blocs, uint32_t taille,
               uint32_t echantillons, uint32_t pas_sortie) {
  for (uint32_t bloc = 0; bloc < blocs; bloc++) {
    const uint8_t* s = base + source;
    int32_t predicteur = int16_t(uint16_t(s[0] | (uint16_t(s[1]) << 8)));
    int32_t indice = s[2];
    if (indice >= 89) return bloc;
    S16(sortie, destination, uint32_t(predicteur));
    destination += pas_sortie;
    uint32_t reste = echantillons - 1;
    const uint8_t* p = s + 4;
    while (reste) {
      const uint32_t octet = *p++;
      predicteur = AdpcmEchantillon(base, octet & 0xF, predicteur, indice);
      S16(sortie, destination, uint32_t(predicteur));
      destination += pas_sortie;
      if (--reste == 0) break;
      predicteur = AdpcmEchantillon(base, octet >> 4, predicteur, indice);
      S16(sortie, destination, uint32_t(predicteur));
      destination += pas_sortie;
      --reste;
    }
    source += taille;
  }
  return blocs;
}

// --- std::wstring de la bibliothèque du jeu (Dinkumware) ---
// 24 octets : tampon de 8 caractères ou pointeur +0, taille +16, capacité +20 ; le pointeur sert dès que la capacité vaut 8.
constexpr uint32_t kCapaciteMax = 0x7FFFFFFE;

inline uint32_t WDonnees(const uint8_t* base, uint32_t s) { return L32(base, s + 20) >= 8 ? L32(base, s) : s; }

uint32_t WAllouer(PPCContext& ctx, uint8_t* base, uint32_t n) {
  ctx.r3.u64 = 0;
  ctx.r4.u64 = uint64_t(n) * 2;
  sub_821CD7F8(ctx, base);
  const uint32_t bloc = ctx.r3.u32;
  if (!bloc) {  // l'original lève l'exception
    ctx.r3.u64 = n;
    __imp__sub_82226B50(ctx, base);
    return ctx.r3.u32;
  }
  return bloc;
}

void WLiberer(PPCContext& ctx, uint8_t* base, uint32_t bloc) {
  ctx.r3.u64 = bloc;
  sub_821CD9A8(ctx, base);
}

// _Copy (sub_82482528)
void WCopy(PPCContext& ctx, uint8_t* base, uint32_t s, uint32_t taille, uint32_t garder) {
  uint32_t capacite = taille | 7;
  if (capacite > kCapaciteMax) {
    capacite = taille;
  } else {
    const uint32_t ancienne = L32(base, s + 20), moitie = ancienne >> 1;
    if (moitie > capacite / 3) {
      capacite = ancienne > kCapaciteMax - moitie ? kCapaciteMax : moitie + ancienne;
    }
  }
  const uint32_t bloc = WAllouer(ctx, base, capacite + 1);
  if (garder) std::memcpy(base + bloc, base + WDonnees(base, s), size_t(garder) * 2);
  if (L32(base, s + 20) >= 8) {
    const uint32_t ancien = L32(base, s);
    if (ancien) WLiberer(ctx, base, ancien);
  }
  S16(base, s, 0);
  S32(base, s, bloc);
  S32(base, s + 20, capacite);
  S32(base, s + 16, garder);
  S16(base, (capacite >= 8 ? bloc : s) + garder * 2, 0);
}

// _Grow(n, false) (sub_821B3350) : n <= kCapaciteMax.
bool WGrow(PPCContext& ctx, uint8_t* base, uint32_t s, uint32_t n) {
  if (L32(base, s + 20) < n) {
    WCopy(ctx, base, s, n, L32(base, s + 16));
  } else if (n == 0) {
    S32(base, s + 16, 0);
    S16(base, WDonnees(base, s), 0);
  }
  return n != 0;
}

inline void WFinir(uint8_t* base, uint32_t s, uint32_t n) {
  S32(base, s + 16, n);
  S16(base, WDonnees(base, s) + n * 2, 0);
}

// assign(ptr, n), ptr hors de la chaîne.
void WAssign(PPCContext& ctx, uint8_t* base, uint32_t s, uint32_t ptr, uint32_t n) {
  if (WGrow(ctx, base, s, n)) {
    std::memcpy(base + WDonnees(base, s), base + ptr, size_t(n) * 2);
    WFinir(base, s, n);
  }
}

inline bool WDedans(const uint8_t* base, uint32_t s, uint32_t ptr) {
  if (!ptr) return false;
  const uint32_t d = WDonnees(base, s);
  return ptr >= d && d + L32(base, s + 16) * 2 > ptr;
}

inline uint32_t WLongueur(const uint8_t* base, uint32_t ptr) {
  uint32_t n = 0;
  for (;;) {
    uint16_t c;
    std::memcpy(&c, base + ptr + size_t(n) * 2, 2);
    if (!c) return n;
    n++;
  }
}

// Validation : copie complète d'une chaîne dans le tas du jeu, sur laquelle le code natif travaille.
uint32_t OmbreDe(PPCContext& ctx, uint8_t* base, uint32_t s) {
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 24;
  sub_821CD7F8(ctx, base);
  const uint32_t o = ctx.r3.u32;
  std::memcpy(base + o, base + s, 24);
  const uint32_t capacite = L32(base, s + 20);
  if (capacite >= 8 && L32(base, s)) {
    ctx.r3.u64 = 0;
    ctx.r4.u64 = uint64_t(capacite + 1) * 2;
    sub_821CD7F8(ctx, base);
    const uint32_t bloc = ctx.r3.u32;
    std::memcpy(base + bloc, base + L32(base, s), size_t(L32(base, s + 16) + 1) * 2);
    S32(base, o, bloc);
  }
  return o;
}

bool OmbreEgale(const uint8_t* base, uint32_t a, uint32_t b) {
  if (L32(base, a + 16) != L32(base, b + 16) || L32(base, a + 20) != L32(base, b + 20)) return false;
  return std::memcmp(base + WDonnees(base, a), base + WDonnees(base, b), size_t(L32(base, a + 16) + 1) * 2) == 0;
}

void OmbreFin(PPCContext& ctx, uint8_t* base, uint32_t o) {
  if (L32(base, o + 20) >= 8 && L32(base, o)) WLiberer(ctx, base, L32(base, o));
  WLiberer(ctx, base, o);
}

// Fait une opération sur une ombre de s (natif), puis l'original sur s, et compare.
template <typename F>
void Valider(PPCContext& ctx, uint8_t* base, Compteur& c, uint32_t s, void (*original)(PPCContext&, uint8_t*), F natif) {
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  const uint32_t o = OmbreDe(ctx, base, s);
  natif(o);
  ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
  original(ctx, base);
  const uint64_t rendu = ctx.r3.u64;
  c.Noter(OmbreEgale(base, o, s));
  OmbreFin(ctx, base, o);
  ctx.r3.u64 = rendu;
}

// createKeyFrame : premier élément de temps strictement supérieur (std::upper_bound sur le temps, +4 de l'image clé).
uint32_t PositionImageCle(const uint8_t* base, uint32_t debut, uint32_t fin, float temps) {
  uint32_t pos = debut;
  int32_t n = int32_t(fin - debut) >> 2;
  while (n > 0) {
    const int32_t moitie = n / 2;
    const uint32_t milieu = pos + uint32_t(moitie) * 4;
    if (temps < LF(base, L32(base, milieu) + 4)) {
      n = moitie;
    } else {
      pos = milieu + 4;
      n -= moitie + 1;
    }
  }
  return pos;
}

}  // namespace

REX_HOOK_RAW(sub_82474560) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_82474560(ctx, base);
  const uint32_t d = kFormats + ctx.r7.u32 * 44, drapeaux = L32(base, d + 8);
  if (!(drapeaux & 0x10)) return __imp__sub_82474560(ctx, base);
  const uint32_t pr = ctx.r3.u32, pg = ctx.r4.u32, pb = ctx.r5.u32, pa = ctx.r6.u32;
  uint8_t c[4];
  Deballer(base, d, drapeaux, ctx.r8.u32, c);
  if (mode == 1) {
    base[pr] = c[0], base[pg] = c[1], base[pb] = c[2], base[pa] = c[3];
    return;
  }
  __imp__sub_82474560(ctx, base);
  g_unpack.Noter(base[pr] == c[0] && base[pg] == c[1] && base[pb] == c[2] && base[pa] == c[3]);
}

REX_HOOK_RAW(sub_82473FA0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_82473FA0(ctx, base);
  const uint32_t d = kFormats + ctx.r7.u32 * 44;
  if (!(L32(base, d + 8) & 0x10)) return __imp__sub_82473FA0(ctx, base);
  const uint32_t v = Emballer(base, d, ctx.r3.u32 & 0xFF, ctx.r4.u32 & 0xFF, ctx.r5.u32 & 0xFF, ctx.r6.u32 & 0xFF);
  const uint32_t n = base[d + 4], cible = ctx.r8.u32;
  if (mode == 1) {
    IntWrite(base, cible, n, v);
    return;
  }
  // Le tampon de comparaison remplace la cible : IntWrite écrit dans une copie de 4 octets.
  uint8_t attendu[4];
  std::memcpy(attendu, base + cible, 4);
  IntWrite(attendu - cible, cible, n, v);
  __imp__sub_82473FA0(ctx, base);
  g_pack.Noter(std::memcmp(attendu, base + cible, 4) == 0);
}

REX_HOOK_RAW(sub_82650028) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_82650028(ctx, base);
  const uint32_t infos = ctx.r4.u32, filtre = ctx.r7.u32;
  const uint32_t octets = L32(base, infos + 4), bpp = (uint32_t(base[infos + 11]) + 7) >> 3;
  uint8_t* ligne = base + ctx.r5.u32;
  const uint8_t* avant = base + ctx.r6.u32;
  if (mode == 1) {
    if (!FiltrePng(ligne, avant, octets, bpp, filtre)) __imp__sub_82650028(ctx, base);
    return;
  }
  std::vector<uint8_t> copie(ligne, ligne + octets);
  const bool fait = FiltrePng(copie.data(), avant, octets, bpp, filtre);
  __imp__sub_82650028(ctx, base);
  if (fait) g_png.Noter(std::memcmp(copie.data(), ligne, octets) == 0);
}

REX_HOOK_RAW(sub_826FF2E8) {
  const int mode = Mode();
  const uint32_t source = ctx.r3.u32, destination = ctx.r4.u32, blocs = ctx.r5.u32, taille = ctx.r6.u32;
  const uint32_t echantillons = ctx.r7.u32, pas_sortie = ctx.r8.u32 * 2;
  if (mode == 0 || echantillons == 0) return __imp__sub_826FF2E8(ctx, base);
  if (mode == 1) {
    const uint32_t faits = Adpcm(base, base, source, destination, blocs, taille, echantillons, pas_sortie);
    if (faits < blocs) {
      // Indice invalide : l'original reprend à ce bloc et rend son code d'erreur.
      ctx.r3.u64 = source + faits * taille;
      ctx.r4.u64 = destination + faits * echantillons * pas_sortie;
      ctx.r5.u64 = blocs - faits;
      return __imp__sub_826FF2E8(ctx, base);
    }
    ctx.r3.u64 = 0;
    return;
  }
  const size_t octets = size_t(blocs) * echantillons * pas_sortie;
  std::vector<uint8_t> tampon(octets + 2);
  const uint32_t faits = Adpcm(base, tampon.data() - destination, source, destination, blocs, taille, echantillons, pas_sortie);
  __imp__sub_826FF2E8(ctx, base);
  bool egal = true;
  for (size_t i = 0; i < size_t(faits) * echantillons && egal; i++) {
    const size_t o = i * pas_sortie;
    egal = std::memcmp(tampon.data() + o, base + destination + o, 2) == 0;
  }
  g_adpcm.Noter(egal);
}

REX_HOOK_RAW(sub_821AC4F8) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821AC4F8(ctx, base);
  const uint32_t n = WLongueur(base, ctx.r3.u32);
  if (mode == 1) {
    ctx.r3.u64 = n;
    return;
  }
  __imp__sub_821AC4F8(ctx, base);
  g_wcslen.Noter(ctx.r3.u32 == n);
}

REX_HOOK_RAW(sub_821AC490) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821AC490(ctx, base);
  const uint32_t s = ctx.r3.u32, ptr = ctx.r4.u32;
  const uint32_t n = WLongueur(base, ptr);
  if (n > kCapaciteMax) return __imp__sub_821AC490(ctx, base);
  if (mode == 1) {
    S32(base, s + 16, 0);
    S32(base, s + 20, 7);
    S16(base, s, 0);
    WAssign(ctx, base, s, ptr, n);
    ctx.r3.u64 = s;
    return;
  }
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 24;
  sub_821CD7F8(ctx, base);
  const uint32_t o = ctx.r3.u32;
  S32(base, o + 16, 0);
  S32(base, o + 20, 7);
  S16(base, o, 0);
  WAssign(ctx, base, o, ptr, n);
  ctx.r3.u64 = s;
  ctx.r4.u64 = ptr;
  __imp__sub_821AC490(ctx, base);
  g_ctor.Noter(OmbreEgale(base, o, s));
  OmbreFin(ctx, base, o);
  ctx.r3.u64 = s;
}

REX_HOOK_RAW(sub_821B1C28) {
  const int mode = Mode();
  const uint32_t s = ctx.r3.u32, ptr = ctx.r4.u32, n = ctx.r5.u32;
  if (mode == 0 || n > kCapaciteMax || WDedans(base, s, ptr)) return __imp__sub_821B1C28(ctx, base);
  if (mode == 1) {
    WAssign(ctx, base, s, ptr, n);
    ctx.r3.u64 = s;
    return;
  }
  Valider(ctx, base, g_assign, s, __imp__sub_821B1C28, [&](uint32_t o) { WAssign(ctx, base, o, ptr, n); });
}

REX_HOOK_RAW(sub_821EDB78) {
  const int mode = Mode();
  const uint32_t s = ctx.r3.u32, autre = ctx.r4.u32, debut = ctx.r5.u32, demande = ctx.r6.u32;
  const uint32_t taille = L32(base, autre + 16);
  if (mode == 0 || s == autre || taille < debut) return __imp__sub_821EDB78(ctx, base);
  const uint32_t n = demande < taille - debut ? demande : taille - debut;
  if (n > kCapaciteMax) return __imp__sub_821EDB78(ctx, base);
  const auto faire = [&](uint32_t cible) {
    if (WGrow(ctx, base, cible, n)) {
      std::memcpy(base + WDonnees(base, cible), base + WDonnees(base, autre) + size_t(debut) * 2, size_t(n) * 2);
      WFinir(base, cible, n);
    }
  };
  if (mode == 1) {
    faire(s);
    ctx.r3.u64 = s;
    return;
  }
  Valider(ctx, base, g_assign2, s, __imp__sub_821EDB78, faire);
}

REX_HOOK_RAW(sub_821B3350) {
  const int mode = Mode();
  const uint32_t s = ctx.r3.u32, n = ctx.r4.u32;
  if (mode == 0 || n > kCapaciteMax || (ctx.r5.u32 & 0xFF)) return __imp__sub_821B3350(ctx, base);
  if (mode == 1) {
    ctx.r3.u64 = WGrow(ctx, base, s, n) ? 1 : 0;
    return;
  }
  Valider(ctx, base, g_grow, s, __imp__sub_821B3350, [&](uint32_t o) { WGrow(ctx, base, o, n); });
}

REX_HOOK_RAW(sub_82482528) {
  const int mode = Mode();
  const uint32_t s = ctx.r3.u32, taille = ctx.r4.u32, garder = ctx.r5.u32;
  if (mode == 0) return __imp__sub_82482528(ctx, base);
  if (mode == 1) return WCopy(ctx, base, s, taille, garder);
  Valider(ctx, base, g_copy, s, __imp__sub_82482528, [&](uint32_t o) { WCopy(ctx, base, o, taille, garder); });
}

REX_HOOK_RAW(sub_821B3988) {
  if (Mode() != 1) return __imp__sub_821B3988(ctx, base);
  const uint32_t s = ctx.r3.u32;
  if (L32(base, s + 20) >= 8) {
    const uint32_t bloc = L32(base, s);
    if (bloc) WLiberer(ctx, base, bloc);
  }
  S32(base, s + 16, 0);
  S32(base, s + 20, 7);
  S16(base, s, 0);
}

REX_HOOK_RAW(sub_821A1478) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_821A1478(ctx, base);
  const uint32_t s = ctx.r3.u32, ptr = ctx.r5.u32, n = ctx.r6.u32;
  const uint32_t taille = L32(base, s + 16);
  const uint32_t n0 = ctx.r4.u32 < taille ? ctx.r4.u32 : taille;
  const uint32_t commun = n0 < n ? n0 : n;
  // Caractères gros-boutistes : l'ordre des octets est celui des valeurs.
  const int c = commun ? std::memcmp(base + WDonnees(base, s), base + ptr, size_t(commun) * 2) : 0;
  uint64_t rendu;
  if (c) {
    rendu = c > 0 ? 1 : 0xFFFFFFFFull;
  } else if (n0 < n) {
    rendu = ~uint64_t(0);
  } else {
    rendu = n0 != n ? 1 : 0;
  }
  if (mode == 1) {
    ctx.r3.u64 = rendu;
    return;
  }
  __imp__sub_821A1478(ctx, base);
  g_cmp.Noter(ctx.r3.u32 == uint32_t(rendu));
}

REX_HOOK_RAW(sub_824EE860) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_824EE860(ctx, base);
  const uint32_t piste = ctx.r3.u32;
  if (mode == 2) {
    const uint32_t debut = L32(base, piste + 4);
    const uint32_t pos = PositionImageCle(base, debut, L32(base, piste + 8), float(ctx.f1.f64));
    __imp__sub_824EE860(ctx, base);
    g_kf.Noter(L32(base, L32(base, piste + 4) + (pos - debut)) == ctx.r3.u32);
    return;
  }
  const uint32_t table = L32(base, piste);
  const uint32_t impl = L32(base, table + 60);
  if (impl == 0x824EF810u) {
    sub_824EF810(ctx, base);
  } else {
    FonctionJeu(base, impl)(ctx, base);
  }
  const uint32_t image = ctx.r3.u32;
  const uint32_t debut = L32(base, piste + 4);
  const uint32_t indice = (PositionImageCle(base, debut, L32(base, piste + 8), LF(base, image + 4)) - debut) >> 2;
  if (L32(base, piste + 8) == L32(base, piste + 12)) {
    ctx.r3.u64 = piste + 4;
    sub_82480CF0(ctx, base);
  }
  const uint32_t fin = L32(base, piste + 8);
  if (fin) S32(base, fin, image);
  const uint32_t a = L32(base, piste + 4) + indice * 4;
  S32(base, piste + 8, fin + 4);
  if (a != fin) {
    ctx.r3.u64 = a;
    ctx.r4.u64 = fin;
    ctx.r5.u64 = fin + 4;
    sub_825BE488(ctx, base);
  }
  const uint32_t change = L32(base, L32(base, piste) + 36);
  ctx.r3.u64 = piste;
  if (change == 0x824EF3B0u) {
    sub_824EF3B0(ctx, base);
  } else {
    FonctionJeu(base, change)(ctx, base);
  }
  base[L32(base, piste + 20) + 120] = 1;
  ctx.r3.u64 = image;
}
