// torchlight - mises à jour des systèmes de ParticleUniverse sur d'autres cœurs.
//
// En combat, la mise à jour des systèmes actifs (ParticleSystemUpdateValue::setValue sub_821C8A58 ->
// ParticleSystem::_update sub_821C8618) fait ~10 % du thread principal sur la console, qui est à 99 % quand un cœur et
// demi reste libre. La boucle native des contrôleurs (torchlight_ogre_natif.cpp) met de côté les systèmes admis
// (Prendre), puis Executer les met à jour à la fin de la boucle sur le thread principal et des threads du jeu du SDK
// (XHostThread : pile et PCR propres), et attend qu'ils aient tous fini.
// Particules non identiques à l'octet (accepté par l'utilisateur le 2026-10-28, tant qu'elles restent visuellement
// équivalentes) : rand du CRT (sub_821BDDA8) garde sa graine par thread, chaque thread a donc sa suite de tirages.
// État partagé relevé sur PC (switch/tools/suivi_memoire, journal du 2026-10-28) et ce qui en est fait ici :
//   - _notifyCurrentCamera (sub_821C3A88, appelée par _update dès qu'une technique a des particules) : caméra lue
//     (à jour, voir plus bas : seules des réécritures de la même valeur), _notifyCurrentCamera des renderers (leurs
//     propres données) ; le tri des particules d'une technique (sub_821A1CA8 : tri à base fixe aux tampons
//     statiques 0x834A4B18..., liste globale 0x834A5F4C) en série (verrou commun). Console, 2026-10-28 : toute la
//     notification sous le verrou faisait attendre le thread principal (~1 % en combat) ;
//   - Node::requestUpdate (sub_821CCDD0) : insertion dans le std::set des enfants à mettre à jour du parent, souvent
//     partagé : différée, rejouée sur le thread principal dans l'ordre des systèmes ;
//   - test de visibilité de chaque technique contre le frustum de sa caméra (case 372 de la caméra +788,
//     sub_821C6FD8 -> sub_821C4D30) : mise à jour paresseuse du frustum (sub_8219FDC0) ; faite avant, une fois par
//     caméra, sur le thread principal (même appel), les tâches ne font plus que la lire ;
//   - réserve de particules du ParticleSystemManager (vecteur +228 : prises sub_8258F8F8, retours sub_825A4C68),
//     createController et ~Controller : en série ;
//   - BillboardSet::setPoolSize (sub_8247A588, case 0xe4 ; rare) : détruit les tampons du Direct3D du jeu (recréés au
//     rendu, sur le thread principal) : en série ;
//   - préparation d'une technique (sub_821DD4F8) : création du BillboardSet du renderer et de ses tampons du
//     Direct3D du jeu (vus par nos interceptions du rendu natif), réserves prises au gestionnaire (sub_825A5378...) :
//     le système n'est admis que si elle n'a plus rien à faire ;
//   - sans effet : statistiques du gestionnaire (+192..+208, jamais relues), allocateur du jeu (verrou récursif,
//     tas par thread), données par thread.
// Admis : système démarré (+284 = 1), techniques toutes rendues par des billboards (renderer +600) et déjà
// préparées (sauf celles que _update saute), pas de fin de durée possible pendant cette image (+436 : durée fixe, temps +384 avancé de +476 x dt
// contre +432). Les autres restent au jeu, à leur place dans la boucle.
// --torchlight_particules : 0 = comme le jeu ; 1 = mises à jour mises de côté puis faites en série sur le thread
// principal (mêmes interceptions : validation par le suivi mémoire) ; 2 = en parallèle (défaut depuis l'A/B du
// 2026-10-28 sur la console : boucle des contrôleurs en combat 6,8-8,7 -> 3,7-5,0 ms par image, effets normaux).

#include "torchlight_coeurs.h"
#include "torchlight_particules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

REXCVAR_DEFINE_INT32(torchlight_particules, 2, "Torchlight",
                     "Systèmes de ParticleUniverse mis à jour : 0 = comme le jeu ; 1 = mis de côté puis en série "
                     "(validation) ; 2 = en parallèle sur d'autres cœurs")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(torchlight_billboards, 0, "Torchlight",
                     "Sommets des billboards de ParticleUniverse : 0 = comme le jeu ; 1 = boucles différées à la fin du "
                     "parcours de l'octree, en série ; 2 = en parallèle ; 3 = validation (faites aux deux moments, "
                     "comparées à l'octet)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(torchlight_particules_threads, 2, "Torchlight",
                     "Threads de travail pour les particules (en plus du thread principal)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__sub_821C8A58);  // ParticleSystemUpdateValue::setValue
REX_EXTERN(__imp__sub_821A1CA8);  // tri des particules d'une technique par la caméra
REX_EXTERN(__imp__sub_821CCDD0);  // Node::requestUpdate
REX_EXTERN(__imp__sub_8258F8F8);  // réserve de particules du ParticleSystemManager : prise
REX_EXTERN(__imp__sub_825A4C68);  // réserve de particules du ParticleSystemManager : retour
REX_EXTERN(__imp__sub_8247A588);  // BillboardSet::setPoolSize

#if !defined(__SWITCH__)
// Suivi mémoire (switch/tools/suivi_memoire, build PC de diagnostic seulement) : sections en série.
extern "C" __attribute__((weak)) void SuiviSerie(int entree) noexcept;
// Suivi mémoire des billboards (switch/tools/suivi_memoire/suivi_billboards.cpp, build PC de diagnostic).
extern "C" __attribute__((weak)) void SuiviBillboardsParcours(PPCContext& ctx, uint8_t* base, int debut) noexcept;
extern "C" __attribute__((weak)) void SuiviBillboardsPoste(uint32_t renderer) noexcept;
extern "C" __attribute__((weak)) void SuiviBillboardsTache(uint32_t renderer, int debut) noexcept;
#endif

REX_EXTERN(sub_821E3C00);
REX_EXTERN(sub_821DD8A8);
REX_EXTERN(sub_821A7AF0);
extern "C" void torchlight_billboards_boucle(PPCContext& __restrict ctx, uint8_t* base);

namespace torchlight::particules {
namespace {

using rex::X_STATUS;

constexpr uint32_t kBillboardRendererVt = 0x82001BBC;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

float LoadFloat(const uint8_t* base, uint32_t a) {
  const uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// Fonction du jeu à une adresse (appel peu fréquent : répartiteur du SDK).
PPCFunc* GuestFunc(uint32_t addr) { return rex::runtime::ResolveIndirectFunction(addr); }

int Mode() {
  static const int mode = REXCVAR_GET(torchlight_particules);
  return mode;
}

struct Differe {
  uint32_t r3, r4, r5;
};

struct Tache {
  uint32_t ctrl, dest;
  double value;
  uint32_t cout;  // estimation : particules actives des techniques (+304) plus un coût fixe
  bool annulee;
  std::vector<Differe> differes;
};

std::vector<Tache> g_taches;
size_t g_nombre = 0;  // tâches valides de g_taches (les vecteurs des différés sont gardés d'une image à l'autre)
std::vector<uint32_t> g_ordre;  // indices des tâches, les plus coûteuses d'abord
thread_local Tache* t_tache = nullptr;
std::recursive_mutex g_serie;
thread_local int t_serie = 0;

// Les verrous du groupe et des sections en série ne sont tenus que quelques µs, mais l'attente passait par le noyau : au
// menu, 2,2 % du thread principal bloqué sur le verrou du tri des particules, pour 0,14 % de tri (relevé du 2026-11-04).
// Quelques µs d'essais avant d'y aller ; un détenteur mis en pause retombe sur l'attente normale.
constexpr int kEssaisVerrou = 400;

inline void PauseCourte() {
#if defined(__aarch64__)
  __asm__ volatile("yield");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

template <typename Verrou>
void PrendreVerrou(Verrou& v) {
  for (int i = 0; i < kEssaisVerrou; i++) {
    if (v.try_lock()) return;
    PauseCourte();
  }
  v.lock();
}

struct Bilan {
  uint64_t images = 0, prises = 0, refus_etat = 0, refus_rendu = 0, refus_prepa = 0, refus_fin = 0, differes = 0,
           annulees = 0, cameras = 0;
  double ms = 0, ms_differes = 0;
} g_bilan;
std::atomic<uint64_t> g_destructions{0};
uint64_t g_prepa[8] = {};  // diagnostic : condition de préparation qui refuse

void Rapport() {
  static auto dernier = std::chrono::steady_clock::now();
  const auto maintenant = std::chrono::steady_clock::now();
  if (maintenant - dernier < std::chrono::seconds(10) || !g_bilan.images) return;
  dernier = maintenant;
  const double n = double(g_bilan.images);
  if (torchlight::coeurs::JournalStats()) REXLOG_INFO("[particules] par image : {:.1f} systèmes en parallèle, refusés {:.1f} (état) {:.1f} (rendu) {:.1f} "
              "(préparation) {:.1f} (fin) ; {:.1f} caméras ; ms : {:.3f} mises à jour, {:.3f} différés rejoués "
              "({:.1f}) ; annulées {}, destructions pendant les tâches {}",
              g_bilan.prises / n, g_bilan.refus_etat / n, g_bilan.refus_rendu / n, g_bilan.refus_prepa / n,
              g_bilan.refus_fin / n, g_bilan.cameras / n, g_bilan.ms / n, g_bilan.ms_differes / n,
              g_bilan.differes / n, g_bilan.annulees, g_destructions.load(std::memory_order_relaxed));
  if (torchlight::coeurs::JournalStats()) REXLOG_INFO("[particules] refus de préparation (+680 +681 +682 renderer +66 +189 +190 +191 +192) : {} {} {} {} {} {} {} {}",
              g_prepa[0], g_prepa[1], g_prepa[2], g_prepa[3], g_prepa[4], g_prepa[5], g_prepa[6], g_prepa[7]);
  std::memset(g_prepa, 0, sizeof g_prepa);
  g_bilan = Bilan{};
}

// Groupe de threads du jeu : le thread principal distribue les tâches, en fait aussi, et revient quand toutes sont
// faites. Rien ne tourne sur les threads de travail en dehors d'un tel appel.
class Groupe {
 public:
  void Demarrer(int travailleurs) {
    auto* ks = rex::system::kernel_state();
    for (int i = 0; i < travailleurs; i++) {
      auto* t = new rex::system::XHostThread(ks, 256 * 1024, 0, [this]() {
        Travailler();
        return 0;
      });
      t->set_name("Particules");
      if (t->Create() == X_STATUS_SUCCESS) {
        threads_.emplace_back(t);
      } else {
        REXLOG_ERROR("[particules] thread de travail {} : création impossible", i);
      }
    }
    REXLOG_INFO("[particules] {} threads de travail", threads_.size());
  }

  // Fait les tâches 0 à nombre - 1 : faire(ctx, base, i), sur le thread appelant et les threads de travail.
  using Faire = void (*)(PPCContext&, uint8_t*, size_t);
  void Lancer(PPCContext& ctx, uint8_t* base, size_t nombre, Faire faire) {
    PrendreVerrou(lock_);
    fpscr_ = ctx.fpscr;
    nombre_ = nombre;
    faire_ = faire;
    suivant_.store(0, std::memory_order_relaxed);
    ouvert_ = true;
    tour_++;
    lock_.unlock();
    reveil_.notify_all();
    Prendre(ctx, base);
    PrendreVerrou(lock_);
    ouvert_ = false;
    // Une aide qui finit sa dernière tâche : quelques µs d'attente active plutôt qu'un aller-retour par le noyau.
    for (int i = 0; i < kEssaisVerrou && occupes_ != 0; i++) {
      lock_.unlock();
      PauseCourte();
      PrendreVerrou(lock_);
    }
    if (occupes_ != 0) {
      std::unique_lock<std::mutex> verrou(lock_, std::adopt_lock);
      fini_.wait(verrou, [this] { return occupes_ == 0; });
      verrou.release();
    }
    lock_.unlock();
  }

 private:
  void Prendre(PPCContext& ctx, uint8_t* base) {
    for (size_t i = suivant_.fetch_add(1, std::memory_order_relaxed); i < nombre_;
         i = suivant_.fetch_add(1, std::memory_order_relaxed)) {
      faire_(ctx, base, i);
    }
  }

  void Travailler() {
#ifdef __SWITCH__
    // Même priorité que le thread du jeu (0x3B), trois cœurs permis (le SDK donne aux threads hôtes celle de la
    // présentation).
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7);
#endif
    PPCContext& ctx = *rex::runtime::ThreadState::Get()->context();
    uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
    // r1 est au sommet de la pile : un cadre pour l'appelé, qui écrit dans celui de son appelant (setValue : r1+156
    // après stwu -128), sinon dans la page de garde.
    const uint32_t sommet = ctx.r1.u32;
    ctx.r1.u64 = sommet - 512;
    std::memset(base + ctx.r1.u32, 0, 512);
    uint64_t vu = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> verrou(lock_);
        reveil_.wait(verrou, [&] { return tour_ != vu; });
        vu = tour_;
        if (!ouvert_) continue;  // tour déjà fini
        occupes_++;
        ctx.fpscr = fpscr_;
      }
      Prendre(ctx, base);
      PrendreVerrou(lock_);
      const bool dernier = --occupes_ == 0;
      lock_.unlock();
      if (dernier) fini_.notify_one();
    }
  }

  std::vector<rex::system::object_ref<rex::system::XHostThread>> threads_;
  std::mutex lock_;
  std::condition_variable reveil_, fini_;
  decltype(PPCContext::fpscr) fpscr_{};
  std::atomic<size_t> suivant_{0};
  size_t nombre_ = 0;
  Faire faire_ = nullptr;
  uint64_t tour_ = 0;
  bool ouvert_ = false;
  int occupes_ = 0;
};

Groupe* g_groupe = nullptr;

Groupe& LeGroupe() {
  if (!g_groupe) {
    g_groupe = new Groupe;
    g_groupe->Demarrer(REXCVAR_GET(torchlight_particules_threads));
  }
  return *g_groupe;
}

void FaireMiseAJour(PPCContext& ctx, uint8_t* base, size_t i) {
  Tache& t = g_taches[g_ordre[i]];
  if (t.annulee) return;
  t_tache = &t;
  ctx.r3.u64 = t.dest;
  ctx.f1.f64 = t.value;
  __imp__sub_821C8A58(ctx, base);
  t_tache = nullptr;
}

}  // namespace

bool EnTache() { return t_tache != nullptr; }

void EnParallele(PPCContext& ctx, uint8_t* base, size_t nombre, void (*faire)(PPCContext&, uint8_t*, size_t)) {
  LeGroupe().Lancer(ctx, base, nombre, faire);
}

Serie::Serie() : pris_(t_tache != nullptr) {
  if (!pris_) return;
  PrendreVerrou(g_serie);
#if !defined(__SWITCH__)
  if (t_serie++ == 0 && SuiviSerie) SuiviSerie(1);
#else
  t_serie++;
#endif
}

Serie::~Serie() {
  if (!pris_) return;
#if !defined(__SWITCH__)
  if (--t_serie == 0 && SuiviSerie) SuiviSerie(0);
#else
  t_serie--;
#endif
  g_serie.unlock();
}

bool Prendre(uint8_t* base, uint32_t ctrl, uint32_t dest, double value) {
  if (Mode() == 0) return false;
  const uint32_t sys = Load32(base, dest + 4);
  if (Load32(base, sys + 284) != 1) {
    g_bilan.refus_etat++;
    return false;
  }
  for (uint32_t t = Load32(base, sys + 348), fin = Load32(base, sys + 352); t != fin; t += 4) {
    const uint32_t tech = Load32(base, t), renderer = Load32(base, tech + 600);
    if (!renderer || Load32(base, renderer) != kBillboardRendererVt) {
      g_bilan.refus_rendu++;
      return false;
    }
    // Préparation de la technique (sub_821DD4F8) sans rien à faire : +680/+681/+682 baissés, renderer préparé
    // (+66), réserves prises (+189..+192). Technique désactivée (+92 nul : sub_821C7B88 sort tout de suite) ou
    // d'octet +136 non nul (sautée par la boucle de sub_821C81F8) : jamais préparée par _update, pas regardée (son
    // renderer l'est quand même, plus haut : _notifyCurrentCamera les parcourt toutes). Console, 2026-10-28 : jusqu'à
    // ~45 systèmes par image refusés à cause des techniques désactivées.
    if (!base[tech + 92] || base[tech + 136]) continue;
    const bool prepa[8] = {base[tech + 680] != 0, base[tech + 681] != 0, base[tech + 682] != 0, !base[renderer + 66],
                           !base[tech + 189],      !base[tech + 190],      !base[tech + 191],      !base[tech + 192]};
    bool refus = false;
    for (int k = 0; k < 8; k++) {
      if (prepa[k]) {
        g_prepa[k]++;
        refus = true;
      }
    }
    if (refus) {
      g_bilan.refus_prepa++;
      return false;
    }
  }
  // Durée fixe : _update arrête le système (sub_82593348) quand +384 (avancé de +476 x dt) atteint +432 ; marge large
  // (setValue peut faire plusieurs pas).
  if (base[sys + 436] &&
      double(LoadFloat(base, sys + 384)) + double(LoadFloat(base, sys + 476)) * (2.0 * value + 0.1) >=
          double(LoadFloat(base, sys + 432))) {
    g_bilan.refus_fin++;
    return false;
  }
  if (g_nombre == g_taches.size()) g_taches.emplace_back();
  Tache& t = g_taches[g_nombre++];
  t.cout = 16;
  for (uint32_t k = Load32(base, sys + 348), fin = Load32(base, sys + 352); k != fin; k += 4)
    t.cout += Load32(base, Load32(base, k) + 304);
  t.ctrl = ctrl;
  t.dest = dest;
  t.value = value;
  t.annulee = false;
  t.differes.clear();
  g_bilan.prises++;
  return true;
}

void ControleurDetruit(uint32_t ctrl) {
  if (t_tache) {
    g_destructions.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (size_t i = 0; i < g_nombre; i++) {
    if (g_taches[i].ctrl == ctrl && !g_taches[i].annulee) {
      g_taches[i].annulee = true;
      g_bilan.annulees++;
    }
  }
}

// Frustum de chaque caméra des systèmes mis de côté (+484, celle que _notifyCurrentCamera donne aux techniques en
// +788) mis à jour avant les tâches, par le test de visibilité de la caméra (case 372) sur une sphère nulle : celui
// des techniques (sub_821C7B88) ne fait ensuite plus que lire.
void Cameras(PPCContext& ctx, uint8_t* base) {
  uint32_t vues[8];
  size_t nombre = 0;
  const uint32_t sp = ctx.r1.u32 - 64;
  std::memset(base + sp, 0, 16);  // Sphere : rayon, centre
  for (size_t i = 0; i < g_nombre; i++) {
    const uint32_t cam = Load32(base, Load32(base, g_taches[i].dest + 4) + 484);
    if (!cam || std::find(vues, vues + nombre, cam) != vues + nombre) continue;
    if (nombre < 8) vues[nombre++] = cam;
    ctx.r3.u64 = cam;
    ctx.r4.u64 = sp;
    ctx.r5.u64 = 0;
    const uint32_t old_sp = ctx.r1.u32;
    ctx.r1.u64 = sp - 128;
    GuestFunc(Load32(base, Load32(base, cam) + 372))(ctx, base);
    ctx.r1.u64 = old_sp;
    g_bilan.cameras++;
  }
}

void Executer(PPCContext& ctx, uint8_t* base) {
  if (Mode() == 0) return;
  g_bilan.images++;
  if (g_nombre) {
    Cameras(ctx, base);
    const auto t0 = std::chrono::steady_clock::now();
    g_ordre.resize(g_nombre);
    for (size_t i = 0; i < g_nombre; i++) g_ordre[i] = uint32_t(i);
    if (Mode() == 2) {
      // Les plus coûteuses d'abord : moins d'attente de la dernière tâche à la fin (profil de la console du
      // 2026-10-28 : 4,8 % du thread principal dans Executer en combat, sa part comprise). Sur des indices, à égalité par
      // rang : le même ordre qu'un tri stable des tâches, sans déplacer leurs vecteurs ni allouer de tampon à chaque image.
      std::sort(g_ordre.begin(), g_ordre.end(), [](uint32_t a, uint32_t b) {
        return g_taches[a].cout != g_taches[b].cout ? g_taches[a].cout > g_taches[b].cout : a < b;
      });
      LeGroupe().Lancer(ctx, base, g_nombre, FaireMiseAJour);
    } else {
      for (size_t i = 0; i < g_nombre; i++) FaireMiseAJour(ctx, base, i);
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_nombre; i++) {
      for (const Differe& d : g_taches[g_ordre[i]].differes) {
        ctx.r3.u64 = d.r3;
        ctx.r4.u64 = d.r4;
        ctx.r5.u64 = d.r5;
        __imp__sub_821CCDD0(ctx, base);
        g_bilan.differes++;
      }
    }
    g_bilan.ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_bilan.ms_differes += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    g_nombre = 0;
  }
  Rapport();
}

// --- Sommets des billboards (BillboardRenderer::_updateRenderQueue sub_821DA090, 4,1 % du thread principal en combat
// sur la console, dont ~2,3 % pour la boucle sur les particules) ---
//
// Pendant le parcours de l'octree, le thread principal fait tout sauf la boucle : préparation, beginBillboards
// (verrouillage), setVisible, mise en file (donc même ordre de la file que le jeu), statistique. Les boucles
// (switch/tools/billboards_differes.py : texte généré recopié) sont faites à la fin du parcours, avant tout dessin,
// puis les tampons sont déverrouillés dans l'ordre. Rien d'autre ne tourne pendant les boucles : le thread principal y
// participe et attend la dernière.
// Une boucle ne touche qu'à sa réserve de particules et à son BillboardSet (sommets, compteur +580, pointeur +392,
// axes par billboard) ; caméra de ce parcours lue (cullIndividually) ; validation : mode 3 et switch/tools/suivi_memoire.
namespace {

struct Billboards {
  uint32_t renderer, pool, set, cout;
  uint32_t verrou = 0;             // mode 3 : pointeur de verrouillage et compteur avant la boucle
  uint16_t compte = 0;
  std::vector<uint8_t> resultat;  // mode 3 : sommets écrits par la boucle faite à sa place d'origine
};

std::vector<Billboards> g_bb;
size_t g_bb_nombre = 0;
std::vector<uint32_t> g_bb_ordre;
bool g_bb_parcours = false;
struct BilanBB {
  uint64_t parcours = 0, differes = 0, refus = 0, particules = 0, comparaisons = 0, differences = 0;
  double ms = 0;
} g_bb_bilan;

bool g_bb_actifs = true;

int ModeBB() {
  static const int mode = REXCVAR_GET(torchlight_billboards);
  return g_bb_actifs ? mode : 0;
}

void Appeler(PPCContext& ctx, uint8_t* base, uint32_t fonction) {
  // Fonctions attendues appelées directement, comme les appels promus du code généré.
  switch (fonction) {
    case 0x821E3C00: return sub_821E3C00(ctx, base);
    case 0x821DD8A8: return sub_821DD8A8(ctx, base);
    case 0x821A7AF0: return sub_821A7AF0(ctx, base);
    default: return GuestFunc(fonction)(ctx, base);
  }
}

void FaireBoucle(PPCContext& ctx, uint8_t* base, size_t i) {
  const Billboards& b = g_bb[g_bb_ordre[i]];
  ctx.r3.u64 = b.renderer;
  ctx.r4.u64 = b.pool;
#if !defined(__SWITCH__)
  if (SuiviBillboardsTache) SuiviBillboardsTache(b.renderer, 1);
#endif
  torchlight_billboards_boucle(ctx, base);
#if !defined(__SWITCH__)
  if (SuiviBillboardsTache) SuiviBillboardsTache(b.renderer, 0);
#endif
}

void RapportBB() {
  static auto dernier = std::chrono::steady_clock::now();
  const auto maintenant = std::chrono::steady_clock::now();
  if (maintenant - dernier < std::chrono::seconds(10) || !g_bb_bilan.parcours) return;
  dernier = maintenant;
  const double n = double(g_bb_bilan.parcours);
  if (torchlight::coeurs::JournalStats()) REXLOG_INFO("[billboards] par parcours : {:.1f} boucles différées ({:.0f} particules), {:.1f} refusées, {:.3f} ms ; "
              "validation : {} comparées, {} différentes",
              g_bb_bilan.differes / n, g_bb_bilan.particules / n, g_bb_bilan.refus / n, g_bb_bilan.ms / n,
              g_bb_bilan.comparaisons, g_bb_bilan.differences);
  g_bb_bilan = BilanBB{};
}

}  // namespace

void BillboardsActifs(bool actifs) { g_bb_actifs = actifs; }

void BillboardsDebut(PPCContext& ctx, uint8_t* base) {
  g_bb_parcours = ModeBB() != 0;
#if !defined(__SWITCH__)
  if (g_bb_parcours && SuiviBillboardsParcours) SuiviBillboardsParcours(ctx, base, 1);
#else
  (void)ctx;
  (void)base;
#endif
}

void BillboardsFin(PPCContext& ctx, uint8_t* base) {
  g_bb_parcours = false;
  if (!ModeBB()) return;
#if !defined(__SWITCH__)
  struct FinSuivi {
    PPCContext& ctx;
    uint8_t* base;
    ~FinSuivi() {
      if (SuiviBillboardsParcours) SuiviBillboardsParcours(ctx, base, 0);
    }
  } fin_suivi{ctx, base};
#endif
  g_bb_bilan.parcours++;
  if (g_bb_nombre) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t ancien = ctx.r1.u32, sp = ancien - 256;
    std::memset(base + sp, 0, 16);
    ctx.r1.u64 = sp;
    g_bb_ordre.resize(g_bb_nombre);
    for (size_t i = 0; i < g_bb_nombre; i++) g_bb_ordre[i] = uint32_t(i);
    if (ModeBB() == 2) {
      // Les plus longues d'abord : moins d'attente de la dernière.
      std::stable_sort(g_bb_ordre.begin(), g_bb_ordre.end(),
                       [](uint32_t a, uint32_t b) { return g_bb[a].cout > g_bb[b].cout; });
      LeGroupe().Lancer(ctx, base, g_bb_nombre, FaireBoucle);
    } else {
      for (size_t i = 0; i < g_bb_nombre; i++) {
        FaireBoucle(ctx, base, i);
        if (ModeBB() != 3) continue;
        const Billboards& b = g_bb[i];
        const uint32_t fin = Load32(base, b.set + 392);
        g_bb_bilan.comparaisons++;
        if (fin - b.verrou != b.resultat.size() ||
            std::memcmp(base + b.verrou, b.resultat.data(), b.resultat.size()) != 0) {
          if (g_bb_bilan.differences++ < 8)
            REXLOG_ERROR("[billboards] différence : renderer {:08X}, {} octets contre {}", b.renderer, fin - b.verrou,
                         b.resultat.size());
        }
      }
    }
    // Déverrouillage (case 20 du tampon +380), comme la suite de la boucle, dans l'ordre d'origine.
    for (size_t i = 0; i < g_bb_nombre; i++) {
      const uint32_t tampon = Load32(base, g_bb[i].set + 380);
      ctx.r3.u64 = tampon;
      Appeler(ctx, base, Load32(base, Load32(base, tampon) + 20));
    }
    ctx.r1.u64 = ancien;
    g_bb_nombre = 0;
    g_bb_bilan.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  RapportBB();
}

}  // namespace torchlight::particules

// Appelée par sub_821DA090 juste après beginBillboards (switch/tools/billboards_differes.py). Vrai : boucle mise de
// côté et suite de la fonction faite ici, sauf le déverrouillage ; le jeu saute à sa sortie.
extern "C" bool torchlight_billboards_differer(PPCContext& ctx, uint8_t* base, uint32_t renderer, uint32_t queue,
                                               uint32_t pool) {
  using namespace torchlight::particules;
  if (!g_bb_parcours) return false;
  const uint32_t set = Load32(base, renderer + 120);
  // Particule inexistante (liste vide) : le jeu ne met rien en file. Données propres du BillboardSet (+588 nul) :
  // _updateRenderQueue remplirait le tampon lui-même.
  const uint32_t tete = Load32(base, pool + 20);
  if (Load32(base, tete) == tete || !base[set + 588]) {
    g_bb_bilan.refus++;
    return false;
  }
  if (g_bb_nombre == g_bb.size()) g_bb.emplace_back();
  Billboards& b = g_bb[g_bb_nombre++];
  b.renderer = renderer;
  b.pool = pool;
  b.set = set;
  b.cout = Load32(base, pool + 24);
  g_bb_bilan.differes++;
#if !defined(__SWITCH__)
  if (SuiviBillboardsPoste) SuiviBillboardsPoste(renderer);
#endif
  g_bb_bilan.particules += b.cout;
  if (ModeBB() == 3) {
    // Boucle faite ici aussi, à sa place d'origine ; sommets gardés pour la comparaison, état remis.
    b.verrou = Load32(base, set + 392);
    b.compte = uint16_t(base[set + 580]) << 8 | base[set + 581];
    ctx.r3.u64 = renderer;
    ctx.r4.u64 = pool;
    torchlight_billboards_boucle(ctx, base);
    const uint32_t fin = Load32(base, set + 392);
    b.resultat.assign(base + b.verrou, base + fin);
    const uint32_t verrou_be = __builtin_bswap32(b.verrou);
    std::memcpy(base + set + 392, &verrou_be, 4);
    base[set + 580] = uint8_t(b.compte >> 8);
    base[set + 581] = uint8_t(b.compte);
  }
  // setVisible(true), _updateRenderQueue(queue), statistique du gestionnaire (+200) : la fin de sub_821DA090.
  ctx.r3.u64 = set;
  ctx.r4.u64 = 1;
  Appeler(ctx, base, Load32(base, Load32(base, set) + 96));
  ctx.r3.u64 = set;
  ctx.r4.u64 = queue;
  Appeler(ctx, base, Load32(base, Load32(base, set) + 92));
  const uint32_t gestionnaire = Load32(base, 0x83506798);
  const uint32_t total = __builtin_bswap32(Load32(base, gestionnaire + 200) + Load32(base, pool + 24));
  std::memcpy(base + gestionnaire + 200, &total, 4);
  return true;
}

using torchlight::particules::Serie;

// Node::requestUpdate(r3 parent, r4 enfant, r5 forceParentUpdate) : différée pendant une tâche.
REX_HOOK_RAW(sub_821CCDD0) {
  if (auto* t = torchlight::particules::t_tache) {
    t->differes.push_back({ctx.r3.u32, ctx.r4.u32, ctx.r5.u32});
    return;
  }
  __imp__sub_821CCDD0(ctx, base);
}

// Tri des particules d'une technique (r3 technique, r4 caméra ; appelé par sub_821BA2A0 sous
// _notifyCurrentCamera) : en série pendant les tâches.
REX_HOOK_RAW(sub_821A1CA8) {
  Serie serie;
  __imp__sub_821A1CA8(ctx, base);
}

// Réserve de particules du ParticleSystemManager : en série pendant les tâches.
REX_HOOK_RAW(sub_8258F8F8) {
  Serie serie;
  __imp__sub_8258F8F8(ctx, base);
}

REX_HOOK_RAW(sub_825A4C68) {
  Serie serie;
  __imp__sub_825A4C68(ctx, base);
}

// BillboardSet::setPoolSize : en série pendant les tâches.
REX_HOOK_RAW(sub_8247A588) {
  Serie serie;
  __imp__sub_8247A588(ctx, base);
}
