// torchlight - fond du menu principal d'après le dernier personnage joué, dès le démarrage.
//
// Le jeu construit le menu avant « Begin » (sub_82212950, état 0) : sub_8221DC20 ouvre la sauvegarde du
// personnage dont le chemin est au champ +5388 de l'objet du jeu (« SAVE:\0.TSV ») et en tire son niveau, donc le
// fond (MAINMENURULES du niveau) ; sans fichier, fond de la ville. Au démarrage ce chemin est vide (il n'est rempli
// qu'en choisissant un personnage) et SAVE: n'est pas monté (il l'est après « Begin ») : toujours la ville.
// Ici : le chemin passé à sub_8221DC20 est retenu dans les données utilisateur (menu_dernier_personnage.txt) ;
// au démarrage suivant, pour ce seul appel, une copie de ce fichier (menu_fond/) est montée en lecture seule sous
// SAVE: et le chemin retenu est passé au jeu, puis le lien SAVE: est retiré (le jeu monte ensuite sa sauvegarde
// comme d'habitude).
// Fichier absent (personnage supprimé) : le jeu retombe sur la ville. Analyse : CLAUDE.md, « Fond du menu ».

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>

#include "torchlight_invite.h"

REXCVAR_DEFINE_BOOL(torchlight_menu_fond, true, "Torchlight",
                    "Fond du menu principal d'après le dernier personnage joué dès le démarrage")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

using torchlight::invite::Load32;

constexpr const char* kFichier = "menu_dernier_personnage.txt";
constexpr const char* kMontage = "\\Device\\TorchlightMenu\\";

std::filesystem::path DonneesUtilisateur() {
  auto* ks = REX_KERNEL_STATE();
  return ks && ks->emulator() ? ks->emulator()->user_data_root() : std::filesystem::path();
}

void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}

// std::wstring MSVC du jeu (gros-boutiste) : tampon interne de 8 caractères ou pointeur, taille +16, capacité +20.
// Seuls les chemins ASCII sont gardés (« SAVE:\0.TSV »).
std::string LireChemin(const uint8_t* base, uint32_t s) {
  const uint32_t size = Load32(base, s + 16);
  const uint32_t cap = Load32(base, s + 20);
  if (size == 0 || size > 64 || cap < size) return {};
  const uint32_t data = cap >= 8 ? Load32(base, s) : s;
  std::string out;
  for (uint32_t i = 0; i < size; i++) {
    const uint16_t c = uint16_t(base[data + i * 2] << 8 | base[data + i * 2 + 1]);
    if (c < 0x20 || c > 0x7E) return {};
    out += char(c);
  }
  return out;
}

void Retenir(const std::string& chemin) {
  static std::string dernier;
  if (chemin == dernier || chemin.rfind("SAVE:\\", 0) != 0) return;
  dernier = chemin;
  const auto dossier = DonneesUtilisateur();
  if (dossier.empty()) return;
  // Écrit en place (pas de fichier temporaire renommé : la console refuse de renommer un fichier ouvert).
  if (FILE* f = std::fopen((dossier / kFichier).string().c_str(), "wb")) {
    std::fwrite(chemin.data(), 1, chemin.size(), f);
    std::fclose(f);
    REXLOG_INFO("[menu] dernier personnage retenu : {}", chemin);
  }
}

std::string Relire() {
  const auto dossier = DonneesUtilisateur();
  if (dossier.empty()) return {};
  std::string chemin;
  if (FILE* f = std::fopen((dossier / kFichier).string().c_str(), "rb")) {
    char tampon[80];
    const size_t n = std::fread(tampon, 1, sizeof(tampon), f);
    std::fclose(f);
    chemin.assign(tampon, n);
  }
  if (chemin.rfind("SAVE:\\", 0) != 0 || chemin.size() > 64) return {};
  for (char c : chemin)
    if (c < 0x20 || c > 0x7E) return {};
  return chemin;
}

// Dossier de la sauvegarde du jeu (ContentManager du SDK : données utilisateur / XUID / titre / type / nom).
std::filesystem::path DossierSauvegarde() {
  auto* ks = REX_KERNEL_STATE();
  if (!ks || !ks->user_profile()) return {};
  const auto dossier = DonneesUtilisateur();
  if (dossier.empty()) return {};
  return dossier / fmt::format("{:016X}", ks->user_profile()->xuid()) / fmt::format("{:08X}", ks->title_id()) /
         "00000001" / "torchlight.sav";
}

// Copie de la sauvegarde du personnage (nom tel que le jeu le demande, « 0.TSV » ; sur la console le fichier est
// « 0.tsv ») dans le dossier de copie, écrasée à chaque démarrage. Faux si absente (personnage supprimé).
bool Copier(const std::filesystem::path& sauvegarde, const std::string& nom, const std::filesystem::path& copie) {
  std::string minuscules = nom;
  for (char& c : minuscules) c = char(std::tolower(static_cast<unsigned char>(c)));
  FILE* src = std::fopen((sauvegarde / nom).string().c_str(), "rb");
  if (!src) src = std::fopen((sauvegarde / minuscules).string().c_str(), "rb");
  if (!src) return false;
  std::error_code ec;
  std::filesystem::create_directories(copie, ec);
  FILE* dst = std::fopen((copie / nom).string().c_str(), "wb");
  bool ok = dst != nullptr;
  char tampon[64 * 1024];
  size_t n;
  while (ok && (n = std::fread(tampon, 1, sizeof(tampon), src)) > 0) ok = std::fwrite(tampon, 1, n, dst) == n;
  std::fclose(src);
  if (dst) ok = std::fclose(dst) == 0 && ok;
  return ok;
}

}  // namespace

REX_EXTERN(__imp__sub_8221DC20);
REX_HOOK_RAW(sub_8221DC20) {
  static bool demarrage_fait = false;
  const std::string chemin = LireChemin(base, ctx.r4.u32);
  if (!chemin.empty()) {
    Retenir(chemin);
    __imp__sub_8221DC20(ctx, base);
    return;
  }
  // Chemin vide : premier menu du démarrage seulement, et seulement si SAVE: n'est pas déjà monté.
  auto* ks = REX_KERNEL_STATE();
  auto* fs = ks ? ks->file_system() : nullptr;
  auto* mem = rex::system::kernel_memory();
  std::string cible;
  if (demarrage_fait || !REXCVAR_GET(torchlight_menu_fond) || !fs || !mem || fs->FindSymbolicLink("SAVE:", cible)) {
    __imp__sub_8221DC20(ctx, base);
    return;
  }
  demarrage_fait = true;
  const std::string retenu = Relire();
  const std::string nom = retenu.empty() ? std::string() : retenu.substr(6);  // après « SAVE:\ »
  const auto sauvegarde = DossierSauvegarde();
  const auto copie = DonneesUtilisateur() / "menu_fond";
  // Le jeu garde ouvert le fichier lu pour le menu : il lit une copie (dans menu_fond/), jamais la vraie
  // sauvegarde, que le jeu remplace ensuite (sur la console, un fichier ouvert ne peut pas être remplacé).
  if (nom.empty() || nom.find_first_of("\\/:") != std::string::npos || sauvegarde.empty() ||
      !Copier(sauvegarde, nom, copie)) {
    __imp__sub_8221DC20(ctx, base);
    return;
  }
  // Périphérique jamais retiré : le fichier resté ouvert y renvoie (le SDK parcourt les fichiers ouverts quand
  // le jeu ferme ses sauvegardes) ; seul le lien SAVE: est retiré après l'appel.
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMontage, copie, /*read_only=*/true);
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
    __imp__sub_8221DC20(ctx, base);
    return;
  }
  fs->RegisterSymbolicLink("SAVE:", kMontage);

  // std::wstring du jeu pour le chemin retenu (capacité >= 8 : tampon séparé), dans le tas système invité.
  const uint32_t n = uint32_t(retenu.size());
  const uint32_t objet = mem->SystemHeapAlloc(32);
  const uint32_t texte = mem->SystemHeapAlloc((n + 1) * 2);
  if (objet && texte) {
    for (uint32_t i = 0; i <= n; i++) {
      const uint16_t c = i < n ? uint16_t(retenu[i]) : 0;
      base[texte + i * 2] = uint8_t(c >> 8);
      base[texte + i * 2 + 1] = uint8_t(c);
    }
    std::memset(base + objet, 0, 32);
    Store32(base, objet, texte);
    Store32(base, objet + 16, n);
    Store32(base, objet + 20, n < 8 ? 8 : n);
    ctx.r4.u64 = objet;
    __imp__sub_8221DC20(ctx, base);
    REXLOG_INFO("[menu] fond du démarrage d'après {} : {}", retenu, ctx.r3.u32 ? "trouvé" : "absent (ville)");
  } else {
    __imp__sub_8221DC20(ctx, base);
  }
  fs->UnregisterSymbolicLink("SAVE:");
  if (texte) mem->SystemHeapFree(texte);
  if (objet) mem->SystemHeapFree(objet);
}
