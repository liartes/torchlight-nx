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
#endif

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
thread_local Tache* t_tache = nullptr;
std::recursive_mutex g_serie;
thread_local int t_serie = 0;

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
  REXLOG_INFO("[particules] par image : {:.1f} systèmes en parallèle, refusés {:.1f} (état) {:.1f} (rendu) {:.1f} "
              "(préparation) {:.1f} (fin) ; {:.1f} caméras ; ms : {:.3f} mises à jour, {:.3f} différés rejoués "
              "({:.1f}) ; annulées {}, destructions pendant les tâches {}",
              g_bilan.prises / n, g_bilan.refus_etat / n, g_bilan.refus_rendu / n, g_bilan.refus_prepa / n,
              g_bilan.refus_fin / n, g_bilan.cameras / n, g_bilan.ms / n, g_bilan.ms_differes / n,
              g_bilan.differes / n, g_bilan.annulees, g_destructions.load(std::memory_order_relaxed));
  REXLOG_INFO("[particules] refus de préparation (+680 +681 +682 renderer +66 +189 +190 +191 +192) : {} {} {} {} {} {} {} {}",
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

  void Lancer(PPCContext& ctx, uint8_t* base) {
    {
      std::lock_guard<std::mutex> verrou(lock_);
      fpscr_ = ctx.fpscr;
      suivant_.store(0, std::memory_order_relaxed);
      ouvert_ = true;
      tour_++;
    }
    reveil_.notify_all();
    Prendre(ctx, base);
    std::unique_lock<std::mutex> verrou(lock_);
    ouvert_ = false;
    fini_.wait(verrou, [this] { return occupes_ == 0; });
  }

  static void Faire(PPCContext& ctx, uint8_t* base, Tache& t) {
    t_tache = &t;
    ctx.r3.u64 = t.dest;
    ctx.f1.f64 = t.value;
    __imp__sub_821C8A58(ctx, base);
    t_tache = nullptr;
  }

 private:
  void Prendre(PPCContext& ctx, uint8_t* base) {
    for (size_t i = suivant_.fetch_add(1, std::memory_order_relaxed); i < g_nombre;
         i = suivant_.fetch_add(1, std::memory_order_relaxed)) {
      if (!g_taches[i].annulee) Faire(ctx, base, g_taches[i]);
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
      std::lock_guard<std::mutex> verrou(lock_);
      if (--occupes_ == 0) fini_.notify_one();
    }
  }

  std::vector<rex::system::object_ref<rex::system::XHostThread>> threads_;
  std::mutex lock_;
  std::condition_variable reveil_, fini_;
  decltype(PPCContext::fpscr) fpscr_{};
  std::atomic<size_t> suivant_{0};
  uint64_t tour_ = 0;
  bool ouvert_ = false;
  int occupes_ = 0;
};

Groupe* g_groupe = nullptr;

}  // namespace

bool EnTache() { return t_tache != nullptr; }

Serie::Serie() : pris_(t_tache != nullptr) {
  if (!pris_) return;
  g_serie.lock();
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
    if (Mode() == 2) {
      // Les plus coûteuses d'abord : moins d'attente de la dernière tâche à la fin (profil de la console du
      // 2026-10-28 : 4,8 % du thread principal dans Executer en combat, sa part comprise).
      std::stable_sort(g_taches.begin(), g_taches.begin() + g_nombre,
                       [](const Tache& a, const Tache& b) { return a.cout > b.cout; });
      if (!g_groupe) {
        g_groupe = new Groupe;
        g_groupe->Demarrer(REXCVAR_GET(torchlight_particules_threads));
      }
      g_groupe->Lancer(ctx, base);
    } else {
      for (size_t i = 0; i < g_nombre; i++)
        if (!g_taches[i].annulee) Groupe::Faire(ctx, base, g_taches[i]);
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_nombre; i++) {
      for (const Differe& d : g_taches[i].differes) {
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

}  // namespace torchlight::particules

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
