// Suivi mémoire des boucles de particules différées des billboards (diagnostic PC, jamais versionné ;
// preparer.py <dépôt> <sortie> billboards, lancer avec --torchlight_billboards=1 et les options de suivi_poses.cpp).
// La boucle d'un BillboardRenderer est mise de côté pendant le parcours de l'octree et faite à la fin (mode 1 : en série
// sur le thread principal). Trois questions :
//   (A) une boucle lit un mot que le thread principal a modifié après sa mise de côté : résultat différent du jeu ;
//   (B) une boucle écrit un mot que le thread principal a lu ou écrit après sa mise de côté : il a vu l'ancienne valeur ;
//   (C) deux boucles du même parcours touchent le même mot, l'une en écriture : course en parallèle.
// Pile exclue. Écritures de la même valeur à part (type « même valeur »). Rapport : TORCHLIGHT_SUIVI_FICHIER.
#include <rex/ppc/context.h>

#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" thread_local unsigned t_suivi_actif = 0;

namespace {

struct Case {
  uint32_t main_ecr, main_lec;      // numéro de séquence du dernier accès du thread principal dans ce parcours
  uint32_t tache_ecr, tache_lec;    // dernière boucle (numéro global) ; 0 : aucune
  const char* f_main;
  const char* f_tache;
};

Case* g_t = nullptr;
uint8_t* g_base = nullptr;
uint32_t g_seq = 1;           // avancé à chaque mise de côté
uint32_t g_num_tache = 1;     // numéro de la boucle en cours (croissant) ; premier du parcours : g_deb_parcours
uint32_t g_deb_parcours = 1;
uint32_t g_tache = 0;         // boucle en cours, 0 : thread principal
uint32_t g_poste_tache = 0;   // séquence de la mise de côté de la boucle en cours
bool g_parcours = false;
std::unordered_map<uint32_t, uint32_t> g_postes;  // renderer -> séquence
uint32_t g_pile_bas = 0, g_pile_haut = 0;

struct Stat {
  uint64_t fois = 0;
  std::unordered_set<uint32_t> adresses;
  uint32_t exemple = 0;
};
std::map<std::tuple<int, std::string, std::string, std::string>, Stat> g_stats;
uint64_t g_parcours_total = 0, g_taches_total = 0, g_postes_total = 0, g_acces_taches = 0;
auto g_dernier = std::chrono::steady_clock::now();

std::string Zone(uint32_t a) {
  if (a >= 0x82000000u && a < 0x90000000u) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", a & ~3u);
    return b;
  }
  return "tas";
}

void Noter(int type, const char* f, const char* autre, uint32_t a) {
  Stat& s = g_stats[{type, f ? f : "?", autre ? autre : "?", Zone(a)}];
  s.fois++;
  if (!s.exemple) s.exemple = a;
  if (s.adresses.size() < 512) s.adresses.insert(a & ~3u);
}

void Ecrire() {
  const char* chemin = std::getenv("TORCHLIGHT_SUIVI_FICHIER");
  if (!chemin) chemin = "suivi_billboards.txt";
  FILE* f = std::fopen((std::string(chemin) + ".tmp").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "# parcours %llu, mises de côté %llu, boucles %llu, accès des boucles %llu\n",
               (unsigned long long)g_parcours_total, (unsigned long long)g_postes_total,
               (unsigned long long)g_taches_total, (unsigned long long)g_acces_taches);
  static const char* kTypes[] = {
      "(A) boucle lit ce que le principal a modifié après la mise de côté",
      "(B) boucle écrit ce que le principal a lu après la mise de côté",
      "(B) boucle écrit ce que le principal a écrit après la mise de côté",
      "(C) boucle lit ce qu'une autre boucle a écrit",
      "(C) boucle écrit ce qu'une autre boucle a écrit",
      "(C) boucle écrit ce qu'une autre boucle a lu",
      "boucle réécrit la même valeur sur un mot d'une autre boucle",
  };
  int type = -1;
  std::vector<std::pair<const std::tuple<int, std::string, std::string, std::string>*, const Stat*>> v;
  for (const auto& [k, s] : g_stats) v.push_back({&k, &s});
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
    if (std::get<0>(*a.first) != std::get<0>(*b.first)) return std::get<0>(*a.first) < std::get<0>(*b.first);
    return a.second->fois > b.second->fois;
  });
  for (auto& [k, s] : v) {
    if (std::get<0>(*k) != type) {
      type = std::get<0>(*k);
      std::fprintf(f, "\n== %s (fois, adresses, fonction <- autre, zone, exemple) ==\n", kTypes[type]);
    }
    std::fprintf(f, "%10llu %5zu  %s <- %s  %s  %08X\n", (unsigned long long)s->fois, s->adresses.size(),
                 std::get<1>(*k).c_str(), std::get<2>(*k).c_str(), std::get<3>(*k).c_str(), s->exemple);
  }
  std::fclose(f);
  std::rename((std::string(chemin) + ".tmp").c_str(), chemin);
}

void AccesMot(uint32_t a, bool ecriture, const char* fonction) {
  Case& c = g_t[a >> 2];
  if (!g_tache) {
    if (ecriture) {
      c.main_ecr = g_seq;
    } else {
      c.main_lec = g_seq;
    }
    c.f_main = fonction;
    return;
  }
  g_acces_taches++;
  if (!ecriture) {
    if (c.main_ecr > g_poste_tache) Noter(0, fonction, c.f_main, a);
    if (c.tache_ecr >= g_deb_parcours && c.tache_ecr != g_tache) Noter(3, fonction, c.f_tache, a);
    c.tache_lec = g_tache;
    c.f_tache = fonction;
    return;
  }
  if (c.main_lec > g_poste_tache) Noter(1, fonction, c.f_main, a);
  if (c.main_ecr > g_poste_tache) Noter(2, fonction, c.f_main, a);
  if (c.tache_ecr >= g_deb_parcours && c.tache_ecr != g_tache) Noter(4, fonction, c.f_tache, a);
  if (c.tache_lec >= g_deb_parcours && c.tache_lec != g_tache) Noter(5, fonction, c.f_tache, a);
  c.tache_ecr = g_tache;
  c.f_tache = fonction;
}

}  // namespace

// Appelés par src/torchlight_particules.cpp (symboles faibles).
extern "C" void SuiviBillboardsParcours(PPCContext& ctx, uint8_t* base, int debut) noexcept {
  if (!g_t) {
    g_t = static_cast<Case*>(mmap(nullptr, (size_t(1) << 30) * sizeof(Case), PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (g_t == MAP_FAILED) std::abort();
  }
  g_base = base;
  g_parcours = debut != 0;
  t_suivi_actif = g_parcours ? 1 : 0;
  if (debut) {
    g_pile_haut = ctx.r1.u32 + 0x10000;
    g_pile_bas = ctx.r1.u32 - 0x100000;
    g_deb_parcours = g_num_tache;
    g_seq++;  // accès du parcours précédent oubliés : séquences plus anciennes que toute mise de côté de celui-ci
    g_postes.clear();
    g_parcours_total++;
  } else {
    const auto maintenant = std::chrono::steady_clock::now();
    if (maintenant - g_dernier > std::chrono::seconds(10)) {
      g_dernier = maintenant;
      Ecrire();
    }
  }
}

extern "C" void SuiviBillboardsPoste(uint32_t renderer) noexcept {
  g_seq++;
  g_postes[renderer] = g_seq;
  g_postes_total++;
  g_seq++;  // accès qui suivent : strictement après cette mise de côté
  // Témoin (TORCHLIGHT_SUIVI_TEMOIN) : faux accès du thread principal à des mots que la boucle lira (+120 du renderer)
  // et écrira (+580 du BillboardSet), pour vérifier que le suivi voit bien les types (A) et (B).
  static const bool temoin = std::getenv("TORCHLIGHT_SUIVI_TEMOIN") != nullptr;
  if (temoin) {
    uint32_t set;
    std::memcpy(&set, g_base + renderer + 120, 4);
    set = __builtin_bswap32(set);
    AccesMot(renderer + 120, true, "temoin");
    AccesMot(set + 580, false, "temoin");
  }
}

extern "C" void SuiviBillboardsTache(uint32_t renderer, int debut) noexcept {
  if (debut) {
    auto it = g_postes.find(renderer);
    g_poste_tache = it != g_postes.end() ? it->second : 0xFFFFFFFF;
    g_tache = g_num_tache++;
    g_taches_total++;
  } else {
    g_tache = 0;
  }
}

extern "C" void SuiviAcces(unsigned adresse, unsigned taille, int ecriture, const char* fonction) noexcept {
  if (adresse >= g_pile_bas && adresse < g_pile_haut) return;
  const uint32_t debut = adresse & ~3u, fin = adresse + taille;
  for (uint32_t a = debut; a < fin; a += 4) AccesMot(a, ecriture != 0, fonction);
}

extern "C" void SuiviEcriture(unsigned adresse, unsigned taille, unsigned long long valeur,
                              const char* fonction) noexcept {
  if (adresse >= g_pile_bas && adresse < g_pile_haut) return;
  unsigned long long ancien = 0;
  for (unsigned i = 0; i < taille; i++) ancien = (ancien << 8) | g_base[adresse + i];
  const unsigned long long masque = taille >= 8 ? ~0ull : ((1ull << (8 * taille)) - 1);
  if ((ancien & masque) != (valeur & masque)) return SuiviAcces(adresse, taille, 1, fonction);
  if (!g_tache) return;
  for (uint32_t a = adresse & ~3u; a < adresse + taille; a += 4) {
    const Case& c = g_t[a >> 2];
    if (c.tache_ecr >= g_deb_parcours && c.tache_ecr != g_tache) Noter(6, fonction, c.f_tache, a);
  }
}

extern "C" void SuiviAlloc(unsigned adresse, unsigned taille) noexcept {
  if (!t_suivi_actif || !adresse) return;
  for (uint32_t a = adresse & ~3u; a < adresse + taille; a += 4) {
    Case& c = g_t[a >> 2];
    if (c.main_ecr || c.main_lec || c.tache_ecr || c.tache_lec) std::memset(&c, 0, sizeof c);
  }
}
