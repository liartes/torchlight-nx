// torchlight - mode boost du processeur pendant les chargements (Switch).
//
// Les chargements de Torchlight se font sur le thread principal du jeu, limité par le processeur (1020 MHz aux
// fréquences de base). Le mode boost officiel (appletSetCpuBoostMode, ApmCpuBoostMode_FastLoad, celui des jeux du
// système : processeur à 1785 MHz, GPU abaissé au minimum) est demandé quand le thread du jeu ne finit plus ses images
// : un thread de surveillance l'active dès qu'aucune image n'est finie depuis kSeuil, le garde tant qu'une image a
// dépassé kSeuil dans la dernière kSortie, et le retire ensuite, ou dès que le jeu n'a plus le premier plan (menu
// HOME, veille). En jeu, même en combat lourd, une image dure moins de 60 ms sur la console (2026-10-28).
// --torchlight_boost_chargement : vrai par défaut.

#include "torchlight_boost.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(torchlight_boost_chargement, true, "Torchlight",
                    "Mode boost du processeur (FastLoad) pendant les chargements (Switch)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#ifdef __SWITCH__

#include <switch.h>

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdint>

namespace torchlight::boost {
namespace {

constexpr int64_t kSeuil = 250'000'000;   // ns sans image finie : chargement
constexpr int64_t kSortie = 500'000'000;  // ns d'images régulières avant de rendre le mode normal

int64_t Maintenant() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

std::atomic<int64_t> g_derniere{0};  // fin de la dernière image
std::atomic<int64_t> g_longue{0};    // fin de la dernière image arrivée après plus de kSeuil
bool g_demarre = false;

void* Surveiller(void*) {
  bool actif = false;
  int64_t debut = 0;
  for (;;) {
    svcSleepThread(20'000'000);
    const int64_t t = Maintenant(), derniere = g_derniere.load(std::memory_order_relaxed);
    const bool premier_plan = appletGetFocusState() == AppletFocusState_InFocus;
    const bool chargement =
        premier_plan && (t - derniere > kSeuil || t - g_longue.load(std::memory_order_relaxed) < kSortie);
    if (chargement == actif) continue;
    const Result rc = appletSetCpuBoostMode(chargement ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
    if (R_FAILED(rc)) {
      REXLOG_WARN("[boost] appletSetCpuBoostMode : erreur {:#x}, mode boost abandonné", rc);
      return nullptr;
    }
    actif = chargement;
    if (actif) {
      debut = derniere;
    } else {
      REXLOG_INFO("[boost] chargement : {:.1f} s en mode boost", double(t - debut) / 1e9);
    }
  }
}

}  // namespace

void Image() {
  if (!g_demarre) {
    g_demarre = true;
    if (!REXCVAR_GET(torchlight_boost_chargement)) return;
    g_derniere.store(Maintenant(), std::memory_order_relaxed);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_t thread;
    if (pthread_create(&thread, &attr, Surveiller, nullptr) == 0) {
      pthread_detach(thread);
      REXLOG_INFO("[boost] mode boost du processeur pendant les chargements");
    }
    pthread_attr_destroy(&attr);
    return;
  }
  const int64_t t = Maintenant();
  if (t - g_derniere.load(std::memory_order_relaxed) > kSeuil) g_longue.store(t, std::memory_order_relaxed);
  g_derniere.store(t, std::memory_order_relaxed);
}

}  // namespace torchlight::boost

#else

namespace torchlight::boost {
void Image() {}
}  // namespace torchlight::boost

#endif
