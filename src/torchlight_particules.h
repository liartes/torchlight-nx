// torchlight - mises à jour des systèmes de ParticleUniverse sur d'autres cœurs (src/torchlight_particules.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>

namespace torchlight::particules {

// Pendant la boucle des contrôleurs : met de côté la mise à jour d'un système admis (destination
// ParticleSystemUpdateValue, sub_821C8A58) au lieu de la faire. Faux : le contrôleur reste au jeu, tout de suite.
bool Prendre(uint8_t* base, uint32_t ctrl, uint32_t dest, double value);

// Fin de la boucle des contrôleurs : fait les mises à jour mises de côté (en parallèle), puis rejoue sur le thread
// principal, dans l'ordre, ce qui a été différé.
void Executer(PPCContext& ctx, uint8_t* base);

// Fait les tâches 0 à nombre - 1 : faire(ctx, base, i), sur le thread appelant et les threads de travail ; revient quand
// toutes sont faites.
void EnParallele(PPCContext& ctx, uint8_t* base, size_t nombre, void (*faire)(PPCContext&, uint8_t*, size_t));

// Parcours de l'octree (sub_821C7158) : entre les deux, les boucles de particules des billboards sont mises de côté
// (--torchlight_billboards) ; BillboardsFin les fait, puis déverrouille leurs tampons.
// Essai : mode de --torchlight_billboards (vrai) ou 0 (faux), hors du parcours.
void BillboardsActifs(bool actifs);
void BillboardsDebut(PPCContext& ctx, uint8_t* base);
void BillboardsFin(PPCContext& ctx, uint8_t* base);

// ~Controller pendant la boucle : la mise à jour mise de côté pour ce contrôleur est oubliée.
void ControleurDetruit(uint32_t ctrl);

// Vrai pendant une mise à jour faite par Executer (thread principal compris).
bool EnTache();

// Section en série entre les mises à jour parallèles (verrou récursif commun) ; sans effet hors d'une tâche.
class Serie {
 public:
  Serie();
  ~Serie();
  Serie(const Serie&) = delete;
  Serie& operator=(const Serie&) = delete;

 private:
  bool pris_;
};

}  // namespace torchlight::particules
