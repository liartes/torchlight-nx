// torchlight - capture d'une image complète du rendu d'Ogre (voir torchlight_capture.h).
//
// Disposition des objets : switch/RENDU_NATIF.md (phase 1).

#include "torchlight_capture.h"
#include "torchlight_invite.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_DOUBLE(torchlight_capture_s, 0.0, "Torchlight",
                      "Capturer l'image qui commence après ce nombre de secondes dans trace/capture/ (0 = jamais)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace torchlight::capture {
namespace {

namespace xg = rex::graphics;
using namespace torchlight::invite;
using Clock = std::chrono::steady_clock;

constexpr int kMaxUnits = 16;

std::mutex g_mutex;
std::atomic<bool> g_enabled{false};  // lu sans verrou : sortie rapide si la capture est inactive
double g_at = 0.0;
Clock::time_point g_start;
std::filesystem::path g_dir;
enum class Phase { kWaiting, kRecording, kDone } g_phase = Phase::kWaiting;
FILE* g_frame = nullptr;
int g_draw = 0;
std::unordered_set<std::string> g_written;
std::function<void(const std::filesystem::path&)> g_screenshot;

struct State {
  uint32_t textures[kMaxUnits] = {};
  bool tex_enabled[kMaxUnits] = {};
  uint32_t blend[6] = {0, 1, 0, 0, 1, 0};  // src, dst, op, src_a, dst_a, op_a (ONE, ZERO, ADD)
  bool separate = false;
  uint32_t cull = 2;  // CULL_CLOCKWISE
  bool depth_check = true, depth_write = true;
  uint32_t depth_func = 3;  // CMPF_LESS_EQUAL (CompareFunction d'Ogre : 0 jamais, 1 toujours, 2 <, 3 <=...)
  bool mask[4] = {true, true, true, true};
  uint32_t alpha_func = 1, alpha_value = 0;  // CMPF_ALWAYS_PASS
  bool scissor = false;
  uint32_t scissor_rect[4] = {};
  uint32_t program[2] = {};
  uint64_t program_hash[2] = {};
  std::string consts[2];
  float matrix[3][16] = {};
  uint32_t blend_mode[kMaxUnits][2][15] = {};  // [unité][0 = couleur, 1 = alpha][LayerBlendModeEx]
  bool blend_set[kMaxUnits][2] = {};
} g_state;

std::string Hex(uint64_t v, int digits = 8) {
  char b[24];
  std::snprintf(b, sizeof(b), "%0*" PRIx64, digits, v);
  return b;
}

bool WriteFile(const std::string& name, const void* data, size_t size) {
  if (!g_written.insert(name).second) return true;
  FILE* f = std::fopen((g_dir / name).string().c_str(), "wb");
  if (!f) return false;
  std::fwrite(data, 1, size, f);
  std::fclose(f);
  return true;
}

// Texture Ogre -> fetch constant -> niveau de base détilé, octets dans l'ordre de l'hôte.
// Renvoie la ligne « tex » de frame.txt, ou une ligne de diagnostic.
std::string CaptureTexture(uint32_t unit, uint32_t tex, const uint8_t* base) {
  const std::string prefix = "tex " + std::to_string(unit) + " ";
  if (!Ptr(tex, 0x130)) return prefix + "illisible";
  const uint32_t res = DeviceMapValue(base, Load32(base, tex + 0x128));
  if (!Ptr(res, 0x14)) return prefix + "sans-ressources";
  uint32_t d3d = Load32(base, res + 0xC);  // pBaseTex
  if (!Ptr(d3d, 0x34)) return prefix + "sans-objet-d3d";
  xg::xenos::xe_gpu_texture_fetch_t fetch{};
  uint32_t* words = reinterpret_cast<uint32_t*>(&fetch);
  for (int k = 0; k < 6; k++) words[k] = Load32(base, d3d + 0x1C + k * 4);
  xg::TextureInfo info;
  if (!xg::TextureInfo::Prepare(fetch, &info)) return prefix + "fetch-invalide " + Hex(words[0]) + Hex(words[1]);
  const xg::FormatInfo* fi = info.format_info();
  const bool cube = info.dimension == xg::xenos::DataDimension::kCube;
  if (!fi || (!cube && info.dimension != xg::xenos::DataDimension::k2DOrStacked))
    return prefix + "format-non-gere " + std::to_string(uint32_t(info.format)) + "-dim" +
           std::to_string(uint32_t(info.dimension));
  // TextureInfo garde la convention de xenia : width/height = dimension - 1.
  const uint32_t width = info.width + 1, height = info.height + 1;
  const uint32_t faces = cube ? 6 : 1;
  const uint32_t bpb = fi->bytes_per_block();
  const uint32_t bw = (width + fi->block_width - 1) / fi->block_width;
  const uint32_t bh = (height + fi->block_height - 1) / fi->block_height;
  const uint8_t* src = Physical(info.memory.base_address);
  if (!src || !bw || !bh || !bpb) return prefix + "vide";
  // Disposition en mémoire du jeu (pas des rangées, écart entre les faces) : même calcul que l'émulation.
  const auto layout = xg::texture_util::GetGuestTextureLayout(info.dimension, fetch.pitch, width, height, faces,
                                                               info.is_tiled, info.format, info.has_packed_mips,
                                                               true, 0);
  const uint32_t pitch_blocks = (fetch.pitch * 32 + fi->block_width - 1) / fi->block_width;
  uint32_t log2_bpb = 0;
  while ((1u << (log2_bpb + 1)) <= bpb) log2_bpb++;
  // Côté le plus court <= 16 texels avec mips compactées : le niveau de base est dans le bloc des mips compactées,
  // décalé (16x16 : 16 texels en X ; 32x16 : 16 texels en Y), comme le lit l'émulation.
  uint32_t px = 0, py = 0, pz = 0;
  if (info.has_packed_mips)
    xg::texture_util::GetPackedMipOffset(width, height, 1, info.format, 0, px, py, pz);
  std::vector<uint8_t> out(size_t(bw) * bh * bpb * faces);
  for (uint32_t f = 0; f < faces; f++) {
    const uint8_t* face = src + size_t(f) * layout.base.array_slice_stride_bytes;
    uint8_t* dst = out.data() + size_t(f) * bw * bh * bpb;
    for (uint32_t y = 0; y < bh; y++) {
      for (uint32_t x = 0; x < bw; x++) {
        const uint32_t gx = x + px, gy = y + py;
        const size_t off = info.is_tiled
                               ? size_t(xg::texture_util::GetTiledOffset2D(int32_t(gx), int32_t(gy), pitch_blocks, log2_bpb))
                               : size_t(gy) * layout.base.row_pitch_bytes + size_t(gx) * bpb;
        xg::texture_conversion::CopySwapBlock(info.endianness, &dst[(size_t(y) * bw + x) * bpb], face + off, bpb);
      }
    }
  }
  const std::string name = "tex_" + Hex(info.memory.base_address) + "_" + std::to_string(uint32_t(info.format)) +
                           "_" + std::to_string(width) + "x" + std::to_string(height) + (cube ? "_cube" : "") + ".bin";
  WriteFile(name, out.data(), out.size());
  const std::string ogre_name = LoadStdString(base, tex + 0x2C);
  return prefix + name + " " + std::to_string(uint32_t(info.format)) + " " + std::to_string(width) + " " +
         std::to_string(height) + " " + std::to_string(fi->block_width) + " " + std::to_string(fi->block_height) + " " +
         std::to_string(bpb) + " " + (info.is_tiled ? "1" : "0") + (cube ? " cube" : (info.has_packed_mips ? " packed" : " -")) +
         " " + (ogre_name.empty() ? "?" : ogre_name);
}

// GpuProgramParameters : mFloatConstants (vector<float>) en +0 ; mNamedConstants (SharedPtr) pRep en +0x44
// -> GpuNamedConstants { floatBufferSize, intBufferSize, ?, map (en-tête +0xC, taille +0x10) } ;
// valeur de la map : pair<String (0x1C), GpuConstantDefinition { type, physicalIndex, logicalIndex,
// elementSize, arraySize, variability }>.
std::string CaptureConstants(uint32_t params, const uint8_t* base) {
  if (!Ptr(params, 0x60)) return "illisible";
  std::string text;
  const uint32_t begin = Load32(base, params), end = Load32(base, params + 4);
  const uint32_t count = (end >= begin && end - begin < 0x10000) ? (end - begin) / 4 : 0;
  text += "floats " + std::to_string(count) + "\n";
  if (count && Readable(begin, count * 4)) {
    for (uint32_t k = 0; k < count; k++) {
      const uint32_t w = Load32(base, begin + k * 4);
      float f;
      std::memcpy(&f, &w, 4);
      char b[32];
      std::snprintf(b, sizeof(b), "%.9g%s", f, (k % 4 == 3 || k + 1 == count) ? "\n" : " ");
      text += b;
    }
  }
  const uint32_t named = Load32(base, params + 0x44);
  if (Ptr(named, 0x20)) {
    WalkMap(base, Load32(base, named + 0xC), 256, [&](uint32_t val) {
      if (!Readable(val, 0x34)) return;
      const std::string name = LoadStdString(base, val);
      if (name.empty() || name.back() == ']') return;  // entrées « nom[0] » redondantes
      text += "name " + name + " " + std::to_string(Load32(base, val + 0x1C)) + " " +
              std::to_string(Load32(base, val + 0x20)) + " " + std::to_string(Load32(base, val + 0x28)) + " " +
              std::to_string(Load32(base, val + 0x2C)) + "\n";
    });
  }
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
  const std::string file = "cst_" + Hex(h, 16) + ".txt";
  WriteFile(file, text.data(), text.size());
  return file;
}

std::string CaptureVertexBuffers(uint32_t binding, const uint8_t* base) {
  std::string lines;
  if (!Ptr(binding, 0x10)) return "vb illisible\n";
  // mBindingMap : std::map<ushort, HardwareVertexBufferSharedPtr>, en-tête en +8.
  WalkMap(base, Load32(base, binding + 8), 16, [&](uint32_t val) {
    const uint32_t source = Load32(base, val) >> 16;  // ushort big-endian
    const uint32_t vb = Load32(base, val + 8);        // SharedPtr { vtable +4, pRep +8 } depuis val
    if (!Ptr(vb, 0x40)) {
      lines += "vb " + std::to_string(source) + " illisible\n";
      return;
    }
    const uint32_t size = Load32(base, vb + 4);
    const uint32_t count = Load32(base, vb + 0x20);
    const uint32_t stride = Load32(base, vb + 0x24);
    const uint32_t res = DeviceMapValue(base, Load32(base, vb + 0x38));
    const uint32_t d3d = Ptr(res, 4) ? Load32(base, res) : 0;
    if (!Ptr(d3d, 0x20)) {
      lines += "vb " + std::to_string(source) + " sans-objet-d3d\n";
      return;
    }
    const uint32_t addr = Load32(base, d3d + 0x18) & ~3u;  // mot 0 : type en bits 0-1
    const uint8_t* src = Physical(addr);
    const std::string name = "vb_" + Hex(addr) + "_" + Hex(size) + ".bin";
    if (src && size) WriteFile(name, src, size);
    lines += "vb " + std::to_string(source) + " " + name + " " + std::to_string(stride) + " " +
             std::to_string(count) + "\n";
  });
  return lines;
}

}  // namespace

bool Enabled() { return REXCVAR_GET(torchlight_capture_s) > 0.0; }

bool Recording() {
  if (!g_enabled) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_phase == Phase::kRecording;
}

void SetScreenshot(std::function<void(const std::filesystem::path&)> screenshot) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_screenshot = std::move(screenshot);
}

void Start(const std::filesystem::path& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_enabled = Enabled();
  g_at = REXCVAR_GET(torchlight_capture_s);
  g_start = Clock::now();
  g_dir = dir;
}

void BeginGameFrame() {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled || g_phase != Phase::kWaiting) return;
  if (std::chrono::duration<double>(Clock::now() - g_start).count() < g_at) return;
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  g_frame = std::fopen((g_dir / "frame.txt").string().c_str(), "w");
  if (!g_frame) {
    g_phase = Phase::kDone;
    return;
  }
  std::fprintf(g_frame,
               "# capture torchlight 1 : un bloc « draw » par appel à _render, dans l'ordre.\n"
               "# prog <hash vs> <hash ps> | op <type Ogre> <indexé> <vertexStart> <vertexCount> <indexStart> "
               "<indexCount>\n"
               "# elem <source> <offset> <VertexElementType> <VertexElementSemantic> <index>\n"
               "# vb <source> <fichier> <taille sommet> <nombre> | ib <fichier> <taille indice> <nombre>\n"
               "# tex <unité> <fichier> <format Xenos> <l> <h> <bloc l> <bloc h> <octets/bloc> <tuilé> <mips "
               "compactées> <nom Ogre>\n"
               "# state blend <src> <dst> <op> <srcA> <dstA> <opA> <séparé> cull <m> depth <test> <écriture> "
               "<fonction> mask <rgba> alpha <fonction> <valeur> scissor <actif> <g> <h> <d> <b>\n"
               "# consts <vs|ps> <fichier>\n"
               "# target <objet> <vtable> <nom> raw <mots...> : changement de cible (RenderTarget d'Ogre)\n"
               "# clear <FBT : 1 couleur, 2 profondeur, 4 stencil> <r> <g> <b> <a> <profondeur> <stencil>\n"
               "# begin_frame / end_frame : bornes d'une cible ; viewport <objet> raw <mots...>\n"
               "# ffp_matrix <world|view|proj> <16 flottants, Matrix4 d'Ogre par lignes> (dessins sans programme)\n"
               "# ffp_stage <unité> <colour|alpha> <opération> <source1> <source2> <colourArg1 rgba> <colourArg2 rgba> "
               "<alphaArg1> <alphaArg2> <factor>\n");
  g_phase = Phase::kRecording;
  g_draw = 0;
  REXLOG_INFO("[capture] début de l'image, dans {}", g_dir.string());
}

void BeginFrame() {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase == Phase::kRecording) std::fprintf(g_frame, "begin_frame\n");
}

void EndFrame() {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase == Phase::kRecording) std::fprintf(g_frame, "end_frame\n");
}

// RenderTarget : vtable +0, mName (std::string) +4. Le reste de la disposition est encore à établir :
// les premiers mots sont écrits bruts (« raw ») pour la trouver.
void SetRenderTarget(uint32_t target, const uint8_t* base) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase != Phase::kRecording) return;
  if (!Ptr(target, 0x80)) {
    std::fprintf(g_frame, "target %08X illisible\n", target);
    return;
  }
  std::string name = LoadStdString(base, target + 4);
  for (char& c : name)
    if (c == ' ') c = '_';
  std::fprintf(g_frame, "target %08X %08X %s raw", target, Load32(base, target), name.empty() ? "?" : name.c_str());
  for (uint32_t o = 0; o < 0x80; o += 4) std::fprintf(g_frame, " %08X", Load32(base, target + o));
  std::fprintf(g_frame, "\n");
}

void SetViewport(uint32_t viewport, const uint8_t* base) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase != Phase::kRecording || !Ptr(viewport, 0x60)) return;
  std::fprintf(g_frame, "viewport %08X raw", viewport);
  for (uint32_t o = 0; o < 0x60; o += 4) std::fprintf(g_frame, " %08X", Load32(base, viewport + o));
  std::fprintf(g_frame, "\n");
}

void Clear(uint32_t buffers, const float colour[4], double depth, uint32_t stencil) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase != Phase::kRecording) return;
  std::fprintf(g_frame, "clear %u %.6g %.6g %.6g %.6g %.9g %u\n", buffers, colour[0], colour[1], colour[2], colour[3],
               depth, stencil & 0xFFFF);
}

void EndGameFrame() {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase != Phase::kRecording) return;
  std::fprintf(g_frame, "end %d\n", g_draw);
  std::fclose(g_frame);
  g_frame = nullptr;
  g_phase = Phase::kDone;
  REXLOG_INFO("[capture] terminée : {} dessins", g_draw);
  // L'émulation présente l'image avec un peu de retard : capture d'écran 0,5 s plus tard, sur un autre thread
  // (le presenter prend ses propres verrous).
  if (g_screenshot) {
    std::thread([shot = g_screenshot, path = g_dir / "emulation.ppm"] {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      shot(path);
    }).detach();
  }
}

void SetTexture(uint32_t unit, bool enabled, uint32_t texture) {
  if (unit >= kMaxUnits) return;
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.textures[unit] = texture;
  g_state.tex_enabled[unit] = enabled && texture;
}

void SetSceneBlending(uint32_t src, uint32_t dst, uint32_t op) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.blend[0] = g_state.blend[3] = src;
  g_state.blend[1] = g_state.blend[4] = dst;
  g_state.blend[2] = g_state.blend[5] = op;
  g_state.separate = false;
}

void SetSeparateSceneBlending(uint32_t src, uint32_t dst, uint32_t src_a, uint32_t dst_a, uint32_t op,
                              uint32_t op_a) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.blend[0] = src;
  g_state.blend[1] = dst;
  g_state.blend[2] = op;
  g_state.blend[3] = src_a;
  g_state.blend[4] = dst_a;
  g_state.blend[5] = op_a;
  g_state.separate = true;
}

void SetCullingMode(uint32_t mode) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.cull = mode;
}
void SetDepthCheck(bool enabled) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.depth_check = enabled;
}
void SetDepthWrite(bool enabled) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.depth_write = enabled;
}
void SetDepthFunction(uint32_t func) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.depth_func = func;
}
void SetColourWrite(bool r, bool g, bool b, bool a) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.mask[0] = r;
  g_state.mask[1] = g;
  g_state.mask[2] = b;
  g_state.mask[3] = a;
}
void SetAlphaReject(uint32_t func, uint32_t value) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.alpha_func = func;
  g_state.alpha_value = value;
}
void SetScissor(bool enabled, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.scissor = enabled;
  g_state.scissor_rect[0] = left;
  g_state.scissor_rect[1] = top;
  g_state.scissor_rect[2] = right;
  g_state.scissor_rect[3] = bottom;
}

void BindProgram(bool vertex, uint32_t program, uint64_t hash) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.program[vertex ? 0 : 1] = program;
  g_state.program_hash[vertex ? 0 : 1] = hash;
}

void UnbindProgram(uint32_t type) {
  if (type > 1) return;
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.program[type] = 0;
  g_state.program_hash[type] = 0;
}

void SetMatrix(int kind, const float m[16]) {
  if (kind < 0 || kind > 2) return;
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::memcpy(g_state.matrix[kind], m, sizeof(g_state.matrix[kind]));
}

void SetTextureBlendMode(uint32_t unit, const uint32_t words[15]) {
  if (unit >= kMaxUnits) return;
  const uint32_t type = words[0] ? 1 : 0;  // LBT_COLOUR = 0, LBT_ALPHA = 1
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::memcpy(g_state.blend_mode[unit][type], words, sizeof(g_state.blend_mode[unit][type]));
  g_state.blend_set[unit][type] = true;
}

void DisableTextureUnit(uint32_t unit) {
  if (unit >= kMaxUnits) return;
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.tex_enabled[unit] = false;
}

void DisableTextureUnitsFrom(uint32_t unit) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (uint32_t u = unit; u < kMaxUnits; u++) g_state.tex_enabled[u] = false;
}

void BindParameters(uint32_t type, uint32_t params, const uint8_t* base) {
  if (type > 1) return;
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase != Phase::kRecording) return;
  g_state.consts[type] = CaptureConstants(params, base);
}

void Render(uint32_t op, const uint8_t* base) {
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_phase != Phase::kRecording || !Ptr(op, 0x18)) return;
  const State& s = g_state;
  FILE* f = g_frame;
  std::fprintf(f, "draw %d\n", g_draw++);
  std::fprintf(f, "prog %s %s\n", Hex(s.program_hash[0], 16).c_str(), Hex(s.program_hash[1], 16).c_str());

  // RenderOperation { VertexData* +0, type +4, bool useIndexes +8, IndexData* +0xC }
  const uint32_t vd = Load32(base, op);
  const uint32_t type = Load32(base, op + 4);
  const bool indexed = (Load32(base, op + 8) >> 24) != 0;
  const uint32_t id = Load32(base, op + 0xC);
  uint32_t vstart = 0, vcount = 0, istart = 0, icount = 0;
  if (Ptr(vd, 0x18)) {
    vstart = Load32(base, vd + 0x10);
    vcount = Load32(base, vd + 0x14);
  }
  if (indexed && Ptr(id, 0x18)) {
    istart = Load32(base, id + 0x10);
    icount = Load32(base, id + 0x14);
  }
  std::fprintf(f, "op %u %d %u %u %u %u\n", type, indexed ? 1 : 0, vstart, vcount, istart, icount);

  if (Ptr(vd, 0x18)) {
    // VertexDeclaration : éléments dans un std::vector (+4 début, +8 fin), 0x14 octets chacun.
    const uint32_t decl = Load32(base, vd + 4);
    if (Ptr(decl, 0x10)) {
      const uint32_t b = Load32(base, decl + 4), e = Load32(base, decl + 8);
      if (e > b && e - b <= 32 * 0x14 && Readable(b, e - b)) {
        for (uint32_t p = b; p < e; p += 0x14) {
          std::fprintf(f, "elem %u %u %u %u %u\n", Load32(base, p) >> 16, Load32(base, p + 4),
                       Load32(base, p + 8), Load32(base, p + 0xC), Load32(base, p + 0x10) >> 16);
        }
      }
    }
    std::fputs(CaptureVertexBuffers(Load32(base, vd + 8), base).c_str(), f);
  }

  if (indexed && Ptr(id, 0x18)) {
    const uint32_t ib = Load32(base, id + 4);
    if (Ptr(ib, 0x40)) {
      const uint32_t size = Load32(base, ib + 4);
      const uint32_t isize = Load32(base, ib + 0x28);
      const uint32_t n = Load32(base, ib + 0x24);
      const uint32_t res = DeviceMapValue(base, Load32(base, ib + 0x34));
      const uint32_t d3d = Ptr(res, 4) ? Load32(base, res) : 0;
      if (Ptr(d3d, 0x20)) {
        const uint32_t addr = Load32(base, d3d + 0x18);
        const std::string name = "ib_" + Hex(addr) + "_" + Hex(size) + ".bin";
        if (const uint8_t* src = Physical(addr)) WriteFile(name, src, size);
        std::fprintf(f, "ib %s %u %u\n", name.c_str(), isize, n);
      } else {
        std::fprintf(f, "ib sans-objet-d3d\n");
      }
    }
  }

  for (uint32_t u = 0; u < kMaxUnits; u++) {
    if (s.tex_enabled[u]) std::fprintf(f, "%s\n", CaptureTexture(u, s.textures[u], base).c_str());
  }
  std::fprintf(f, "state blend %u %u %u %u %u %u %d cull %u depth %d %d %u mask %d%d%d%d alpha %u %u scissor %d %u %u %u %u\n",
               s.blend[0], s.blend[1], s.blend[2], s.blend[3], s.blend[4], s.blend[5], s.separate ? 1 : 0, s.cull,
               s.depth_check ? 1 : 0, s.depth_write ? 1 : 0, s.depth_func, s.mask[0], s.mask[1], s.mask[2],
               s.mask[3], s.alpha_func, s.alpha_value, s.scissor ? 1 : 0, s.scissor_rect[0], s.scissor_rect[1],
               s.scissor_rect[2], s.scissor_rect[3]);
  // Pipeline fixe : matrices et modes de combinaison (pour les dessins sans programme).
  if (!s.program_hash[0] || !s.program_hash[1]) {
    static const char* kMat[3] = {"world", "view", "proj"};
    for (int k = 0; k < 3; k++) {
      std::fprintf(f, "ffp_matrix %s", kMat[k]);
      for (float v : s.matrix[k]) std::fprintf(f, " %.9g", v);
      std::fprintf(f, "\n");
    }
    for (uint32_t u = 0; u < 4; u++) {
      for (int t = 0; t < 2; t++) {
        if (!s.blend_set[u][t]) continue;
        const uint32_t* w = s.blend_mode[u][t];
        float fl[11];
        std::memcpy(fl, &w[4], sizeof(fl));
        std::fprintf(f, "ffp_stage %u %s %u %u %u", u, t ? "alpha" : "colour", w[1], w[2], w[3]);
        for (float v : fl) std::fprintf(f, " %.6g", v);
        std::fprintf(f, "\n");
      }
    }
  }
  if (!s.consts[0].empty()) std::fprintf(f, "consts vs %s\n", s.consts[0].c_str());
  if (!s.consts[1].empty()) std::fprintf(f, "consts ps %s\n", s.consts[1].c_str());
}

}  // namespace torchlight::capture
