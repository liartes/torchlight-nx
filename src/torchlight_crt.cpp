// torchlight - memcpy et memset du CRT du jeu remplacés par ceux de l'hôte (méthode de nfsmw-nx,
// app/src/nfsmw_crt_nativo.cpp).
//
// Identifiés en lisant le PowerPC généré :
//  - sub_82860A50 : memcpy (octets jusqu'à l'alignement, puis blocs de 8 octets ld/stdu et le reste ; vers l'avant
//    seulement, r3 rendu inchangé). Remplacé par std::memmove, qui donne le même résultat sans recouvrement et reste
//    juste s'il y en avait un.
//  - sub_821C28D8 : memcpy vectorisé du SDK Xbox (XMemCpy : dcbt, copies VMX128 par blocs, octets de fin ; r3
//    rendu inchangé), appelé par les copies de paramètres de shaders d'Ogre. Même remplacement (1,7 % du thread
//    principal en temps propre sur la console).
//  - sub_821A7138 : copie vers la mémoire graphique (type XMemCpyStreaming : début par memcpy jusqu'à l'alignement
//    de 128 octets, milieu par blocs de 128 avec sub_82884320 ou sub_82884644 selon l'alignement de la source,
//    reste par memcpy ; rend r3). Appelée par la mise à jour des tampons de sommets d'Ogre (0,8 + 0,7 % du thread
//    principal sur la console pour les deux variantes).
//  - sub_82861140 : memset (octets jusqu'à l'alignement, mots de l'octet répété, octets de fin ; r3 inchangé).
// Le code recompilé écrit les mêmes octets, un par un ou par mots, avec inversion d'octets : la mémoire finit
// identique. Seuls des registres volatils changent, que l'appelant ne relit pas.
// Profil de la console (test n°17, ville) : memcpy du jeu 1,85 % du thread principal en temps propre.
// Sur la console, une écriture sur une page surveillée est rattrapée par le gestionnaire d'exceptions du SDK comme
// pour le memcpy de l'hôte de nfsmw-nx ([rexcrt]).

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(torchlight_crt_natif, true, "Torchlight",
                    "memcpy et memset du jeu (sub_82860A50, sub_821C28D8, sub_821A7138, sub_82861140) remplacés par ceux de l'hôte")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
bool Natif() {
  static const bool on = REXCVAR_GET(torchlight_crt_natif);
  return on;
}
}  // namespace

REX_EXTERN(__imp__sub_82860A50);
REX_HOOK_RAW(sub_82860A50) {
  if (!Natif()) {
    __imp__sub_82860A50(ctx, base);
    return;
  }
  if (const uint32_t n = ctx.r5.u32) std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, n);
}

REX_EXTERN(__imp__sub_821C28D8);
REX_HOOK_RAW(sub_821C28D8) {
  if (!Natif()) {
    __imp__sub_821C28D8(ctx, base);
    return;
  }
  if (const uint32_t n = ctx.r5.u32) std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, n);
}

REX_EXTERN(__imp__sub_821A7138);
REX_HOOK_RAW(sub_821A7138) {
  if (!Natif()) {
    __imp__sub_821A7138(ctx, base);
    return;
  }
  if (const uint32_t n = ctx.r5.u32) std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, n);
}

REX_EXTERN(__imp__sub_82861140);
REX_HOOK_RAW(sub_82861140) {
  if (!Natif()) {
    __imp__sub_82861140(ctx, base);
    return;
  }
  if (const uint32_t n = ctx.r5.u32) std::memset(base + ctx.r3.u32, ctx.r4.u8, n);
}
