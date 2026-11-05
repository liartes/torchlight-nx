// torchlight - essai : un cœur réservé au thread principal du jeu (src/torchlight_coeurs.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace torchlight::coeurs {

// Fin d'une image du jeu (thread du jeu, EndGameFrame du rendu natif) : dessins de l'image, temps de l'image et attente
// du rendu en ms.
void Image(size_t dessins, double image_ms, double attente_ms, double releve_ms, double textures_ms);

// Bilans périodiques au journal (--torchlight_journal_stats) : coupés pour les joueurs, voir src/torchlight_coeurs.cpp.
bool JournalStats();

// Temps passé dans D3DXCompileShader, pour le journal des images longues.
uint64_t ChronoD3dxDebut();
void ChronoD3dxFin(uint64_t debut);

// Un tour de l'attente du GPU par le Direct3D du jeu (src/torchlight_trace.cpp) : vraie pause sur la console.
void PauseAttenteGpu();

}  // namespace torchlight::coeurs
