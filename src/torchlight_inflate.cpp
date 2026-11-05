// torchlight - inflate du jeu (zlib 1.2.3 recompilée) remplacé par la zlib de l'hôte, et lectures du zip en tampon.
//
// Relevé du chien de garde (2026-11-04) : à la première rencontre d'un monstre, le jeu charge ses données sur le thread
// principal (blocage de ~280 ms), dont 17 % de décompression. Les chargements de zone en passent aussi par là. La sortie de
// DEFLATE est définie par le format : une autre implémentation donne les mêmes octets.
//
// Les fonctions du jeu servent à deux bibliothèques (archives, PNG). L'initialisation, la remise à zéro et la fin restent
// celles du jeu : son état reste valide, et un flux natif lui est associé par l'adresse du z_stream. Seul inflate passe par
// le flux natif, qui met à jour les champs publics du z_stream du jeu (+0 next_in, +4 avail_in, +8 total_in, +12 next_out,
// +16 avail_out, +20 total_out, +24 msg, +44 data_type, +48 adler). Un flux inconnu passe par l'original.
//
// Lecteur zip (zzip_file_read, sub_8258A068) : les fichiers se lisent par toutes petites lectures (un squelette : 4, 16 puis 12
// octets par image clé), et chacune appelait inflate, qui passe alors par son chemin lent, octet par octet, faute de place en
// sortie (inflate_fast demande 258 octets) : 13 % des blocages du chargement à la première rencontre d'un monstre. Ici on
// décompresse par blocs de 64 Ko dans un tampon de l'hôte, et les lectures en sont servies. Le champ du jeu restlen (+12)
// compte les octets pas encore livrés : zzip_tell et la fin de fichier restent justes. Un retour en arrière passe par
// inflateReset (sub_823E12C0), qui vide le tampon ; le poste de lecture du fichier n'est déplacé que si le tampon se remplit.
// Champs du ZZIP_FILE : +0 dossier, +8 méthode, +12 restlen, +16 crestlen, +32 tampon de 32 Ko, +36 offset, +40 z_stream, +96
// fonctions d'E/S (+8 read, +12 seek). Du ZZIP_DIR : +0 fd, +4 code d'erreur, +32 fichier courant.
//
// --torchlight_inflate_natif : vrai par défaut. --torchlight_inflate_verif : l'original écrit la sortie, le flux natif écrit
// dans un tampon à part, et les deux sont comparés : lignes « [inflate] » du journal. Une différence rend le flux au jeu. Le
// lecteur zip reste alors celui du jeu. --torchlight_zip_verif : ligne « [zip] taille crc32 » pour chaque fichier lu en
// entier par le tampon, à comparer au dossier de l'archive (switch/tools/verif_zip.py).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(torchlight_inflate_natif, true, "Torchlight",
                    "Décompression des fichiers du jeu par la zlib de la console")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_inflate_verif, false, "Torchlight",
                    "Essai : décompression faite des deux côtés et comparée")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_zip_verif, false, "Torchlight",
                    "Essai : taille et CRC32 de chaque fichier lu par le tampon du zip, dans le journal")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

REX_EXTERN(__imp__sub_823E1340);  // inflateInit2_(strm, windowBits, version, taille)
REX_EXTERN(__imp__sub_823E12C0);  // inflateReset(strm)
REX_EXTERN(__imp__sub_823E1590);  // inflate(strm, flush)
REX_EXTERN(__imp__sub_823E2E30);  // inflateEnd(strm)
REX_EXTERN(__imp__sub_8258A068);  // zzip_file_read(fichier, tampon, taille)
REX_EXTERN(sub_82589C98);         // zzip_file_saveoffset(fichier)

namespace {

uint32_t Lire(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void Ecrire(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

constexpr uint32_t kTampon = 64 * 1024;   // décompression d'avance du lecteur zip
constexpr uint32_t kDirect = 32 * 1024;   // une lecture au moins aussi grande va droit dans la destination

struct Flux {
  z_stream z{};
  bool hors_service = false;  // vérification : première différence, flux rendu au jeu
  std::unique_ptr<uint8_t[]> tampon;  // lecteur zip : octets décompressés pas encore livrés, de pos à fin
  uint32_t pos = 0, fin = 0;
  uLong crc = 0;  // --torchlight_zip_verif
  uint64_t livre = 0;
};

std::mutex g_verrou;
std::unordered_map<uint32_t, std::unique_ptr<Flux>> g_flux;
std::vector<std::unique_ptr<uint8_t[]>> g_tampons_libres;
std::atomic<uint64_t> g_generation{1};
std::atomic<uint64_t> g_appels{0}, g_octets{0}, g_comparaisons{0}, g_differences{0}, g_erreurs{0};

bool Actif() {
  static const bool actif = REXCVAR_GET(torchlight_inflate_natif);
  return actif;
}

bool Verif() {
  static const bool verif = REXCVAR_GET(torchlight_inflate_verif);
  return verif;
}

bool ZipVerif() {
  static const bool verif = REXCVAR_GET(torchlight_zip_verif);
  return verif;
}

// Dernier flux trouvé par ce thread : les petites lectures se suivent sur le même fichier.
Flux* Chercher(uint32_t strm) {
  thread_local uint32_t dernier = 0;
  thread_local Flux* flux = nullptr;
  thread_local uint64_t generation = 0;
  const uint64_t g = g_generation.load(std::memory_order_relaxed);
  if (strm == dernier && g == generation) return flux;
  std::lock_guard<std::mutex> lock(g_verrou);
  const auto it = g_flux.find(strm);
  dernier = strm;
  flux = it == g_flux.end() ? nullptr : it->second.get();
  generation = g_generation.load(std::memory_order_relaxed);
  return flux;
}

void Oublier(uint32_t strm) {
  std::unique_ptr<Flux> f;
  {
    std::lock_guard<std::mutex> lock(g_verrou);
    const auto it = g_flux.find(strm);
    if (it == g_flux.end()) return;
    f = std::move(it->second);
    g_flux.erase(it);
    g_generation.fetch_add(1, std::memory_order_relaxed);
    if (f->tampon && g_tampons_libres.size() < 4) g_tampons_libres.push_back(std::move(f->tampon));
  }
  inflateEnd(&f->z);
}

void PrendreTampon(Flux* f) {
  std::lock_guard<std::mutex> lock(g_verrou);
  if (g_tampons_libres.empty()) {
    f->tampon.reset(new uint8_t[kTampon]);
  } else {
    f->tampon = std::move(g_tampons_libres.back());
    g_tampons_libres.pop_back();
  }
}

PPCFunc* FonctionJeu(uint8_t* base, uint32_t addr) {
  constexpr uint64_t kImageEnd = 0x82000000ull + 0x15C0000ull, kCodeBase = 0x82190000ull;
  constexpr uint64_t kCodeSpan = 0xE97448ull + 0x10000ull;
  if (uint64_t(addr) - kCodeBase < kCodeSpan) {
    PPCFunc* fn;
    std::memcpy(&fn, base + kImageEnd + (uint64_t(addr) - kCodeBase) * 2, sizeof(fn));
    if (fn) return fn;
  }
  return rex::runtime::ResolveIndirectFunction(addr);
}

void Bilan() {
  const uint64_t n = g_appels.load(std::memory_order_relaxed);
  if (n % 20000 != 0) return;
  REXLOG_INFO("[inflate] {} appels, {} Mo produits ; {} comparaisons, {} différences, {} erreurs", n,
              g_octets.load(std::memory_order_relaxed) >> 20, g_comparaisons.load(std::memory_order_relaxed),
              g_differences.load(std::memory_order_relaxed), g_erreurs.load(std::memory_order_relaxed));
}

}  // namespace

REX_HOOK_RAW(sub_823E1340) {
  const uint32_t strm = ctx.r3.u32;
  const int bits = int32_t(ctx.r4.u32);
  __imp__sub_823E1340(ctx, base);
  if (!Actif() || ctx.r3.s32 != Z_OK) return;
  Oublier(strm);
  auto f = std::make_unique<Flux>();
  if (inflateInit2(&f->z, bits) != Z_OK) return;  // le jeu garde son propre état
  std::lock_guard<std::mutex> lock(g_verrou);
  g_flux[strm] = std::move(f);
  g_generation.fetch_add(1, std::memory_order_relaxed);
}

REX_HOOK_RAW(sub_823E12C0) {
  const uint32_t strm = ctx.r3.u32;
  __imp__sub_823E12C0(ctx, base);
  if (Flux* f = Actif() ? Chercher(strm) : nullptr) {
    inflateReset(&f->z);
    f->hors_service = false;
    f->pos = f->fin = 0;
    f->crc = 0;
    f->livre = 0;
  }
}

REX_HOOK_RAW(sub_823E2E30) {
  const uint32_t strm = ctx.r3.u32;
  __imp__sub_823E2E30(ctx, base);
  if (Actif()) Oublier(strm);
}

REX_HOOK_RAW(sub_823E1590) {
  const uint32_t strm = ctx.r3.u32;
  const int flush = int32_t(ctx.r4.u32);
  Flux* f = Actif() ? Chercher(strm) : nullptr;
  if (!f || f->hors_service) return __imp__sub_823E1590(ctx, base);

  const uint32_t in = Lire(base, strm + 0), n_in = Lire(base, strm + 4);
  const uint32_t out = Lire(base, strm + 12), n_out = Lire(base, strm + 16);
  z_stream& z = f->z;
  z.next_in = base + in;
  z.avail_in = n_in;

  if (Verif()) {
    thread_local std::vector<uint8_t> tampon;
    tampon.resize(n_out);
    z.next_out = tampon.data();
    z.avail_out = n_out;
    const int rc = inflate(&z, flush);
    __imp__sub_823E1590(ctx, base);
    const uint32_t produit = n_out - Lire(base, strm + 16);
    const uint32_t produit_natif = n_out - z.avail_out;
    const bool pareil = ctx.r3.s32 == rc && Lire(base, strm + 4) == z.avail_in && produit == produit_natif &&
                        std::memcmp(tampon.data(), base + out, produit) == 0;
    g_comparaisons.fetch_add(1, std::memory_order_relaxed);
    if (!pareil) {
      f->hors_service = true;
      if (g_differences.fetch_add(1, std::memory_order_relaxed) < 8)
        REXLOG_WARN("[inflate] différence : rc {} / {}, entrée restante {} / {}, produit {} / {}", ctx.r3.s32, rc,
                    Lire(base, strm + 4), z.avail_in, produit, produit_natif);
    }
    g_appels.fetch_add(1, std::memory_order_relaxed);
    g_octets.fetch_add(produit, std::memory_order_relaxed);
    Bilan();
    return;
  }

  z.next_out = base + out;
  z.avail_out = n_out;
  const uLong total_in = z.total_in, total_out = z.total_out;
  const int rc = inflate(&z, flush);
  const uint32_t lu = uint32_t(z.total_in - total_in), produit = uint32_t(z.total_out - total_out);
  Ecrire(base, strm + 0, in + lu);
  Ecrire(base, strm + 4, z.avail_in);
  Ecrire(base, strm + 8, Lire(base, strm + 8) + lu);
  Ecrire(base, strm + 12, out + produit);
  Ecrire(base, strm + 16, z.avail_out);
  Ecrire(base, strm + 20, Lire(base, strm + 20) + produit);
  Ecrire(base, strm + 44, uint32_t(z.data_type));
  Ecrire(base, strm + 48, uint32_t(z.adler));
  if (rc < 0 && rc != Z_BUF_ERROR) {
    // Les messages de la zlib native sont hors de la mémoire du jeu : pas de message (les appelants testent d'abord le code).
    Ecrire(base, strm + 24, 0);
    if (g_erreurs.fetch_add(1, std::memory_order_relaxed) < 8)
      REXLOG_WARN("[inflate] erreur {} : {}", rc, z.msg ? z.msg : "");
  }
  ctx.r3.s64 = rc;
  g_appels.fetch_add(1, std::memory_order_relaxed);
  g_octets.fetch_add(produit, std::memory_order_relaxed);
}

REX_HOOK_RAW(sub_8258A068) {
  const uint32_t fp = ctx.r3.u32;
  const uint32_t dst0 = ctx.r4.u32;
  if (!Actif() || Verif() || !fp) return __imp__sub_8258A068(ctx, base);
  const uint32_t dir = Lire(base, fp + 0);
  if (!dir || !Lire(base, fp + 8)) return __imp__sub_8258A068(ctx, base);  // sans dossier, ou fichier stocké tel quel
  Flux* f = Chercher(fp + 40);
  if (!f || f->hors_service) return __imp__sub_8258A068(ctx, base);

  uint32_t reste = Lire(base, fp + 12);
  if (!reste) {
    ctx.r3.u64 = 0;
    return;
  }
  uint32_t dst = dst0;
  uint32_t a_livrer = ctx.r5.u32 < reste ? ctx.r5.u32 : reste;
  uint32_t total = 0;

  const auto terminer = [&]() {
    Ecrire(base, fp + 12, reste);
    if (ZipVerif() && total) {
      f->crc = crc32(f->crc, base + dst0, total);
      f->livre += total;
      if (!reste) REXLOG_INFO("[zip] {} {:08x}", f->livre, uint32_t(f->crc));
    }
    ctx.r3.u64 = total;
  };
  // Erreur : ce qui est déjà livré d'abord ; l'appel suivant retombe sur la même erreur.
  const auto echec = [&](int32_t code) {
    Ecrire(base, dir + 4, uint32_t(code));
    if (total) return terminer();
    Ecrire(base, fp + 12, reste);
    ctx.r3.s64 = -1;
  };

  const uint32_t dispo = f->fin - f->pos;
  const uint32_t n0 = dispo < a_livrer ? dispo : a_livrer;
  if (n0) {
    std::memcpy(base + dst, f->tampon.get() + f->pos, n0);
    f->pos += n0;
    dst += n0;
    a_livrer -= n0;
    reste -= n0;
    total = n0;
  }
  if (!a_livrer) return terminer();

  // Le tampon est vide : au tour du fichier d'avoir le poste de lecture, comme au début de zzip_file_read.
  if (Lire(base, dir + 32) != fp) {
    ctx.r3.u64 = Lire(base, dir + 32);
    sub_82589C98(ctx, base);
    if (ctx.r3.s32 < 0) return echec(-4119);
    ctx.r3.u64 = Lire(base, dir + 0);
    ctx.r4.u64 = Lire(base, fp + 36);
    ctx.r5.u64 = 0;
    FonctionJeu(base, Lire(base, Lire(base, fp + 96) + 12))(ctx, base);
    if (ctx.r3.s32 < 0) return echec(-4119);
    Ecrire(base, dir + 32, fp);
  }

  const bool direct = a_livrer >= kDirect;
  uint32_t capacite = a_livrer;
  uint8_t* sortie = base + dst;
  if (!direct) {
    if (!f->tampon) PrendreTampon(f);
    sortie = f->tampon.get();
    capacite = reste < kTampon ? reste : kTampon;
  }
  z_stream& z = f->z;
  z.next_out = sortie;
  z.avail_out = capacite;
  uint32_t a_decoder = reste;
  for (;;) {
    uint32_t en_entree = Lire(base, fp + 44);
    const uint32_t crest = Lire(base, fp + 16);
    if (crest && !en_entree) {
      ctx.r3.u64 = Lire(base, dir + 0);
      ctx.r4.u64 = Lire(base, fp + 32);
      ctx.r5.u64 = crest < 32768 ? crest : 32768;
      FonctionJeu(base, Lire(base, Lire(base, fp + 96) + 8))(ctx, base);
      const int32_t lus = ctx.r3.s32;
      if (lus <= 0) return echec(-4120);
      en_entree = uint32_t(lus);
      Ecrire(base, fp + 44, en_entree);
      Ecrire(base, fp + 16, crest - en_entree);
      Ecrire(base, fp + 40, Lire(base, fp + 32));
    }
    const uint32_t in = Lire(base, fp + 40);
    z.next_in = base + in;
    z.avail_in = en_entree;
    const uLong avant_in = z.total_in, avant_out = z.total_out;
    const int rc = inflate(&z, Z_NO_FLUSH);
    const uint32_t lu = uint32_t(z.total_in - avant_in), produit = uint32_t(z.total_out - avant_out);
    Ecrire(base, fp + 40, in + lu);
    Ecrire(base, fp + 44, z.avail_in);
    Ecrire(base, fp + 48, Lire(base, fp + 48) + lu);
    Ecrire(base, fp + 60, Lire(base, fp + 60) + produit);
    g_appels.fetch_add(1, std::memory_order_relaxed);
    g_octets.fetch_add(produit, std::memory_order_relaxed);
    if (rc == Z_STREAM_END) {
      a_decoder = 0;
      break;
    }
    if (rc != Z_OK) {
      if (g_erreurs.fetch_add(1, std::memory_order_relaxed) < 8)
        REXLOG_WARN("[inflate] erreur {} : {}", rc, z.msg ? z.msg : "");
      return echec(rc);
    }
    a_decoder -= produit;
    if (!a_decoder || !z.avail_out) break;
  }
  const uint32_t produit = capacite - z.avail_out;
  if (direct) {
    total += produit;
    reste = a_decoder;
  } else {
    const uint32_t n1 = produit < a_livrer ? produit : a_livrer;
    std::memcpy(base + dst, sortie, n1);
    f->pos = n1;
    f->fin = produit;
    total += n1;
    reste = a_decoder + (produit - n1);
  }
  terminer();
}
