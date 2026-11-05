// Suivi mémoire des poses des squelettes (diagnostic PC, jamais versionné ; preparer.py <dépôt> <sortie> poses).
// Question : Entity::cacheBoneMatrices (sub_821B93B0) peut-il quitter le thread principal, ou être différé, voire
// évité quand personne ne lit le résultat ?
//
// Thread principal entier suivi. Chaque mot de 4 octets (table directe, adresse invitée / 4) garde la dernière pose qui
// l'a écrit en changeant sa valeur et la dernière pose qui l'a lu comme entrée. Numéro d'image : Root +224.
// Le rapport (TORCHLIGHT_SUIVI_FICHIER, réécrit toutes les 10 s) donne :
//   - pour chaque pose exécutée : qui lit sa sortie, et quand (même image, image suivante) ; poses jamais lues ;
//   - les lecteurs de sorties hors des poses (fonction, étape) : là où il faudrait attendre une pose différée ;
//   - les écritures d'entrées après la pose : un calcul différé verrait une autre valeur ;
//   - les accès croisés entre poses : ce qui empêche de les calculer en parallèle ;
//   - par entité : qui est lue pendant la logique, donc ne pourrait pas être différée sans attendre ;
//   - règles pour éviter les poses inutiles : par état de l'entité au moment de la pose (rendue à l'image précédente,
//     objets attachés), ce qui serait évité et ce qui aurait été lu (erreurs de la règle) ;
//   - calcul de sub_821BB578 en parallèle au début de la boucle sub_8219D288 : ce que le reste de la boucle change ou lit
//     avant chaque appel (ordre modifié par le précalcul), et les accès croisés entre appels.
// L'allocateur du jeu (tas par thread) n'est ni une sortie ni un lecteur : ses métadonnées fausseraient tout.
// Lancer avec --torchlight_ogre_natif=0 --torchlight_crt_natif=false --torchlight_controleurs=0
// --torchlight_particules=0 --torchlight_blocs=0 : le code natif ne passe pas par le suivi.
#include <rex/ppc/context.h>

#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "torchlight_invite.h"

extern "C" thread_local unsigned t_suivi_actif = 0;

namespace {

constexpr uint32_t kRootSingleton = 0x835067D4;

// Étapes (preparer.py pose les gardes). Masques : logique = 1, 2, 3 ; rendu = 4 à 7.
constexpr const char* kEtapes[] = {"hors étape", "logique",  "unités avant poses (E6BD8)", "boucle des poses (D288)",
                                   "rendu",      "contrôleurs", "objets visibles",            "cibles de rendu"};
constexpr unsigned kLogique = (1u << 1) | (1u << 2) | (1u << 3);
constexpr unsigned kRendu = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);

struct Case {
  uint32_t ecr, lec;
  uint16_t f_ecr, f_lec;
  uint32_t reserve;
};

struct Pose {
  uint32_t entite = 0;
  uint8_t regle = 0;  // bits : 1 rendue à l'image précédente, 2 objets attachés à l'entité, 4 objets attachés à l'unité,
                      // 8 dans la boucle des unités
  uint16_t lecteur_logique = 0;  // premier lecteur de la sortie pendant la logique
  uint32_t acces = 0, ecrits = 0, identiques = 0, entrees = 0;
  uint8_t etape = 0;
  uint8_t lu = 0;     // masque d'étapes : sortie lue hors des poses, même image
  uint8_t lu2 = 0;    // image suivante, avant d'être récrite
  uint8_t lu_pose = 0;  // lue par une autre pose de la même image
};

enum Type {
  kLuMeme,
  kLuSuivante,
  kLuAncienne,
  kEntreeModifiee,
  kSortieEcrasee,
  kPoseLitAutre,
  kPoseEcritAutre,
  kPoseEcritLu,
  kPoseLitAncienne,
  kBbLitModifie,
  kBbEcritLu,
  kBbEcritEcrit,
  kBbLitAutre,
  kBbEcritAutre,
  kBbEcritLuAutre,
  kTypes
};
constexpr const char* kNomsTypes[kTypes] = {
    "lit une sortie de pose (même image)",
    "lit une sortie de pose (image suivante)",
    "lit une sortie de pose plus ancienne",
    "modifie une entrée après la pose",
    "écrase une sortie de pose",
    "pose : lit ce qu'une autre pose a écrit",
    "pose : écrit ce qu'une autre pose a écrit",
    "pose : écrit ce qu'une autre pose a lu",
    "pose : lit une sortie d'une image passée",
    "BB578 lit un mot modifié plus tôt dans la boucle par le reste du jeu (précalcul : ancienne valeur)",
    "BB578 écrit un mot lu plus tôt dans la boucle par le reste du jeu (précalcul : il verrait la nouvelle valeur)",
    "BB578 écrit un mot écrit plus tôt dans la boucle par le reste du jeu (ordre inversé)",
    "BB578 lit ce qu'un autre BB578 de la boucle a écrit (course en parallèle)",
    "BB578 écrit ce qu'un autre BB578 de la boucle a écrit (course en parallèle)",
    "BB578 écrit ce qu'un autre BB578 de la boucle a lu (course en parallèle)",
};

struct Stat {
  uint64_t fois = 0, images = 0;
  uint32_t derniere = 0, exemple = 0;
  std::unordered_set<uint32_t> adresses;
};

// Boucle des unités : accès du reste du jeu (numéro de boucle) et des appels de sub_821BB578 (numéro d'appel).
struct Case2 {
  uint32_t m_ecr, m_lec, b_ecr, b_lec;
  uint16_t f_m, f_b;
  uint32_t reserve;
};
Case2* g_t2 = nullptr;
uint32_t g_boucle = 0, g_boucle_active = 0;  // numéro de la boucle ; g_boucle_active : dedans
uint32_t g_bb = 0, g_bb_suivant = 1, g_bb_premier = 1;
int g_bb_prof = 0;
uint64_t g_bb_appels = 0, g_bb_hors_boucle = 0, g_acces_bb = 0, g_acces_boucle_hors_bb = 0, g_boucles = 0;
uint32_t g_unite = 0;
std::unordered_map<uint32_t, uint32_t> g_rendues;  // entité -> dernière image rendue (sub_821E0D18)

// Règles : par combinaison de bits de Pose::regle.
struct Regle {
  uint64_t n = 0, acces = 0, lu_logique = 0, lu_rendu = 0, jamais = 0, lu2_logique = 0;
};
Regle g_regles[16];

Case* g_t = nullptr;
uint8_t* g_base = nullptr;
uint32_t g_pile_bas = 0, g_pile_haut = 0;
int g_etape = 0;
uint32_t g_image = 0xFFFFFFFF;
uint32_t g_num_image = 0;  // images vues depuis le début

std::vector<Pose> g_cour, g_prec;
uint32_t g_id_suivant = 1, g_deb_cour = 1, g_deb_prec = 1;
uint32_t g_pose = 0;
int g_prof = 0;

std::unordered_map<uint64_t, Stat> g_stats;
std::unordered_map<const char*, uint16_t> g_fonctions;
std::vector<const char*> g_noms{"?"};
std::unordered_map<uint32_t, uint16_t> g_zones;
std::vector<uint32_t> g_zones_noms{0};

// Bilan : [étape de la pose][catégorie] ; catégorie = lu en logique | rendu << 1 | lu2 en logique << 2 | lu2 rendu << 3.
struct Bilan {
  uint64_t n = 0, acces = 0;
};
Bilan g_bilan[8][16];
uint64_t g_immediates[8], g_immediates_acces[8];
uint64_t g_images = 0, g_poses_total = 0, g_acces_total = 0, g_lu_pose = 0;
struct Entite {
  uint64_t poses = 0, immediates = 0, executees = 0, acces = 0, lu_logique = 0, lu_rendu = 0, jamais = 0, lu_suivante = 0;
  uint64_t en_logique = 0, en_rendu = 0;  // étape de la pose
  uint32_t squelette = 0;
  std::unordered_map<uint16_t, uint64_t> lecteurs;
};
std::unordered_map<uint32_t, Entite> g_entites;
std::vector<char> g_allocateur{0};  // par identifiant de fonction

struct Ligne {
  double s;
  uint64_t images, poses, executees, jamais, logique;
};
std::vector<Ligne> g_chrono;
uint64_t g_ch_images = 0, g_ch_poses = 0, g_ch_exec = 0, g_ch_jamais = 0, g_ch_logique = 0;
const auto g_debut = std::chrono::steady_clock::now();
auto g_dernier = g_debut;

uint32_t Lire32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, g_base + a, 4);
  return __builtin_bswap32(v);
}

uint16_t IdFonction(const char* f) {
  static const char* s_dernier = nullptr;
  static uint16_t s_id = 0;
  if (f == s_dernier) return s_id;
  auto [it, neuf] = g_fonctions.try_emplace(f, uint16_t(g_noms.size()));
  if (neuf) {
    g_noms.push_back(f);
    static const char* const kAllocateur[] = {"sub_8241F6F8", "sub_8241F820", "sub_8241EEC0", "sub_8241E228",
                                              "sub_821CD7F8", "sub_821CD9A8"};
    bool alloc = false;
    for (const char* a : kAllocateur) alloc = alloc || std::strstr(f, a) != nullptr;
    g_allocateur.push_back(alloc);
  }
  s_dernier = f;
  s_id = it->second;
  return s_id;
}

uint16_t IdZone(uint32_t a) {
  if (!(a >= 0x82000000u && a < 0x90000000u)) return 0;
  auto [it, neuf] = g_zones.try_emplace(a & ~3u, uint16_t(g_zones_noms.size()));
  if (neuf) g_zones_noms.push_back(a & ~3u);
  return it->second;
}

void Noter(Type type, uint16_t f, uint16_t autre, uint32_t a) {
  const uint64_t cle = uint64_t(type) << 52 | uint64_t(unsigned(g_etape) & 0xF) << 48 | uint64_t(f) << 32 |
                       uint64_t(autre) << 16 | IdZone(a);
  Stat& s = g_stats[cle];
  s.fois++;
  if (s.derniere != g_num_image) {
    s.derniere = g_num_image;
    s.images++;
  }
  if (!s.exemple) s.exemple = a;
  if (s.adresses.size() < 2048) s.adresses.insert(a & ~3u);
}

void Compter(const Pose& p) {
  g_poses_total++;
  g_ch_poses++;
  g_acces_total += p.acces;
  Entite& e = g_entites[p.entite];
  e.poses++;
  if (!p.ecrits) {
    e.immediates++;
    g_immediates[p.etape & 7]++;
    g_immediates_acces[p.etape & 7] += p.acces;
    return;
  }
  e.executees++;
  e.acces += p.acces;
  if ((1u << (p.etape & 7)) & kLogique) e.en_logique++; else e.en_rendu++;
  if (!e.squelette) e.squelette = Lire32(p.entite + 444);
  {
    Regle& rg = g_regles[p.regle & 15];
    rg.n++;
    rg.acces += p.acces;
    if (p.lu & kLogique) rg.lu_logique++;
    if (p.lu & kRendu) rg.lu_rendu++;
    if (p.lu2 & kLogique) rg.lu2_logique++;
    if (!p.lu && !p.lu2) rg.jamais++;
  }
  if (p.lu & kLogique) {
    e.lu_logique++;
    e.lecteurs[p.lecteur_logique]++;
  }
  if (p.lu & kRendu) e.lu_rendu++;
  if (p.lu2) e.lu_suivante++;
  if (!p.lu && !p.lu2) e.jamais++;
  g_ch_exec++;
  const unsigned cat = ((p.lu & kLogique) ? 1 : 0) | ((p.lu & kRendu) ? 2 : 0) | ((p.lu2 & kLogique) ? 4 : 0) |
                       ((p.lu2 & kRendu) ? 8 : 0);
  Bilan& b = g_bilan[p.etape & 7][cat];
  b.n++;
  b.acces += p.acces;
  if (p.lu_pose) g_lu_pose++;
  if (cat == 0) g_ch_jamais++;
  if (cat & 1) g_ch_logique++;
}

std::string Chaine(uint32_t a) {
  if (!torchlight::invite::Readable(a, 0x18)) return "";
  std::string r = torchlight::invite::LoadStdString(g_base, a);
  if (r.size() < 4 || r.size() > 120) return "";
  for (char c : r)
    if (c < 32 || c > 126) return "";
  return r;
}

// Noms lisibles autour d'une entité (nom, maillage, parents) : pour reconnaître qui elle est dans le rapport.
std::string Noms(uint32_t entite) {
  std::string sortie;
  auto essayer = [&](uint32_t a) {
    const std::string c = Chaine(a);
    if (!c.empty() && sortie.find(c) == std::string::npos && sortie.size() < 200) sortie += " " + c;
  };
  for (uint32_t k = 0; k < 0x60; k += 4) essayer(entite + k);
  for (uint32_t off = 0; off < 0x240; off += 4) {
    const uint32_t w = Lire32(entite + off);
    if (!torchlight::invite::Readable(w, 0x98)) continue;
    for (uint32_t k = 0; k < 0x80; k += 4) essayer(w + k);
  }
  return sortie;
}

void Ecrire() {
  const char* chemin = std::getenv("TORCHLIGHT_SUIVI_FICHIER");
  if (!chemin) chemin = "suivi_poses.txt";
  FILE* f = std::fopen((std::string(chemin) + ".tmp").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "# images %llu, poses %llu (%.1f par image), entités distinctes %zu, accès en pose %llu\n",
               (unsigned long long)g_images, (unsigned long long)g_poses_total,
               g_images ? double(g_poses_total) / double(g_images) : 0.0, g_entites.size(),
               (unsigned long long)g_acces_total);
  std::fprintf(f, "\n== chronologie (s, images, poses/image, exécutées/image, dont jamais lues, dont lues en logique) ==\n");
  for (const Ligne& l : g_chrono) {
    const double n = l.images ? double(l.images) : 1.0;
    std::fprintf(f, "%7.0f %6llu %7.1f %7.1f %7.1f %7.1f\n", l.s, (unsigned long long)l.images, l.poses / n,
                 l.executees / n, l.jamais / n, l.logique / n);
  }
  std::fprintf(f, "\n== poses par étape : sorties immédiates (rien d'écrit) ==\n");
  for (int e = 0; e < 8; e++)
    if (g_immediates[e])
      std::fprintf(f, "%-30s %10llu poses, %12llu accès\n", kEtapes[e], (unsigned long long)g_immediates[e],
                   (unsigned long long)g_immediates_acces[e]);
  std::fprintf(f, "\n== poses exécutées : qui lit la sortie (étape de la pose ; poses, part des accès) ==\n");
  uint64_t total = 0;
  for (auto& ligne : g_bilan)
    for (auto& b : ligne) total += b.acces;
  for (int e = 0; e < 8; e++)
    for (int c = 0; c < 16; c++) {
      const Bilan& b = g_bilan[e][c];
      if (!b.n) continue;
      std::string nom;
      if (!c) nom = "jamais lue";
      if (c & 1) nom += "même image, logique ; ";
      if (c & 2) nom += "même image, rendu ; ";
      if (c & 4) nom += "image suivante, logique ; ";
      if (c & 8) nom += "image suivante, rendu ; ";
      std::fprintf(f, "%-26s %10llu %6.2f %%  %s\n", kEtapes[e], (unsigned long long)b.n,
                   total ? 100.0 * double(b.acces) / double(total) : 0.0, nom.c_str());
    }
  std::fprintf(f, "poses lues par une autre pose de la même image : %llu\n", (unsigned long long)g_lu_pose);

  std::vector<std::pair<uint32_t, const Entite*>> es;
  for (const auto& [a, e] : g_entites) es.push_back({a, &e});
  std::sort(es.begin(), es.end(), [](auto& a, auto& b) { return a.second->acces > b.second->acces; });
  uint64_t acces_tous = 0, acces_logique = 0, acces_jamais_logique = 0;
  for (auto& [a, e] : es) {
    acces_tous += e->acces;
    if (e->lu_logique * 1000 > e->executees) {
      acces_logique += e->acces;
    } else {
      acces_jamais_logique += e->acces;
    }
  }
  {
    uint64_t total_regles = 0;
    for (const Regle& rg : g_regles) total_regles += rg.acces;
    std::fprintf(f, "\n== règles pour éviter une pose : rendue image précédente / objets entité / objets unité / boucle ; "
                    "poses, %% des accès, lues en logique, lues au rendu, jamais, lues en logique image suivante ==\n");
    for (int i = 0; i < 16; i++) {
      const Regle& rg = g_regles[i];
      if (!rg.n) continue;
      std::fprintf(f, "%s %s %s %s %9llu %6.2f %% %9llu %9llu %9llu %9llu\n", (i & 1) ? "rendue" : "  non ",
                   (i & 2) ? "objets" : "  -   ", (i & 4) ? "objets" : "  -   ", (i & 8) ? "boucle" : "autre ",
                   (unsigned long long)rg.n, total_regles ? 100.0 * rg.acces / total_regles : 0.0,
                   (unsigned long long)rg.lu_logique, (unsigned long long)rg.lu_rendu, (unsigned long long)rg.jamais,
                   (unsigned long long)rg.lu2_logique);
    }
    std::fprintf(f, "\n== boucle des unités : %llu boucles, %.1f appels de sub_821BB578 par boucle (hors boucle : %llu), "
                    "accès : %llu dans BB578, %llu ailleurs dans la boucle ==\n",
                 (unsigned long long)g_boucles, g_boucles ? double(g_bb_appels) / double(g_boucles) : 0.0,
                 (unsigned long long)g_bb_hors_boucle, (unsigned long long)g_acces_bb,
                 (unsigned long long)g_acces_boucle_hors_bb);
  }

  std::fprintf(f, "\n== par entité (%zu) : part des accès des entités lues en logique > 0,1 %% de leurs poses : %.1f %%, "
                  "les autres : %.1f %% ==\n",
               es.size(), acces_tous ? 100.0 * acces_logique / acces_tous : 0.0,
               acces_tous ? 100.0 * acces_jamais_logique / acces_tous : 0.0);
  std::fprintf(f, "  entité   squelette  poses  imméd.   exéc. %%accès   logique rendu  lues:logique rendu jamais suivante  lecteurs\n");
  int rang = 0;
  for (auto& [a, e] : es) {
    if (rang++ >= 80) break;
    std::fprintf(f, "%08X %08X %7llu %7llu %7llu %5.1f %7llu %7llu  %7llu %7llu %7llu %7llu ", a, e->squelette,
                 (unsigned long long)e->poses, (unsigned long long)e->immediates, (unsigned long long)e->executees,
                 acces_tous ? 100.0 * e->acces / acces_tous : 0.0, (unsigned long long)e->en_logique,
                 (unsigned long long)e->en_rendu, (unsigned long long)e->lu_logique, (unsigned long long)e->lu_rendu,
                 (unsigned long long)e->jamais, (unsigned long long)e->lu_suivante);
    std::vector<std::pair<uint64_t, uint16_t>> ls;
    for (const auto& [l, n] : e->lecteurs) ls.push_back({n, l});
    std::sort(ls.rbegin(), ls.rend());
    for (size_t i = 0; i < ls.size() && i < 4; i++)
      std::fprintf(f, " %s:%llu", g_noms[ls[i].second], (unsigned long long)ls[i].first);
    if (rang <= 24) std::fprintf(f, " |%s", Noms(a).c_str());
    std::fprintf(f, "\n");
  }

  struct Vue {
    uint64_t cle;
    const Stat* s;
  };
  std::vector<Vue> v;
  for (const auto& [k, s] : g_stats) v.push_back({k, &s});
  std::sort(v.begin(), v.end(), [](const Vue& a, const Vue& b) {
    const unsigned ta = unsigned(a.cle >> 52), tb = unsigned(b.cle >> 52);
    if (ta != tb) return ta < tb;
    return a.s->images > b.s->images;
  });
  int type = -1;
  for (const Vue& x : v) {
    const int t = int(x.cle >> 52);
    if (t != type) {
      type = t;
      std::fprintf(f, "\n== %s (images, fois, adresses, étape, fonction <- fonction de la pose, zone, exemple) ==\n",
                   kNomsTypes[t]);
    }
    const unsigned etape = unsigned(x.cle >> 48) & 0xF;
    const unsigned fn = unsigned(x.cle >> 32) & 0xFFFF, autre = unsigned(x.cle >> 16) & 0xFFFF,
                   zone = unsigned(x.cle) & 0xFFFF;
    char z[16] = "tas";
    if (zone) std::snprintf(z, sizeof z, "%08X", g_zones_noms[zone]);
    std::fprintf(f, "%7llu %10llu %5zu  %-26s %s <- %s  %s  %08X\n", (unsigned long long)x.s->images,
                 (unsigned long long)x.s->fois, x.s->adresses.size(), kEtapes[etape & 7], g_noms[fn], g_noms[autre], z,
                 x.s->exemple);
  }
  std::fclose(f);
  std::rename((std::string(chemin) + ".tmp").c_str(), chemin);
}

// Changement d'image : les poses de l'image d'avant-dernière ne peuvent plus être lues « à l'image suivante ».
void VoirImage() {
  const uint32_t image = Lire32(Lire32(kRootSingleton) + 224);
  if (image == g_image) return;
  g_image = image;
  g_num_image++;
  for (const Pose& p : g_prec) Compter(p);
  g_prec.swap(g_cour);
  g_cour.clear();
  g_deb_prec = g_deb_cour;
  g_deb_cour = g_id_suivant;
  g_images++;
  g_ch_images++;
  const auto maintenant = std::chrono::steady_clock::now();
  if (maintenant - g_dernier > std::chrono::seconds(10)) {
    g_dernier = maintenant;
    g_chrono.push_back({std::chrono::duration<double>(maintenant - g_debut).count(), g_ch_images, g_ch_poses, g_ch_exec,
                        g_ch_jamais, g_ch_logique});
    g_ch_images = g_ch_poses = g_ch_exec = g_ch_jamais = g_ch_logique = 0;
    Ecrire();
  }
}

Pose* Enregistrement(uint32_t id) {
  if (id >= g_deb_cour) return id - g_deb_cour < g_cour.size() ? &g_cour[id - g_deb_cour] : nullptr;
  if (id >= g_deb_prec) return id - g_deb_prec < g_prec.size() ? &g_prec[id - g_deb_prec] : nullptr;
  return nullptr;
}

void AccesBoucle(uint32_t a, bool ecriture, const char* fonction) {
  const uint16_t fn = IdFonction(fonction);
  if (g_allocateur[fn]) return;
  Case2& d = g_t2[a >> 2];
  if (!g_bb) {
    g_acces_boucle_hors_bb++;
    if (ecriture) {
      d.m_ecr = g_boucle;
    } else {
      d.m_lec = g_boucle;
    }
    d.f_m = fn;
    return;
  }
  g_acces_bb++;
  if (!ecriture) {
    if (d.m_ecr == g_boucle) Noter(kBbLitModifie, fn, d.f_m, a);
    if (d.b_ecr >= g_bb_premier && d.b_ecr != g_bb) Noter(kBbLitAutre, fn, d.f_b, a);
    d.b_lec = g_bb;
    d.f_b = fn;
    return;
  }
  if (d.m_lec == g_boucle) Noter(kBbEcritLu, fn, d.f_m, a);
  if (d.m_ecr == g_boucle) Noter(kBbEcritEcrit, fn, d.f_m, a);
  if (d.b_ecr >= g_bb_premier && d.b_ecr != g_bb) Noter(kBbEcritAutre, fn, d.f_b, a);
  if (d.b_lec >= g_bb_premier && d.b_lec != g_bb) Noter(kBbEcritLuAutre, fn, d.f_b, a);
  d.b_ecr = g_bb;
  d.f_b = fn;
}

void AccesMot(uint32_t a, bool ecriture, const char* fonction) {
  if (g_boucle_active) AccesBoucle(a, ecriture, fonction);
  Case& c = g_t[a >> 2];
  if (g_pose) {
    Pose& p = g_cour[g_pose - g_deb_cour];
    p.acces++;
    if (g_allocateur[IdFonction(fonction)]) return;
    if (!ecriture) {
      if (c.ecr == g_pose) return;  // sa propre écriture
      if (c.ecr >= g_deb_cour) {
        Noter(kPoseLitAutre, IdFonction(fonction), c.f_ecr, a);
        if (Pose* autre = Enregistrement(c.ecr)) autre->lu_pose = 1;
      } else if (c.ecr) {
        Noter(kPoseLitAncienne, IdFonction(fonction), c.f_ecr, a);
      }
      c.lec = g_pose;
      c.f_lec = IdFonction(fonction);
      p.entrees++;
    } else {
      if (c.ecr >= g_deb_cour && c.ecr != g_pose) Noter(kPoseEcritAutre, IdFonction(fonction), c.f_ecr, a);
      if (c.lec >= g_deb_cour && c.lec != g_pose) Noter(kPoseEcritLu, IdFonction(fonction), c.f_lec, a);
      c.ecr = g_pose;
      c.f_ecr = IdFonction(fonction);
      p.ecrits++;
    }
    return;
  }
  if (!ecriture) {
    if (!c.ecr) return;
    if (g_allocateur[IdFonction(fonction)]) return;
    if (c.ecr >= g_deb_cour) {
      if (Pose* p = Enregistrement(c.ecr)) {
        p->lu |= uint8_t(1u << (g_etape & 7));
        if (!p->lecteur_logique && ((1u << (g_etape & 7)) & kLogique)) p->lecteur_logique = IdFonction(fonction);
      }
      Noter(kLuMeme, IdFonction(fonction), c.f_ecr, a);
    } else if (c.ecr >= g_deb_prec) {
      if (Pose* p = Enregistrement(c.ecr)) p->lu2 |= uint8_t(1u << (g_etape & 7));
      Noter(kLuSuivante, IdFonction(fonction), c.f_ecr, a);
    } else {
      Noter(kLuAncienne, IdFonction(fonction), c.f_ecr, a);
    }
    return;
  }
  if (g_allocateur[IdFonction(fonction)]) {
    c.ecr = c.lec = 0;
    return;
  }
  if (c.lec >= g_deb_cour) Noter(kEntreeModifiee, IdFonction(fonction), c.f_lec, a);
  if (c.ecr) {
    if (c.ecr >= g_deb_cour) Noter(kSortieEcrasee, IdFonction(fonction), c.f_ecr, a);
    c.ecr = 0;  // plus une sortie de pose
  }
}

}  // namespace

extern "C" int SuiviEtapeEntree(PPCContext& ctx, uint8_t* base, int etape) noexcept {
  if (!t_suivi_actif) {
    if (etape != 1) return 0;  // le thread de la logique est le thread suivi
    t_suivi_actif = 1;
    g_base = base;
    g_pile_haut = ctx.r1.u32 + 0x10000;
    g_pile_bas = ctx.r1.u32 - 0x100000;
    if (!g_t) {
      g_t = static_cast<Case*>(mmap(nullptr, (size_t(1) << 30) * sizeof(Case), PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
      if (g_t == MAP_FAILED) std::abort();
      g_t2 = static_cast<Case2*>(mmap(nullptr, (size_t(1) << 30) * sizeof(Case2), PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
      if (g_t2 == MAP_FAILED) std::abort();
    }
  }
  VoirImage();
  const int avant = g_etape;
  g_etape = etape;
  if (etape == 3) {
    g_boucle++;
    g_boucles++;
    g_boucle_active = 1;
    g_bb_premier = g_bb_suivant;
  }
  return avant;
}

extern "C" void SuiviEtapeSortie(int avant) noexcept {
  if (!t_suivi_actif) return;
  if (g_etape == 3) g_boucle_active = 0;
  g_etape = avant;
}

extern "C" void SuiviBbEntree() noexcept {
  if (!t_suivi_actif || g_bb_prof++ > 0) return;
  if (g_boucle_active) {
    g_bb = g_bb_suivant++;
    g_bb_appels++;
  } else {
    g_bb_hors_boucle++;
  }
}

extern "C" void SuiviBbSortie() noexcept {
  if (!t_suivi_actif || --g_bb_prof > 0) return;
  g_bb = 0;
}

extern "C" unsigned SuiviUniteEntree(unsigned unite) noexcept {
  const unsigned avant = g_unite;
  g_unite = unite;
  return avant;
}

extern "C" void SuiviUniteSortie(unsigned avant) noexcept { g_unite = avant; }

extern "C" void SuiviRendue(unsigned entite) noexcept {
  if (t_suivi_actif) g_rendues[entite] = g_num_image;
}

extern "C" void SuiviPoseEntree(PPCContext& ctx, uint8_t*) noexcept {
  if (!t_suivi_actif || g_prof++ > 0) return;
  VoirImage();
  g_cour.emplace_back();
  Pose& p = g_cour.back();
  p.entite = ctx.r3.u32;
  p.etape = uint8_t(g_etape);
  {
    auto it = g_rendues.find(p.entite);
    uint8_t regle = 0;
    if (it != g_rendues.end() && it->second + 1 >= g_num_image) regle |= 1;
    if (Lire32(p.entite + 528)) regle |= 2;
    if (g_unite && (Lire32(g_unite + 588) || Lire32(g_unite + 592))) regle |= 4;
    if (g_etape == 3) regle |= 8;
    p.regle = regle;
  }
  g_pose = g_id_suivant++;
}

extern "C" void SuiviPoseSortie() noexcept {
  if (!t_suivi_actif || --g_prof > 0) return;
  g_pose = 0;
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
  // Même valeur : ni une sortie (lire l'ancienne valeur revient au même), ni une entrée modifiée.
  if (g_pose) {
    Pose& p = g_cour[g_pose - g_deb_cour];
    p.acces++;
    p.identiques++;
  }
}

extern "C" void SuiviAlloc(unsigned adresse, unsigned taille) noexcept {
  if (!t_suivi_actif || !adresse) return;
  for (uint32_t a = adresse & ~3u; a < adresse + taille; a += 4) {
    Case& c = g_t[a >> 2];
    if (c.ecr || c.lec) c.ecr = c.lec = 0;
    Case2& d = g_t2[a >> 2];
    if (d.m_ecr || d.m_lec || d.b_ecr || d.b_lec) d = Case2{};
  }
}
