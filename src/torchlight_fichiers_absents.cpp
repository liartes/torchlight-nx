// torchlight - cache des fichiers absents de GAME:.
//
// Idée et point d'interception repris de torchlight-recomp (YaPeL, GPL-3.0, commit d3d79f8,
// src/hooks/find_file_hooks.cpp et missing_file_cache.{h,cpp}).
//
// OGRE cherche chaque ressource dans ses dossiers l'un après l'autre (game:\music\, game:\programs\,
// game:\RTShaderLib\, GAME:\MEDIA\...) par FindFirstFileA (sub_8287E0C8), qui appelle la recherche native
// sub_82883A68 (r3 = OBJECT_ATTRIBUTES : dossier racine +0, nom +4 -> ANSI_STRING ; r4 tampon, r5 taille,
// r6 poignée en sortie). Celle-ci coupe le chemin à la dernière '\', ouvre le dossier (NtOpenFile) puis y
// cherche le nom. Sur la carte ces dossiers n'existent pas (tout est dans pak.zip) : chaque recherche coûte une
// ouverture en échec (~50 µs sur la console, 12 683 pour 494 dossiers dans la mesure du 2026-11-05).
//
// Ici : une recherche sur GAME: dont le résultat est « introuvable » est retenue, et la même recherche
// rend ensuite ce statut sans appeler l'original (qui, en échec, n'écrit rien chez l'appelant). Deux niveaux :
//  - le dossier lui-même est absent (vérifié une fois par le VFS) : toute recherche dans ce dossier échoue
//    de la même façon, quel que soit le nom ;
//  - le dossier existe mais pas le fichier : retenu par chemin complet, sans joker.
// GAME: est en lecture seule : un fichier absent ne peut pas apparaître en cours de partie.
// --torchlight_fichiers_absents=2 : l'original est toujours appelé et son statut comparé (journal).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "torchlight_invite.h"

REXCVAR_DEFINE_INT32(torchlight_fichiers_absents, 1, "Torchlight",
                     "Cache des fichiers absents de GAME: (0 : coupé, 1 : actif, 2 : validation contre l'original)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

using torchlight::invite::Load32;

std::shared_mutex g_verrou;
std::unordered_map<std::string, uint32_t> g_absents;  // dossier (finit par '\') ou chemin complet -> statut
std::atomic<uint64_t> g_recherches{0}, g_evitees{0}, g_differences{0};

bool Introuvable(uint32_t statut) {
  return statut == 0xC000000F ||  // STATUS_NO_SUCH_FILE
         statut == 0xC0000034 ||  // STATUS_OBJECT_NAME_NOT_FOUND
         statut == 0xC000003A ||  // STATUS_OBJECT_PATH_NOT_FOUND
         statut == 0x80000006;    // STATUS_NO_MORE_FILES
}

// Chemin de la recherche en majuscules ('/' gardé : l'original ne coupe qu'aux '\'), et longueur de sa partie
// dossier, '\' finale comprise. Faux si la recherche n'est pas à retenir : pas sur GAME:, racine autre que
// ObDosDevices, pas de '\' ou nom vide (l'original répond alors 0xC000000D sans rien ouvrir).
bool Cle(const uint8_t* base, uint32_t attributs, std::string& chemin, size_t& dossier) {
  if (Load32(base, attributs) != 0xFFFFFFFDu) return false;
  const uint32_t nom = Load32(base, attributs + 4);
  if (!nom) return false;
  const uint32_t longueur = uint32_t(base[nom] << 8 | base[nom + 1]);
  const uint32_t texte = Load32(base, nom + 4);
  if (!texte || longueur < 7 || longueur > 1024) return false;
  chemin.resize(longueur);
  for (uint32_t i = 0; i < longueur; i++) {
    char c = char(base[texte + i]);
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    chemin[i] = c;
  }
  if (chemin.compare(0, 5, "GAME:") != 0) return false;
  const size_t barre = chemin.rfind('\\');
  if (barre == std::string::npos || barre + 1 == chemin.size()) return false;
  dossier = barre + 1;
  return true;
}

bool Trouver(const std::string& cle, uint32_t& statut) {
  const auto it = g_absents.find(cle);
  if (it == g_absents.end()) return false;
  statut = it->second;
  return true;
}

bool DossierAbsent(const std::string& dossier) {
  auto* ks = REX_KERNEL_STATE();
  auto* fs = ks ? ks->file_system() : nullptr;
  if (!fs) return false;
  // Sans la '\' finale, sauf pour la racine (« GAME:\ »).
  std::string chemin = dossier;
  if (chemin.size() > 6) chemin.pop_back();
  return fs->ResolvePath(chemin) == nullptr;
}

}  // namespace

REX_EXTERN(__imp__sub_82883A68);
REX_HOOK_RAW(sub_82883A68) {
  const int mode = REXCVAR_GET(torchlight_fichiers_absents);
  std::string chemin;
  size_t dossier = 0;
  if (mode <= 0 || !Cle(base, ctx.r3.u32, chemin, dossier)) {
    __imp__sub_82883A68(ctx, base);
    return;
  }
  const uint64_t n = g_recherches.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((n & 0xFFF) == 0)
    REXLOG_INFO("[fichiers absents] {} recherches sur GAME:, {} évitées, {} différences", n,
                g_evitees.load(std::memory_order_relaxed), g_differences.load(std::memory_order_relaxed));
  const std::string cle_dossier = chemin.substr(0, dossier);
  const bool joker = chemin.find_first_of("*?", dossier) != std::string::npos;
  uint32_t retenu = 0;
  bool connu;
  {
    std::shared_lock lecture(g_verrou);
    connu = Trouver(cle_dossier, retenu) || (!joker && Trouver(chemin, retenu));
  }
  if (connu) {
    g_evitees.fetch_add(1, std::memory_order_relaxed);
    if (mode != 2) {
      ctx.r3.u64 = retenu;  // rien d'ouvert, rien d'écrit chez l'appelant
      return;
    }
    __imp__sub_82883A68(ctx, base);
    if (ctx.r3.u32 != retenu) {
      g_differences.fetch_add(1, std::memory_order_relaxed);
      REXLOG_ERROR("[fichiers absents] différence pour {} : retenu {:08X}, original {:08X}", chemin, retenu,
                   ctx.r3.u32);
    }
    return;
  }
  __imp__sub_82883A68(ctx, base);
  const uint32_t statut = ctx.r3.u32;
  if (!Introuvable(statut)) return;
  const bool absent = DossierAbsent(cle_dossier);
  if (!absent && joker) return;
  std::unique_lock ecriture(g_verrou);
  g_absents.emplace(absent ? cle_dossier : chemin, statut);
}
