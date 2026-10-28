// torchlight - rendu natif dans le jeu (voir torchlight_natif.h).
//
// Lecture des objets d'Ogre : mêmes dispositions que la capture (torchlight_capture.cpp,
// switch/RENDU_NATIF.md). Le thread de rendu d'Ogre (celui du jeu) relève chaque image : commandes (cibles,
// viewports, effacements, dessins) et copie des données qui peuvent changer ensuite (plages de sommets et
// d'indices, valeurs des constantes). Un thread dédié rejoue l'image dans Renderer (conversion, pipelines,
// descripteurs, enregistrement Vulkan), la soumet et la présente : sur un autre cœur que le jeu.

#include "torchlight_natif.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <pthread.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "natif/d3dx_cache.h"
#include "natif/ffp_sources.h"
#include "natif/hlsl_compiler.h"
#include "natif/rendu.h"
#include "torchlight_boost.h"
#include "torchlight_capture.h"
#include "torchlight_invite.h"

REXCVAR_DEFINE_BOOL(torchlight_natif, false, "Torchlight",
                    "Rendu natif : redessiner les images du jeu avec Vulkan et les présenter à la place de l'émulation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_natif_seul, false, "Torchlight",
                    "Rendu natif seul : ne plus transmettre les dessins au Direct3D du jeu (émulation du GPU à vide)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_natif_thread, true, "Torchlight",
                    "Rendu natif : enregistrement Vulkan sur un thread dédié (sinon sur le thread du jeu)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_natif_d3dx_allege, false, "Torchlight",
                    "Expérimental (casse le rendu) : source allégée pour le compilateur HLSL de la Xbox")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_natif_verrou_sans_attente, true, "Torchlight",
                    "Rendu natif seul : Ogre verrouille ses tampons sans attendre le GPU émulé")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_envoi_avant_attente, false, "Torchlight",
                    "Envoie le tampon de commandes au GPU avant d'attendre une fence non atteinte (blocage au démarrage)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(torchlight_natif_d3dx_cache, true, "Torchlight",
                    "Résultats du compilateur HLSL de la Xbox gardés d'un lancement à l'autre (cache/natif/d3dx.bin)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(torchlight_natif_ecrans, "", "Torchlight",
                      "Captures d'écran de l'image présentée à ces instants (secondes, séparées par des virgules) : "
                      "trace/ecran_<s>.ppm")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(torchlight_natif_shaders, "", "Torchlight",
                      "Bibliothèque de shaders du rendu natif (vide = cache construit sur cette machine, "
                      "<données utilisateur>/cache/natif)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace torchlight::natif {
namespace {

namespace xg = rex::graphics;
using namespace torchlight::invite;
using Clock = std::chrono::steady_clock;

// Chronométrage du relevé, à chaque dessin : sur la console, lecture directe du compteur du processeur (une
// instruction) au lieu de steady_clock::now (chaîne d'appels jusqu'à libnx, visible dans le profil).
#ifdef __SWITCH__
inline uint64_t FastTicks() { return armGetSystemTick(); }
inline double TicksToMs(uint64_t ticks) { return double(armTicksToNs(ticks)) / 1e6; }
#else
inline uint64_t FastTicks() { return uint64_t(Clock::now().time_since_epoch().count()); }
inline double TicksToMs(uint64_t ticks) {
  return std::chrono::duration<double, std::milli>(Clock::duration(Clock::rep(ticks))).count();
}
#endif

constexpr int kMaxUnits = 16;
constexpr int kReportSeconds = 10;

std::atomic<rex::ui::GraphicsProvider*> g_provider{nullptr};
std::atomic<rex::ui::Presenter*> g_presenter{nullptr};

std::unique_ptr<Renderer> g_renderer;
std::unique_ptr<HlslCompiler> g_compiler;
std::filesystem::path g_shader_cache;
std::mutex g_compile_log_mutex;
uint32_t g_compiled = 0, g_compile_failed = 0;
// Compilation en arrière-plan : file des sources à compiler, thread dédié, compteur pour le rendu.
struct CompileJob {
  std::string name, source;
  bool vertex = false;
  int64_t d3dx_flags = -1;
  std::function<void()> task;  // autre travail d'arrière-plan (écriture dans un cache), à la place d'une compilation
};
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<CompileJob> g_queue;
std::atomic<uint32_t> g_queue_size{0};
std::atomic<uint32_t> g_shader_generation{0};
bool g_compile_thread = false;
void* CompileThreadMain(void*);
bool g_failed = false;
bool g_in_game_frame = false;
// Thread de rendu d'Ogre (celui de la première image) : nos états ne sont pas protégés par des verrous,
// tout appel venu d'un autre thread est compté et ignoré.
std::atomic<std::thread::id> g_thread{};
std::atomic<uint64_t> g_foreign_calls{0};

// Vrai sur le thread du jeu enregistré (g_thread) : booléen trivial par thread, testé sans appeler get_id()
// (pthread_self), car OwnThread est appelé par presque toutes les interceptions.
thread_local bool t_own_thread = false;

bool OwnThread() {
  if (t_own_thread) [[likely]] return true;
  const std::thread::id id = g_thread.load(std::memory_order_relaxed);
  if (id == std::this_thread::get_id()) return true;
  if (id != std::thread::id()) g_foreign_calls.fetch_add(1, std::memory_order_relaxed);
  return false;
}

struct NamedConstants;

struct State {
  uint32_t textures[kMaxUnits] = {};
  bool tex_enabled[kMaxUnits] = {};
  int8_t address[kMaxUnits][3];  // -1 = jamais fixé
  uint8_t border[kMaxUnits] = {};
  State() { std::memset(address, -1, sizeof(address)); }
  uint32_t blend[6] = {0, 1, 0, 0, 1, 0};
  uint32_t cull = 2;
  bool depth_check = true, depth_write = true;
  uint32_t depth_func = 3;
  bool mask[4] = {true, true, true, true};
  uint32_t alpha_func = 1, alpha_value = 0;
  float depth_bias = 0.0f, depth_slope_bias = 0.0f;
  uint64_t program_hash[2] = {};
  const NamedConstants* consts[2] = {};
  float matrix[3][16] = {};
  uint32_t blend_mode[4][2][15] = {};
  bool blend_set[4][2] = {};
} g_state;

// Caches par objet d'Ogre (adresse dans le jeu).
struct TargetInfo {
  std::string name;
  uint32_t width, height;
};
std::unordered_map<uint32_t, TargetInfo> g_targets;
// GpuNamedConstants (pRep) -> constantes : la table des noms est lue une fois (partagée avec le thread de rendu),
// les valeurs à chaque liaison (copiées dans l'image à chaque dessin).
struct NamedConstants {
  uint32_t names_count = 0;
  std::shared_ptr<ConstantNames> names;
  uint32_t floats_addr = 0, floats_count = 0;  // mFloatConstants en mémoire du jeu (copiées à chaque dessin)
};
std::unordered_map<uint32_t, std::unique_ptr<NamedConstants>> g_constants;
// Petit cache à correspondance directe devant g_constants (deux recherches par dessin), par adresse de la table.
struct ConstantsCacheEntry {
  uint32_t named = 0;
  NamedConstants* slot = nullptr;
};
ConstantsCacheEntry g_constants_cache[256];
// Tables de noms remplacées : libérées 8 images plus tard (les images en circulation les lisent par pointeur brut).
std::deque<std::pair<uint64_t, std::shared_ptr<ConstantNames>>> g_retired_names;

// Empreinte du contenu des textures (par adresse, format et taille), calculée sur le thread de rendu, vérifiée de
// moins en moins souvent si elle ne change pas (jusqu'à toutes les kMaxCheckInterval images).
constexpr uint32_t kMaxCheckInterval = 32;
struct ContentCheck {
  uint64_t hash = 0;
  uint64_t next_check = 0;
  uint64_t frame = 0;  // image du thread de rendu où l'empreinte a été examinée
  uint32_t interval = 1;
};
std::unordered_map<uint64_t, ContentCheck> g_content;  // thread de rendu
uint64_t g_render_frame = 0;

// Textures d'Ogre (thread du jeu) : description relue à chaque image (fetch constant de l'objet Direct3D),
// tout le reste réutilisé tant qu'elle ne change pas.
struct TextureCache {
  uint64_t frame = 0;
  uint32_t d3d = 0;
  uint32_t words[6] = {};
  bool ok = false;
  TextureRef ref;
};
std::unordered_map<uint32_t, TextureCache> g_texture_cache;
// Petit cache à correspondance directe devant g_texture_cache (une recherche par unité et par dessin), par adresse
// de la texture d'Ogre ; les nœuds de l'unordered_map ne bougent pas.
struct TextureFront {
  uint32_t tex = 0;
  TextureCache* entry = nullptr;
};
TextureFront g_texture_front[256];
inline TextureFront& TextureFrontSlot(uint32_t tex) { return g_texture_front[(tex >> 4) & 255]; }
uint64_t g_frame = 0;
// Écritures du jeu dans ses tampons de sommets et d'indices Direct3D : toutes passent par leur Lock (intercepté,
// BufferLocked). Nombre de Lock par adresse physique : une copie faite depuis le dernier Lock est encore juste.
// Table fixe de compteurs atomiques indexée par une empreinte de l'adresse (sans verrou : le profil de la console
// montrait le verrou et la map) ; deux tampons qui partagent une case ne coûtent qu'une copie de trop.
constexpr uint32_t kBufferLockSlots = 1u << 16;
std::atomic<uint32_t> g_buffer_locks[kBufferLockSlots];
inline uint32_t BufferLockSlot(uint32_t phys) { return (phys >> 8) * 0x9E3779B1u >> 16; }
void ForgetOldGeometry();
void FreeRetiredGeometry();
// Octets de géométrie copiés et repris (relevé), pour le bilan : comptés dans l'image (thread du jeu, sans atomique),
// cumulés par le thread de rendu.
uint64_t g_geometry_reused = 0, g_geometry_copied = 0;

Clock::time_point g_last_report;
uint32_t g_frames_since_report = 0;
// Accumulateurs du bilan (thread de rendu) : thread du jeu (relevés dans chaque Frame) et thread de rendu.
double g_cpu_ms = 0.0;    // présentation et soumission
double g_draw_ms = 0.0;   // relevé des dessins sur le thread du jeu (lecture d'Ogre, copies)
double g_record_ms = 0.0; // rejeu de l'image (Renderer : conversion, pipelines, descripteurs, enregistrement)
double g_tex_ms = 0.0;    // dont textures sur le thread du jeu (lecture, empreinte)
double g_frame_ms = 0.0;  // _updateAllRenderTargets complet sur le thread du jeu (jeu + nous)
double g_wait_ms = 0.0;   // thread du jeu en attente d'une image libre (rendu en retard)
Clock::time_point g_frame_start;

// Zone mémoire d'une image, par blocs (les adresses déjà données ne bougent pas).
class Arena {
 public:
  uint8_t* Alloc(size_t size) {
    size = (size + 15) & ~size_t(15);
    if (block_ >= blocks_.size() || used_ + size > blocks_[block_].size) {
      if (block_ < blocks_.size()) block_++;
      while (block_ < blocks_.size() && blocks_[block_].size < size) block_++;
      if (block_ >= blocks_.size()) {
        const size_t n = std::max<size_t>(size, 8u << 20);
        blocks_.push_back({std::unique_ptr<uint8_t[]>(new uint8_t[n]), n});
        block_ = blocks_.size() - 1;
      }
      used_ = 0;
    }
    uint8_t* p = blocks_[block_].data.get() + used_;
    used_ += size;
    return p;
  }
  void Reset() {
    block_ = 0;
    used_ = 0;
  }

 private:
  struct Block {
    std::unique_ptr<uint8_t[]> data;
    size_t size;
  };
  std::vector<Block> blocks_;
  size_t block_ = 0, used_ = 0;
};

// Une image du jeu relevée par le thread du jeu, rejouée par le thread de rendu.
struct Command {
  enum Kind : uint8_t { kTarget, kViewport, kClear, kEndTarget, kDraw } kind;
  uint32_t a = 0, b = 0;   // cible : largeur, hauteur ; effacement : tampons ; dessin : indice dans draws
  int32_t vp[4] = {};      // viewport
  float colour[4] = {}, depth = 1.0f;
  std::string name;        // cible
};
struct Frame {
  std::vector<Command> commands;
  std::vector<DrawDesc> draws;   // réutilisées d'une image à l'autre (draws_used en service)
  size_t draws_used = 0;
  std::deque<Constants> consts;  // adresses stables, réutilisées (consts_used en service)
  size_t consts_used = 0;
  Arena arena;
  uint32_t width = 1280, height = 720;
  double game_ms = 0, read_ms = 0, tex_ms = 0, wait_ms = 0;  // thread du jeu
  uint64_t geometry_reused = 0, geometry_copied = 0;          // octets (relevé)
  void Clear() {
    commands.clear();
    draws_used = 0;
    consts_used = 0;
    arena.Reset();
    game_ms = read_ms = tex_ms = wait_ms = 0;
    geometry_reused = geometry_copied = 0;
  }
};
constexpr int kFrames = 3;
Frame g_frames[kFrames];
std::mutex g_frames_mutex;
std::condition_variable g_frames_cv;
std::deque<Frame*> g_free_frames, g_ready_frames;
Frame* g_cur = nullptr;
bool g_render_thread = false;
void* RenderThreadMain(void*);
void ProcessFrame(Frame& f);

// Captures d'écran programmées (torchlight_natif_ecrans).
std::vector<double> g_shots;
Clock::time_point g_start;

void Screenshot(double at) {
  rex::ui::Presenter* presenter = g_presenter.load();
  if (!presenter) return;
  try {
  std::thread([presenter, at] {
    rex::ui::RawImage image;
    if (!presenter->CaptureGuestOutput(image) || !image.width) return;
    const auto dir = rex::filesystem::GetExecutableFolder() / "trace";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[32];
    std::snprintf(name, sizeof(name), "ecran_%03d.ppm", int(at));
    if (FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
      for (uint32_t y = 0; y < image.height; y++)
        for (uint32_t x = 0; x < image.width; x++) std::fwrite(&image.data[y * image.stride + x * 4], 1, 3, f);
      std::fclose(f);
    }
  }).detach();
  } catch (const std::exception&) {
    REXLOG_WARN("[natif] capture d'écran impossible (pas de thread)");
  }
}

void Log(const std::string& s) { REXLOG_INFO("{}", s); }

bool Init() {
  auto* provider = static_cast<rex::ui::vulkan::VulkanProvider*>(g_provider.load());
  auto* presenter = g_presenter.load();
  if (!provider || !presenter || !provider->vulkan_device()) {
    Log("[natif] pas de périphérique Vulkan ou de presenter : rendu natif désactivé");
    return false;
  }
  const rex::ui::vulkan::VulkanDevice* device = provider->vulkan_device();
  Context ctx;
  ctx.get_instance_proc_addr = device->vulkan_instance()->functions().vkGetInstanceProcAddr;
  ctx.instance = device->vulkan_instance()->instance();
  ctx.physical = device->physical_device();
  ctx.device = device->device();
  ctx.queue_family = device->queue_family_graphics_compute();
  ctx.submit = [device](const VkSubmitInfo& si, VkFence fence) {
    const auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    return device->functions().vkQueueSubmit(queue.queue(), 1, &si, fence);
  };
  ctx.log = [](const std::string& s) { REXLOG_INFO("[natif] {}", s); };
  if (!g_shader_cache.empty()) ctx.pipeline_cache_file = g_shader_cache / "pipelines.bin";
  ctx.shader_generation = &g_shader_generation;
  std::filesystem::path lib = REXCVAR_GET(torchlight_natif_shaders);
  if (lib.empty()) lib = g_shader_cache;
  g_renderer = Renderer::Create(ctx, lib, rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat);
  if (!g_renderer) {
    Log("[natif] échec de l'initialisation du rendu natif");
    return false;
  }
  presenter->SetGuestOutputFromApp(true);
  g_last_report = g_start = Clock::now();
  const std::string shots = REXCVAR_GET(torchlight_natif_ecrans);
  for (size_t p = 0; p < shots.size();) {
    size_t e = shots.find(',', p);
    if (e == std::string::npos) e = shots.size();
    if (e > p) g_shots.push_back(std::atof(shots.substr(p, e - p).c_str()));
    p = e + 1;
  }
  REXLOG_INFO("[natif] actif, shaders : {}", lib.string());
  return true;
}

bool Active() { return g_renderer && g_in_game_frame && OwnThread(); }

std::string Ms(double total) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.2f", g_frames_since_report ? total / g_frames_since_report : 0.0);
  return b;
}

void Report() {
  const auto now = Clock::now();
  if (now - g_last_report < std::chrono::seconds(kReportSeconds)) return;
  const double s = std::chrono::duration<double>(now - g_last_report).count();
  g_last_report = now;
  const Stats& st = g_renderer->stats();
  std::string text = "[natif] " + std::to_string(int(g_frames_since_report / s)) + " images/s, " +
                     std::to_string(g_frames_since_report ? st.drawn / g_frames_since_report : 0) +
                     " dessins/image ; ms par image, thread du jeu : image " + Ms(g_frame_ms) + ", relevé " +
                     Ms(g_draw_ms) + " (textures " + Ms(g_tex_ms) + "), attente du rendu " + Ms(g_wait_ms) +
                     " ; thread de rendu" + (g_render_thread ? "" : " (sur le thread du jeu)") + " : Vulkan " +
                     Ms(g_record_ms) + ", présentation " + Ms(g_cpu_ms) + " ; envoi " +
                     std::to_string(g_frames_since_report ? st.upload_bytes / g_frames_since_report / 1024 : 0) +
                     " Ko/image, " + std::to_string(st.textures_loaded) + " textures et " +
                     std::to_string(st.pipelines_created) + " pipelines créés (" +
                     std::to_string(int(st.ms_pipelines)) + " ms)";
  if (g_frames_since_report) {
    const uint64_t copied = g_geometry_copied, reused = g_geometry_reused;
    g_geometry_copied = g_geometry_reused = 0;
    text += "\n  relevé de la géométrie, Ko par image : " + std::to_string(copied / g_frames_since_report / 1024) +
            " copiés, " + std::to_string(reused / g_frames_since_report / 1024) + " repris (tampons inchangés)";
  }
  text += "\n  dans Vulkan, ms par image : sommets " + Ms(st.ms_vertices) + ", pipeline et constantes " +
          Ms(st.ms_constants) + ", textures " + Ms(st.ms_textures) + ", descripteurs " + Ms(st.ms_descriptors);
  if (const uint64_t foreign = g_foreign_calls.exchange(0))
    text += "\n  appels hors du thread de rendu ignorés : " + std::to_string(foreign);
  for (const auto& [why, n] : st.skipped) text += "\n  ignorés : " + std::to_string(n) + " (" + why + ")";
  for (const auto& [what, n] : st.substitutions) text += "\n  substitution : " + what + " (" + std::to_string(n) + ")";
  int shown = 0;
  for (const auto& [name, n] : st.missing_shaders)
    if (shown++ < 8) text += "\n  shader absent : " + name;
  REXLOG_INFO("{}", text);
  // Cache de pipelines réécrit une fois la scène stable (aucun pipeline créé pendant ces 10 s) : pas d'écriture
  // sur la carte SD en plein chargement.
  if (st.pipelines_created == 0 && g_renderer->SavePipelineCache())
    REXLOG_INFO("[natif] cache de pipelines enregistré");
  g_renderer->ResetStats();
  g_frames_since_report = 0;
  g_cpu_ms = g_draw_ms = g_frame_ms = g_record_ms = g_tex_ms = g_wait_ms = 0.0;
}

// Texture d'Ogre -> fetch constant -> TextureRef (identité ; empreinte du contenu sur le thread de rendu,
// détilage à la demande).
const TextureRef* Texture(uint32_t tex, const uint8_t* base) {
  TextureFront& front = TextureFrontSlot(tex);
  if (front.tex != tex || !front.entry) front = {tex, &g_texture_cache[tex]};
  TextureCache& c = *front.entry;
  if (c.frame == g_frame) return c.ok ? &c.ref : nullptr;
  c.frame = g_frame;
  if (!Ptr(tex, 0x130)) return (c.ok = false, nullptr);
  const uint32_t res = DeviceMapValue(base, Load32(base, tex + 0x128));
  if (!Ptr(res, 0x14)) return (c.ok = false, nullptr);
  const uint32_t d3d = Load32(base, res + 0xC);  // pBaseTex
  if (!Ptr(d3d, 0x34)) return (c.ok = false, nullptr);
  uint32_t w[6];
  for (int k = 0; k < 6; k++) w[k] = Load32(base, d3d + 0x1C + k * 4);
  if (c.ok && c.d3d == d3d && std::memcmp(c.words, w, sizeof(w)) == 0) return &c.ref;  // inchangée
  c.d3d = d3d;
  std::memcpy(c.words, w, sizeof(w));
  c.ok = false;
  xg::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, w, sizeof(w));
  xg::TextureInfo info;
  if (!xg::TextureInfo::Prepare(fetch, &info)) return nullptr;
  const xg::FormatInfo* fi = info.format_info();
  const bool cube = info.dimension == xg::xenos::DataDimension::kCube;
  if (!fi || (!cube && info.dimension != xg::xenos::DataDimension::k2DOrStacked)) return nullptr;
  // TextureInfo garde la convention de xenia : width/height = dimension - 1.
  const uint32_t width = info.width + 1, height = info.height + 1;
  const uint32_t faces = cube ? 6 : 1;
  const auto layout = xg::texture_util::GetGuestTextureLayout(info.dimension, fetch.pitch, width, height, faces,
                                                               info.is_tiled, info.format, info.has_packed_mips,
                                                               true, 0);
  const uint8_t* src = Physical(info.memory.base_address);
  const uint32_t extent = layout.base.level_data_extent_bytes;
  if (!src || !extent) return nullptr;
  const uint32_t ident[6] = {info.memory.base_address, uint32_t(info.format), width, height, faces,
                             uint32_t(info.endianness)};
  TextureRef& r = c.ref;
  r = TextureRef();
  r.ident = XXH3_64bits(ident, sizeof(ident));
  r.content = src;
  r.content_size = extent;
  r.format = uint32_t(info.format);
  r.width = width;
  r.height = height;
  r.cube = cube;
  // Relu quand la description change : Ogre peut recréer une autre texture à la même adresse.
  r.name = LoadStdString(base, tex + 0x2C);
  // Détilage au moment de l'envoi (seulement si la texture n'est pas déjà en cache), comme la capture.
  r.load = std::make_shared<const std::function<bool(std::vector<uint8_t>&)>>(
      [info, fetch, layout, src, width, height, faces](std::vector<uint8_t>& out) {
        const xg::FormatInfo* fi = info.format_info();
        const uint32_t bpb = fi->bytes_per_block();
        const uint32_t bw = (width + fi->block_width - 1) / fi->block_width;
        const uint32_t bh = (height + fi->block_height - 1) / fi->block_height;
        if (!bw || !bh || !bpb) return false;
        const uint32_t pitch_blocks = (fetch.pitch * 32 + fi->block_width - 1) / fi->block_width;
        uint32_t log2_bpb = 0;
        while ((1u << (log2_bpb + 1)) <= bpb) log2_bpb++;
        // Côté le plus court <= 16 texels avec mips compactées : niveau de base décalé dans le bloc des mips
        // compactées (voir la capture ; sans cela, poisson de la pêche en 32x16 couvert de bruit).
        uint32_t px = 0, py = 0, pz = 0;
        if (info.has_packed_mips)
          xg::texture_util::GetPackedMipOffset(width, height, 1, info.format, 0, px, py, pz);
        out.resize(size_t(bw) * bh * bpb * faces);
        for (uint32_t f = 0; f < faces; f++) {
          const uint8_t* face = src + size_t(f) * layout.base.array_slice_stride_bytes;
          uint8_t* dst = out.data() + size_t(f) * bw * bh * bpb;
          for (uint32_t y = 0; y < bh; y++) {
            for (uint32_t x = 0; x < bw; x++) {
              const uint32_t gx = x + px, gy = y + py;
              const size_t off = info.is_tiled ? size_t(xg::texture_util::GetTiledOffset2D(
                                                     int32_t(gx), int32_t(gy), pitch_blocks, log2_bpb))
                                               : size_t(gy) * layout.base.row_pitch_bytes + size_t(gx) * bpb;
              xg::texture_conversion::CopySwapBlock(info.endianness, &dst[(size_t(y) * bw + x) * bpb], face + off,
                                                    bpb);
            }
          }
        }
        return true;
      });
  c.ok = true;
  return &r;
}

// Valeurs brutes des constantes (gros-boutistes) -> flottants.
void ConvertConstants(Constants& c) {
  if (!c.raw_be || !c.floats.empty()) return;
  c.floats.resize(c.raw_count);
  for (size_t k = 0; k < c.raw_count; k++) {
    uint32_t w;
    std::memcpy(&w, c.raw_be + k * 4, 4);
    w = __builtin_bswap32(w);
    std::memcpy(&c.floats[k], &w, 4);
  }
}

// Empreinte du contenu d'une texture (thread de rendu), une fois par image au plus.
uint64_t ContentKey(const TextureRef& t) {
  ContentCheck& cc = g_content[t.ident];
  if (cc.frame != g_render_frame) {
    cc.frame = g_render_frame;
    if (g_render_frame >= cc.next_check) {
      const uint64_t h = XXH3_64bits(t.content, t.content_size);
      if (h == cc.hash && cc.next_check) {
        cc.interval = std::min<uint32_t>(cc.interval * 2, kMaxCheckInterval);
      } else {
        cc.hash = h;
        cc.interval = 1;
      }
      cc.next_check = g_render_frame + cc.interval;
    }
  }
  return XXH3_64bits_withSeed(&cc.hash, sizeof(cc.hash), t.ident);
}

// Relevé des dessins pendant l'image capturée (trace/capture/natif.txt), pour comparer à frame.txt :
// empreintes FNV-1a 64 des sommets, indices, textures détilées et constantes (calculables en Python).
uint64_t Fnv(const void* data, size_t size) {
  uint64_t h = 1469598103934665603ull;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

void Journal(const DrawDesc& d) {
  static FILE* f = nullptr;
  static int n = 0;
  if (!f) {
    const auto dir = rex::filesystem::GetExecutableFolder() / "trace" / "capture";
    f = std::fopen((dir / "natif.txt").string().c_str(), "w");
    if (!f) return;
  }
  std::fprintf(f, "draw %d prog %016llx %016llx ffp %d op %u %d %u %u %u %u\n", n++, (unsigned long long)d.vs,
               (unsigned long long)d.ps, d.ffp ? 1 : 0, d.type, d.indexed ? 1 : 0, d.vstart, d.vcount, d.istart,
               d.icount);
  for (const auto& v : d.streams)
    std::fprintf(f, "  vb %u %016llx %zu %u fnv %016llx\n", v.source, (unsigned long long)v.id, v.size, v.stride,
                 v.data ? (unsigned long long)Fnv(v.data, v.size) : 0ull);
  if (d.indexed)
    std::fprintf(f, "  ib %016llx %zu %u fnv %016llx\n", (unsigned long long)d.index.id, d.index.size,
                 d.index.index_size, d.index.data ? (unsigned long long)Fnv(d.index.data, d.index.size) : 0ull);
  for (const auto& t : d.textures) {
    std::vector<uint8_t> data;
    const bool ok = t.load && (*t.load)(data);
    std::fprintf(f, "  tex %u %016llx %u %ux%u fnv %016llx adresse %d %d %d bordure %u %s\n", t.unit,
                 (unsigned long long)t.key, t.format, t.width, t.height,
                 ok ? (unsigned long long)Fnv(data.data(), data.size()) : 0ull, t.address[0], t.address[1], t.address[2],
                 t.border, t.name.c_str());
  }
  std::fprintf(f, "  state blend %u %u %u %u %u %u cull %u depth %d %d %u mask %d%d%d%d alpha %u %u\n", d.blend[0],
               d.blend[1], d.blend[2], d.blend[3], d.blend[4], d.blend[5], d.cull, d.depth_check ? 1 : 0,
               d.depth_write ? 1 : 0, d.depth_func, d.mask[0], d.mask[1], d.mask[2], d.mask[3], d.alpha_func,
               d.alpha_value);
  if (d.ffp) {
    for (int k = 0; k < 3; k++) {
      std::fprintf(f, "  ffp_matrix %d", k);
      for (float v : d.matrix[k]) std::fprintf(f, " %.9g", v);
      std::fprintf(f, "\n");
    }
    for (int u = 0; u < 4; u++)
      for (int t = 0; t < 2; t++) {
        const FfpStage& st = d.stage[u][t];
        if (!st.set) continue;
        std::fprintf(f, "  ffp_stage %d %d %u %u %u", u, t, st.op, st.src1, st.src2);
        for (float v : st.arg) std::fprintf(f, " %.6g", v);
        std::fprintf(f, "\n");
      }
  }
  for (int k = 0; k < 2; k++)
    if (d.consts[k]) ConvertConstants(const_cast<Constants&>(*d.consts[k]));
  for (int k = 0; k < 2; k++)
    if (d.consts[k])
      std::fprintf(f, "  consts %s floats %zu fnv %016llx noms %zu\n", k ? "ps" : "vs", d.consts[k]->floats.size(),
                   (unsigned long long)Fnv(d.consts[k]->floats.data(), d.consts[k]->floats.size() * 4),
                   d.consts[k]->Names().size());
  std::fflush(f);
}

}  // namespace

bool Enabled() { return REXCVAR_GET(torchlight_natif); }

bool StubXboxShaders() {
  static const bool on = REXCVAR_GET(torchlight_natif) && REXCVAR_GET(torchlight_natif_seul) &&
                         REXCVAR_GET(torchlight_natif_d3dx_allege);
  return on;
}

bool KickBeforeWait() {
  static const bool on = REXCVAR_GET(torchlight_envoi_avant_attente);
  return on;
}

bool LockWithoutWait() {
  static const bool on = REXCVAR_GET(torchlight_natif_verrou_sans_attente);
  return on && DrawsHandled();
}

bool DrawsHandled() {
  static const bool seul = REXCVAR_GET(torchlight_natif_seul);
  return seul && Active();
}

void SetPresentation(rex::ui::GraphicsProvider* provider, rex::ui::Presenter* presenter) {
  g_provider.store(provider);
  g_presenter.store(presenter);
}

void SetPaths(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root) {
  if (!Enabled()) return;
  g_shader_cache = (user_data_root.empty() ? rex::filesystem::GetExecutableFolder() : user_data_root) / "cache" / "natif";
  g_compiler = HlslCompiler::Create(game_data_root / "rtshaderlib", g_shader_cache);
  if (!g_compiler) {
    REXLOG_ERROR("[natif] compilateur de shaders indisponible (rtshaderlib introuvable dans {})",
                 game_data_root.string());
    return;
  }
  // Émulation du pipeline fixe : sources écrites pour le rendu natif (switch/shaders/ffp).
  std::string error;
  if (!g_compiler->CompileRaw("ffp_vs_3_0_main", kFfpSource_vs, kFfpIo_vs, true, error) ||
      !g_compiler->CompileRaw("ffp_ps_3_x_main", kFfpSource_ps, kFfpIo_ps, false, error))
    REXLOG_ERROR("[natif] pipeline fixe : échec de compilation : {}", error);
  if (REXCVAR_GET(torchlight_natif_d3dx_cache)) {
    d3dx::Open((g_shader_cache / "d3dx.bin").string());
    REXLOG_INFO("[natif] cache de D3DXCompileShader : {}", d3dx::Summary());
  }
  // Thread de compilation, créé ici (thread principal de l'application, pas un thread du jeu) avec une pile
  // de 1 Mo : les expressions régulières de libstdc++ et l'analyseur de glslang sont récursifs.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1 << 20);
  pthread_t thread;
  g_compile_thread = pthread_create(&thread, &attr, CompileThreadMain, nullptr) == 0;
  pthread_attr_destroy(&attr);
  if (g_compile_thread) pthread_detach(thread);
  // Thread de rendu (enregistrement Vulkan, présentation), même méthode.
  for (Frame& f : g_frames) g_free_frames.push_back(&f);
  if (REXCVAR_GET(torchlight_natif_thread)) {
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    g_render_thread = pthread_create(&thread, &attr, RenderThreadMain, nullptr) == 0;
    pthread_attr_destroy(&attr);
    if (g_render_thread) pthread_detach(thread);
  }
  REXLOG_INFO("[natif] enregistrement Vulkan {}", g_render_thread ? "sur un thread dédié" : "sur le thread du jeu");
  REXLOG_INFO("[natif] shaders compilés sur cette machine ({}), cache : {}",
              g_compile_thread ? "en arrière-plan" : "sur le thread du jeu", g_shader_cache.string());
}

namespace {

void CompileNow(const CompileJob& job) {
  const auto t0 = Clock::now();
  std::string error;
  const bool ok = g_compiler->Compile(job.name, job.source, job.vertex, job.d3dx_flags, error);
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::lock_guard<std::mutex> lock(g_compile_log_mutex);
  if (ok) {
    g_compiled++;
    g_shader_generation.fetch_add(1, std::memory_order_release);
    REXLOG_INFO("[natif] shader {} compilé en {:.1f} ms ({} au total, {} en attente) : {}", job.name, ms,
                g_compiled, g_queue_size.load(), g_compiler->LastTiming());
  } else {
    g_compile_failed++;
    REXLOG_ERROR("[natif] shader {} : échec de compilation : {}", job.name, error.substr(0, 400));
  }
}

// Thread de compilation : priorité 0x3C sur la console (sous celle du jeu, 0x3B : il ne lui prend jamais le
// processeur, mais tourne pleinement sur un cœur libre), trois cœurs permis.
void* CompileThreadMain(void*) {
#ifdef __SWITCH__
  svcSetThreadPriority(threadGetCurHandle(), 0x3C);
  svcSetThreadCoreMask(threadGetCurHandle(), -1, 0x7);
#endif
  for (;;) {
    CompileJob job;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return !g_queue.empty(); });
      job = std::move(g_queue.front());
      g_queue.pop_front();
      g_queue_size.store(uint32_t(g_queue.size()));
    }
    if (job.task)
      job.task();
    else
      CompileNow(job);
  }
  return nullptr;
}

// Thread de rendu : même priorité que le jeu sur la console (0x3B, temps partagé), trois cœurs permis.
void* RenderThreadMain(void*) {
#ifdef __SWITCH__
  svcSetThreadPriority(threadGetCurHandle(), 0x3B);
  svcSetThreadCoreMask(threadGetCurHandle(), -1, 0x7);
#endif
  for (;;) {
    Frame* f;
    {
      std::unique_lock<std::mutex> lock(g_frames_mutex);
      g_frames_cv.wait(lock, [] { return !g_ready_frames.empty(); });
      f = g_ready_frames.front();
      g_ready_frames.pop_front();
    }
    ProcessFrame(*f);
    {
      std::lock_guard<std::mutex> lock(g_frames_mutex);
      f->Clear();
      g_free_frames.push_back(f);
    }
    g_frames_cv.notify_all();
  }
  return nullptr;
}

// Rejoue une image relevée : Renderer, présentation, bilan.
void ProcessFrame(Frame& f) {
  if (!g_renderer) return;
  const auto t0 = Clock::now();
  g_render_frame++;
  for (size_t i = 0; i < f.consts_used; i++) ConvertConstants(f.consts[i]);
  for (size_t i = 0; i < f.draws_used; i++)
    for (TextureRef& t : f.draws[i].textures)
      if (!t.key && t.content) t.key = ContentKey(t);
  g_renderer->BeginFrame();
  for (const Command& c : f.commands) {
    switch (c.kind) {
      case Command::kTarget: g_renderer->SetTarget(c.name, c.a, c.b); break;
      case Command::kViewport: g_renderer->SetViewport(c.vp[0], c.vp[1], c.vp[2], c.vp[3]); break;
      case Command::kClear: g_renderer->Clear(c.a, c.colour, c.depth); break;
      case Command::kEndTarget: g_renderer->EndTarget(); break;
      case Command::kDraw: g_renderer->Draw(f.draws[c.a]); break;
    }
  }
  const auto t1 = Clock::now();
  const uint32_t width = f.width, height = f.height;
  rex::ui::Presenter* presenter = g_presenter.load();
  const bool presented = presenter->RefreshGuestOutputFromApp(
      width, height, 16, 9, [width, height](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        auto& vc = static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        g_renderer->Present(vc.image(), vc.image_view(), width, height, vc.image_ever_written_previously());
        g_renderer->Submit();
        context.SetIs8bpc(true);
        return true;
      });
  if (!presented) g_renderer->Submit();  // le rappel n'a pas été appelé : soumettre quand même
  const auto t2 = Clock::now();
  g_record_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
  g_cpu_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
  g_frame_ms += f.game_ms;
  g_draw_ms += f.read_ms;
  g_geometry_copied += f.geometry_copied;
  g_geometry_reused += f.geometry_reused;
  g_tex_ms += f.tex_ms;
  g_wait_ms += f.wait_ms;
  g_frames_since_report++;
  const double elapsed = std::chrono::duration<double>(t2 - g_start).count();
  for (auto it = g_shots.begin(); it != g_shots.end();) {
    if (elapsed >= *it) {
      Screenshot(*it);
      it = g_shots.erase(it);
    } else {
      ++it;
    }
  }
  Report();
}

}  // namespace

void ShaderSource(uint64_t hash, const std::string& profile, const std::string& entry, const std::string& source,
                  uint32_t d3dx_flags) {
  if (!g_compiler) return;
  char name[160];
  std::snprintf(name, sizeof(name), "%016llx_%s_%s", (unsigned long long)hash, profile.c_str(), entry.c_str());
  if (g_compiler->InCache(name)) return;
  CompileJob job{name, source, profile.rfind("vs", 0) == 0, d3dx_flags};
  if (!g_compile_thread) {
    CompileNow(job);  // pas de thread de compilation : sur le thread du jeu
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push_back(std::move(job));
    g_queue_size.store(uint32_t(g_queue.size()));
  }
  g_queue_cv.notify_one();
}

void BufferLocked(uint32_t gpu_address) {
  const uint32_t phys = torchlight::invite::ToPhysical(gpu_address & ~3u);
  g_buffer_locks[BufferLockSlot(phys)].fetch_add(1, std::memory_order_acq_rel);
}

void RunInBackground(std::function<void()> task) {
  if (!g_compile_thread) {
    task();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    CompileJob job;
    job.task = std::move(task);
    g_queue.push_back(std::move(job));
  }
  g_queue_cv.notify_one();
}

void BeginGameFrame() {
  if (!Enabled() || g_failed) return;
  if (!g_renderer) {
    if (!Init()) {
      g_failed = true;
      return;
    }
    g_thread.store(std::this_thread::get_id());
    t_own_thread = true;
  }
  if (!OwnThread()) return;
  g_frame_start = Clock::now();
  g_frame++;
  if ((g_frame & 63) == 0) {
    torchlight::invite::ForgetVerifiedPages();
    ForgetOldGeometry();
  }
  FreeRetiredGeometry();
  // Image libre : le thread de rendu en rend une quand il a fini de la rejouer (au plus une d'avance).
  {
    std::unique_lock<std::mutex> lock(g_frames_mutex);
    g_frames_cv.wait(lock, [] { return !g_free_frames.empty(); });
    g_cur = g_free_frames.front();
    g_free_frames.pop_front();
  }
  g_cur->wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - g_frame_start).count();
  g_in_game_frame = true;
}

void EndGameFrame() {
  torchlight::boost::Image();  // mode boost pendant les chargements (src/torchlight_boost.cpp)
  if (!Active()) return;
  g_in_game_frame = false;
  Frame* f = g_cur;
  g_cur = nullptr;
  f->game_ms = std::chrono::duration<double, std::milli>(Clock::now() - g_frame_start).count();
  if (!g_render_thread) {
    ProcessFrame(*f);  // pas de thread de rendu : tout de suite, sur le thread du jeu
    f->Clear();
    std::lock_guard<std::mutex> lock(g_frames_mutex);
    g_free_frames.push_back(f);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_frames_mutex);
    g_ready_frames.push_back(f);
  }
  g_frames_cv.notify_all();
}

void BeginFrame() {}

void EndFrame() {
  if (Active()) g_cur->commands.push_back({Command::kEndTarget});
}

// RenderTarget : vtable +0, mName +4, largeur +0x24, hauteur +0x28.
void SetRenderTarget(uint32_t target, const uint8_t* base) {
  if (!Active()) return;
  auto it = g_targets.find(target);
  if (it == g_targets.end()) {
    TargetInfo info{"?", 0, 0};
    if (Ptr(target, 0x80)) {
      info.name = LoadStdString(base, target + 4);
      for (char& c : info.name)
        if (c == ' ') c = '_';
      info.width = Load32(base, target + 0x24);
      info.height = Load32(base, target + 0x28);
    }
    it = g_targets.emplace(target, info).first;
  }
  if (it->second.name == "Game") {
    g_cur->width = it->second.width ? it->second.width : 1280;
    g_cur->height = it->second.height ? it->second.height : 720;
  }
  Command c{Command::kTarget, it->second.width, it->second.height};
  c.name = it->second.name;
  g_cur->commands.push_back(std::move(c));
}

// Viewport d'Ogre : mActLeft, mActTop, mActWidth, mActHeight en +0x28..+0x34.
void SetViewport(uint32_t viewport, const uint8_t* base) {
  if (!Active() || !Ptr(viewport, 0x60)) return;
  Command c{Command::kViewport};
  for (int k = 0; k < 4; k++) c.vp[k] = int32_t(Load32(base, viewport + 0x28 + k * 4));
  g_cur->commands.push_back(c);
}

void Clear(uint32_t buffers, const float colour[4], double depth) {
  if (!Active()) return;
  Command c{Command::kClear, buffers};
  std::memcpy(c.colour, colour, sizeof(c.colour));
  c.depth = float(depth);
  g_cur->commands.push_back(c);
}

void SetTexture(uint32_t unit, bool enabled, uint32_t texture) {
  if (unit >= kMaxUnits || !OwnThread()) return;
  g_state.textures[unit] = texture;
  g_state.tex_enabled[unit] = enabled && texture;
}

void SetSceneBlending(uint32_t src, uint32_t dst, uint32_t op) {
  if (!OwnThread()) return;
  g_state.blend[0] = g_state.blend[3] = src;
  g_state.blend[1] = g_state.blend[4] = dst;
  g_state.blend[2] = g_state.blend[5] = op;
}

void SetSeparateSceneBlending(uint32_t src, uint32_t dst, uint32_t src_a, uint32_t dst_a, uint32_t op,
                              uint32_t op_a) {
  if (!OwnThread()) return;
  const uint32_t b[6] = {src, dst, op, src_a, dst_a, op_a};
  std::memcpy(g_state.blend, b, sizeof(b));
}

void SetCullingMode(uint32_t mode) {
  if (OwnThread()) g_state.cull = mode;
}
void SetDepthCheck(bool enabled) {
  if (OwnThread()) g_state.depth_check = enabled;
}
void SetDepthWrite(bool enabled) {
  if (OwnThread()) g_state.depth_write = enabled;
}
void SetDepthFunction(uint32_t func) {
  if (OwnThread()) g_state.depth_func = func;
}
void SetColourWrite(bool r, bool g, bool b, bool a) {
  if (!OwnThread()) return;
  g_state.mask[0] = r;
  g_state.mask[1] = g;
  g_state.mask[2] = b;
  g_state.mask[3] = a;
}
void SetAlphaReject(uint32_t func, uint32_t value) {
  if (!OwnThread()) return;
  g_state.alpha_func = func;
  g_state.alpha_value = value;
}

void SetDepthBias(float constant, float slope) {
  if (!OwnThread()) return;
  g_state.depth_bias = constant;
  g_state.depth_slope_bias = slope;
}

void SetTextureAddressing(uint32_t unit, uint32_t u, uint32_t v, uint32_t w) {
  if (unit >= kMaxUnits || !OwnThread()) return;
  g_state.address[unit][0] = int8_t(u & 3);
  g_state.address[unit][1] = int8_t(v & 3);
  g_state.address[unit][2] = int8_t(w & 3);
}

void SetTextureBorder(uint32_t unit, const float rgba[4]) {
  if (unit >= kMaxUnits || !OwnThread()) return;
  // Vulkan n'a que trois couleurs de bordure standard : blanc, noir, noir transparent.
  const bool white = rgba[0] > 0.5f && rgba[1] > 0.5f && rgba[2] > 0.5f;
  g_state.border[unit] = white ? 2 : (rgba[3] > 0.5f ? 1 : 0);
}

void BindProgram(bool vertex, uint64_t hash) {
  if (OwnThread()) g_state.program_hash[vertex ? 0 : 1] = hash;
}

void UnbindProgram(uint32_t type) {
  if (type <= 1 && OwnThread()) {
    g_state.program_hash[type] = 0;
    g_state.consts[type] = nullptr;
  }
}

void SetMatrix(int kind, const float m[16]) {
  if (kind >= 0 && kind <= 2 && OwnThread()) std::memcpy(g_state.matrix[kind], m, sizeof(g_state.matrix[kind]));
}

void SetTextureBlendMode(uint32_t unit, const uint32_t words[15]) {
  if (unit >= 4 || !OwnThread()) return;
  const uint32_t type = words[0] ? 1 : 0;  // LBT_COLOUR = 0, LBT_ALPHA = 1
  std::memcpy(g_state.blend_mode[unit][type], words, sizeof(g_state.blend_mode[unit][type]));
  g_state.blend_set[unit][type] = true;
}

void DisableTextureUnit(uint32_t unit) {
  if (unit < kMaxUnits && OwnThread()) g_state.tex_enabled[unit] = false;
}

void DisableTextureUnitsFrom(uint32_t unit) {
  if (!OwnThread()) return;
  for (uint32_t u = unit; u < kMaxUnits; u++) g_state.tex_enabled[u] = false;
}

// GpuProgramParameters : mFloatConstants (vector<float>) en +0 ; mNamedConstants (SharedPtr) pRep en +0x44
// -> GpuNamedConstants { floatBufferSize, intBufferSize, ?, map (en-tête +0xC, taille +0x10) } ;
// valeur de la map : pair<String (0x1C), GpuConstantDefinition { type, physicalIndex, logicalIndex,
// elementSize, arraySize, variability }>.
void BindParameters(uint32_t type, uint32_t params, const uint8_t* base) {
  if (type > 1 || !Enabled() || !OwnThread()) return;
  g_state.consts[type] = nullptr;
  if (!Ptr(params, 0x60)) return;
  const uint32_t named = Load32(base, params + 0x44);
  const uint32_t named_count = Ptr(named, 0x20) ? Load32(base, named + 0x10) : 0;
  ConstantsCacheEntry& cached = g_constants_cache[(named >> 4) & 255];
  NamedConstants* slot = cached.named == named ? cached.slot : nullptr;
  if (!slot) {
    auto& owned = g_constants[named];
    if (!owned) owned = std::make_unique<NamedConstants>();
    slot = owned.get();
    cached = {named, slot};
  }
  // Table relue si elle a changé de taille (objet recréé à la même adresse) ; l'ancienne est libérée plus tard.
  if (slot->names && slot->names_count != named_count) {
    g_retired_names.emplace_back(g_frame, std::move(slot->names));
    slot->names.reset();
  }
  if (!slot->names) {
    slot->names_count = named_count;
    slot->names = std::make_shared<ConstantNames>();
    if (Ptr(named, 0x20)) {
      WalkMap(base, Load32(base, named + 0xC), 256, [&](uint32_t val) {
        if (!Readable(val, 0x34)) return;
        const std::string name = LoadStdString(base, val);
        if (name.empty() || name.back() == ']') return;  // entrées « nom[0] » redondantes
        (*slot->names)[name] = {Load32(base, val + 0x1C), Load32(base, val + 0x20), Load32(base, val + 0x28),
                             Load32(base, val + 0x2C)};
      });
    }
  }
  const uint32_t begin = Load32(base, params), end = Load32(base, params + 4);
  const uint32_t count = (end >= begin && end - begin < 0x10000) ? (end - begin) / 4 : 0;
  const bool ok = count && Readable(begin, count * 4);
  slot->floats_addr = ok ? begin : 0;
  slot->floats_count = ok ? count : 0;
  g_state.consts[type] = slot;
}

namespace {


uint32_t BufferLocks(uint32_t phys) {
  return g_buffer_locks[BufferLockSlot(phys)].load(std::memory_order_acquire);
}

// Copies de plages de tampons gardées d'une image à l'autre (thread du jeu) : les tampons statiques (décor,
// personnages) ne sont copiés qu'une fois ; seuls ceux réécrits (particules, interface) le sont à chaque image.
struct GeometryKey {
  uint32_t phys, first, bytes;
  bool operator==(const GeometryKey& o) const { return phys == o.phys && first == o.first && bytes == o.bytes; }
};
struct GeometryKeyHash {
  size_t operator()(const GeometryKey& k) const {
    return std::hash<uint64_t>()((uint64_t(k.phys) << 32 | k.first) * 0x9E3779B97F4A7C15ull ^ k.bytes);
  }
};
struct GeometryCopy {
  uint32_t locks = 0;      // BufferLocks au moment de la copie
  uint64_t last_used = 0;  // image
  uint32_t max_index = 0;  // tampon d'indices : plus grand indice de la plage
  std::vector<uint8_t> bytes;  // adresse stable tant que la copie n'est pas remplacée (nœud de la map)
};
// Table plate (adressage ouvert, sondage linéaire, valeurs en place) : une recherche lit en général une seule ligne
// de cache, contre 2 ou 3 nœuds dispersés pour une unordered_map (~700 recherches par image en ville : 0,46 ms sur la
// console avec la RAM d'origine). Une référence rendue vaut jusqu'à l'insertion suivante ; bytes.data() reste
// valable tant que la copie n'est pas remplacée (déplacer le vecteur garde son tampon).
class GeometryMap {
 public:
  GeometryCopy& operator[](const GeometryKey& key) {
    if ((count_ + 1) * 2 > slots_.size()) Grow();
    for (size_t i = Index(key);; i = (i + 1) & (slots_.size() - 1)) {
      Slot& s = slots_[i];
      if (!s.used) {
        s.used = true;
        s.key = key;
        count_++;
        return s.copy;
      }
      if (s.key == key) return s.copy;
    }
  }
  // Case où la recherche de `key` commencera (préchargée tôt dans Render : sur la console, la recherche attendait
  // la mémoire).
  void Prefetch(const GeometryKey& key) const {
    if (!slots_.empty()) __builtin_prefetch(&slots_[Index(key)]);
  }
  // Supprime les copies pour lesquelles drop(copie) est vrai (table reconstruite : appelé toutes les 64 images).
  template <class Drop>
  void EraseIf(Drop drop) {
    std::vector<Slot> old(slots_.size());
    old.swap(slots_);
    count_ = 0;
    for (Slot& s : old) {
      if (!s.used || drop(s.copy)) continue;
      GeometryCopy& c = (*this)[s.key];
      c = std::move(s.copy);
    }
  }

 private:
  struct Slot {
    GeometryKey key{};
    bool used = false;
    GeometryCopy copy;
  };
  size_t Index(const GeometryKey& k) const {
    return size_t((GeometryKeyHash()(k) * 0x9E3779B97F4A7C15ull) >> (64 - bits_));
  }
  void Grow() {
    std::vector<Slot> old(slots_.empty() ? 1024 : slots_.size() * 2);
    bits_ = 0;
    while ((size_t(1) << bits_) < old.size()) bits_++;
    old.swap(slots_);
    count_ = 0;
    for (Slot& s : old)
      if (s.used) (*this)[s.key] = std::move(s.copy);
  }
  std::vector<Slot> slots_;
  size_t count_ = 0;
  int bits_ = 0;
};
GeometryMap g_geometry;
// Copies remplacées ou oubliées : libérées seulement 8 images plus tard, quand plus aucune image en circulation
// (au plus 3) ne peut les lire. Évite un shared_ptr par plage et par dessin (incréments atomiques, coûteux sur le
// Cortex-A57 sans instructions LSE).
constexpr uint64_t kRetireFrames = 8;
std::deque<std::pair<uint64_t, std::vector<uint8_t>>> g_retired;

void Retire(std::vector<uint8_t>&& bytes) {
  if (!bytes.empty()) g_retired.emplace_back(g_frame, std::move(bytes));
}

void FreeRetiredGeometry() {
  while (!g_retired.empty() && g_frame - g_retired.front().first > kRetireFrames) g_retired.pop_front();
  while (!g_retired_names.empty() && g_frame - g_retired_names.front().first > kRetireFrames)
    g_retired_names.pop_front();
}

// Copie de [src + first, + bytes) reprise ou refaite. id : identité du tampon (adresse GPU << 32 | taille).
const GeometryCopy& CopyRange(uint64_t id, const uint8_t* src, size_t first, size_t bytes, bool index16,
                              bool index32) {
  const uint32_t phys = torchlight::invite::ToPhysical(uint32_t(id >> 32) & ~3u);
  const uint32_t locks = BufferLocks(phys);
  GeometryCopy& c = g_geometry[{phys, uint32_t(first), uint32_t(bytes)}];
  c.last_used = g_frame;
  if (!c.bytes.empty() && c.locks == locks) {
    g_cur->geometry_reused += bytes;
  } else {
    Retire(std::move(c.bytes));
    c.bytes.assign(src + first, src + first + bytes);
    const uint8_t* copy = c.bytes.data();
    uint32_t max_index = 0;
    if (index16) {
      for (size_t k = 0; k < bytes / 2; k++) {
        uint16_t v;
        std::memcpy(&v, copy + k * 2, 2);
        max_index = std::max<uint32_t>(max_index, __builtin_bswap16(v));
      }
    } else if (index32) {
      for (size_t k = 0; k < bytes / 4; k++) {
        uint32_t v;
        std::memcpy(&v, copy + k * 4, 4);
        max_index = std::max(max_index, __builtin_bswap32(v));
      }
    }
    c.locks = locks;
    c.max_index = max_index;
    g_cur->geometry_copied += bytes;
  }
  return c;
}

// Copies inutilisées depuis un moment : oubliées (appelé toutes les 64 images).
void ForgetOldGeometry() {
  g_geometry.EraseIf([](GeometryCopy& c) {
    if (g_frame - c.last_used <= 120) return false;
    Retire(std::move(c.bytes));
    return true;
  });
}

// Plages d'indices et de sommets du dessin, copiées (le jeu peut réécrire ses tampons juste après, voir
// l'interface CEGUI) ou reprises d'une image précédente si le tampon n'a pas été verrouillé depuis ; plages
// ramenées à 0 (vstart, istart). Faux pour un dessin vide.
bool CopyGeometry(DrawDesc& d) {
  if (d.indexed ? !d.icount : !d.vcount) return false;
  uint32_t vertex_count = d.vcount;
  if (d.indexed) {
    const size_t isize = d.index.index_size == 2 ? 2 : 4;
    const size_t first = size_t(d.istart) * isize, bytes = size_t(d.icount) * isize;
    if (!d.index.data || first + bytes > d.index.size) {
      d.index.data = nullptr;  // le rendu l'ignorera (raison dans le bilan)
      return true;
    }
    const GeometryCopy& c = CopyRange(d.index.id, d.index.data, first, bytes, isize == 2, isize == 4);
    d.index.data = c.bytes.data();
    d.index.size = bytes;
    d.istart = 0;
    vertex_count = c.max_index + 1;
  }
  for (VertexStream& vb : d.streams) {
    const size_t first = size_t(d.vstart) * vb.stride, bytes = size_t(vertex_count) * vb.stride;
    if (!vb.data || !vb.stride || first + bytes > vb.size) {
      vb.data = nullptr;
      continue;
    }
    vb.data = CopyRange(vb.id, vb.data, first, bytes, false, false).bytes.data();
    vb.size = bytes;
  }
  d.vstart = 0;
  return true;
}

}  // namespace

// Préchargements du relevé (console, profil du 2026-10-15 : le relevé attendait surtout la mémoire, sans boucle
// chaude) : descripteur du dessin suivant et ses tableaux (~300 descripteurs de 920 octets par image, 3 images en
// circulation : froids quand on les réutilise), cases des compteurs de Lock et de la table des géométries dès que
// l'adresse d'un tampon est connue. Aucun effet sur ce qui est relevé.
static void PrefetchNextDraw(const Frame& f) {
  if (f.draws_used + 1 >= f.draws.size()) return;
  const DrawDesc& n = f.draws[f.draws_used + 1];
  const char* p = reinterpret_cast<const char*>(&n);
  for (size_t off = 0; off < 256; off += 64) __builtin_prefetch(p + off, 1);  // champs avant matrix et stage
  if (!n.elems.empty()) __builtin_prefetch(n.elems.data(), 1);
  if (!n.streams.empty()) __builtin_prefetch(n.streams.data(), 1);
  if (!n.textures.empty()) __builtin_prefetch(n.textures.data(), 1);
}

static void PrefetchBuffer(uint32_t addr) {
  __builtin_prefetch(&g_buffer_locks[BufferLockSlot(torchlight::invite::ToPhysical(addr & ~3u))]);
}

void Render(uint32_t op, const uint8_t* base) {
  if (!Active() || !Ptr(op, 0x18)) return;
  Frame& f = *g_cur;
  PrefetchNextDraw(f);
  for (uint32_t u = 0; u < kMaxUnits; u++) {
    if (!g_state.tex_enabled[u]) continue;
    const TextureFront& t = TextureFrontSlot(g_state.textures[u]);
    if (t.tex == g_state.textures[u] && t.entry) __builtin_prefetch(t.entry);
  }
  struct Timer {
    Frame& f;
    uint64_t t0 = FastTicks();
    ~Timer() { f.read_ms += TicksToMs(FastTicks() - t0); }
  } timer{f};
  const State& s = g_state;
  if (f.draws_used == f.draws.size()) f.draws.emplace_back();
  DrawDesc& d = f.draws[f.draws_used];
  d.Clear();
  d.vs = s.program_hash[0];
  d.ps = s.program_hash[1];
  d.ffp = !d.vs && !d.ps;

  // RenderOperation { VertexData* +0, type +4, bool useIndexes +8, IndexData* +0xC }
  const uint32_t vd = Load32(base, op);
  d.type = Load32(base, op + 4);
  d.indexed = (Load32(base, op + 8) >> 24) != 0;
  const uint32_t id = Load32(base, op + 0xC);
  if (!Ptr(vd, 0x18)) return;
  d.vstart = Load32(base, vd + 0x10);
  d.vcount = Load32(base, vd + 0x14);
  if (d.indexed) {
    if (!Ptr(id, 0x18)) return;
    d.istart = Load32(base, id + 0x10);
    d.icount = Load32(base, id + 0x14);
  }

  // VertexDeclaration : éléments dans un std::vector (+4 début, +8 fin), 0x14 octets chacun.
  const uint32_t decl = Load32(base, vd + 4);
  if (Ptr(decl, 0x10)) {
    const uint32_t b = Load32(base, decl + 4), e = Load32(base, decl + 8);
    if (e > b && e - b <= 32 * 0x14 && Readable(b, e - b)) {
      for (uint32_t p = b; p < e; p += 0x14)
        d.elems.push_back({Load32(base, p) >> 16, Load32(base, p + 4), Load32(base, p + 8), Load32(base, p + 0xC),
                           Load32(base, p + 0x10) >> 16});
    }
  }
  // mBindingMap : std::map<ushort, HardwareVertexBufferSharedPtr>, en-tête en +8 du VertexBufferBinding.
  const uint32_t binding = Load32(base, vd + 8);
  bool vb_ok = Ptr(binding, 0x10);
  if (vb_ok) {
    WalkMap(base, Load32(base, binding + 8), 16, [&](uint32_t val) {
      const uint32_t source = Load32(base, val) >> 16;  // ushort big-endian
      const uint32_t vb = Load32(base, val + 8);        // SharedPtr { vtable, pRep } depuis val + 4
      if (!Ptr(vb, 0x40)) {
        vb_ok = false;
        return;
      }
      const uint32_t size = Load32(base, vb + 4);
      const uint32_t stride = Load32(base, vb + 0x24);
      const uint32_t res = DeviceMapValue(base, Load32(base, vb + 0x38));
      const uint32_t d3d = Ptr(res, 4) ? Load32(base, res) : 0;
      if (!Ptr(d3d, 0x20)) {
        vb_ok = false;
        return;
      }
      const uint32_t addr = Load32(base, d3d + 0x18) & ~3u;  // mot 0 : type en bits 0-1
      PrefetchBuffer(addr);
      if (!d.indexed)
        g_geometry.Prefetch({torchlight::invite::ToPhysical(addr), d.vstart * stride, d.vcount * stride});
      d.streams.push_back({source, Physical(addr), size, stride, uint64_t(addr) << 32 | size});
    });
  }
  if (!vb_ok) return;

  if (d.indexed) {
    const uint32_t ib = Load32(base, id + 4);
    if (!Ptr(ib, 0x40)) return;
    const uint32_t size = Load32(base, ib + 4);
    const uint32_t isize = Load32(base, ib + 0x28);
    const uint32_t res = DeviceMapValue(base, Load32(base, ib + 0x34));
    const uint32_t d3d = Ptr(res, 4) ? Load32(base, res) : 0;
    if (!Ptr(d3d, 0x20)) return;
    const uint32_t addr = Load32(base, d3d + 0x18);
    PrefetchBuffer(addr);
    const uint32_t ibytes = isize == 2 ? 2 : 4;
    g_geometry.Prefetch({torchlight::invite::ToPhysical(addr & ~3u), d.istart * ibytes, d.icount * ibytes});
    d.index = {Physical(addr), size, isize, uint64_t(addr) << 32 | size};
  }

  const uint64_t tt = FastTicks();
  // Éléments de l'image précédente réutilisés : l'affectation garde la capacité des noms (une allocation par
  // texture et par dessin sinon, visible dans le profil de la console).
  std::vector<TextureRef> textures = std::move(d.reused_textures);
  size_t nt = 0;
  for (uint32_t u = 0; u < kMaxUnits; u++) {
    if (!s.tex_enabled[u]) continue;
    if (const TextureRef* t = Texture(s.textures[u], base)) {
      if (nt < textures.size())
        textures[nt] = *t;
      else
        textures.push_back(*t);
      TextureRef& r = textures[nt++];
      r.unit = u;
      std::memcpy(r.address, s.address[u], 3);
      r.border = s.border[u];
    }
  }
  textures.resize(nt);
  d.textures = std::move(textures);
  f.tex_ms += TicksToMs(FastTicks() - tt);
  std::memcpy(d.blend, s.blend, sizeof(d.blend));
  d.cull = s.cull;
  d.depth_check = s.depth_check;
  d.depth_write = s.depth_write;
  d.depth_func = s.depth_func;
  std::memcpy(d.mask, s.mask, sizeof(d.mask));
  d.alpha_func = s.alpha_func;
  d.alpha_value = s.alpha_value;
  d.depth_bias = s.depth_bias;
  d.depth_slope_bias = s.depth_slope_bias;
  // Valeurs des constantes au moment du dessin, copiées dans l'image ; table des noms partagée.
  for (int k = 0; k < 2; k++) {
    if (!s.consts[k]) continue;
    if (f.consts_used == f.consts.size()) f.consts.emplace_back();
    Constants& c = f.consts[f.consts_used++];
    // Octets bruts : conversion en flottants sur le thread de rendu (ProcessFrame).
    const uint32_t n = s.consts[k]->floats_count;
    uint8_t* raw = n ? f.arena.Alloc(size_t(n) * 4) : nullptr;
    if (n) std::memcpy(raw, base + s.consts[k]->floats_addr, size_t(n) * 4);
    c.raw_be = raw;
    c.raw_count = n;
    c.floats.clear();
    if (c.shared_names) c.shared_names.reset();
    c.names_ptr = s.consts[k]->names.get();  // table gardée en vie par g_constants (remplacée : libération différée)
    d.consts[k] = &c;
  }
  if (d.ffp) {
    std::memcpy(d.matrix, s.matrix, sizeof(d.matrix));
    for (auto& unit : d.stage)
      for (auto& st : unit) st = FfpStage();
    for (int u = 0; u < 4; u++)
      for (int t = 0; t < 2; t++) {
        if (!s.blend_set[u][t]) continue;
        const uint32_t* w = s.blend_mode[u][t];
        FfpStage& st = d.stage[u][t];
        st.set = true;
        st.op = w[1];
        st.src1 = w[2];
        st.src2 = w[3];
        std::memcpy(st.arg, &w[4], sizeof(st.arg));
      }
  }
  if (!CopyGeometry(d)) return;
  if (torchlight::capture::Recording()) Journal(d);
  f.commands.push_back({Command::kDraw, uint32_t(f.draws_used)});
  f.draws_used++;
}

}  // namespace torchlight::natif
