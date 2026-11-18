// torchlight - durée minimale de l'avis de sauvegarde.
//
// sub_82344EE0 (r3 = objet du jeu, r4 = afficher / cacher) montre ou cache l'avis de sauvegarde (objet en +980).
// En l'affichant, le jeu note l'heure (+6332, ms, sub_821F8EE0) ; en le cachant, il ENDORT le thread principal
// (Sleep, sub_8287D878) pour que l'avis soit resté assez longtemps à l'écran : jusqu'à 1 s après l'affichage (3 s
// si la sauvegarde a duré plus de 1 s), ou 5 s si l'octet +276 de l'avis est levé. Sur la console : une image de
// ~1 s à chaque sauvegarde (départ et arrivée dans une zone, scène affichée et figée) et une de 5 s au démarrage
// (switch-logs-archive/20261110-essai-absents/, images de 992-999 et 4998 ms).
//
// --torchlight_attente_sauvegarde=N (0 à 1000 ms) : l'avis reste au moins N ms depuis son affichage, pas plus.
// L'heure notée est décalée avant l'appel pour que le calcul du jeu donne cette attente ; rien d'autre ne change.
// -1 (défaut) : comme le jeu.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "torchlight_invite.h"

REXCVAR_DEFINE_INT32(torchlight_attente_sauvegarde, -1, "Torchlight",
                     "Durée minimale de l'avis de sauvegarde en ms (0 à 1000) ; -1 : comme le jeu (1 à 5 s, jeu figé)");

namespace {
using torchlight::invite::Load32;
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
}  // namespace

REX_EXTERN(__imp__sub_82344EE0);
REX_EXTERN(__imp__sub_821F8EE0);
REX_HOOK_RAW(sub_82344EE0) {
  const int reglage = REXCVAR_GET(torchlight_attente_sauvegarde);
  const uint32_t objet = ctx.r3.u32;
  const uint32_t avis = Load32(base, objet + 980);
  if (reglage < 0 || !avis || (ctx.r4.u32 & 0xFF) != 0) {
    __imp__sub_82344EE0(ctx, base);
    return;
  }
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  __imp__sub_821F8EE0(ctx, base);  // heure du jeu en ms : lit seulement une variable du noyau
  const uint32_t maintenant = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  const uint32_t ecoule = maintenant - Load32(base, objet + 6332);
  const uint32_t minimum = uint32_t(std::min(reglage, 1000));
  const uint32_t attente = ecoule < minimum ? minimum - ecoule : 0;
  // Écoulé à faire croire au jeu pour qu'il attende « attente » : avec l'octet +276, 5000 - écoulé (jamais 0 :
  // au-delà de 5000 le jeu dort « écoulé » ms) ; sinon 1000 - écoulé, et rien au-delà de 3000.
  uint32_t fictif;
  if (base[avis + 276]) fictif = 5000 - std::max(attente, 1u);
  else fictif = attente ? 1000 - attente : 3001;
  Store32(base, objet + 6332, maintenant - fictif);
  static bool dit = false;
  if (!dit) {
    dit = true;
    REXLOG_INFO("[sauvegarde] avis affiché {} ms, attente ramenée à {} ms (le jeu : {})", ecoule, attente,
                base[avis + 276] ? "5 s" : "1 à 3 s");
  }
  __imp__sub_82344EE0(ctx, base);
}
