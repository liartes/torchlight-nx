// Installation depuis le paquet XBLA d'origine (conteneur STFS « LIVE »).
//
// Le joueur copie seulement son paquet (fichier 35F62CE3FE43845B1625A000921D5A6C48CCC7C058, ou le dossier
// 58410A7E/000D0000 qui le contient) dans le dossier du jeu (sur la console, à côté du .nro). Au premier démarrage,
// avant la construction du runtime (ReXApp::OnFinalizePaths), une fenêtre ImGui montre l'installation, faite dans un
// thread à part :
//   - contrôles : titre du paquet, liste et tailles des fichiers (versions connues), paquet tronqué, place libre ;
//   - extraction de chaque fichier en <nom>.part avec son SHA-256, renommé seulement si l'empreinte est la bonne
//     (une installation interrompue reprend : les fichiers déjà en place sont revérifiés) ;
//   - fichier témoin écrit en dernier (kTemoin), puis proposition de supprimer le paquet (place sur la carte).
// Sans paquet ni default.xex : message expliquant où copier le paquet. Une copie manuelle des fichiers (default.xex
// présent, sans paquet) démarre comme avant, sans contrôle.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace torchlight::installation {

inline constexpr const char* kTemoin = "installation_paquet.txt";

// Vrai si le dossier contient les fichiers du jeu (default.xex) ou un paquet à installer.
bool DossierDuJeu(const std::filesystem::path& dossier);

// Vrai si la fenêtre d'installation a été ouverte : continuer() sera appelé sur le thread de l'interface une fois
// l'installation terminée (ou déjà faite), quitter() si le joueur abandonne après une erreur. Faux : rien à faire,
// le jeu peut démarrer tout de suite.
bool Demarrer(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& contexte,
              const std::filesystem::path& racine, std::function<void()> continuer,
              std::function<void()> quitter);

// Fenêtre de message (même présentation que l'installation) avec un seul bouton « Quit », qui appelle quitter() sur
// le thread de l'interface. Pour les erreurs qui empêchent le jeu de démarrer (lancement en mode applet...).
void MessageBloquant(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& contexte, std::string titre,
                     std::string entete, std::string message, std::function<void()> quitter);

}  // namespace torchlight::installation
