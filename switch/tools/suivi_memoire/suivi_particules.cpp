// Suivi mémoire des mises à jour des systèmes de ParticleUniverse (diagnostic PC, jamais versionné).
// Pour chaque image (numéro de Root +224), chaque mot de 4 octets touché pendant la mise à jour d'un système
// (sub_821C8618 depuis sub_821C8A58) garde son dernier écrivain et son dernier lecteur. Conflit : un système lit ou
// écrit un mot qu'un autre système a écrit dans la même image, ou écrit un mot qu'un autre a lu. Pile exclue.
// Fichier TORCHLIGHT_SUIVI_FICHIER réécrit toutes les 10 s.
#include <rex/ppc/context.h>

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
#include <algorithm>

#include "torchlight_particules.h"

extern "C" thread_local unsigned t_suivi_actif = 0;
// Sections en série de torchlight_particules.cpp (verrou commun) : deux accès en série ne se gênent pas.
thread_local bool t_serie = false;

namespace {

constexpr uint32_t kRootSingleton = 0x835067D4;

struct Mot {
  uint32_t ecrivain = 0, lecteur = 0;
  const char* f_ecr = nullptr;
  const char* f_lec = nullptr;
  bool lecteurs_multiples = false;
  bool ecr_serie = false, lec_serie = false;
};

struct Conflit {
  uint64_t fois = 0;
  std::unordered_set<uint32_t> images;
  uint32_t exemple = 0;
  std::unordered_set<uint32_t> adresses;
};

uint8_t* g_base = nullptr;
uint32_t g_image = 0xFFFFFFFF;
uint32_t g_pile_bas = 0, g_pile_haut = 0;
int g_profondeur = 0;
int g_tache_seule = -1;
std::unordered_map<uint32_t, Mot> g_mots;
// (type, fonction, autre fonction, zone) -> conflit
std::map<std::tuple<int, std::string, std::string, std::string>, Conflit> g_conflits;
std::unordered_map<const char*, uint64_t> g_fonctions;  // accès par fonction
std::unordered_set<uint32_t> g_systemes_image;
uint64_t g_images = 0, g_maj = 0, g_acces = 0, g_systemes_total = 0;
std::chrono::steady_clock::time_point g_dernier = std::chrono::steady_clock::now();

uint32_t Lire32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, g_base + a, 4);
  return __builtin_bswap32(v);
}

std::string Zone(uint32_t a) {
  if (a >= 0x82000000u && a < 0x90000000u) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", a & ~3u);
    return b;
  }
  return "tas";
}

const char* Nom(const char* f) { return f ? f : "?"; }

void Noter(int type, const char* f, const char* autre, uint32_t a, bool serie_autre = false) {
  // Fonctions marquées « * » : accès fait en série (sous le verrou commun).
  Conflit& c = g_conflits[{type, std::string(Nom(f)) + (t_serie ? "*" : ""),
                           std::string(Nom(autre)) + (serie_autre ? "*" : ""), Zone(a)}];
  c.fois++;
  c.images.insert(g_image);
  if (!c.exemple) c.exemple = a;
  if (c.adresses.size() < 4096) c.adresses.insert(a & ~3u);
}

void Ecrire() {
  const char* chemin = std::getenv("TORCHLIGHT_SUIVI_FICHIER");
  if (!chemin) chemin = "suivi_particules.txt";
  FILE* f = std::fopen((std::string(chemin) + ".tmp").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "# images %llu, mises à jour %llu (%.1f systèmes par image), accès suivis %llu\n",
               (unsigned long long)g_images, (unsigned long long)g_maj,
               g_images ? double(g_systemes_total) / double(g_images) : 0.0, (unsigned long long)g_acces);
  static const char* kTypes[] = {"lit ce qu'un autre a écrit", "écrit ce qu'un autre a écrit",
                                 "écrit ce qu'un autre a lu", "réécrit la même valeur"};
  std::vector<std::pair<const std::tuple<int, std::string, std::string, std::string>*, const Conflit*>> v;
  for (const auto& [k, c] : g_conflits) v.push_back({&k, &c});
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second->images.size() > b.second->images.size(); });
  std::fprintf(f, "\n== conflits (images, fois, adresses distinctes, type, fonction, autre fonction, zone, exemple) ==\n");
  for (auto& [k, c] : v)
    std::fprintf(f, "%6zu %9llu %5zu  %-30s %s <- %s  %s  %08X\n", c->images.size(), (unsigned long long)c->fois,
                 c->adresses.size(), kTypes[std::get<0>(*k)], std::get<1>(*k).c_str(), std::get<2>(*k).c_str(),
                 std::get<3>(*k).c_str(), c->exemple);
  std::vector<std::pair<const char*, uint64_t>> fs(g_fonctions.begin(), g_fonctions.end());
  std::sort(fs.begin(), fs.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::fprintf(f, "\n== fonctions exécutées (accès mémoire hors pile) ==\n");
  for (auto& [n, c] : fs) std::fprintf(f, "%12llu %s\n", (unsigned long long)c, n);
  std::fclose(f);
  std::rename((std::string(chemin) + ".tmp").c_str(), chemin);
}

}  // namespace

extern "C" void SuiviSerie(int entree) noexcept { t_serie = entree != 0; }

extern "C" void SuiviDebut(PPCContext& ctx, uint8_t* base, unsigned sys) noexcept {
  if (g_profondeur++ > 0) return;
  // Avec torchlight_particules (1 ou 2), seules les mises à jour faites comme tâches comptent : les autres restent à
  // leur place, sur le thread principal. Sans l'option, tout est suivi.
  if (g_tache_seule < 0) g_tache_seule = std::getenv("TORCHLIGHT_SUIVI_TACHES") ? 1 : 0;
  if (g_tache_seule && !torchlight::particules::EnTache()) return;
  g_base = base;
  const uint32_t image = Lire32(Lire32(kRootSingleton) + 224);
  if (image != g_image) {
    if (g_image != 0xFFFFFFFF) {
      g_images++;
      g_systemes_total += g_systemes_image.size();
    }
    g_image = image;
    g_mots.clear();
    g_systemes_image.clear();
    const auto maintenant = std::chrono::steady_clock::now();
    if (maintenant - g_dernier > std::chrono::seconds(10)) {
      g_dernier = maintenant;
      Ecrire();
    }
  }
  g_systemes_image.insert(sys);
  g_maj++;
  g_pile_haut = ctx.r1.u32 + 0x1000;
  g_pile_bas = ctx.r1.u32 - 0x100000;
  t_suivi_actif = sys;
}

extern "C" void SuiviFin() noexcept {
  if (--g_profondeur > 0) return;
  t_suivi_actif = 0;
}

extern "C" void SuiviAcces(unsigned adresse, unsigned taille, int ecriture, const char* fonction) noexcept {
  if (adresse >= g_pile_bas && adresse < g_pile_haut) return;
  const uint32_t sys = t_suivi_actif;
  g_acces++;
  g_fonctions[fonction]++;
  const uint32_t debut = adresse & ~3u, fin = adresse + taille;
  for (uint32_t a = debut; a < fin; a += 4) {
    Mot& m = g_mots[a];
    if (ecriture) {
      if (m.ecrivain && m.ecrivain != sys && !(t_serie && m.ecr_serie)) Noter(1, fonction, m.f_ecr, a, m.ecr_serie);
      if (m.lecteur && (m.lecteur != sys || m.lecteurs_multiples) && !(t_serie && m.lec_serie))
        Noter(2, fonction, m.f_lec, a, m.lec_serie);
      m.ecrivain = sys;
      m.f_ecr = fonction;
      m.ecr_serie = t_serie;
    } else {
      if (m.ecrivain && m.ecrivain != sys && !(t_serie && m.ecr_serie)) Noter(0, fonction, m.f_ecr, a, m.ecr_serie);
      if (m.lecteur && m.lecteur != sys) m.lecteurs_multiples = true;
      m.lecteur = sys;
      m.f_lec = fonction;
      m.lec_serie = t_serie;
    }
  }
}

extern "C" void SuiviEcriture(unsigned adresse, unsigned taille, unsigned long long valeur,
                              const char* fonction) noexcept {
  if (adresse >= g_pile_bas && adresse < g_pile_haut) return;
  unsigned long long ancien = 0;
  for (unsigned i = 0; i < taille; i++) ancien = (ancien << 8) | g_base[adresse + i];
  const unsigned long long masque = taille >= 8 ? ~0ull : ((1ull << (8 * taille)) - 1);
  if ((ancien & masque) != (valeur & masque)) return SuiviAcces(adresse, taille, 1, fonction);
  const uint32_t sys = t_suivi_actif;
  g_acces++;
  g_fonctions[fonction]++;
  for (uint32_t a = adresse & ~3u; a < adresse + taille; a += 4) {
    auto it = g_mots.find(a);
    if (it == g_mots.end()) continue;
    const Mot& m = it->second;
    if ((m.ecrivain && m.ecrivain != sys) || (m.lecteur && m.lecteur != sys))
      Noter(3, fonction, m.ecrivain && m.ecrivain != sys ? m.f_ecr : m.f_lec, a);
  }
}

extern "C" void SuiviAlloc(unsigned adresse, unsigned taille) noexcept {
  if (!t_suivi_actif || !adresse) return;
  for (uint32_t a = adresse & ~3u; a < adresse + taille; a += 4) g_mots.erase(a);
}
