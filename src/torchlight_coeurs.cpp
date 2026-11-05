// torchlight - essais sur la console : un cœur réservé au thread principal du jeu, comparaisons dans la même session.
//
// Le jeu est limité par son thread principal, qui tourne déjà à ~99 % d'un cœur : le temps qu'on lui prend est
// négligeable (hors CPU ≈ attente du rendu, journal du 2026-11-03). Le gain vient d'ailleurs : plus de migration ni
// d'interruption par les autres threads du processus, donc des caches L1 et un TLB qui restent chauds.
//
// --torchlight_coeur_principal=N (0 à 2 ; -1, défaut : rien ne change) : masque exclusif sur le cœur N pour le thread
// principal, cœur N retiré du masque de tous les autres threads connus (RexSwitchReserverCoeur, switch_perf.cpp du SDK).
// Repassé à chaque fenêtre : les threads créés depuis, ou qui ont refait leur masque (rendu, particules, Mesa), sont
// repris. Éviter le cœur 0 : le thread de sortie audio (priorité 0x2B, ~1 %) y est épinglé par libnx.
//
// --torchlight_alterner="coeur,billboards,recyclage,poses,double" (une partie de la liste) : ces réglages changent en cours de
// session, un seul à chaque fenêtre (code de Gray), pour comparer des fenêtres voisines au même endroit : l'écart entre
// deux sessions cache des effets aussi petits. Billboards : mode de --torchlight_billboards contre 0. Recyclage : tampons
// du relevé recyclés (src/torchlight_natif.cpp) contre alloués à chaque copie. Coeur : réservation contre masques
// d'origine (avec --torchlight_coeur_principal). Poses et double : --torchlight_poses et --torchlight_poses_double
// (src/torchlight_poses.cpp) contre 0. Vulkan : images relevées traitées contre jetées par le thread de rendu, écran figé
// (src/torchlight_natif.cpp) : ce que le thread principal gagnerait au mieux si le rendu ne gênait plus sa mémoire.
// Geometrie : géométrie inchangée gardée en mémoire du GPU contre renvoyée à chaque image (src/natif/rendu.cpp).
//
// Dans la liste, le premier réglage change le plus souvent. --torchlight_alterner_ms : durée d'une fenêtre (7 s par défaut ;
// 2 s pour le combat, où la scène change trop en 7 s). Éviter un cycle de 10 s, en phase avec les rapports du profileur et
// le bilan du rendu natif (session du 2026-11-03).
//
// Chaque fenêtre donne une ligne « [coeurs] » : réglages de la fenêtre, masques relus auprès du noyau (masque du thread
// principal, nombre d'autres threads qui peuvent encore aller sur ce cœur), temps moyen de l'image, écart entre deux
// images (moyen, médian, 99e centile, maximum, nombre de saccades). Analyse : switch/tools/ab_coeurs.py.

#include "torchlight_coeurs.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(torchlight_coeur_principal, -1, "Torchlight",
                     "Essai : cœur réservé au thread principal du jeu (0 à 2 ; -1 = aucun) (Switch)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(torchlight_alterner_ms, 7000, "Torchlight", "Essai : durée d'une fenêtre de l'alternance, en ms")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(torchlight_alterner, "", "Torchlight",
                      "Essai : réglages qui changent à chaque fenêtre de 7 s, pour comparer "
                      "(coeur,billboards,recyclage,poses,double)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_attente_dort, true, "Torchlight",
                    "Attente du GPU par le Direct3D du jeu : vraie pause à chaque tour au lieu de tourner à vide (Switch)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_journal_stats, false, "Torchlight",
                    "Bilans périodiques dans le journal (particules, contrôleurs, rendu...) : écrits sur la carte, ils peuvent "
                    "bloquer le thread qui écrit jusqu'à ~100 ms")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_images_longues, false, "Torchlight",
                    "Essai : images longues signalées au profileur (45 ms et plus) et au journal (60 ms et plus)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(torchlight_images_longues_ms, 40, "Torchlight",
                     "Essai : ms d'image après lesquelles le chien de garde lit la pile du thread principal ; plus bas : voir le "
                     "début d'une image longue. Seules les images d'au moins 40 ms sont gardées")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

#include "torchlight_natif.h"
#include "torchlight_particules.h"
#include "torchlight_poses.h"

#ifdef __SWITCH__
#include <switch.h>

extern "C" int RexSwitchReserverCoeur(int coeur);
extern "C" int RexSwitchEtatCoeur(int coeur, u64* masque);
extern "C" void RexSwitchPerfTiron(u64 inicio, u64 fin);
extern "C" uint64_t RexChronoTicks(void);
extern "C" void RexChronoAjoute(int genre, uint64_t ticks);
extern "C" void RexChronoLire(uint64_t* appels, uint64_t* ticks);
extern "C" void RexChronoFichierCrochet(void (*fn)(const char*));

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Chien de garde (--torchlight_images_longues) : quand une image dépasse 40 ms, un thread lit la pile du thread principal
// toutes les 2 ms, jusqu'à la fin de l'image. Rien d'autre le reste du temps : les temps ne sont pas faussés comme avec le
// relevé des piles, et une image longue reçoit des dizaines d'échantillons. logs/rex/rex_longues.txt, écrit entre les
// images longues : « #i début fin dessins image attente » (images >= 45 ms), « #f tick chemin » (fichiers ouverts),
// « tick|pause_us|pc retours... » (adresses relatives à l'image, durée de la mise en pause du thread principal).
namespace torchlight::coeurs {
namespace {
constexpr uint64_t kChienSeuilNs = 40'000'000, kChienPasNs = 2'000'000, kChienFinNs = 500'000'000;
constexpr int kChienProfondeur = 32, kChienEchantillons = 2048, kChienFichiers = 1024, kChienImages = 256;
struct EchantillonChien {
  uint64_t tick, pc, pile[kChienProfondeur];
  uint32_t pause_us, n;
};
struct FichierChien {
  uint64_t tick;
  char chemin[112];
};
struct ImageChien {
  uint64_t debut, fin;
  uint32_t dessins;
  float image, attente;
};
EchantillonChien* g_chien_ech = nullptr;
FichierChien* g_chien_fic = nullptr;
ImageChien g_chien_img[kChienImages];
int g_chien_n = 0;  // thread du chien seulement
std::atomic<uint32_t> g_chien_nf{0}, g_chien_ni{0}, g_chien_nf_lu{0}, g_chien_ni_lu{0};
std::atomic<uint64_t> g_chien_fin{0};
Handle g_chien_principal = INVALID_HANDLE;
Thread g_chien_thread;
uint64_t g_chien_base = 0;

void ChienFichier(const char* chemin) {
  const uint32_t i = g_chien_nf.fetch_add(1, std::memory_order_acq_rel);
  FichierChien& f = g_chien_fic[i % kChienFichiers];
  f.tick = armGetSystemTick();
  std::strncpy(f.chemin, chemin, sizeof f.chemin - 1);
  f.chemin[sizeof f.chemin - 1] = 0;
}

void ChienEcrire(FILE* f) {
  for (int i = 0; i < g_chien_n; i++) {
    const EchantillonChien& e = g_chien_ech[i];
    std::fprintf(f, "%llu|%u|%llx", (unsigned long long)e.tick, e.pause_us, (unsigned long long)(e.pc - g_chien_base));
    for (uint32_t k = 0; k < e.n; k++) std::fprintf(f, " %llx", (unsigned long long)(e.pile[k] - g_chien_base));
    std::fputc('\n', f);
  }
  g_chien_n = 0;
  uint32_t lu = g_chien_nf_lu.load(), fin = g_chien_nf.load(std::memory_order_acquire);
  if (fin - lu > kChienFichiers) lu = fin - kChienFichiers;
  for (; lu != fin; lu++) {
    const FichierChien& x = g_chien_fic[lu % kChienFichiers];
    std::fprintf(f, "#f %llu %s\n", (unsigned long long)x.tick, x.chemin);
  }
  g_chien_nf_lu.store(lu);
  uint32_t li = g_chien_ni_lu.load(), fi = g_chien_ni.load(std::memory_order_acquire);
  if (fi - li > kChienImages) li = fi - kChienImages;
  for (; li != fi; li++) {
    const ImageChien& x = g_chien_img[li % kChienImages];
    std::fprintf(f, "#i %llu %llu %u %.1f %.1f\n", (unsigned long long)x.debut, (unsigned long long)x.fin, x.dessins, x.image,
                 x.attente);
  }
  g_chien_ni_lu.store(li);
  std::fflush(f);
}

void ChienCapture(uint64_t t) {
  const Handle h = g_chien_principal;
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) return;
  ThreadContext ctx;
  const Result rc = svcGetThreadContext3(&ctx, h);
  if (R_SUCCEEDED(rc)) {
    EchantillonChien& e = g_chien_ech[g_chien_n++];
    e.tick = t;
    e.pc = ctx.pc.x;
    e.n = 0;
    MemoryInfo mi;
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.sp)) && (mi.perm & Perm_R)) {
      const uint64_t lo = mi.addr, hi = mi.addr + mi.size;
      uint64_t fp = ctx.fp;
      while (e.n < kChienProfondeur && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
        e.pile[e.n++] = reinterpret_cast<const uint64_t*>(fp)[1];
        const uint64_t suite = reinterpret_cast<const uint64_t*>(fp)[0];
        if (suite <= fp) break;
        fp = suite;
      }
    }
    e.pause_us = uint32_t(armTicksToNs(armGetSystemTick() - t) / 1000);
  }
  svcSetThreadActivity(h, ThreadActivity_Runnable);
}

void ChienFil(void*) {
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x3);
  MemoryInfo mi{};
  u32 pi = 0;
  svcQueryMemory(&mi, &pi, reinterpret_cast<uint64_t>(&ChienFil));
  g_chien_base = mi.addr;
  FILE* f = std::fopen("sdmc:/switch/torchlight/logs/rex/rex_longues.txt", "w");
  if (!f) return;
  const uint64_t seuil = uint64_t(REXCVAR_GET(torchlight_images_longues_ms)) * 1'000'000;
  uint64_t debut_image = 0;
  for (;;) {
    svcSleepThread(kChienPasNs);
    const uint64_t fin = g_chien_fin.load(std::memory_order_acquire), t = armGetSystemTick();
    const uint64_t ecoule = fin ? armTicksToNs(t - fin) : 0;
    if (ecoule < seuil) {
      // Seuil abaissé : la plupart de ces images ne sont pas longues, leurs piles ne sont pas écrites.
      if (g_chien_n && armTicksToNs(g_chien_ech[g_chien_n - 1].tick - debut_image) < kChienSeuilNs) g_chien_n = 0;
      if (g_chien_n || g_chien_nf.load() != g_chien_nf_lu.load() || g_chien_ni.load() != g_chien_ni_lu.load()) ChienEcrire(f);
    } else if (ecoule < kChienFinNs && g_chien_n < kChienEchantillons) {
      debut_image = fin;
      ChienCapture(t);
    }
  }
}

void ChienDemarrer() {
  g_chien_ech = static_cast<EchantillonChien*>(std::malloc(sizeof(EchantillonChien) * kChienEchantillons));
  g_chien_fic = static_cast<FichierChien*>(std::malloc(sizeof(FichierChien) * kChienFichiers));
  if (!g_chien_ech || !g_chien_fic) return;
  g_chien_principal = threadGetCurHandle();
  RexChronoFichierCrochet(ChienFichier);
  if (R_SUCCEEDED(threadCreate(&g_chien_thread, ChienFil, nullptr, nullptr, 64 * 1024, 0x2C, -2))) threadStart(&g_chien_thread);
}
}  // namespace
}  // namespace torchlight::coeurs
#endif

namespace torchlight::ogre {
void PiloteActif(bool actif);  // src/torchlight_ogre_natif.cpp
}

namespace torchlight::coeurs {
namespace {

uint64_t g_fenetre_ns = 7'000'000'000;
constexpr uint32_t kCases = 1024;             // écarts entre images par tranches de 0,25 ms, jusqu'à 256 ms

bool g_lu = false, g_actif = false, g_longues = false;
int g_coeur = -1;
bool g_reserve = false;
// Réglages qui alternent, dans l'ordre de la liste : bit i du code de Gray de la fenêtre.
enum Facteur { kCoeur, kBillboards, kRecyclage, kPoses, kDouble, kRendu, kNatives, kVulkan, kGeometrie, kFacteurs };
constexpr const char* kNoms[kFacteurs] = {"coeur", "billboards", "recyclage", "poses", "double", "rendu", "natives", "vulkan",
                                          "geometrie"};
int g_rang[kFacteurs] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
int g_listes = 0;
uint32_t g_fenetre = 0;
bool g_etat[kFacteurs] = {true, true, true, true, true, true, true, true, true};

uint64_t Maintenant() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint64_t g_debut = 0, g_derniere = 0;
constexpr int kChronoGenres = 11;  // enum de guest_memory_switch.h du SDK
uint64_t g_chrono_appels[kChronoGenres], g_chrono_ticks[kChronoGenres];
uint32_t g_images = 0, g_ecarts = 0, g_sup33 = 0, g_sup50 = 0;
uint64_t g_dessins = 0;
double g_image_ms = 0, g_attente_ms = 0, g_somme_ms = 0, g_max_ms = 0;
uint32_t g_cases[kCases];

double Centile(double part) {
  const uint32_t rang = uint32_t(part * g_ecarts);
  uint32_t vus = 0;
  for (uint32_t i = 0; i < kCases; i++) {
    vus += g_cases[i];
    if (vus > rang) return (i + 1) * 0.25;
  }
  return g_max_ms;
}

void AppliquerReservation(bool reserver) {
#ifdef __SWITCH__
  if (g_coeur < 0) return;
  const int restes = RexSwitchReserverCoeur(reserver ? g_coeur : -1);
  if (restes > 0) REXLOG_WARN("[coeurs] {} threads n'ont que le cœur {} : ils y restent", restes, g_coeur);
#else
  (void)reserver;
#endif
}

void FinDeFenetre(uint64_t maintenant) {
  if (g_images) {
    unsigned long long masque = 0;
    int autres = 0;
#ifdef __SWITCH__
    if (g_coeur >= 0) {
      u64 m = 0;
      autres = RexSwitchEtatCoeur(g_coeur, &m);
      masque = m;
    }
#endif
    std::string etat;
    for (int f = 0; f < kFacteurs; f++) {
      if (g_rang[f] < 0) continue;
      if (!etat.empty()) etat += ',';
      etat += std::string(kNoms[f]) + (g_etat[f] ? ":1" : ":0");
    }
    if (g_listes || g_longues || JournalStats()) REXLOG_INFO(
        "[coeurs] etat={} masque={:#x} autres={} reserve={} images={} dessins={} image={:.2f} attente={:.2f} ecart: "
        "moyen={:.2f} p50={:.2f} p99={:.2f} max={:.2f} sup33={} sup50={}",
        etat.empty() ? "-" : etat, masque, autres, g_reserve ? g_coeur : -1, g_images, g_dessins / g_images,
        g_image_ms / g_images, g_attente_ms / g_images, g_ecarts ? g_somme_ms / g_ecarts : 0.0, Centile(0.5),
        Centile(0.99), g_max_ms, g_sup33, g_sup50);
  }
  g_images = g_ecarts = g_sup33 = g_sup50 = 0;
  g_dessins = 0;
  g_image_ms = g_attente_ms = g_somme_ms = g_max_ms = 0;
  std::fill(g_cases, g_cases + kCases, 0u);
  g_debut = maintenant;

  // Fenêtre suivante : un seul réglage change.
  if (g_listes) {
    const uint32_t i = g_fenetre++, gray = i ^ (i >> 1);
    for (int f = 0; f < kFacteurs; f++)
      if (g_rang[f] >= 0) g_etat[f] = ((gray >> g_rang[f]) & 1) != 0;
    if (g_rang[kBillboards] >= 0) torchlight::particules::BillboardsActifs(g_etat[kBillboards]);
    if (g_rang[kRecyclage] >= 0) torchlight::natif::RecyclageActif(g_etat[kRecyclage]);
    if (g_rang[kPoses] >= 0) torchlight::poses::PrecalculActif(g_etat[kPoses]);
    if (g_rang[kDouble] >= 0) torchlight::poses::DoubleActif(g_etat[kDouble]);
    if (g_rang[kRendu] >= 0) torchlight::poses::RenduActif(g_etat[kRendu]);
    if (g_rang[kNatives] >= 0) torchlight::ogre::PiloteActif(g_etat[kNatives]);
    if (g_rang[kVulkan] >= 0) torchlight::natif::VulkanActif(g_etat[kVulkan]);
    if (g_rang[kGeometrie] >= 0) torchlight::natif::GeometrieActive(g_etat[kGeometrie]);
  }
  const bool reserver = g_coeur >= 0 && g_etat[kCoeur];
  AppliquerReservation(reserver);
  g_reserve = reserver;
}

void Lire() {
  g_lu = true;
  g_coeur = REXCVAR_GET(torchlight_coeur_principal);
  if (g_coeur > 2 || g_coeur < 0) g_coeur = -1;
  const std::string liste = REXCVAR_GET(torchlight_alterner);
  g_fenetre_ns = uint64_t(std::max(500, int(REXCVAR_GET(torchlight_alterner_ms)))) * 1'000'000;
  // Rang : ordre dans la liste.
  for (size_t debut = 0; debut < liste.size();) {
    size_t fin = liste.find(',', debut);
    if (fin == std::string::npos) fin = liste.size();
    const std::string nom = liste.substr(debut, fin - debut);
    for (int f = 0; f < kFacteurs; f++) {
      if (nom == kNoms[f] && g_rang[f] < 0) g_rang[f] = g_listes++;
    }
    debut = fin + 1;
  }
  g_longues = REXCVAR_GET(torchlight_images_longues);
#ifdef __SWITCH__
  if (g_longues) ChienDemarrer();
#endif
  g_actif = g_coeur >= 0 || g_listes > 0 || g_longues;
  if (g_coeur >= 0) REXLOG_INFO("[coeurs] cœur {} réservé au thread principal", g_coeur);
  if (g_listes) REXLOG_INFO("[coeurs] réglages en alternance toutes les {} ms : {}", g_fenetre_ns / 1000000, liste);
}

}  // namespace

void Image(size_t dessins, double image_ms, double attente_ms, double releve_ms, double textures_ms) {
  if (!g_lu) Lire();
  if (!g_actif) return;

  const uint64_t maintenant = Maintenant();
#ifdef __SWITCH__
  if (g_longues) g_chien_fin.store(armGetSystemTick(), std::memory_order_release);
#endif
  // Avant la fin de fenêtre : l'image à cheval sur deux fenêtres n'y est pas comptée, et c'est souvent la plus longue.
  if (g_longues && g_derniere) {
    const double longue_ms = double(maintenant - g_derniere) / 1e6;
    if (longue_ms >= 45.0) {
#ifdef __SWITCH__
      const u64 fin = armGetSystemTick();
      RexSwitchPerfTiron(fin - armNsToTicks(maintenant - g_derniere), fin);
      if (g_chien_ech) {
        const uint32_t i = g_chien_ni.load(std::memory_order_relaxed);
        ImageChien& x = g_chien_img[i % kChienImages];
        x.debut = fin - armNsToTicks(maintenant - g_derniere);
        x.fin = fin;
        x.dessins = uint32_t(dessins);
        x.image = float(image_ms);
        x.attente = float(attente_ms);
        g_chien_ni.store(i + 1, std::memory_order_release);
      }
#endif
      if (longue_ms >= 60.0) {
        std::string detail;
#ifdef __SWITCH__
        // Ce qui a coûté depuis l'image précédente, tous threads confondus : appels et ms.
        static constexpr const char* kGenres[kChronoGenres] = {"commit", "faute", "protect", "decommit", "alloc phys",
                                                               "lib phys", "alloc virt", "lib virt", "lecture", "ouverture",
                                                               "D3DX"};
        uint64_t appels[kChronoGenres], ticks[kChronoGenres];
        RexChronoLire(appels, ticks);
        for (int i = 0; i < kChronoGenres; i++) {
          if (appels[i] != g_chrono_appels[i])
            detail += " ; " + std::string(kGenres[i]) + " " + std::to_string(appels[i] - g_chrono_appels[i]) + " : " +
                      std::to_string(armTicksToNs(ticks[i] - g_chrono_ticks[i]) / 1000000) + " ms";
        }
#endif
        REXLOG_INFO("[image longue] {:.0f} ms : image {:.1f}, attente du rendu {:.1f}, reste {:.1f}, relevé {:.1f}, textures {:.1f} ; {} dessins{}",
                    longue_ms, image_ms, attente_ms, longue_ms - image_ms - attente_ms, releve_ms, textures_ms, dessins, detail);
      }
    }
  }
#ifdef __SWITCH__
  if (g_longues) RexChronoLire(g_chrono_appels, g_chrono_ticks);
#endif
  if (g_debut == 0 || maintenant - g_debut >= g_fenetre_ns) {
    FinDeFenetre(maintenant);
    g_derniere = maintenant;  // écart à cheval sur deux fenêtres et sur le changement de réglage : pas compté
    return;
  }
  const double ecart = double(maintenant - g_derniere) / 1e6;
  g_derniere = maintenant;
  g_ecarts++;
  g_somme_ms += ecart;
  g_max_ms = std::max(g_max_ms, ecart);
  if (ecart > 33.4) g_sup33++;
  if (ecart > 50.0) g_sup50++;
  g_cases[std::min(uint32_t(ecart * 4.0), kCases - 1)]++;
  g_images++;
  g_dessins += dessins;
  g_image_ms += image_ms;
  g_attente_ms += attente_ms;
}

bool JournalStats() {
  static const bool actif = REXCVAR_GET(torchlight_journal_stats);
  return actif;
}

uint64_t ChronoD3dxDebut() {
#ifdef __SWITCH__
  return RexChronoTicks();
#else
  return 0;
#endif
}

void ChronoD3dxFin(uint64_t debut) {
#ifdef __SWITCH__
  RexChronoAjoute(10, RexChronoTicks() - debut);  // kRexChronoD3dx
#else
  (void)debut;
#endif
}

void PauseAttenteGpu() {
#ifdef __SWITCH__
  static const bool dort = REXCVAR_GET(torchlight_attente_dort);
  if (dort) svcSleepThread(100'000);  // 100 µs : retard négligeable sur 16,67 ms, le cœur se repose entre deux tours
#endif
}

}  // namespace torchlight::coeurs
