// torchlight - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <rex/logging.h>

#include "torchlight_installation.h"

// Rendu natif (TORCHLIGHT_NATIF) et outils de développement (TORCHLIGHT_OUTILS : manette de script, capture d'écran
// de l'émulation) : compilés seulement par switch/CMakeLists.txt ; les outils sont absents de la build des joueurs
// (option TORCHLIGHT_OUTILS=OFF).
#if defined(TORCHLIGHT_NATIF)
#include "torchlight_natif.h"
#endif
#if defined(TORCHLIGHT_OUTILS)
#include <rex/ui/presenter.h>

#include <cstdio>

#include "torchlight_capture.h"
#include "torchlight_manette.h"
#endif

#if REX_PLATFORM_SWITCH
#include <rex/logging.h>

#include <cstdlib>
#include <string>

// Sortie de la console demandée au système (comme ReXApp::OnClosing du SDK) : symboles internes de libnx, déclarés
// sans switch.h (ses types entrent en conflit avec ceux de rex).
extern "C" {
#include <switch/types.h>
#include <switch/result.h>
extern u32 __nx_applet_exit_mode;
NX_NORETURN void __libnx_exit(int rc);
// AppletType de libnx (switch/services/applet.h, enum de la taille d'un int) : AppletType_Application = 0.
int appletGetAppletType(void);
// Langue du système (switch/services/set.h ; SetLanguage est une enum de la taille d'un int).
Result setInitialize(void);
void setExit(void);
Result setGetSystemLanguage(u64* LanguageCode);
Result setMakeLanguage(u64 LanguageCode, int* Language);
}

// Variables d'environnement du pilote Mesa/NVK, posées avant la création de Vulkan.
// Format "NOM=valeur;NOM2=valeur2", par exemple "NVK_SWITCH_ZCULL=0".
REXCVAR_DEFINE_STRING(mesa_env, "", "GPU",
                      "Variables d'environnement Mesa/NVK (NOM=valeur;...) avant Vulkan");
#endif

class TorchlightApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // Le jeu a terminé son titre (« Return to Game Library » : XamLoaderLaunchTitle(NULL) ou XamLoaderTerminateTitle,
  // puis KernelState::TerminateTitle) : son thread principal vient de finir. Le SDK enchaînerait sur la destruction
  // du runtime (OnDestroy -> Runtime::Shutdown), qui plantait (MnkInputDriver::~MnkInputDriver appelle le thread de
  // l'interface déjà arrêté : std::system_error) avec des threads du jeu encore vivants. On sort comme à la fermeture
  // de la fenêtre (ReXApp::OnClosing) : journal vidé, puis sur la console __libnx_exit en mode applet (le système
  // ferme proprement et revient au menu ; std::_Exit ou svcExitProcess sont signalés comme une erreur), sur PC
  // std::_Exit. Posté au thread de l'interface, avant la demande de fin du SDK qui suit cet appel.
  void OnGuestThreadExit(rex::system::XThread* thread) override {
    (void)thread;
    REXLOG_INFO("[torchlight] fin du titre demandée par le jeu : sortie directe (sans destruction du runtime)");
    app_context().CallInUIThread([] { SortirDirectement(); });
  }

  // Installation depuis le paquet XBLA (torchlight_installation.h) avant la construction du runtime : fenêtre ImGui,
  // le runtime est construit ensuite par resume.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
#if REX_PLATFORM_SWITCH
    // Lancé depuis l'album (mode applet) : ~400 Mo de mémoire, le jeu ne peut pas démarrer (le SDK ne fait qu'écrire
    // un avertissement dans le journal). Message à l'écran à la place.
    if (appletGetAppletType() != 0) {
      REXLOG_ERROR("[torchlight] lancé en mode applet (type {}) : mémoire insuffisante", appletGetAppletType());
      torchlight::installation::MessageBloquant(
          imgui_drawer(), app_context(), "Torchlight", "Torchlight cannot run in applet mode.",
          "It was started from the Album, which only gives it a fraction of the console's memory. "
          "Start it with full memory instead: from a forwarder on the Home menu (recommended, see README.md), "
          "or hold R while launching any installed game (title override).",
          [] { SortirDirectement(); });
      return std::nullopt;
    }
#endif
    if (torchlight::installation::Demarrer(
            imgui_drawer(), app_context(), defaults.game_data_root,
            [resume, defaults] { resume(defaults); }, [] { SortirDirectement(); })) {
      return std::nullopt;
    }
    return defaults;
  }

  // Sortie comme à la fermeture de la fenêtre (ReXApp::OnClosing) : journal vidé, puis sur la console __libnx_exit
  // en mode applet, sur PC std::_Exit. Sur le thread de l'interface.
  [[noreturn]] static void SortirDirectement() {
    rex::FlushLogging();
    std::fflush(nullptr);
#if REX_PLATFORM_SWITCH
    __nx_applet_exit_mode = 1;
    __libnx_exit(0);
#else
    std::_Exit(0);
#endif
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TorchlightApp>(new TorchlightApp(ctx, "torchlight",
        PPCImageConfig));
  }

#if REX_PLATFORM_SWITCH
  // Pas de ligne de commande sur la console : les fichiers du jeu (contenu de
  // assets/, default.xex compris) ou le paquet XBLA à installer (torchlight_installation.h)
  // sont à côté du .nro, normalement dans sdmc:/switch/torchlight. Si le .nro est ailleurs
  // sans eux, on garde ce dossier. Doit être fait ici, car torchlight.toml n'est lu
  // qu'après OnConfigurePaths.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (!exe_dir.empty() && torchlight::installation::DossierDuJeu(exe_dir)) {
      paths.game_data_root = exe_dir;
    } else {
      paths.game_data_root = "sdmc:/switch/torchlight";
    }
  }

  // Réglages de la console, sauf si torchlight.toml (facultatif, à côté du .nro) les fixe déjà : équivalents de
  // --gpu_plugin=xenos --license_mask=1 du lancement PC, puis les valeurs de switch/torchlight.toml (expliquées
  // dans ce fichier), pour que le joueur n'ait à copier que le .nro et son paquet.
  void OnPostInitLogging() override {
    SetDefault("gpu_plugin", "xenos");
    SetDefault("license_mask", "1");
    SetDefault("torchlight_natif", "true");
    SetDefault("torchlight_natif_seul", "true");
    SetDefault("mesa_env", "NVK_SWITCH_ZCULL=0");
    SetDefault("nfsmw_io_ventana_kb", "0");
    SetDefault("nfsmw_io_cache_mb", "64");
    SetDefault("nfsmw_io_cache_bloque_kb", "64");
    SetDefault("nfsmw_io_rangos_mb", "0");
    // Langue du jeu (XGetLanguage du SDK lit user_language) : celle de la console, si le jeu la propose, sauf si
    // torchlight.toml la fixe, même à 1 (anglais, la valeur par défaut : d'où le test de la source, pas de la valeur).
    if (const auto* info = rex::cvar::GetFlagInfo("user_language");
        info && info->source == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("user_language", LangueDuSysteme());
    }
    // [log.levels] krnl = "error" : des milliers d'avertissements NtCreateFile bénins, chacun écrit sur la SD.
    const auto levels =
        rex::ParseCategoryLevelsFromConfig(rex::filesystem::GetExecutableFolder() / "torchlight.toml");
    if (!levels.contains("krnl")) rex::SetCategoryLevel(rex::log::krnl(), spdlog::level::err);
    ApplyMesaEnv(REXCVAR_GET(mesa_env));
  }

 private:
  // Langue de la console -> XLanguage du 360 (1 anglais, 3 allemand, 4 français, 5 espagnol). Le jeu n'a que ces
  // quatre langues (assets/translations/{de,fr,es}) : toute autre langue donne l'anglais.
  static const char* LangueDuSysteme() {
    const char* langue = "1";
    int code = -1;
    if (R_SUCCEEDED(setInitialize())) {
      u64 lc = 0;
      if (R_SUCCEEDED(setGetSystemLanguage(&lc)) && R_SUCCEEDED(setMakeLanguage(lc, &code))) {
        switch (code) {
          case 2:   // SetLanguage_FR
          case 13:  // SetLanguage_FRCA
            langue = "4";
            break;
          case 3:  // SetLanguage_DE
            langue = "3";
            break;
          case 5:   // SetLanguage_ES
          case 14:  // SetLanguage_ES419
            langue = "5";
            break;
          default:
            break;
        }
      }
      setExit();
    }
    REXLOG_INFO("[torchlight] langue de la console {} -> langue du jeu {}", code, langue);
    return langue;
  }

  static void SetDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) != nullptr &&
        !rex::cvar::HasNonDefaultValue(name)) {
      rex::cvar::SetFlagByName(name, value);
    }
  }

  // Vulkan est créé plus tard (SetupPresentation) : le pilote lit ces variables
  // avec getenv à ce moment-là.
  static void ApplyMesaEnv(const std::string& list) {
    size_t start = 0;
    while (start < list.size()) {
      size_t end = list.find(';', start);
      if (end == std::string::npos) end = list.size();
      const std::string item = list.substr(start, end - start);
      const size_t eq = item.find('=');
      if (eq != std::string::npos && eq > 0) {
        const std::string name = item.substr(0, eq);
        const std::string value = item.substr(eq + 1);
        setenv(name.c_str(), value.c_str(), 1);
        REXLOG_INFO("mesa_env: {}={}", name, value);
      }
      start = end + 1;
    }
  }

 public:
#endif

#if defined(TORCHLIGHT_OUTILS)
  void OnPreSetup(rex::RuntimeConfig& config) override { torchlight::manette::WrapInput(config); }
#endif

#if defined(TORCHLIGHT_NATIF)
  // Rendu natif (torchlight_natif) : périphérique Vulkan et presenter du SDK.
  // Outils : capture d'écran de l'émulation pour la comparer au rendu natif (torchlight_capture_s), en PPM.
  void OnPostSetup() override {
    if (const auto* gs = runtime() ? runtime()->graphics_system() : nullptr)
      torchlight::natif::SetPresentation(gs->provider(), gs->presenter());
    if (const auto* rt = runtime()) torchlight::natif::SetPaths(rt->game_data_root(), rt->user_data_root());
#if defined(TORCHLIGHT_OUTILS)
    torchlight::capture::SetScreenshot([this](const std::filesystem::path& path) {
      const auto* rt = runtime();
      const auto* gs = rt ? rt->graphics_system() : nullptr;
      rex::ui::Presenter* presenter = gs ? gs->presenter() : nullptr;
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) return;
      if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
        for (uint32_t y = 0; y < image.height; y++)
          for (uint32_t x = 0; x < image.width; x++) std::fwrite(&image.data[y * image.stride + x * 4], 1, 3, f);
        std::fclose(f);
      }
    });
#endif
  }
#endif

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
