// torchlight - rendu natif (phase 3, switch/RENDU_NATIF.md) : redessine avec Vulkan ce que le
// D3D9RenderSystem d'Ogre reçoit, dessin par dessin.
//
// Partagé par le jeu (src/torchlight_natif.cpp, périphérique Vulkan du SDK) et par l'outil hors ligne
// (switch/tools/rendu_hors_ligne, qui relit une capture). Ne dépend pas du SDK : les fonctions Vulkan
// sont chargées par vkGetInstanceProcAddr.
//
// Conventions (identiques à switch/shaders/build_lib.sh) : shader de sommets set 0, shader de pixels
// set 1, $Global binding 0, échantillonneur de l'unité N binding 16 + N ; emplacements des attributs
// d'après la sémantique (prep_hlsl.py). Constantes d'Ogre = registres Direct3D 9 (4 flottants).

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace torchlight::natif {

// --- Description d'un dessin (valeurs d'Ogre, données en mémoire du jeu) ---------------------------

struct VertexElement {
  uint32_t source, offset, type, semantic, index;  // VertexElement d'Ogre
};

// Tampon de sommets tel qu'en mémoire du jeu (big-endian). Seule la plage du dessin est lue et convertie,
// à chaque dessin : le jeu réécrit certains tampons au même endroit au cours d'une image (interface CEGUI).
// id : identité du tampon (adresse), pour les relevés.
struct VertexStream {
  uint32_t source = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t stride = 0;
  uint64_t id = 0;
};

struct IndexStream {
  const uint8_t* data = nullptr;  // big-endian
  size_t size = 0;
  uint32_t index_size = 2;
  uint64_t id = 0;
};

// Texture : key identifie le contenu (adresse, format, taille, empreinte des données). load n'est appelé
// que si key est absente du cache : données détilées, dans l'ordre des octets de l'hôte (faces à la suite
// pour une cubemap).
struct TextureRef {
  uint32_t unit = 0;
  uint64_t key = 0;
  uint32_t format = 0, width = 0, height = 0;  // format Xenos
  bool cube = false;
  std::string name;  // nom Ogre (cibles de rendu : « rtt/<adresse>/<nom> »)
  // Adressage d'Ogre (TextureAddressingMode : 0 répéter, 1 miroir, 2 bloquer, 3 bordure) en u, v, w ; -1 = inconnu
  // (textures ordinaires répétées, textures rendues bloquées). Bordure : 0 noir transparent, 1 noir, 2 blanc.
  int8_t address[3] = {-1, -1, -1};
  uint8_t border = 0;
  // Jeu : contenu brut à prendre en empreinte (thread de rendu) pour calculer key avant Draw, si key est nul.
  uint64_t ident = 0;
  const uint8_t* content = nullptr;
  size_t content_size = 0;
  // Partagé (pas recopié à chaque dessin) : une fonction par texture.
  std::shared_ptr<const std::function<bool(std::vector<uint8_t>&)>> load;
};

// Constantes d'Ogre : GpuProgramParameters (mFloatConstants) et table des noms.
struct OgreConst {
  uint32_t type, phys, elem_size, array_size;
};
using ConstantNames = std::unordered_map<std::string, OgreConst>;
struct Constants {
  std::vector<float> floats;
  ConstantNames names;
  // Table des noms partagée (jeu : relue une fois par programme, commune à toutes les images) ; prioritaire.
  std::shared_ptr<const ConstantNames> shared_names;
  // Jeu : même table par pointeur brut (pas d'incrément atomique par dessin ; libération différée par le relevé).
  const ConstantNames* names_ptr = nullptr;
  // Jeu : valeurs brutes (flottants gros-boutistes) copiées par le thread du jeu, converties dans floats par le
  // thread de rendu avant Draw.
  const uint8_t* raw_be = nullptr;
  size_t raw_count = 0;
  const ConstantNames& Names() const { return names_ptr ? *names_ptr : shared_names ? *shared_names : names; }
};

// Étage de combinaison du pipeline fixe (LayerBlendModeEx).
struct FfpStage {
  bool set = false;
  uint32_t op = 2, src1 = 1, src2 = 2;
  float arg[11] = {};  // colourArg1 rgba, colourArg2 rgba, alphaArg1, alphaArg2, factor
};

struct DrawDesc {
  uint64_t vs = 0, ps = 0;  // empreintes des sources HLSL ; ffp = dessin sans programme
  bool ffp = false;
  uint32_t type = 4;  // OperationType d'Ogre
  bool indexed = false;
  uint32_t vstart = 0, vcount = 0, istart = 0, icount = 0;
  std::vector<VertexElement> elems;
  std::vector<VertexStream> streams;
  IndexStream index;
  std::vector<TextureRef> textures;
  std::vector<TextureRef> reused_textures;  // relevé : éléments de l'image précédente, réutilisés (Clear)
  uint32_t blend[6] = {0, 1, 0, 0, 1, 0};  // src, dst, op, srcA, dstA, opA (SceneBlendFactor d'Ogre)
  uint32_t cull = 2;
  bool depth_check = true, depth_write = true;
  uint32_t depth_func = 3;
  bool mask[4] = {true, true, true, true};
  uint32_t alpha_func = 1, alpha_value = 0;
  // Décalage de profondeur Direct3D (_setDepthBias converti comme le portage Xbox d'Ogre) : constante en unités de
  // profondeur absolues, pente sans unité.
  float depth_bias = 0.0f, depth_slope_bias = 0.0f;
  const Constants* consts[2] = {};  // sommets, pixels
  // Pipeline fixe : matrices Ogre (monde, vue, projection ; par lignes) et étages [unité][couleur, alpha].
  float matrix[3][16] = {};
  FfpStage stage[4][2];

  void Reset() { *this = DrawDesc(); }
  // Comme Reset, en gardant la capacité des tableaux (descriptions réutilisées d'une image à l'autre). matrix et
  // stage ne sont pas remis à zéro : à remplir par l'appelant pour un dessin du pipeline fixe.
  void Clear() {
    vs = ps = 0;
    ffp = false;
    type = 4;
    indexed = false;
    vstart = vcount = istart = icount = 0;
    elems.clear();
    streams.clear();
    index = IndexStream();
    reused_textures = std::move(textures);  // gardées pour le prochain relevé (noms déjà alloués)
    textures.clear();
    consts[0] = consts[1] = nullptr;
  }
};

// --- Vulkan -----------------------------------------------------------------------------------------

struct Context {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  // Soumission sur la file (le SDK impose son verrou). Sans elle, vkQueueSubmit sur la file 0.
  std::function<VkResult(const VkSubmitInfo&, VkFence)> submit;
  std::function<void(const std::string&)> log;
  // Cache de pipelines du pilote, relu au démarrage et réécrit par SavePipelineCache (vide = en mémoire seulement).
  std::filesystem::path pipeline_cache_file;
  // Incrémenté quand des shaders sont ajoutés à la bibliothèque pendant le jeu (compilation en arrière-plan) :
  // les shaders absents sont alors recherchés à nouveau. Nul = bibliothèque fixe.
  const std::atomic<uint32_t>* shader_generation = nullptr;
};

// Pour l'outil hors ligne : instance et périphérique propres (prefer : sous-chaîne du nom du GPU).
bool CreateStandaloneContext(PFN_vkGetInstanceProcAddr gipa, const char* prefer, Context& out);

struct Stats {
  uint32_t drawn = 0;
  std::map<std::string, uint32_t> skipped;           // raison -> nombre de dessins
  std::map<std::string, uint32_t> missing_consts;    // constante du shader sans valeur d'Ogre
  std::map<std::string, uint32_t> missing_shaders;   // empreinte_profil absente de la bibliothèque
  std::map<std::string, uint32_t> substitutions;     // « texture -> cible » : cible rendue utilisée comme texture
  uint32_t textures_loaded = 0, pipelines_created = 0;
  double ms_vertices = 0, ms_constants = 0, ms_descriptors = 0, ms_textures = 0;  // parties de Draw
  double ms_pipelines = 0;  // dont création de pipelines (vkCreateGraphicsPipelines)
  uint64_t upload_bytes = 0;
};

class Renderer {
 public:
  // shader_lib : sortie de switch/shaders/build_lib.sh. output_format : format de l'image de sortie
  // (Present), 0 = pas de présentation.
  static std::unique_ptr<Renderer> Create(const Context& ctx, const std::filesystem::path& shader_lib,
                                          VkFormat output_format = VK_FORMAT_UNDEFINED);
  virtual ~Renderer() = default;

  // Une image du jeu (_updateAllRenderTargets) : plusieurs cibles, chacune entre _beginFrame et _endFrame.
  virtual void BeginFrame() = 0;
  virtual void SetTarget(const std::string& name, uint32_t width, uint32_t height) = 0;
  // Viewport d'Ogre en pixels (mActLeft/Top/Width/Height) ; largeur nulle = toute la cible.
  virtual void SetViewport(int32_t x, int32_t y, int32_t w, int32_t h) = 0;
  // FrameBufferType d'Ogre : 1 couleur, 2 profondeur, 4 stencil.
  virtual void Clear(uint32_t buffers, const float colour[4], float depth) = 0;
  virtual void EndTarget() = 0;
  virtual void Draw(const DrawDesc& d) = 0;

  // Fin de l'image : dessine la cible finale (« Game », sinon la dernière) dans dst (format output_format,
  // passe de rendu), qui finit en SHADER_READ_ONLY_OPTIMAL. dst_written_before : dst a déjà servi
  // (barrière depuis la lecture par le fragment shader), sinon depuis UNDEFINED.
  virtual void Present(VkImage dst, VkImageView dst_view, uint32_t width, uint32_t height,
                       bool dst_written_before) = 0;
  // Soumet l'image enregistrée (sans attendre).
  virtual void Submit() = 0;
  // Outil : contenu RGBA d'une cible (après Submit ; attend la fin du GPU). name vide = cible finale.
  virtual bool ReadTarget(const std::string& name, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) = 0;
  virtual std::vector<std::string> RenderedTargets() const = 0;

  // Écrit le cache de pipelines s'il a des pipelines nouveaux depuis la dernière fois. Vrai si écrit.
  virtual bool SavePipelineCache() = 0;

  virtual const Stats& stats() const = 0;
  virtual void ResetStats() = 0;
};

}  // namespace torchlight::natif
