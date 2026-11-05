// torchlight - poses des squelettes pendant la logique du jeu.
//
// Entity::cacheBoneMatrices (sub_821B93B0) fait 7,4 % du thread principal en combat sur la console, dont 5,7 % dans la
// logique : la boucle des unités (sub_8219D288) met à jour chaque unité (sub_821D5410), qui commence par animer ses
// modèles (sub_821BB578 : temps des animations, puis pose). Étude sur PC avec switch/tools/suivi_memoire (poses),
// journal du 2026-11-03 :
//   - la seconde instance du familier (modèle +432 de l'unité, sans objet attaché à l'entité) n'est jamais lue : sa pose
//     est évitée (--torchlight_poses_double). Si elle est dessinée, le rendu la recalcule (sub_821E0EA8 : le numéro
//     d'image de la pose ne correspond pas) ;
//   - les appels de sub_821BB578 ne se gênent presque pas entre eux, et le reste de la boucle ne change presque rien de
//     ce qu'ils lisent (5 images sur 13 760 dans la session étudiée) : ils sont précalculés au début de la boucle, sur le
//     thread principal et les threads de travail des particules (--torchlight_poses), puis sautés à leur place d'origine.
//     Non identique à l'octet quand une animation change dans la boucle avant l'appel d'origine : la pose suit à l'image
//     suivante.
//
// Conditions reproduites de sub_821D5410 et sub_8219D340 ; ce qui ne les remplit pas reste à sa place d'origine. Entités
// avec des objets attachés (+528) : laissées au jeu, leur mise à jour touche au nœud parent, partagé
// (Node::requestUpdate).
// Partagé entre appels : liste des temps d'une animation, construite à la première utilisation (sub_8246E800, drapeau
// +120) : sous verrou. L'allocateur du jeu a son verrou et un tas par thread.
//
// --torchlight_poses : 0 = comme le jeu ; 1 = précalcul en série ; 2 = en parallèle ; 3 = en parallèle avec validation
// (chaque pose recalculée à la place d'origine et comparée à l'octet : les différences comptent les changements d'ordre).
// Défaut : 2, décision de l'utilisateur du 2026-11-03 (console, vrai combat : 1 pose différente sur 103 923).
//
// Poses du rendu (--torchlight_poses_rendu : 0 = comme le jeu ; 1 = en série ; 2 = en parallèle ; 3 = en parallèle avec
// validation ; 2,9 % du thread principal en combat, dont 2,3 % pour la pose) : Entity::updateAnimation (sub_821E0EA8), appelée
// par _updateRenderQueue pendant le parcours de l'octree pour les entités visibles que la logique n'a pas animées. La mise en
// file est déjà faite à ce moment : l'appel peut attendre la fin du parcours, avant tout dessin. Admis : animation matérielle
// (dernier résultat en +424, pas de demande logicielle +432), squelette, pas d'animation de sommets (+180 -> +412), aucun objet
// attaché (+524, +528), pose pas encore faite à cette image.
// Seule la pose (sub_821B93B0) part en parallèle, une fois par squelette (les entités peuvent en partager un) ; le reste de
// sub_821E0EA8 revient ensuite sur le thread principal, dans l'ordre : il touche à des données partagées (cache de techniques des
// matériaux, std::set, verrou). Faire toute la fonction en parallèle plantait une session sur ~30 sur le Ryzen (lecture de
// _Isnil d'un std::set à l'adresse 0x11) ; le suivi mémoire montrait les accès croisés (journal du 2026-11-03).

#include "torchlight_coeurs.h"
#include "torchlight_poses.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "torchlight_particules.h"

REXCVAR_DEFINE_INT32(torchlight_poses, 2, "Torchlight",
                     "Poses de la logique précalculées au début de la boucle des unités : 0 = comme le jeu ; 1 = en série ; "
                     "2 = en parallèle ; 3 = en parallèle, validation à l'octet")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_poses_double, true, "Torchlight",
                    "Pose de la seconde instance d'une unité (+432, sans objet attaché) évitée : personne ne la lit")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(torchlight_poses_rendu, 0, "Torchlight",
                     "Poses du rendu faites à la fin du parcours de l'octree : 0 = comme le jeu ; 1 = en série ; 2 = en parallèle ; "
                     "3 = en parallèle, validation à l'octet")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Suivi mémoire (switch/tools/suivi_memoire/suivi_billboards.cpp, build PC de diagnostic).
#if !defined(__SWITCH__)
extern "C" __attribute__((weak)) void SuiviBillboardsPoste(uint32_t objet) noexcept;
extern "C" __attribute__((weak)) void SuiviBillboardsTache(uint32_t objet, int debut) noexcept;
#endif

REX_EXTERN(__imp__sub_821E0EA8);
REX_EXTERN(__imp__sub_8219D288);
REX_EXTERN(__imp__sub_821BB578);
REX_EXTERN(__imp__sub_821B9478);
REX_EXTERN(__imp__sub_821B93B0);
REX_EXTERN(sub_821B93B0);
REX_EXTERN(__imp__sub_821E2D90);
REX_EXTERN(__imp__sub_8246E800);

namespace torchlight::poses {
namespace {

constexpr uint32_t kMiseAJourUnite = 0x821D5410, kMiseAJourHeros = 0x8219D340, kUniteActive = 0x821B76B8;
constexpr uint32_t kRoot = 0x835067D4;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t a, uint32_t valeur) {
  const uint32_t v = __builtin_bswap32(valeur);
  std::memcpy(base + a, &v, 4);
}

struct Tache {
  uint32_t modele;
  double dt;
};

std::vector<Tache> g_taches;
std::vector<uint32_t> g_faits;    // modèles précalculés, pas encore rencontrés à leur place d'origine
std::vector<uint32_t> g_doubles;  // entités dont la pose est évitée pendant cette boucle
thread_local bool t_tache = false;
std::mutex g_verrou_animations;
bool g_precalcul_actif = true, g_double_actif = true, g_rendu_actif = true;
std::vector<uint32_t> g_rendu;          // entités dont la mise à jour attend la fin du parcours, dans l'ordre
std::vector<uint32_t> g_rendu_poses;    // une entité par squelette : poses calculées en parallèle
bool g_rendu_parcours = false;

struct Bilan {
  uint64_t boucles = 0, modeles = 0, doubles = 0, comparaisons = 0, differences = 0, parcours = 0, rendu = 0;
  double ms = 0, ms_rendu = 0;
} g_bilan;

int ModeRendu() {
  static const int mode = REXCVAR_GET(torchlight_poses_rendu);
  return g_rendu_actif ? mode : 0;
}

int ModePrecalcul() {
  static const int mode = REXCVAR_GET(torchlight_poses);
  return g_precalcul_actif ? mode : 0;
}

bool DoubleEvite() {
  static const bool option = REXCVAR_GET(torchlight_poses_double);
  return option && g_double_actif;
}

void FaireRendu(PPCContext& ctx, uint8_t* base, size_t i) {
  const uint32_t entite = g_rendu_poses[i];
#if !defined(__SWITCH__)
  if (SuiviBillboardsTache) SuiviBillboardsTache(entite, 1);
#endif
  t_tache = true;
  ctx.r3.u64 = entite;
  sub_821B93B0(ctx, base);
  t_tache = false;
#if !defined(__SWITCH__)
  if (SuiviBillboardsTache) SuiviBillboardsTache(entite, 0);
#endif
}

bool RenduAdmissible(const uint8_t* base, uint32_t e) {
  if (!base[e + 448] || !Load32(base, e + 444)) return false;  // la fonction d'origine sort tout de suite, ou pas de pose
  if (!base[e + 424] || Load32(base, e + 432) || Load32(base, e + 436)) return false;  // animation logicielle
  if (Load32(base, Load32(base, e + 180) + 412)) return false;                          // animation de sommets
  const uint32_t tete = Load32(base, e + 524);
  if (Load32(base, tete) != tete || Load32(base, e + 528)) return false;  // objets attachés : mis en file juste après
  const uint32_t image = Load32(base, Load32(base, kRoot) + 224);
  return Load32(base, Load32(base, e + 396)) != image;  // pose déjà faite : rien de lourd à attendre
}

void FaireTache(PPCContext& ctx, uint8_t* base, size_t i) {
  const Tache& t = g_taches[i];
  t_tache = true;
  ctx.r3.u64 = t.modele;
  ctx.r5.u64 = 0;
  ctx.f1.f64 = t.dt;
  __imp__sub_821BB578(ctx, base);
  t_tache = false;
}

// Modèle laissé au jeu s'il n'a pas d'entité (sub_821BB578 sort tout de suite) ou s'il a des objets attachés.
bool Admissible(const uint8_t* base, uint32_t modele) {
  const uint32_t entite = Load32(base, modele + 92);
  return entite && !Load32(base, entite + 528);
}

// Parcours de la liste de sub_8219D288 (+84 : { objet +0, suivant +4 }) ; pas de temps de la fonction : le sien pour le
// joueur (r4), multiplié par f2 pour les autres.
void Preparer(uint8_t* base, uint32_t self, uint32_t joueur, double dt_joueur, double dt_autres, int mode) {
  g_taches.clear();
  g_doubles.clear();
  for (uint32_t noeud = Load32(base, Load32(base, self + 84)); noeud; noeud = Load32(base, noeud + 4)) {
    const uint32_t unite = Load32(base, noeud), vt = Load32(base, unite);
    const uint32_t mise_a_jour = Load32(base, vt + 332);
    if (mise_a_jour != kMiseAJourUnite && mise_a_jour != kMiseAJourHeros) continue;
    const uint32_t principal = Load32(base, unite + 428), second = Load32(base, unite + 432);
    if (mise_a_jour == kMiseAJourHeros && !principal) continue;  // sub_8219D340 sort tout de suite
    const double dt = unite == joueur ? dt_joueur : dt_autres;
    if (second && Admissible(base, second)) {
      if (DoubleEvite()) g_doubles.push_back(Load32(base, second + 92));
      if (mode) g_taches.push_back({second, dt});
    }
    if (!mode || !principal) continue;
    // sub_821D5410 avant son appel pour le modèle +428 : état +632, +109, puis +1605 et case 32.
    const uint32_t etat = Load32(base, unite + 632);
    if (etat != 12 && etat != 5 && !base[unite + 109]) {
      if (!base[unite + 1605] || Load32(base, vt + 32) != kUniteActive || !base[unite + 110]) continue;
    }
    if (Admissible(base, principal)) g_taches.push_back({principal, dt});
  }
}

void ComparerEntite(PPCContext& ctx, uint8_t* base, uint32_t entite) {
  const uint32_t squelette = entite ? Load32(base, entite + 444) : 0;
  if (!squelette) return;
  const uint32_t image = Load32(base, Load32(base, kRoot) + 224), repere = Load32(base, entite + 396);
  if (Load32(base, repere) != image) return;  // pas de pose à cette image
  const uint32_t sortie = Load32(base, entite + 384);
  const uint32_t octets = (Load32(base, squelette + 192) - Load32(base, squelette + 188)) / 4 * 64;
  static thread_local std::vector<uint8_t> copie;
  copie.assign(base + sortie, base + sortie + octets);
  Store32(base, repere, image - 1);
  ctx.r3.u64 = entite;
  __imp__sub_821B93B0(ctx, base);
  g_bilan.comparaisons++;
  if (std::memcmp(base + sortie, copie.data(), octets) != 0 && g_bilan.differences++ < 8) {
    REXLOG_WARN("[poses] pose différente à sa place d'origine : entité {:08X}, image {}", entite, image);
  }
}

void Comparer(PPCContext& ctx, uint8_t* base, uint32_t modele) { ComparerEntite(ctx, base, Load32(base, modele + 92)); }

void Rapport() {
  static auto dernier = std::chrono::steady_clock::now();
  const auto maintenant = std::chrono::steady_clock::now();
  if (maintenant - dernier < std::chrono::seconds(10) || !g_bilan.boucles) return;
  dernier = maintenant;
  const double n = double(g_bilan.boucles);
  const double p = g_bilan.parcours ? double(g_bilan.parcours) : 1.0;
  if (torchlight::coeurs::JournalStats()) REXLOG_INFO("[poses] par boucle : {:.1f} modèles précalculés, {:.1f} poses évitées, {:.3f} ms ; validation : {} comparées, "
              "{} différentes ; rendu : {:.1f} entités par parcours, {:.3f} ms",
              g_bilan.modeles / n, g_bilan.doubles / n, g_bilan.ms / n, g_bilan.comparaisons, g_bilan.differences,
              g_bilan.rendu / p, g_bilan.ms_rendu / p);
  g_bilan = Bilan{};
}

}  // namespace

void PrecalculActif(bool actif) { g_precalcul_actif = actif; }
void DoubleActif(bool actif) { g_double_actif = actif; }
void RenduActif(bool actif) { g_rendu_actif = actif; }

void ParcoursDebut() { g_rendu_parcours = ModeRendu() != 0; }

void ParcoursFin(PPCContext& ctx, uint8_t* base) {
  g_rendu_parcours = false;
  if (!ModeRendu()) return;
  g_bilan.parcours++;
  if (g_rendu.empty()) return;
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t ancien = ctx.r1.u32, sp = ancien - 256;
  std::memset(base + sp, 0, 16);
  ctx.r1.u64 = sp;
  if (ModeRendu() == 1) {
    for (size_t i = 0; i < g_rendu_poses.size(); i++) FaireRendu(ctx, base, i);
  } else {
    torchlight::particules::EnParallele(ctx, base, g_rendu_poses.size(), FaireRendu);
  }
  if (ModeRendu() == 3) {
    for (const uint32_t entite : g_rendu_poses) ComparerEntite(ctx, base, entite);
  }
  // Le reste de sub_821E0EA8 : la pose y est déjà faite (numéro d'image à jour).
  for (const uint32_t entite : g_rendu) {
    ctx.r3.u64 = entite;
    __imp__sub_821E0EA8(ctx, base);
  }
  ctx.r1.u64 = ancien;
  g_bilan.rendu += g_rendu.size();
  g_rendu.clear();
  g_rendu_poses.clear();
  g_bilan.ms_rendu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace torchlight::poses

using namespace torchlight::poses;

// Entity::updateAnimation (r3 l'entité), depuis _updateRenderQueue.
REX_HOOK_RAW(sub_821E0EA8) {
  if (g_rendu_parcours && RenduAdmissible(base, ctx.r3.u32)) {
    const uint32_t entite = ctx.r3.u32, squelette = Load32(base, entite + 444);
    g_rendu.push_back(entite);
    const bool deja = std::any_of(g_rendu_poses.begin(), g_rendu_poses.end(), [&](uint32_t e) {
      return Load32(base, e + 444) == squelette;
    });
    if (!deja) {
      g_rendu_poses.push_back(entite);
#if !defined(__SWITCH__)
      if (SuiviBillboardsPoste) SuiviBillboardsPoste(entite);
#endif
    }
    return;
  }
  __imp__sub_821E0EA8(ctx, base);
}

// Boucle des unités (r3 l'objet, r4 le joueur, f1 et f2 : pas de temps).
REX_HOOK_RAW(sub_8219D288) {
  const int mode = ModePrecalcul();
  if (!mode && !DoubleEvite()) return __imp__sub_8219D288(ctx, base);
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  const double f1 = ctx.f1.f64, f2 = ctx.f2.f64;
  Preparer(base, ctx.r3.u32, ctx.r4.u32, f1, double(float(f1 * f2)), mode);
  g_bilan.boucles++;
  g_bilan.doubles += g_doubles.size();
  if (!g_taches.empty()) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t ancien = ctx.r1.u32, sp = ancien - 256;
    std::memset(base + sp, 0, 16);
    ctx.r1.u64 = sp;
    if (mode == 1) {
      for (size_t i = 0; i < g_taches.size(); i++) FaireTache(ctx, base, i);
    } else {
      torchlight::particules::EnParallele(ctx, base, g_taches.size(), FaireTache);
    }
    ctx.r1.u64 = ancien;
    g_faits.clear();
    for (const Tache& t : g_taches) g_faits.push_back(t.modele);
    g_bilan.modeles += g_taches.size();
    g_bilan.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.f1.f64 = f1;
  ctx.f2.f64 = f2;
  __imp__sub_8219D288(ctx, base);
  g_faits.clear();
  g_doubles.clear();
  Rapport();
}

// Animation d'un modèle (r3, f1 pas de temps, r5) : sautée si elle a été précalculée.
REX_HOOK_RAW(sub_821BB578) {
  if (!g_faits.empty()) {
    const auto it = std::find(g_faits.begin(), g_faits.end(), ctx.r3.u32);
    if (it != g_faits.end()) {
      g_faits.erase(it);
      if (ModePrecalcul() == 3) Comparer(ctx, base, ctx.r3.u32);
      return;
    }
  }
  __imp__sub_821BB578(ctx, base);
}

// Entity::_updateAnimation pendant la logique (r3 l'entité).
REX_HOOK_RAW(sub_821B9478) {
  if (!g_doubles.empty() && std::find(g_doubles.begin(), g_doubles.end(), ctx.r3.u32) != g_doubles.end()) return;
  __imp__sub_821B9478(ctx, base);
}

// Animation::getTimeIndex (r4 l'animation) : liste des temps construite à la première utilisation, sous verrou pendant le
// précalcul. Le verrou ordonne aussi la lecture du drapeau et de la liste entre les threads.
REX_HOOK_RAW(sub_821E2D90) {
  if (t_tache) {
    std::lock_guard<std::mutex> verrou(g_verrou_animations);
    if (base[ctx.r4.u32 + 120]) {
      const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
      const double f1 = ctx.f1.f64;
      ctx.r3.u64 = r4;
      __imp__sub_8246E800(ctx, base);
      ctx.r3.u64 = r3;
      ctx.r4.u64 = r4;
      ctx.f1.f64 = f1;
    }
  }
  __imp__sub_821E2D90(ctx, base);
}
