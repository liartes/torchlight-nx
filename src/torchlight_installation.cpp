// Installation depuis le paquet XBLA d'origine : voir torchlight_installation.h.

#include "torchlight_installation.h"

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_container_entry.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include <imgui.h>

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if REX_PLATFORM_SWITCH
// SHA-256 de libnx (instructions de chiffrement ARMv8) ; seulement ces en-têtes : switch.h entre en conflit avec rex.
extern "C" {
#include <switch/types.h>
#include <switch/crypto/sha256.h>
}
#endif

namespace torchlight::installation {
namespace {

constexpr uint32_t kTitreTorchlight = 0x58410A7E;
constexpr uint32_t kTypeJeuArcade = 0x000D0000;
constexpr size_t kTailleTampon = 1 << 20;

struct FichierAttendu {
  const char* chemin;
  uint64_t taille;
  const char* sha256;
};

struct Version {
  const char* nom;
  std::vector<FichierAttendu> fichiers;
};

const std::vector<Version>& VersionsConnues() {
  static const std::vector<Version> versions = {
      {"full game", {
#include "torchlight_paquet_complet.inc"
                   }},
  };
  return versions;
}

// --- SHA-256 ----------------------------------------------------------------------------------------------------

#if REX_PLATFORM_SWITCH
class Sha256 {
 public:
  Sha256() { sha256ContextCreate(&contexte_); }
  void Ajouter(const void* donnees, size_t taille) { sha256ContextUpdate(&contexte_, donnees, taille); }
  std::array<uint8_t, 32> Fin() {
    std::array<uint8_t, 32> resultat;
    sha256ContextGetHash(&contexte_, resultat.data());
    return resultat;
  }

 private:
  Sha256Context contexte_;
};
#else
class Sha256 {
 public:
  void Ajouter(const void* donnees, size_t taille) {
    const auto* p = static_cast<const uint8_t*>(donnees);
    total_ += taille;
    while (taille > 0) {
      const size_t n = std::min(taille, size_t(64) - rempli_);
      std::memcpy(bloc_ + rempli_, p, n);
      rempli_ += n;
      p += n;
      taille -= n;
      if (rempli_ == 64) {
        Bloc(bloc_);
        rempli_ = 0;
      }
    }
  }

  std::array<uint8_t, 32> Fin() {
    const uint64_t bits = total_ * 8;
    const uint8_t un = 0x80;
    Ajouter(&un, 1);
    const uint8_t zero = 0;
    while (rempli_ != 56) Ajouter(&zero, 1);
    uint8_t longueur[8];
    for (int i = 0; i < 8; i++) longueur[i] = uint8_t(bits >> (56 - 8 * i));
    Ajouter(longueur, 8);
    std::array<uint8_t, 32> resultat;
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 4; j++) resultat[i * 4 + j] = uint8_t(h_[i] >> (24 - 8 * j));
    return resultat;
  }

 private:
  static uint32_t Rot(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void Bloc(const uint8_t* p) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = Rot(w[i - 15], 7) ^ Rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rot(w[i - 2], 17) ^ Rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t t1 = h + (Rot(e, 6) ^ Rot(e, 11) ^ Rot(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (Rot(a, 2) ^ Rot(a, 13) ^ Rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t bloc_[64];
  size_t rempli_ = 0;
  uint64_t total_ = 0;
};
#endif

std::string Hex(const std::array<uint8_t, 32>& octets) {
  static const char chiffres[] = "0123456789abcdef";
  std::string s;
  for (uint8_t o : octets) {
    s += chiffres[o >> 4];
    s += chiffres[o & 15];
  }
  return s;
}

// --- Fichiers (stat, opendir et FILE : std::filesystem liste mal les dossiers sur la console) ----------------------

std::optional<uint64_t> TailleFichier(const std::filesystem::path& chemin) {
  struct stat st;
  if (::stat(chemin.string().c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
  return uint64_t(st.st_size);
}

bool EstDossier(const std::filesystem::path& chemin) {
  struct stat st;
  return ::stat(chemin.string().c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool CreerDossiers(const std::filesystem::path& dossier) {
  std::filesystem::path courant;
  for (const auto& partie : dossier) {
    courant /= partie;
    ::mkdir(courant.string().c_str(), 0777);  // erreurs ignorées : déjà présent, ou « sdmc: »
  }
  return EstDossier(dossier);
}

std::string Mo(uint64_t octets) {
  char texte[32];
  std::snprintf(texte, sizeof(texte), "%.1f MB", double(octets) / (1024.0 * 1024.0));
  return texte;
}

bool EstPaquet(const std::filesystem::path& chemin) {
  const auto taille = TailleFichier(chemin);
  if (!taille || *taille < 0xD000) return false;
  FILE* f = std::fopen(chemin.string().c_str(), "rb");
  if (!f) return false;
  // Signature, puis type de contenu (big-endian en 0x344) : jeu Arcade (0x000D0000, d'où le dossier 000D0000).
  // AvatarAwards, dans les fichiers du jeu, est aussi un conteneur (PIRS, type 2).
  uint8_t entete[0x348] = {};
  const bool lu = std::fread(entete, 1, sizeof(entete), f) == sizeof(entete);
  std::fclose(f);
  if (!lu || (std::memcmp(entete, "LIVE", 4) && std::memcmp(entete, "PIRS", 4) && std::memcmp(entete, "CON ", 4)))
    return false;
  const uint32_t type = uint32_t(entete[0x344]) << 24 | uint32_t(entete[0x345]) << 16 |
                        uint32_t(entete[0x346]) << 8 | entete[0x347];
  return type == kTypeJeuArcade;
}

// Paquet dans le dossier du jeu, ou dans 58410A7E/000D0000 (arborescence du disque dur de la Xbox 360).
std::optional<std::filesystem::path> TrouverPaquet(const std::filesystem::path& racine) {
  for (const auto& dossier : {racine, racine / "58410A7E" / "000D0000", racine / "58410A7E"}) {
    DIR* d = ::opendir(dossier.string().c_str());
    if (!d) continue;
    std::vector<std::filesystem::path> candidats;
    while (const dirent* e = ::readdir(d)) {
      if (e->d_name[0] == '.') continue;
      candidats.push_back(dossier / e->d_name);
    }
    ::closedir(d);
    std::sort(candidats.begin(), candidats.end());
    for (const auto& c : candidats)
      if (EstPaquet(c)) return c;
  }
  return std::nullopt;
}

// --- Installation ----------------------------------------------------------------------------------------------

enum class Phase { kTravail, kQuestion, kErreur };

struct Etat {
  std::mutex verrou;
  Phase phase = Phase::kTravail;
  std::string etape;    // sous verrou
  std::string fichier;  // sous verrou
  std::string erreur;   // sous verrou
  std::string entete = "The installation could not be completed.";
  std::string titre = "Torchlight installation";
  std::atomic<uint64_t> octets_faits{0};
  std::atomic<uint64_t> octets_total{0};
  uint64_t taille_paquet = 0;

  void Etape(std::string texte, std::string nom = {}) {
    std::lock_guard<std::mutex> l(verrou);
    etape = std::move(texte);
    fichier = std::move(nom);
  }
  void Erreur(std::string texte) {
    REXLOG_ERROR("[installation] {}", texte);
    std::lock_guard<std::mutex> l(verrou);
    erreur = std::move(texte);
    phase = Phase::kErreur;
  }
};

class Installation {
 public:
  Installation(std::filesystem::path racine, std::filesystem::path paquet, std::shared_ptr<Etat> etat)
      : racine_(std::move(racine)), paquet_(std::move(paquet)), etat_(std::move(etat)) {}

  void Executer() {
    tampon_.resize(kTailleTampon);
    if (!Installer()) return;
    std::lock_guard<std::mutex> l(etat_->verrou);
    etat_->phase = Phase::kQuestion;
  }

 private:
  struct AExtraire {
    const FichierAttendu* attendu;
    rex::filesystem::StfsContainerEntry* entree;
  };

  bool Installer() {
    REXLOG_INFO("[installation] package {}", paquet_.string());
    etat_->taille_paquet = TailleFichier(paquet_).value_or(0);
    etat_->Etape("Reading the package...");
    // Paquet tronqué : vérifié avant StfsContainerDevice::Initialize, qui plante en lisant les tables d'empreintes
    // au-delà de la fin du fichier. Taille attendue : en-tête arrondi au bloc + contenu annoncé.
    const auto entete = rex::filesystem::StfsContainerDevice::ReadPackageHeader(paquet_);
    if (!entete) {
      etat_->Erreur("The file " + paquet_.filename().string() + " is not a readable Xbox 360 package.");
      return false;
    }
    const uint64_t attendu = ((uint64_t(entete->header.header_size) + 0xFFF) & ~uint64_t(0xFFF)) +
                             uint64_t(entete->metadata.content_size);
    if (etat_->taille_paquet < attendu) {
      etat_->Erreur("The package is incomplete (" + Mo(etat_->taille_paquet) + " of " + Mo(attendu) +
                    ": interrupted copy?). Copy it again from the Xbox 360.");
      return false;
    }
    rex::filesystem::StfsContainerDevice paquet("", paquet_);
    if (!paquet.Initialize()) {
      etat_->Erreur("The package " + paquet_.filename().string() +
                    " is unreadable or incomplete (interrupted copy?). Copy it again from the Xbox 360.");
      return false;
    }
    const uint32_t titre = paquet.header().metadata.execution_info.title_id;
    if (titre != kTitreTorchlight) {
      char texte[160];
      std::snprintf(texte, sizeof(texte),
                    "This package is not Torchlight (title %08X instead of %08X).", titre,
                    kTitreTorchlight);
      etat_->Erreur(texte);
      return false;
    }

    // Fichiers du paquet, par chemin.
    std::vector<std::pair<std::string, rex::filesystem::StfsContainerEntry*>> contenu;
    Lister(paquet.ResolvePath("/"), contenu);

    // Version : la première dont tous les fichiers sont présents avec la bonne taille.
    const Version* version = nullptr;
    std::string ecart;
    std::vector<AExtraire> a_extraire;
    for (const auto& v : VersionsConnues()) {
      a_extraire.clear();
      ecart.clear();
      for (const auto& attendu : v.fichiers) {
        auto it = std::find_if(contenu.begin(), contenu.end(),
                               [&](const auto& c) { return c.first == attendu.chemin; });
        if (it == contenu.end()) {
          ecart = std::string("file ") + attendu.chemin + " missing";
          break;
        }
        if (it->second->size() != attendu.taille) {
          ecart = std::string("size of ") + attendu.chemin + " differs";
          break;
        }
        a_extraire.push_back({&attendu, it->second});
      }
      if (ecart.empty()) {
        version = &v;
        break;
      }
    }
    if (!version) {
      etat_->Erreur("Unknown package version (" + ecart +
                    "). Only the full version of Torchlight XBLA is supported.");
      return false;
    }
    REXLOG_INFO("[installation] version: {} ({} files)", version->nom, a_extraire.size());

    // Paquet tronqué : un bloc d'un fichier au-delà de la fin du paquet. Taille lue une fois par fichier hôte
    // (un fseeko par bloc : 12 s sur la console pour ~50 000 blocs).
    std::vector<std::pair<FILE*, uint64_t>> tailles;
    auto fin_de = [&](FILE* source) {
      for (const auto& t : tailles)
        if (t.first == source) return t.second;
      ::fseeko(source, 0, SEEK_END);
      tailles.emplace_back(source, uint64_t(::ftello(source)));
      return tailles.back().second;
    };
    for (const auto& f : a_extraire) {
      for (const auto& bloc : f.entree->block_list()) {
        if (bloc.offset + bloc.length > fin_de(f.entree->files()->at(bloc.file))) {
          etat_->Erreur("The package is incomplete (only " + Mo(etat_->taille_paquet) +
                        ": interrupted copy?). Copy it again from the Xbox 360.");
          return false;
        }
      }
    }

    // Restes d'une installation interrompue, place nécessaire, place libre.
    uint64_t total = 0;
    uint64_t a_ecrire = 0;
    for (const auto& f : a_extraire) {
      const auto destination = racine_ / f.attendu->chemin;
      std::remove(Partiel(destination).string().c_str());
      total += f.attendu->taille;
      if (TailleFichier(destination) != f.attendu->taille) a_ecrire += f.attendu->taille;
    }
    etat_->octets_total = total;
    struct statvfs sv;
    if (::statvfs(racine_.string().c_str(), &sv) == 0) {
      const uint64_t libre = uint64_t(sv.f_bavail) * uint64_t(sv.f_frsize);
      const uint64_t necessaire = a_ecrire + (4u << 20);
      REXLOG_INFO("[installation] {} to write, {} free", Mo(a_ecrire), Mo(libre));
      if (libre < necessaire) {
        etat_->Erreur("Not enough free space: " + Mo(libre) + " free, " + Mo(necessaire) +
                      " needed. Free some space, then restart the game.");
        return false;
      }
    } else {
      REXLOG_WARN("[installation] free space unknown (statvfs failed)");
    }

    for (const auto& f : a_extraire)
      if (!Extraire(f)) return false;

    // Témoin écrit en dernier : sa présence signifie que tous les fichiers sont en place et vérifiés.
    const auto temoin = racine_ / kTemoin;
    if (FILE* t = std::fopen(Partiel(temoin).string().c_str(), "wb")) {
      std::fprintf(t, "Torchlight installed from package %s (%s, %zu files verified).\n",
                   paquet_.filename().string().c_str(), version->nom, a_extraire.size());
      const bool ok = std::fclose(t) == 0;
      std::remove(temoin.string().c_str());
      if (ok && std::rename(Partiel(temoin).string().c_str(), temoin.string().c_str()) == 0) {
        REXLOG_INFO("[installation] complete");
        return true;
      }
    }
    etat_->Erreur("Cannot write " + temoin.string() + ".");
    return false;
  }

  static std::filesystem::path Partiel(const std::filesystem::path& chemin) {
    return chemin.string() + ".part";
  }

  void Lister(rex::filesystem::Entry* entree,
              std::vector<std::pair<std::string, rex::filesystem::StfsContainerEntry*>>& contenu) {
    if (!entree) return;
    for (const auto& enfant : entree->children()) {
      if (enfant->attributes() & rex::filesystem::kFileAttributeDirectory) {
        Lister(enfant.get(), contenu);
      } else {
        std::string chemin = enfant->path();
        std::replace(chemin.begin(), chemin.end(), '\\', '/');
        while (!chemin.empty() && chemin.front() == '/') chemin.erase(0, 1);
        contenu.emplace_back(chemin, static_cast<rex::filesystem::StfsContainerEntry*>(enfant.get()));
      }
    }
  }

  // Fichier déjà en place avec la bonne taille (reprise) : SHA-256 relu.
  bool DejaInstalle(const AExtraire& f, const std::filesystem::path& destination) {
    if (TailleFichier(destination) != f.attendu->taille) return false;
    etat_->Etape("Verifying files already installed", f.attendu->chemin);
    FILE* in = std::fopen(destination.string().c_str(), "rb");
    if (!in) return false;
    Sha256 sha;
    size_t n;
    while ((n = std::fread(tampon_.data(), 1, tampon_.size(), in)) > 0) {
      sha.Ajouter(tampon_.data(), n);
      etat_->octets_faits += n;
    }
    std::fclose(in);
    if (Hex(sha.Fin()) == f.attendu->sha256) return true;
    REXLOG_WARN("[installation] {} already present but different: rewritten", f.attendu->chemin);
    return false;
  }

  bool Extraire(const AExtraire& f) {
    const auto destination = racine_ / f.attendu->chemin;
    const uint64_t depart = etat_->octets_faits;
    if (DejaInstalle(f, destination)) return true;
    etat_->octets_faits = depart;
    etat_->Etape("Installing game files", f.attendu->chemin);
    if (!CreerDossiers(destination.parent_path())) {
      etat_->Erreur("Cannot create folder " + destination.parent_path().string() + ".");
      return false;
    }
    const auto partiel = Partiel(destination);
    FILE* out = std::fopen(partiel.string().c_str(), "wb");
    if (!out) {
      etat_->Erreur("Cannot create " + partiel.string() + ".");
      return false;
    }

    // Blocs du fichier dans le paquet, contigus regroupés (les tables d'empreintes de STFS les coupent).
    Sha256 sha;
    bool ok = true;
    uint64_t ecrit = 0;
    const auto& blocs = f.entree->block_list();
    for (size_t i = 0; ok && i < blocs.size();) {
      FILE* source = f.entree->files()->at(blocs[i].file);
      const uint64_t debut = blocs[i].offset;
      uint64_t longueur = blocs[i].length;
      for (++i; i < blocs.size() && blocs[i].file == blocs[i - 1].file &&
                blocs[i].offset == debut + longueur;
           ++i)
        longueur += blocs[i].length;
      longueur = std::min<uint64_t>(longueur, f.attendu->taille - ecrit);
      ok = ::fseeko(source, off_t(debut), SEEK_SET) == 0;
      while (ok && longueur > 0) {
        const size_t n = size_t(std::min<uint64_t>(longueur, tampon_.size()));
        ok = std::fread(tampon_.data(), 1, n, source) == n && std::fwrite(tampon_.data(), 1, n, out) == n;
        sha.Ajouter(tampon_.data(), n);
        longueur -= n;
        ecrit += n;
        etat_->octets_faits += n;
      }
      if (ecrit == f.attendu->taille) break;
    }
    ok = (std::fclose(out) == 0) && ok;
    if (!ok || ecrit != f.attendu->taille) {
      std::remove(partiel.string().c_str());
      etat_->Erreur("Error writing " + destination.string() +
                    " (storage full or removed?). Restart the game to resume the installation.");
      return false;
    }
    if (Hex(sha.Fin()) != f.attendu->sha256) {
      std::remove(partiel.string().c_str());
      etat_->Erreur(std::string("The file ") + f.attendu->chemin +
                    " in the package is damaged (checksum mismatch). Copy the package again from the Xbox 360.");
      return false;
    }
    std::remove(destination.string().c_str());  // un fichier existant ne peut pas être remplacé par rename
    if (std::rename(partiel.string().c_str(), destination.string().c_str()) != 0) {
      etat_->Erreur("Cannot rename " + partiel.string() + ".");
      return false;
    }
    return true;
  }

  std::filesystem::path racine_;
  std::filesystem::path paquet_;
  std::shared_ptr<Etat> etat_;
  std::vector<uint8_t> tampon_;
};

// --- Fenêtre ---------------------------------------------------------------------------------------------------

class Fenetre final : public rex::ui::ImGuiDialog {
 public:
  Fenetre(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& contexte, std::shared_ptr<Etat> etat,
          std::filesystem::path paquet, std::function<void()> continuer, std::function<void()> quitter)
      : ImGuiDialog(drawer),
        contexte_(contexte),
        etat_(std::move(etat)),
        paquet_(std::move(paquet)),
        continuer_(std::move(continuer)),
        quitter_(std::move(quitter)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    Phase phase;
    std::string etape, fichier, erreur;
    {
      std::lock_guard<std::mutex> l(etat_->verrou);
      phase = etat_->phase;
      etape = etat_->etape;
      fichier = etat_->fichier;
      erreur = etat_->erreur;
    }
    const float echelle = std::max(1.0f, std::floor(io.DisplaySize.y / 360.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.75f, 0.0f), ImGuiCond_Always);
    // Un identifiant par phase : la fenêtre « apparaît » à chaque changement, ce qui place le focus de la manette
    // sur le bouton par défaut.
    const std::string identifiant = etat_->titre + (phase == Phase::kTravail    ? "###travail"
                                                    : phase == Phase::kQuestion ? "###question"
                                                                                : "###erreur");
    if (phase != phase_affichee_) {
      ImGui::SetNextWindowFocus();
      phase_affichee_ = phase;
    }
    ImGui::Begin(identifiant.c_str(), nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(echelle);
    switch (phase) {
      case Phase::kTravail: {
        const uint64_t faits = etat_->octets_faits, total = etat_->octets_total;
        ImGui::TextWrapped("First launch: installing the game files from the "
                           "Xbox Live Arcade package (%s).",
                           paquet_.filename().string().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(etape.c_str());
        const std::string texte = Mo(faits) + " / " + Mo(total);
        ImGui::ProgressBar(total ? float(double(faits) / double(total)) : 0.0f, ImVec2(-1.0f, 0.0f),
                           texte.c_str());
        ImGui::TextUnformatted(fichier.empty() ? " " : fichier.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Do not remove the SD card or quit the game.");
        break;
      }
      case Phase::kQuestion: {
        ImGui::TextWrapped("Installation complete: all files have been verified.");
        ImGui::Spacing();
        ImGui::TextWrapped("The original package is no longer needed. Deleting it frees %s.",
                           Mo(etat_->taille_paquet).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Delete the package")) Terminer(true);
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Keep the package")) Terminer(false);
        break;
      }
      case Phase::kErreur: {
        ImGui::TextWrapped("%s", etat_->entete.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", erreur.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Quit")) {
          ImGui::End();
          Close();
          contexte_.CallInUIThreadDeferred(quitter_);
          return;
        }
        ImGui::SetItemDefaultFocus();
        break;
      }
    }
    ImGui::End();
  }

 private:
  void Terminer(bool supprimer) {
    if (supprimer) {
      if (std::remove(paquet_.string().c_str()) == 0) {
        REXLOG_INFO("[installation] package deleted: {}", paquet_.string());
        // Dossiers 000D0000 puis 58410A7E s'ils sont vides (rmdir échoue sinon).
        const auto parent = paquet_.parent_path();
        if (parent.filename() == "000D0000") {
          ::rmdir(parent.string().c_str());
          if (parent.parent_path().filename() == "58410A7E") ::rmdir(parent.parent_path().string().c_str());
        } else if (parent.filename() == "58410A7E") {
          ::rmdir(parent.string().c_str());
        }
      } else {
        REXLOG_WARN("[installation] cannot delete the package: {}", paquet_.string());
      }
    }
    Close();
    contexte_.CallInUIThreadDeferred(continuer_);
  }

  rex::ui::WindowedAppContext& contexte_;
  std::shared_ptr<Etat> etat_;
  std::filesystem::path paquet_;
  std::function<void()> continuer_;
  std::function<void()> quitter_;
  std::optional<Phase> phase_affichee_;
};

}  // namespace

void MessageBloquant(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& contexte, std::string titre,
                     std::string entete, std::string message, std::function<void()> quitter) {
  auto etat = std::make_shared<Etat>();
  etat->titre = std::move(titre);
  etat->entete = std::move(entete);
  etat->Erreur(std::move(message));
  new Fenetre(drawer, contexte, etat, std::filesystem::path(), [] {}, std::move(quitter));
}

bool DossierDuJeu(const std::filesystem::path& dossier) {
  return TailleFichier(dossier / "default.xex").has_value() || TrouverPaquet(dossier).has_value();
}

bool Demarrer(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& contexte,
              const std::filesystem::path& racine, std::function<void()> continuer,
              std::function<void()> quitter) {
  if (racine.empty() || !drawer) return false;
  const bool xex = TailleFichier(racine / "default.xex").has_value();
  if (xex && TailleFichier(racine / kTemoin)) return false;  // déjà installé depuis un paquet
  const auto paquet = TrouverPaquet(racine);
  if (!paquet && xex) return false;  // fichiers copiés à la main : démarrage comme avant

  auto etat = std::make_shared<Etat>();
  if (paquet) {
    // pthread avec une pile explicite, comme les threads du rendu natif : std::thread lève std::system_error sur la
    // console (même depuis le thread de l'interface).
    auto* installation = new Installation(racine, *paquet, etat);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_t thread;
    const int erreur = pthread_create(
        &thread, &attr,
        [](void* p) -> void* {
          std::unique_ptr<Installation> i(static_cast<Installation*>(p));
          i->Executer();
          return nullptr;
        },
        installation);
    pthread_attr_destroy(&attr);
    if (erreur == 0) {
      pthread_detach(thread);
    } else {
      delete installation;
      etat->Erreur("Cannot start the installation thread (error " + std::to_string(erreur) + ").");
    }
  } else {
    etat->Erreur("Game files not found. Copy your Torchlight Xbox Live Arcade package (file "
                 "35F62CE3FE43845B1625A000921D5A6C48CCC7C058, from the Xbox 360 folder Content/0000000000000000/"
                 "58410A7E/000D0000) to " +
                 racine.string() + ", then restart the game.");
  }
  new Fenetre(drawer, contexte, etat, paquet.value_or(std::filesystem::path()), std::move(continuer),
              std::move(quitter));
  return true;
}

}  // namespace torchlight::installation
