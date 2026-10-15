// torchlight - côté PC des appels indirects marqués et promus (switch/tools/appels_indirects.py), d'après
// app/src/cpu/sa2_codegen_checks.cpp de sa2-nx (GPL-3.0). Sur la Switch, les macros du pch généré sont l'opération
// nue : rien ici.
//   - relevé (torchlight_releve_appels_indirects = <fichier>) : par site, nombre d'appels et cibles (4 cibles
//     distinguées, le reste dans « other ») ; fichier réécrit toutes les 10 s, entrée de
//     switch/tools/appels_indirects_promus.py ;
//   - vérification : chaque appel promu compte et vérifie que la table de répartition contient bien la fonction
//     appelée pour cette adresse (RexPromotedMismatch sinon) ; bilan dans le journal toutes les 10 s ;
//   - constantes de .rdata repliées (switch/tools/constantes_lecture_seule.py) : sur PC, le chargement reste et est
//     comparé à la constante (RexRoMismatch sinon), compté dans le même bilan.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_STRING(torchlight_releve_appels_indirects, "", "Torchlight",
                      "Diagnostic (PC) : fichier du relevé des appels indirects (par site : appels et cibles), "
                      "pour switch/tools/appels_indirects_promus.py. Vide : pas de relevé")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if !REX_PLATFORM_SWITCH

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
volatile unsigned long long g_rex_promoted_calls = 0;  // comptes approximatifs (sans synchronisation)
volatile unsigned long long g_rex_ro_checked = 0;
unsigned char g_rex_indirect_census = 0;
}

namespace {

std::atomic<uint64_t> g_erreurs{0};
std::atomic<uint64_t> g_erreurs_ro{0};

// Un site : (adresse de la fonction << 32) | numéro.
struct Site {
  uint64_t total = 0;
  uint32_t cible[4] = {};
  uint64_t compte[4] = {};
  uint64_t autres = 0;
};
constexpr size_t kBandes = 64;
struct Bande {
  std::mutex mutex;
  std::unordered_map<uint64_t, Site> sites;
};
Bande g_bandes[kBandes];

void Ecrire(const std::string& chemin) {
  struct Ligne {
    uint64_t cle;
    Site site;
  };
  std::vector<Ligne> lignes;
  for (Bande& b : g_bandes) {
    std::lock_guard<std::mutex> verrou(b.mutex);
    for (const auto& [cle, site] : b.sites) lignes.push_back({cle, site});
  }
  std::sort(lignes.begin(), lignes.end(), [](const Ligne& x, const Ligne& y) {
    return x.site.total != y.site.total ? x.site.total > y.site.total : x.cle < y.cle;
  });
  uint64_t appels = 0, haut = 0;
  for (const Ligne& l : lignes) {
    appels += l.site.total;
    haut += *std::max_element(l.site.compte, l.site.compte + 4);
  }
  REXLOG_INFO("[appels indirects] relevé : {} sites, {} appels, {:.1f} % vers la cible la plus fréquente du site",
              lignes.size(), appels, appels ? 100.0 * double(haut) / double(appels) : 0.0);
  const std::string temp = chemin + ".tmp";
  if (FILE* f = std::fopen(temp.c_str(), "w")) {
    std::fprintf(f, "# relevé des appels indirects (cumulé) : <fonction> <site> <appels> <cible>:<appels>... "
                    "other:<appels>\n");
    for (const Ligne& l : lignes) {
      std::fprintf(f, "%08X %u %llu", unsigned(l.cle >> 32), unsigned(l.cle & 0xFFFFFFFFu),
                   static_cast<unsigned long long>(l.site.total));
      for (int i = 0; i < 4; ++i)
        if (l.site.compte[i])
          std::fprintf(f, " %08X:%llu", l.site.cible[i], static_cast<unsigned long long>(l.site.compte[i]));
      std::fprintf(f, " other:%llu\n", static_cast<unsigned long long>(l.site.autres));
    }
    std::fclose(f);
    std::remove(chemin.c_str());
    std::rename(temp.c_str(), chemin.c_str());
  }
}

void* Bilan(void*) {
  const std::string chemin = REXCVAR_GET(torchlight_releve_appels_indirects);
  for (;;) {
    sleep(10);
    const uint64_t promus = g_rex_promoted_calls, erreurs = g_erreurs.load(std::memory_order_relaxed);
    if (promus) {
      if (erreurs)
        REXLOG_ERROR("[appels indirects] DIFFÉRENCE : {} appels promus sur {} ne correspondent pas à la table",
                     erreurs, promus);
      else
        REXLOG_INFO("[appels indirects] vérification : {} appels promus, 0 différence", promus);
    }
    const uint64_t ro = g_rex_ro_checked, erreurs_ro = g_erreurs_ro.load(std::memory_order_relaxed);
    if (ro) {
      if (erreurs_ro)
        REXLOG_ERROR("[constantes] DIFFÉRENCE : {} constantes repliées sur {} ne correspondent pas à la mémoire",
                     erreurs_ro, ro);
      else
        REXLOG_INFO("[constantes] vérification : {} chargements de constantes repliées, 0 différence", ro);
    }
    if (g_rex_indirect_census) Ecrire(chemin);
  }
  return nullptr;
}

struct Lanceur {
  Lanceur() {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&t, &attr, Bilan, nullptr) == 0) pthread_detach(t);
    pthread_attr_destroy(&attr);
  }
};

}  // namespace

extern "C" void RexPromotedMismatch(unsigned target) noexcept {
  if (g_erreurs.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_ERROR("[appels indirects] DIFFÉRENCE : la table de répartition ne contient pas, pour {:08X}, la fonction "
                 "appelée par le code généré",
                 target);
}

extern "C" void RexRoMismatch(unsigned address) noexcept {
  if (g_erreurs_ro.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_ERROR("[constantes] DIFFÉRENCE : la mémoire du jeu à {:08X} ne contient pas la constante du code généré",
                 address);
}

extern "C" void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
  const uint64_t cle = (uint64_t(function) << 32) | site;
  Bande& b = g_bandes[(cle * 0x9E3779B97F4A7C15ull) >> 58];
  std::lock_guard<std::mutex> verrou(b.mutex);
  Site& s = b.sites[cle];
  ++s.total;
  for (int i = 0; i < 4; ++i) {
    if (s.cible[i] == target || s.compte[i] == 0) {
      s.cible[i] = target;
      ++s.compte[i];
      return;
    }
  }
  ++s.autres;
}

namespace torchlight::appels_indirects {
// Appelé une fois les options lues (OnPostInitLogging) : active le relevé et lance le bilan des 10 s.
void Initialiser() {
  static Lanceur lanceur;
  g_rex_indirect_census = REXCVAR_GET(torchlight_releve_appels_indirects).empty() ? 0 : 1;
  if (g_rex_indirect_census)
    REXLOG_INFO("[appels indirects] relevé actif : {}", REXCVAR_GET(torchlight_releve_appels_indirects));
}
}  // namespace torchlight::appels_indirects

#else

#include <arm_neon.h>

#include <cstdint>
#include <cstring>

namespace torchlight::appels_indirects {
// Auto-contrôle de la forme ARM des chargements flottants (switch/tools/chargements_flottants.py, macro
// REX_LOAD_F32 du pch généré : même assembleur ici), qui ne peut pas tourner sur PC : comparée à la forme entière
// (inversion des octets puis réinterprétation) sur des valeurs ordinaires et les cas limites, bit pour bit.
void Initialiser() {
  static const uint32_t valeurs[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u,
                                     0x7FC00000u, 0x7FA00001u, 0xFFFFFFFFu, 0x00000001u, 0x807FFFFFu, 0x3DCCCCCDu,
                                     0x12345678u, 0x89ABCDEFu, 0x40490FDBu, 0x00800000u};
  int erreurs = 0;
  for (uint32_t v : valeurs) {
    const uint32_t memoire = __builtin_bswap32(v);  // la mémoire du jeu est gros-boutiste
    float32x2_t r;
    asm volatile("ldr %s0, %1\n\trev32 %0.8b, %0.8b" : "=w"(r) : "m"(memoire));
    const float vectoriel = vget_lane_f32(r, 0);
    uint32_t bits;
    std::memcpy(&bits, &vectoriel, 4);
    if (bits != v) erreurs++;
  }
  if (erreurs)
    REXLOG_ERROR("[auto-contrôle] chargements flottants : {} valeurs sur {} différentes de la forme entière", erreurs,
                 sizeof(valeurs) / sizeof(valeurs[0]));
  else
    REXLOG_INFO("[auto-contrôle] chargements flottants : forme ARM identique à la forme entière ({} valeurs)",
                sizeof(valeurs) / sizeof(valeurs[0]));
}
}  // namespace torchlight::appels_indirects

#endif
