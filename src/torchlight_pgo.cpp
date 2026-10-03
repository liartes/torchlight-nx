// torchlight - vidage du profil de la build instrumentée pour le PGO (portage de app/src/nfsmw_pgo.cpp de nfsmw-nx).
//
// Compilé seulement avec TORCHLIGHT_PGO=generer (switch/CMakeLists.txt). Pour chaque objet, GCC inscrit dans
// l'exécutable le chemin TORCHLIGHT_PGO_DIR\CMakeFiles#...#fichier.gcda (dossier du profil sur le PC qui compile) :
// introuvable sur la console, donc fopen (--wrap) le redirige vers sdmc:/switch/torchlight/pgo/CMakeFiles#...gcda.
// La console ne ferme pas toujours le programme proprement (le menu HOME le tue) : un thread vide le profil toutes
// les 3 minutes et remet les compteurs à zéro ; libgcov additionne à ce que chaque .gcda contient déjà.
// Profils jamais versionnés (dossier pgo/ ignoré par git) : ils viennent du code du jeu.
#if defined(TORCHLIGHT_PGO_GENERER) && defined(__SWITCH__)

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

extern "C" {
FILE* __real_fopen(const char* path, const char* mode);
void __gcov_dump(void);
void __gcov_reset(void);

FILE* __wrap_fopen(const char* path, const char* mode) {
  static constexpr char kPrefix[] = TORCHLIGHT_PGO_DIR;
  if (path && std::strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) {
    const char* rest = path + sizeof(kPrefix) - 1;
    while (*rest == '\\' || *rest == '/') ++rest;
    std::string redirected = "sdmc:/switch/torchlight/pgo/";
    for (; *rest; ++rest) redirected += (*rest == '\\') ? '/' : *rest;
    return __real_fopen(redirected.c_str(), mode);
  }
  return __real_fopen(path, mode);
}
}

namespace {
Thread g_pgo_thread;

void PgoThread(void*) {
  for (int dump = 1;; ++dump) {
    svcSleepThread(180LL * 1000 * 1000 * 1000);
    mkdir("sdmc:/switch/torchlight/pgo", 0777);
    const u64 before = armGetSystemTick();
    __gcov_dump();
    __gcov_reset();
    const double ms = double(armTicksToNs(armGetSystemTick() - before)) / 1e6;
    if (FILE* f = __real_fopen("sdmc:/switch/torchlight/pgo/vidages.txt", "a")) {
      std::fprintf(f, "vidage %d : %.0f ms\n", dump, ms);
      std::fclose(f);
    }
  }
}

__attribute__((constructor)) void StartPgo() {
  // Priorité normale du jeu (0x3B), n'importe quel cœur : il ne travaille qu'une fois toutes les 3 minutes.
  if (R_SUCCEEDED(threadCreate(&g_pgo_thread, PgoThread, nullptr, nullptr, 64 * 1024, 0x3B, -2)))
    threadStart(&g_pgo_thread);
}
}  // namespace

#endif
