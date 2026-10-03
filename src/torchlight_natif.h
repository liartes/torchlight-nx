// torchlight - rendu natif dans le jeu (phase 3, switch/RENDU_NATIF.md).
//
// Avec --torchlight_natif=true, chaque image du jeu (_updateAllRenderTargets) est redessinée par
// src/natif/rendu.cpp à partir des appels du D3D9RenderSystem d'Ogre (interceptés par
// torchlight_trace.cpp), et c'est elle qui est présentée à la place de l'image de l'émulation.
// L'émulation du GPU continue de tourner en dessous (étape 3b) : le jeu reste synchronisé.
// Bibliothèque de shaders : --torchlight_natif_shaders (par défaut <dossier de l'exécutable>/shaders,
// produite par switch/shaders/build_lib.sh).

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rex::ui {
class GraphicsProvider;
class Presenter;
}  // namespace rex::ui

namespace torchlight::natif {

bool Enabled();

// Vrai si le dessin en cours a été pris en charge par le rendu natif et ne doit pas être transmis au
// Direct3D du jeu (--torchlight_natif_seul).
bool DrawsHandled();

// Lock d'un tampon de sommets ou d'indices du Direct3D du jeu (adresse GPU, mot en +0x18 de l'objet) : ses copies
// gardées par le relevé ne sont plus à jour.
void BufferLocked(uint32_t gpu_address);

// Envoi du tampon de commandes au GPU avant d'attendre une fence non atteinte (correctif du blocage au démarrage).
bool KickBeforeWait();

// Rendu natif seul : verrouillage des tampons de sommets et d'indices sans attendre le GPU émulé.
bool LockWithoutWait();

// Rendu natif seul : le compilateur HLSL de la Xbox (D3DXCompileShader) peut recevoir une source allégée (son
// microcode ne sert plus, seule sa table de constantes compte pour Ogre).
bool StubXboxShaders();

// Fourni par l'application (OnPostSetup) : périphérique Vulkan et presenter du SDK, fichiers du jeu (pour
// rtshaderlib) et données utilisateur (cache des shaders compilés : <données>/cache/natif).
void SetPresentation(rex::ui::GraphicsProvider* provider, rex::ui::Presenter* presenter);
void SetPaths(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root);

// Source HLSL compilée par le jeu (D3DXCompileShader) : compilée en SPIR-V dans le cache si besoin.
// hash : empreinte de la source (traceur), profile : vs_3_0 / ps_3_x, d3dx_flags : options de compilation.
void ShaderSource(uint64_t hash, const std::string& profile, const std::string& entry, const std::string& source,
                  uint32_t d3dx_flags);

// Tâche exécutée par le thread de compilation, après celles déjà en file (tout de suite s'il n'existe pas) :
// écritures dans un cache, hors du thread du jeu.
void RunInBackground(std::function<void()> task);

// Mêmes événements que torchlight::capture (voir torchlight_capture.h).
void BeginGameFrame();
void EndGameFrame();
void BeginFrame();
void EndFrame();
void SetRenderTarget(uint32_t target, const uint8_t* base);
void SetViewport(uint32_t viewport, const uint8_t* base);
void Clear(uint32_t buffers, const float colour[4], double depth);

void SetTexture(uint32_t unit, bool enabled, uint32_t texture);
void SetSceneBlending(uint32_t src, uint32_t dst, uint32_t op);
void SetSeparateSceneBlending(uint32_t src, uint32_t dst, uint32_t src_a, uint32_t dst_a, uint32_t op,
                              uint32_t op_a);
void SetCullingMode(uint32_t mode);
void SetDepthCheck(bool enabled);
void SetDepthWrite(bool enabled);
void SetDepthFunction(uint32_t func);
void SetColourWrite(bool r, bool g, bool b, bool a);
void SetAlphaReject(uint32_t func, uint32_t value);
// _setTextureAddressingMode (TextureAddressingMode d'Ogre en u, v, w) et _setTextureBorderColour.
void SetTextureAddressing(uint32_t unit, uint32_t u, uint32_t v, uint32_t w);
void SetTextureBorder(uint32_t unit, const float rgba[4]);
// _setDepthBias, déjà converti en valeurs Direct3D (constante absolue, pente).
void SetDepthBias(float constant, float slope);

void BindProgram(bool vertex, uint64_t hash);
void UnbindProgram(uint32_t type);
void SetMatrix(int kind, const float m[16]);
void SetTextureBlendMode(uint32_t unit, const uint32_t words[15]);
void DisableTextureUnit(uint32_t unit);
void DisableTextureUnitsFrom(uint32_t unit);
void BindParameters(uint32_t type, uint32_t params, const uint8_t* base);
void Render(uint32_t op, const uint8_t* base);

}  // namespace torchlight::natif
