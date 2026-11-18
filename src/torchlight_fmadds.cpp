// Vérification des preuves de switch/tools/fmadds_prouvees.py (build avec -DTORCHLIGHT_VERIF_SIMPLES=ON seulement) :
// chaque REX_FMADDS_KNOWN et REX_FRSP_SIMPLE du code généré contrôle que ses entrées sont des float exacts et appelle
// cette fonction sinon. Compteur et premières fonctions au journal ; bilan « [fmadds prouvées] » toutes les 2^20 fautes.
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/logging.h>

namespace {
std::atomic<uint64_t> g_fautes{0};
}

extern "C" void TorchlightPreuveSimpleFausse(const char* fonction, double a, double b, double c) {
  const uint64_t n = g_fautes.fetch_add(1, std::memory_order_relaxed);
  if (n < 32 || (n & ((1u << 20) - 1)) == 0) {
    uint64_t ba, bb, bc;
    std::memcpy(&ba, &a, 8);
    std::memcpy(&bb, &b, 8);
    std::memcpy(&bc, &c, 8);
    REXLOG_ERROR("[fmadds prouvées] preuve fausse n° {} dans {} : {:016X} {:016X} {:016X}", n + 1, fonction, ba, bb,
                 bc);
  }
}
