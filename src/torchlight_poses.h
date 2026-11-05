// torchlight - poses des squelettes pendant la logique du jeu (src/torchlight_poses.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace torchlight::poses {

// Essais (src/torchlight_coeurs.cpp) : réglages activés ou coupés entre deux images.
void PrecalculActif(bool actif);
void DoubleActif(bool actif);
void RenduActif(bool actif);

// Parcours de l'octree (sub_821C7158) : entre les deux, les mises à jour d'animation des entités admises attendent ;
// ParcoursFin les fait, avant tout dessin.
void ParcoursDebut();
void ParcoursFin(PPCContext& ctx, uint8_t* base);

}  // namespace torchlight::poses
