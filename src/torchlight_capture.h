// torchlight - capture d'une image complète du rendu d'Ogre (phase 2 du rendu natif).
//
// Appelé par les interceptions de torchlight_trace.cpp. Avec --torchlight_capture_s=T, la première
// image qui commence après T secondes est enregistrée dans <exe>/trace/capture/ :
//   frame.txt        un bloc par dessin (programmes, états, déclaration, tampons, textures, constantes)
//   vb_*.bin ib_*.bin  sommets et indices tels qu'en mémoire du jeu (big-endian)
//   tex_*.bin        niveau de base des textures, détilé et dans l'ordre des octets de l'hôte
//   cst_*.txt        constantes flottantes et table des noms d'Ogre
// Le format est décrit en tête de frame.txt ; switch/tools/rendu_hors_ligne le relit.

#pragma once

#include <cstdint>
#include <filesystem>

#include <functional>

namespace torchlight::capture {

// Capture d'écran de l'image émulée, prise un peu après l'image enregistrée (pour comparer au rendu natif).
// Fournie par l'application (accès au presenter) ; appelée hors du thread du jeu.
void SetScreenshot(std::function<void(const std::filesystem::path&)> screenshot);

bool Enabled();
// Vrai pendant l'image en cours d'enregistrement (le rendu natif y joint son propre relevé).
bool Recording();
void Start(const std::filesystem::path& dir);

// Une image du jeu = un appel à _updateAllRenderTargets : plusieurs cibles, chacune entre _beginFrame et
// _endFrame (textures rendues par le jeu, puis la fenêtre). La capture couvre tout cet appel.
void BeginGameFrame();
void EndGameFrame();
void BeginFrame();
void EndFrame();
void SetRenderTarget(uint32_t target, const uint8_t* base);
void SetViewport(uint32_t viewport, const uint8_t* base);
void Clear(uint32_t buffers, const float colour[4], double depth, uint32_t stencil);

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
void SetScissor(bool enabled, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);

// vertex = true pour un programme de sommets. hash = empreinte de la source HLSL (0 si inconnue).
void BindProgram(bool vertex, uint32_t program, uint64_t hash);
// unbindGpuProgram(type) : 0 = sommets, 1 = pixels. Le dessin suivant passe par le pipeline fixe émulé.
void UnbindProgram(uint32_t type);
// Pipeline fixe (dessins sans programme, émulé par le portage Xbox d'Ogre) : matrices Ogre (0 = monde,
// 1 = vue, 2 = projection ; 16 flottants, Matrix4 d'Ogre par lignes), modes de combinaison des textures
// (LayerBlendModeEx : 15 mots), unités désactivées.
void SetMatrix(int kind, const float m[16]);
void SetTextureBlendMode(uint32_t unit, const uint32_t words[15]);
void DisableTextureUnit(uint32_t unit);
void DisableTextureUnitsFrom(uint32_t unit);
// type Ogre : 0 = sommets, 1 = pixels.
void BindParameters(uint32_t type, uint32_t params, const uint8_t* base);
void Render(uint32_t op, const uint8_t* base);

}  // namespace torchlight::capture
