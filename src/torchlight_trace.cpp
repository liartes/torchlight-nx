// torchlight - traceur du rendu (phase 1 du rendu natif, voir switch/RENDU_NATIF.md)
//
// Ne change rien au rendu : chaque fonction interceptée appelle l'original.
// Chaque fonction recompilée sub_XXXXXXXX est un alias faible de __imp__sub_XXXXXXXX ;
// la définition forte ci-dessous l'emporte pour tous les appels, virtuels compris.
//
// Activé par --torchlight_trace=true (ou torchlight_trace = true dans torchlight.toml).
// Écrit dans <dossier de l'exécutable>/trace/ :
//   hlsl/<empreinte>_<profil>_<entrée>.hlsl   chaque source HLSL compilée par le jeu (une fois)
//   rendu.log                                  compteurs toutes les 10 s, premiers appels détaillés

#include <atomic>
#if defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "torchlight_capture.h"
#include "natif/d3dx_cache.h"
#include "natif/hlsl_prep.h"
#include "torchlight_invite.h"
#include "torchlight_natif.h"

REXCVAR_DEFINE_BOOL(torchlight_trace, false, "Torchlight",
                    "Tracer le rendu (appels du D3D9RenderSystem d'Ogre, sources HLSL) dans trace/")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(torchlight_trace_objets, false, "Torchlight",
                    "Vidages d'objets pour la cartographie (trace/objets.txt) : lourds, à n'activer qu'au besoin")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace torchlight::trace {
namespace {

enum Fn : int {
  kCompileShader,
  kSetTexture,
  kSetSceneBlending,
  kBeginFrame,
  kEndFrame,
  kSetViewport,
  kSetCullingMode,
  kSetDepthBufferParams,
  kSetVertexDeclaration,
  kSetVertexBufferBinding,
  kRender,
  kBindGpuProgram,
  kBindGpuProgramParameters,
  kClearFrameBuffer,
  kSetRenderTarget,
  kSetSeparateSceneBlending,
  kSetAlphaReject,
  kSetDepthCheck,
  kSetDepthWrite,
  kSetDepthFunction,
  kSetColourWrite,
  kSetScissor,
  kCount
};

constexpr const char* kNames[kCount] = {
    "D3DXCompileShader",        "_setTexture",          "_setSceneBlending",
    "_beginFrame",              "_endFrame",            "_setViewport",
    "_setCullingMode",          "_setDepthBufferParams", "setVertexDeclaration",
    "setVertexBufferBinding",   "_render",              "bindGpuProgram",
    "bindGpuProgramParameters", "clearFrameBuffer",     "_setRenderTarget",
    "_setSeparateSceneBlending", "_setAlphaReject",      "_setDepthCheck",
    "_setDepthWrite",           "_setDepthFunction",    "_setColourWrite",
    "setScissorTest",
};

constexpr int kDetail = 6;          // appels détaillés par fonction
constexpr int kReportSeconds = 10;

std::atomic<uint64_t> g_calls[kCount];
std::atomic<int> g_detailed[kCount];
std::once_flag g_start;
bool g_on = false;
std::filesystem::path g_dir;
std::mutex g_file_mutex;
FILE* g_log = nullptr;  // rendu.log, seulement avec le traceur ou la capture
#define TL_LOG(...)                                   \
  do {                                                \
    if (g_log) std::fprintf(g_log, __VA_ARGS__);      \
  } while (0)
// Fichiers de trace (rendu.log, sources HLSL) : seulement si le traceur ou la capture sont demandés. Le rendu
// natif seul a besoin des interceptions (empreintes des programmes), pas des fichiers.
bool g_write = false;
std::unordered_set<uint64_t> g_hlsl_seen;
// D3D9HLSLProgram en cours de compilation (loadFromSource), puis objet HLSL -> empreinte de sa source.
thread_local uint32_t t_hlsl_object = 0;
std::unordered_map<uint32_t, uint64_t> g_hlsl_objects;
std::unordered_set<uint32_t> g_programs_linked;
std::unordered_map<uint32_t, uint64_t> g_program_hash;  // programme assembleur -> source HLSL
std::chrono::steady_clock::time_point g_last_report;
uint64_t g_last_calls[kCount];
std::atomic<uint64_t> g_slot_calls[128];
uint64_t g_last_slot_calls[128];
// Objets distincts vus (programmes GPU liés, textures, déclarations de sommets).
std::mutex g_seen_mutex;
std::unordered_set<uint32_t> g_programs, g_textures, g_decls;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + addr, &value, 4);
}

std::string LoadCString(const uint8_t* base, uint32_t addr, size_t max = 128) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; i++) {
    const char c = static_cast<char>(base[addr + i]);
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

void Start() {
  g_write = REXCVAR_GET(torchlight_trace) || torchlight::capture::Enabled();
  g_on = g_write || torchlight::natif::Enabled();
  if (!g_write) return;
  g_dir = rex::filesystem::GetExecutableFolder() / "trace";
  torchlight::capture::Start(g_dir / "capture");
  std::error_code ec;
  std::filesystem::create_directories(g_dir / "hlsl", ec);
  g_log = std::fopen((g_dir / "rendu.log").string().c_str(), "w");
  if (g_log) std::setvbuf(g_log, nullptr, _IOLBF, 0);  // lisible même si le jeu est tué
  g_last_report = std::chrono::steady_clock::now();
  REXLOG_INFO("[trace] actif, sortie dans {}", g_dir.string());
}

// Appelé à chaque interception : après le démarrage, une simple lecture (pas de call_once, coûteux sur la console).
std::atomic<bool> g_started{false};
[[gnu::noinline]] void StartOnce() {
  std::call_once(g_start, Start);
  g_started.store(true, std::memory_order_release);
}

// Appelée à chaque interception du RenderSystem : intégrée (le profil de la console la montrait à 0,15 ms par
// image hors intégration).
[[gnu::always_inline]] inline bool On() {
  if (!g_started.load(std::memory_order_acquire)) [[unlikely]] StartOnce();
  return g_on && (g_log || !g_write);
}

void LinkProgram(const uint8_t* base, uint32_t prog);

// Empreinte de la source HLSL d'un programme lié (thread de rendu d'Ogre) : cache sans verrou devant g_program_hash.
bool HeapPtr(uint32_t v);

// Programme de sommets ? (vtable D3D9GpuVertexProgram).
bool ProgramIsVertex(const uint8_t* base, uint32_t prog) {
  return HeapPtr(prog) && Load32(base, prog) == 0x82004264u;
}

uint64_t ProgramHashSlow(const uint8_t* base, uint32_t prog) {
  std::lock_guard<std::mutex> lock(g_file_mutex);
  LinkProgram(base, prog);
  auto h = g_program_hash.find(prog);
  return h == g_program_hash.end() ? 0 : h->second;
}

// Empreinte de la source HLSL d'un programme lié et son type, à chaque liaison (~600 par image en ville) : table
// plate par thread (adressage ouvert, une ligne de cache par recherche) au lieu d'une unordered_map et d'une
// vérification de pages ; ProgramHashSlow (verrou, liaison) seulement la première fois.
struct ProgramInfo {
  uint32_t prog;
  bool vertex;
  uint64_t hash;
};
ProgramInfo LookupProgram(const uint8_t* base, uint32_t prog) {
  constexpr uint32_t kSlots = 4096, kProbe = 32;
  // Pointeur brut (type trivial) : un thread_local non trivial appelle sa routine d'initialisation à chaque accès.
  thread_local ProgramInfo* table = nullptr;
  if (!table) table = new ProgramInfo[kSlots]();
  uint32_t i = (prog >> 2) * 0x9E3779B1u >> 20;
  for (uint32_t k = 0; k < kProbe; k++, i = (i + 1) & (kSlots - 1)) {
    ProgramInfo& e = table[i];
    if (e.prog == prog && prog) return e;
    if (e.prog) continue;
    const uint64_t hash = ProgramHashSlow(base, prog);
    const ProgramInfo info{prog, ProgramIsVertex(base, prog), hash};
    if (hash) e = info;  // programme pas encore lié : réessayé à la prochaine liaison
    return info;
  }
  return {prog, ProgramIsVertex(base, prog), ProgramHashSlow(base, prog)};
}

uint64_t ProgramHash(const uint8_t* base, uint32_t prog) { return LookupProgram(base, prog).hash; }

void Report() {
  if (!g_log) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_report < std::chrono::seconds(kReportSeconds)) return;
  const double s = std::chrono::duration<double>(now - g_last_report).count();
  g_last_report = now;
  uint64_t cur[kCount];
  for (int i = 0; i < kCount; i++) cur[i] = g_calls[i].load(std::memory_order_relaxed);
  const double frames = double(cur[kEndFrame] - g_last_calls[kEndFrame]);
  TL_LOG("==== %.1f s, %.0f _endFrame (%.1f/s)", s, frames, frames / s);
  {
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    TL_LOG(" | distincts : %zu programmes GPU, %zu textures, %zu declarations de sommets\n",
                 g_programs.size(), g_textures.size(), g_decls.size());
  }
  for (int i = 0; i < kCount; i++) {
    const uint64_t d = cur[i] - g_last_calls[i];
    if (!d && !cur[i]) continue;
    TL_LOG("  %-26s %8llu  (%.1f par image, total %llu)\n", kNames[i],
                 (unsigned long long)d, frames > 0 ? double(d) / frames : 0.0,
                 (unsigned long long)cur[i]);
    g_last_calls[i] = cur[i];
  }
  for (int i = 0; i < 128; i++) {
    const uint64_t c = g_slot_calls[i].load(std::memory_order_relaxed);
    if (c == g_last_slot_calls[i]) continue;
    TL_LOG("  case %3d                   %8llu  (%.1f par image)\n", i,
                 (unsigned long long)(c - g_last_slot_calls[i]), frames > 0 ? double(c - g_last_slot_calls[i]) / frames : 0.0);
    g_last_slot_calls[i] = c;
  }
  if (g_log) std::fflush(g_log);
}

// Vidage d'objets pour la cartographie (textures, liaisons de tampons de sommets) : l'objet, puis
// un niveau d'objets pointés. On ne suit que les pointeurs dans le tas virtuel 0x40000000-0x50000000,
// où vivent les objets Ogre vus jusqu'ici : lire de la mémoire invitée non validée ferait planter l'hôte.
constexpr int kDumpObjects = 6;
FILE* g_dump = nullptr;
std::unordered_set<uint32_t> g_dumped;
int g_dump_count[kCount];

// Une lecture de mémoire invitée non validée bloque le thread (gestionnaire d'exceptions du runtime) :
// toute lecture du traceur au-delà des arguments vérifie d'abord la plage.
bool Readable(uint32_t addr, uint32_t len) {
  auto* mem = rex::system::kernel_memory();
  if (!mem || !len) return false;
#if defined(__linux__)
  // Sans verrou : process_vm_readv sur notre propre processus échoue (EFAULT) au lieu de provoquer une
  // faute si la mémoire n'est pas lisible. L'ancien test (BaseHeap::QueryRangeAccess) prenait le verrou
  // global du runtime alors que nos propres verrous étaient tenus : blocage du thread de rendu (inversion).
  static thread_local std::vector<uint8_t> scratch;
  if (scratch.size() < len) scratch.resize(len);
  iovec local{scratch.data(), len};
  iovec remote{mem->TranslateVirtual<void*>(addr), len};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(len);
#else
  auto* heap = mem->LookupHeap(addr);
  if (!heap || mem->LookupHeap(addr + len - 1) != heap) return false;
  return heap->QueryRangeAccess(addr, addr + len - 1) != rex::memory::PageAccess::kNoAccess;
#endif
}

bool HeapPtr(uint32_t v) {
  // Tas (0x4xxxxxxx) et piles des threads invités (0x7xxxxxxx : Ogre met les RenderOperation sur la pile). Pages
  // déjà vérifiées retenues (invite::Readable) : la vérification complète (verrou du tas sur la console) à chaque
  // appel coûtait ~0,2 ms par image.
  return torchlight::invite::Ptr(v, 0x60);
}

void DumpWords(const uint8_t* base, const char* label, uint32_t addr, uint32_t bytes) {
  if (!Readable(addr, bytes)) {
    std::fprintf(g_dump, "%s %08X: (0x%X octets non lisibles)\n", label, addr, bytes);
    return;
  }
  std::fprintf(g_dump, "%s %08X:", label, addr);
  for (uint32_t o = 0; o < bytes; o += 4) {
    if ((o & 0x1F) == 0) std::fprintf(g_dump, "\n  +%03X", o);
    std::fprintf(g_dump, " %08X", Load32(base, addr + o));
  }
  std::fprintf(g_dump, "\n");
}

void DumpObject(Fn fn, const uint8_t* base, uint32_t obj, uint32_t bytes = 0x180) {
  if (!HeapPtr(obj) || !Readable(obj, bytes) || g_dump_count[fn] >= kDumpObjects ||
      !g_dumped.insert(obj).second)
    return;
  g_dump_count[fn]++;
  if (!g_dump) {
    g_dump = std::fopen((g_dir / "objets.txt").string().c_str(), "w");
    if (!g_dump) return;
    std::setvbuf(g_dump, nullptr, _IOLBF, 0);
  }
  std::fprintf(g_dump, "===== %s\n", kNames[fn]);
  DumpWords(base, "objet", obj, bytes);
  for (uint32_t o = 0; o < bytes; o += 4) {
    const uint32_t v = Load32(base, obj + o);
    if (HeapPtr(v) && v != obj) {
      char label[32];
      std::snprintf(label, sizeof(label), "  [+%03X] ->", o);
      DumpWords(base, label, v, 0x60);
    }
  }
}

// D3D9Texture -> mMapDeviceToTextureResources (std::map MSVC en +0x128 : en-tête, taille en +0x12C)
// -> nœud racine (_Parent de l'en-tête) -> pair<device, TextureResources*> en +0x0C/+0x10
// -> TextureResources { pNormTex, pCubeTex, pVolumeTex, pBaseTex, pFSAASurface } -> objets Direct3D Xbox.
void DumpTextureChain(const uint8_t* base, uint32_t tex) {
  if (!g_dump) return;
  const uint32_t head = Load32(base, tex + 0x128);
  if (!HeapPtr(head)) return;
  const uint32_t root = Load32(base, head + 4);
  if (!HeapPtr(root)) return;
  DumpWords(base, "  noeud racine", root, 0x20);
  const uint32_t res = Load32(base, root + 0x10);
  if (!HeapPtr(res)) {
    std::fprintf(g_dump, "  TextureResources %08X hors tas\n", res);
    return;
  }
  DumpWords(base, "  TextureResources", res, 0x20);
  for (uint32_t o = 0; o < 0x14; o += 4) {
    const uint32_t d3d = Load32(base, res + o);
    if (HeapPtr(d3d)) {
      char label[40];
      std::snprintf(label, sizeof(label), "    d3d[+%02X]", o);
      DumpWords(base, label, d3d, 0x60);
    } else if (d3d) {
      std::fprintf(g_dump, "    d3d[+%02X] = %08X (hors tas, non lu)\n", o, d3d);
    }
  }
}

// VertexBufferBinding -> mBindingMap (std::map<ushort, HardwareVertexBufferSharedPtr>, en-tête en +8)
// -> nœud racine -> SharedPtr en +0x10 (vtable, pRep) -> D3D9HardwareVertexBuffer
// -> base D3D9Resource en +0x30, map par device en +0x38 (même schéma que D3D9Texture en +0x120/+0x128)
// -> BufferResources { IDirect3DVertexBuffer9* mBuffer, ... } -> tampon Direct3D Xbox.
void DumpVertexBindingChain(const uint8_t* base, uint32_t binding) {
  if (!g_dump) return;
  const uint32_t head = Load32(base, binding + 8);
  if (!HeapPtr(head)) return;
  const uint32_t root = Load32(base, head + 4);
  if (!HeapPtr(root)) return;
  DumpWords(base, "  noeud liaison", root, 0x20);
  const uint32_t vb = Load32(base, root + 0x14);
  if (!HeapPtr(vb)) return;
  DumpWords(base, "  D3D9HardwareVertexBuffer", vb, 0x60);
  const uint32_t vhead = Load32(base, vb + 0x38);
  if (!HeapPtr(vhead)) return;
  const uint32_t vroot = Load32(base, vhead + 4);
  if (!HeapPtr(vroot)) return;
  DumpWords(base, "    noeud device", vroot, 0x20);
  const uint32_t res = Load32(base, vroot + 0x10);
  if (!HeapPtr(res)) return;
  DumpWords(base, "    BufferResources", res, 0x20);
  const uint32_t d3d = Load32(base, res);
  if (HeapPtr(d3d)) DumpWords(base, "      d3d", d3d, 0x40);
  else std::fprintf(g_dump, "      d3d = %08X (hors tas, non lu)\n", d3d);
}

// RenderOperation { VertexData* +0, OperationType +4, bool useIndexes +8, IndexData* +0xC, Renderable* +0x10 }
// VertexData { HardwareBufferManagerBase* mMgr +0, VertexDeclaration* +4, VertexBufferBinding* +8, bool +0xC,
//              vertexStart +0x10, vertexCount +0x14 }
// IndexData { HardwareIndexBufferSharedPtr (vtable, pRep, ...) , indexStart, indexCount }
int g_render_dumps_indexed = 0;
int g_render_dumps_plain = 0;
void DumpRenderChain(const uint8_t* base, uint32_t op) {
  if (!HeapPtr(op)) return;
  // Critère : pointeur indexData non nul (l'octet useIndexes n'a jamais valu 1 dans les traces).
  const bool indexed = Load32(base, op + 0xC) != 0;
  int& count = indexed ? g_render_dumps_indexed : g_render_dumps_plain;
  if (count >= (indexed ? 4 : 2)) return;
  count++;
  if (!g_dump) {
    g_dump = std::fopen((g_dir / "objets.txt").string().c_str(), "w");
    if (!g_dump) return;
    std::setvbuf(g_dump, nullptr, _IOLBF, 0);
  }
  std::fprintf(g_dump, "===== _render (%s)\n", indexed ? "indexe" : "non indexe");
  DumpWords(base, "RenderOperation", op, 0x20);
  const uint32_t vd = Load32(base, op);
  if (HeapPtr(vd)) {
    DumpWords(base, "  VertexData", vd, 0x20);
    const uint32_t decl = Load32(base, vd + 4);
    if (HeapPtr(decl)) {
      DumpWords(base, "  VertexDeclaration", decl, 0x40);
      // mElementList (std::list MSVC) : chercher le nœud de tête parmi les premiers mots
      for (uint32_t o = 4; o < 0x14; o += 4) {
        const uint32_t h = Load32(base, decl + o);
        if (HeapPtr(h)) {
          char label[48];
          std::snprintf(label, sizeof(label), "    [decl+%02X] ->", o);
          DumpWords(base, label, h, 0x20);
          const uint32_t n1 = Load32(base, h);
          // nœuds de liste MSVC : { _Next, _Prev, VertexElement { source, offset, type, semantic, index } }
          uint32_t n = n1;
          for (int k = 0; k < 8 && HeapPtr(n) && n != h; k++) {
            DumpWords(base, "      element", n, 0x20);
            n = Load32(base, n);
          }
        }
      }
    }
  }
  const uint32_t id = Load32(base, op + 0xC);
  if (indexed && HeapPtr(id)) {
    DumpWords(base, "  IndexData", id, 0x20);
    const uint32_t ib = Load32(base, id + 4);
    if (HeapPtr(ib)) {
      DumpWords(base, "  D3D9HardwareIndexBuffer", ib, 0x60);
      for (uint32_t o = 0x2C; o < 0x50; o += 4) {  // map par device après la base D3D9Resource (+0x2C)
        const uint32_t h = Load32(base, ib + o);
        if (!HeapPtr(h)) continue;
        const uint32_t root = Load32(base, h + 4);
        if (!HeapPtr(root) || Load32(base, root + 0xC) != 0x4000A480u) continue;  // clé = device
        std::fprintf(g_dump, "    map par device en +%02X\n", o);
        const uint32_t res = Load32(base, root + 0x10);
        if (!HeapPtr(res)) break;
        DumpWords(base, "    BufferResources", res, 0x20);
        const uint32_t d3d = Load32(base, res);
        if (HeapPtr(d3d)) DumpWords(base, "      d3d", d3d, 0x40);
        break;
      }
    }
  }
}

// Le programme lié par bindGpuProgram est le programme « assembleur » que D3D9HLSLProgram crée après
// compilation. On cherche quel objet HLSL pointe vers lui, et à quel décalage (une fois par programme).
void LinkProgram(const uint8_t* base, uint32_t prog) {
  if (!HeapPtr(prog) || !g_programs_linked.insert(prog).second) return;
  for (const auto& [obj, hash] : g_hlsl_objects) {
    if (!Readable(obj, 0x1F8)) continue;
    // Limité à 0x1F8 : loadFromSource accède jusqu'à +0x1F4, donc l'objet s'étend au moins jusque-là.
    // Lire plus loin a déjà bloqué le thread de rendu (page invitée non validée ou surveillée).
    for (uint32_t o = 0; o < 0x1F8; o += 4) {
      if (Load32(base, obj + o) == prog) {
        g_program_hash[prog] = hash;
        TL_LOG("[programme] %08X (vtable %08X) <- HLSL %08X +%03X, source %016llx\n", prog,
                     Load32(base, prog), obj, o, (unsigned long long)hash);
        return;
      }
    }
  }
  TL_LOG("[programme] %08X (vtable %08X) : aucun objet HLSL connu ne pointe vers lui\n", prog,
               Load32(base, prog));
}

// Parcours en ordre d'un std::map MSVC : l'en-tête est le nœud sentinelle (_Left = plus petit,
// _Parent = racine, _Right = plus grand) ; nœud { _Left +0, _Parent +4, _Right +8, valeur +0xC }.
template <typename F>
void WalkMap(const uint8_t* base, uint32_t head, int max, F&& visit) {
  if (!HeapPtr(head)) return;
  uint32_t n = Load32(base, head);  // plus petit
  for (int k = 0; k < max && HeapPtr(n) && n != head; k++) {
    visit(n + 0xC);
    const uint32_t right = Load32(base, n + 8);
    if (right != head && HeapPtr(right)) {
      n = right;
      for (int g = 0; g < 64; g++) {
        const uint32_t left = Load32(base, n);
        if (left == head || !HeapPtr(left)) break;
        n = left;
      }
    } else {
      uint32_t parent = Load32(base, n + 4);
      for (int g = 0; g < 64 && HeapPtr(parent) && parent != head && n == Load32(base, parent + 8); g++) {
        n = parent;
        parent = Load32(base, n + 4);
      }
      n = parent;
    }
  }
}

// std::string MSVC (Xbox) : tampon de 16 octets ou pointeur en +0, taille en +0x10, capacité en +0x14.
std::string LoadStdString(const uint8_t* base, uint32_t s) {
  if (!Readable(s, 0x18)) return {};
  const uint32_t size = Load32(base, s + 0x10);
  const uint32_t cap = Load32(base, s + 0x14);
  if (size > 256) return {};
  const uint32_t data = cap >= 16 ? Load32(base, s) : s;
  if (!Readable(data, size ? size : 1)) return {};
  return std::string(reinterpret_cast<const char*>(base + data), size);
}

// GpuProgramParameters : SharedPtr mNamedConstants en +0x40 (pRep en +0x44) -> GpuNamedConstants
// { floatBufferSize +0, intBufferSize +4, ?, std::map<String, GpuConstantDefinition> en-tête +0xC, taille +0x10 }.
int g_named_dumps = 0;
void DumpNamedConstants(const uint8_t* base, uint32_t params) {
  if (!g_dump || g_named_dumps >= 6 || !Readable(params, 0x60)) return;
  const uint32_t named = Load32(base, params + 0x44);
  if (!HeapPtr(named)) return;
  g_named_dumps++;
  DumpWords(base, "===== GpuNamedConstants", named, 0x20);
  // std::map MSVC : en-tête (nœud sentinelle) en +0xC, taille en +0x10.
  const uint32_t head = Load32(base, named + 0xC);
  std::fprintf(g_dump, "  %u constantes, en-tête %08X\n", Load32(base, named + 0x10), head);
  WalkMap(base, head, 64, [&](uint32_t val) {
    const std::string name = LoadStdString(base, val);
    std::fprintf(g_dump, "    %-40s", name.c_str());
    for (uint32_t k = 0x18; k < 0x34; k += 4) std::fprintf(g_dump, " %08X", Load32(base, val + k));
    std::fprintf(g_dump, "\n");
  });
}

// Premiers appels : registres d'arguments et, si r4 pointe en mémoire invitée, ses premiers mots.
void Detail(Fn fn, PPCContext& ctx, uint8_t* base) {
  if (g_detailed[fn].fetch_add(1, std::memory_order_relaxed) >= kDetail) return;
  std::lock_guard<std::mutex> lock(g_file_mutex);
  TL_LOG("[%s] this=%08X r4=%08X r5=%08X r6=%08X r7=%08X lr=%08llX", kNames[fn],
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
               (unsigned long long)ctx.lr);
  if (ctx.r4.u32 >= 0x40000000u && ctx.r4.u32 < 0xA0000000u && Readable(ctx.r4.u32, 24)) {
    TL_LOG("  [r4]:");
    for (int k = 0; k < 6; k++) TL_LOG(" %08X", Load32(base, ctx.r4.u32 + k * 4));
  }
  TL_LOG("\n");
}

// Sans traceur ni capture : appels directs au rendu natif, sans compteurs, détails ni capture.
void NativeOnly(Fn fn, PPCContext& ctx, uint8_t* base) {
  namespace nat = torchlight::natif;
  const auto b8 = [](uint32_t v) { return (v & 0xFF) != 0; };
  switch (fn) {
    case kBeginFrame: nat::BeginFrame(); break;
    case kEndFrame: nat::EndFrame(); break;
    case kSetRenderTarget: nat::SetRenderTarget(ctx.r4.u32, base); break;
    case kSetViewport: nat::SetViewport(ctx.r4.u32, base); break;
    case kClearFrameBuffer: {
      float colour[4] = {};
      for (int k = 0; k < 4 && ctx.r5.u32; k++) {
        const uint32_t w = Load32(base, ctx.r5.u32 + k * 4);
        std::memcpy(&colour[k], &w, 4);
      }
      nat::Clear(ctx.r4.u32, colour, ctx.f1.f64);
      break;
    }
    case kSetTexture: nat::SetTexture(ctx.r4.u32, b8(ctx.r5.u32), ctx.r6.u32 ? Load32(base, ctx.r6.u32 + 4) : 0); break;
    case kSetSceneBlending: nat::SetSceneBlending(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); break;
    case kSetSeparateSceneBlending:
      nat::SetSeparateSceneBlending(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
      break;
    case kSetCullingMode: nat::SetCullingMode(ctx.r4.u32); break;
    case kSetDepthBufferParams:
      nat::SetDepthCheck(b8(ctx.r4.u32));
      nat::SetDepthWrite(b8(ctx.r5.u32));
      nat::SetDepthFunction(ctx.r6.u32);
      break;
    case kSetDepthCheck: nat::SetDepthCheck(b8(ctx.r4.u32)); break;
    case kSetDepthWrite: nat::SetDepthWrite(b8(ctx.r4.u32)); break;
    case kSetDepthFunction: nat::SetDepthFunction(ctx.r4.u32); break;
    case kSetColourWrite: nat::SetColourWrite(b8(ctx.r4.u32), b8(ctx.r5.u32), b8(ctx.r6.u32), b8(ctx.r7.u32)); break;
    case kSetAlphaReject: nat::SetAlphaReject(ctx.r4.u32, ctx.r5.u32 & 0xFF); break;
    case kBindGpuProgram: {
      const ProgramInfo info = LookupProgram(base, ctx.r4.u32);
      nat::BindProgram(info.vertex, info.hash);
      break;
    }
    case kBindGpuProgramParameters:
      if (ctx.r5.u32) nat::BindParameters(ctx.r4.u32, Load32(base, ctx.r5.u32 + 4), base);
      break;
    default: break;
  }
}

void Enter(Fn fn, PPCContext& ctx, uint8_t* base) {
  if (!On()) return;
  if (!g_write) {
    NativeOnly(fn, ctx, base);
    return;
  }
  // Compteurs, détails et vidages : seulement avec le traceur ou la capture (les atomiques et les verrous coûtent
  // cher sur la console, à chaque appel du RenderSystem).
  const uint64_t n = g_write ? g_calls[fn].fetch_add(1, std::memory_order_relaxed) : 1;
  if (g_write) Detail(fn, ctx, base);
  if (g_write && (fn == kBindGpuProgram || fn == kSetTexture || fn == kSetVertexDeclaration)) {
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    if (fn == kBindGpuProgram) g_programs.insert(ctx.r4.u32);
    // _setTexture(unit, enabled, const TexturePtr&) : r6 -> SharedPtr (vtable, pRep, pUseCount) ; Texture* à +4
    if (fn == kSetTexture && ctx.r6.u32) g_textures.insert(Load32(base, ctx.r6.u32 + 4));
    if (fn == kSetVertexDeclaration) g_decls.insert(ctx.r4.u32);
  }
  static const bool objets = REXCVAR_GET(torchlight_trace_objets);
  if (fn == kBindGpuProgram && !objets) {
    // Liaison programme -> source HLSL : faite par ProgramHash (cas kBindGpuProgram ci-dessous).
  } else if (!objets) {
    // pas de vidage d'objets
  } else if (fn == kSetTexture && ctx.r6.u32) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    const uint32_t tex = Load32(base, ctx.r6.u32 + 4);
    const int before = g_dump_count[fn];
    DumpObject(fn, base, tex);
    if (g_dump_count[fn] != before) DumpTextureChain(base, tex);
  } else if (fn == kBindGpuProgramParameters && ctx.r5.u32) {
    // bindGpuProgramParameters(type, GpuProgramParametersSharedPtr par valeur -> r5 = copie, masque)
    std::lock_guard<std::mutex> lock(g_file_mutex);
    DumpObject(fn, base, Load32(base, ctx.r5.u32 + 4), 0x100);
    DumpNamedConstants(base, Load32(base, ctx.r5.u32 + 4));
  } else if (fn == kBindGpuProgram) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    LinkProgram(base, ctx.r4.u32);
  } else if (fn == kRender) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    DumpRenderChain(base, ctx.r4.u32);
    if ((n & 4095) == 0 && HeapPtr(ctx.r4.u32)) {  // échantillon brut de la RenderOperation
      TL_LOG("[op] n=%llu", (unsigned long long)n);
      for (int k = 0; k < 8; k++) TL_LOG(" %08X", Load32(base, ctx.r4.u32 + k * 4));
      TL_LOG("\n");
    }
  } else if (fn == kSetVertexBufferBinding) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    const int before = g_dump_count[fn];
    DumpObject(fn, base, ctx.r4.u32);
    if (g_dump_count[fn] != before) DumpVertexBindingChain(base, ctx.r4.u32);
  }
  // Capture : états, programmes, constantes, dessins (voir torchlight_capture.h).
  namespace cap = torchlight::capture;
  namespace nat = torchlight::natif;
  const auto b8 = [](uint32_t v) { return (v & 0xFF) != 0; };
  switch (fn) {
    case kBeginFrame: cap::BeginFrame(); nat::BeginFrame(); break;
    case kEndFrame: cap::EndFrame(); nat::EndFrame(); break;
    case kSetRenderTarget: cap::SetRenderTarget(ctx.r4.u32, base); nat::SetRenderTarget(ctx.r4.u32, base); break;
    case kSetViewport: cap::SetViewport(ctx.r4.u32, base); nat::SetViewport(ctx.r4.u32, base); break;
    case kClearFrameBuffer: {
      // clearFrameBuffer(buffers, const ColourValue& r5, Real depth f1, ushort stencil r6)
      float colour[4] = {};
      for (int k = 0; k < 4 && ctx.r5.u32; k++) {
        const uint32_t w = Load32(base, ctx.r5.u32 + k * 4);
        std::memcpy(&colour[k], &w, 4);
      }
      cap::Clear(ctx.r4.u32, colour, ctx.f1.f64, ctx.r6.u32);
      nat::Clear(ctx.r4.u32, colour, ctx.f1.f64);
      break;
    }
    case kSetTexture:
      cap::SetTexture(ctx.r4.u32, b8(ctx.r5.u32), ctx.r6.u32 ? Load32(base, ctx.r6.u32 + 4) : 0);
      nat::SetTexture(ctx.r4.u32, b8(ctx.r5.u32), ctx.r6.u32 ? Load32(base, ctx.r6.u32 + 4) : 0);
      break;
    case kSetSceneBlending:
      cap::SetSceneBlending(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
      nat::SetSceneBlending(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
      break;
    case kSetSeparateSceneBlending:
      cap::SetSeparateSceneBlending(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
      nat::SetSeparateSceneBlending(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
      break;
    case kSetCullingMode: cap::SetCullingMode(ctx.r4.u32); nat::SetCullingMode(ctx.r4.u32); break;
    case kSetDepthBufferParams:
      cap::SetDepthCheck(b8(ctx.r4.u32));
      cap::SetDepthWrite(b8(ctx.r5.u32));
      cap::SetDepthFunction(ctx.r6.u32);
      nat::SetDepthCheck(b8(ctx.r4.u32));
      nat::SetDepthWrite(b8(ctx.r5.u32));
      nat::SetDepthFunction(ctx.r6.u32);
      break;
    case kSetDepthCheck: cap::SetDepthCheck(b8(ctx.r4.u32)); nat::SetDepthCheck(b8(ctx.r4.u32)); break;
    case kSetDepthWrite: cap::SetDepthWrite(b8(ctx.r4.u32)); nat::SetDepthWrite(b8(ctx.r4.u32)); break;
    case kSetDepthFunction: cap::SetDepthFunction(ctx.r4.u32); nat::SetDepthFunction(ctx.r4.u32); break;
    case kSetColourWrite:
      cap::SetColourWrite(b8(ctx.r4.u32), b8(ctx.r5.u32), b8(ctx.r6.u32), b8(ctx.r7.u32));
      nat::SetColourWrite(b8(ctx.r4.u32), b8(ctx.r5.u32), b8(ctx.r6.u32), b8(ctx.r7.u32));
      break;
    case kSetAlphaReject:
      cap::SetAlphaReject(ctx.r4.u32, ctx.r5.u32 & 0xFF);
      nat::SetAlphaReject(ctx.r4.u32, ctx.r5.u32 & 0xFF);
      break;
    case kSetScissor: cap::SetScissor(b8(ctx.r4.u32), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); break;
    case kBindGpuProgram: {
      const uint32_t prog = ctx.r4.u32;
      const uint64_t hash = ProgramHash(base, prog);
      const bool vertex = ProgramIsVertex(base, prog);
      cap::BindProgram(vertex, prog, hash);
      nat::BindProgram(vertex, hash);
      break;
    }
    case kBindGpuProgramParameters:
      if (ctx.r5.u32) {
        cap::BindParameters(ctx.r4.u32, Load32(base, ctx.r5.u32 + 4), base);
        nat::BindParameters(ctx.r4.u32, Load32(base, ctx.r5.u32 + 4), base);
      }
      break;
    case kRender: break;  // après l'appel original (interception de sub_821C4058)
    default: break;
  }
  // Rapport vérifié depuis n'importe quelle fonction : _endFrame n'est pas forcément appelée à chaque image.
  if (g_write && (fn == kEndFrame || (n & 63) == 0)) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    Report();
  }
}

// Écriture des fichiers HLSL sur un thread à part. Les écrire depuis le thread qui compile (chargement du jeu)
// le ralentissait de quelques millisecondes en plein démarrage, et cela suffisait à bloquer l'émulation GPU une
// fois sur deux (le jeu attendait indéfiniment le GPU). Le thread du jeu ne fait plus que mettre en file.
struct PendingFile {
  std::filesystem::path path;
  std::string data;
  bool append;
};
std::mutex g_writer_mutex;
std::condition_variable g_writer_cv;
std::deque<PendingFile> g_writer_queue;

void WriterMain() {
  for (;;) {
    PendingFile f;
    {
      std::unique_lock<std::mutex> lock(g_writer_mutex);
      g_writer_cv.wait(lock, [] { return !g_writer_queue.empty(); });
      f = std::move(g_writer_queue.front());
      g_writer_queue.pop_front();
    }
    if (FILE* out = std::fopen(f.path.string().c_str(), f.append ? "ab" : "wb")) {
      std::fwrite(f.data.data(), 1, f.data.size(), out);
      std::fclose(out);
    }
  }
}

void QueueFile(std::filesystem::path path, std::string data, bool append) {
  static std::once_flag started;
  static bool threaded = false;
  std::call_once(started, [] {
    try {
      std::thread(WriterMain).detach();
      threaded = true;
    } catch (const std::exception&) {
      // Pas de thread disponible (Switch) : écriture directe, plus lente mais sans planter le jeu.
    }
  });
  if (!threaded) {
    if (FILE* out = std::fopen(path.string().c_str(), append ? "ab" : "wb")) {
      std::fwrite(data.data(), 1, data.size(), out);
      std::fclose(out);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_writer_mutex);
    g_writer_queue.push_back({std::move(path), std::move(data), append});
  }
  g_writer_cv.notify_one();
}

// D3DXCompileShader(source, longueur, defines, include, entrée, profil, ...) :
// la source HLSL générée par le RT Shader System d'Ogre (ou lue dans les matériaux).
void DumpHlsl(PPCContext& ctx, uint8_t* base) {
  if (!On()) return;
  g_calls[kCompileShader].fetch_add(1, std::memory_order_relaxed);
  const uint32_t src = ctx.r3.u32;
  const uint32_t len = ctx.r4.u32;
  if (!src || !len || len > 4 * 1024 * 1024) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    TL_LOG("[hlsl] ignoré : r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X lr=%08llX\n", src, len,
                 ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, (unsigned long long)ctx.lr);
    return;
  }
  const std::string text(reinterpret_cast<const char*>(base + src), len);
  const std::string entry = LoadCString(base, ctx.r7.u32);
  const std::string target = LoadCString(base, ctx.r8.u32);
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
  for (unsigned char c : entry + target) h = (h ^ c) * 1099511628211ull;

  {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    if (t_hlsl_object) g_hlsl_objects[t_hlsl_object] = h;
    if (!g_hlsl_seen.insert(h).second) return;
  }
  // Rendu natif : compilation de la source en SPIR-V sur cette machine (cache), une fois par source. Hors du
  // verrou du traceur : le thread de rendu le prend à chaque liaison de programme, il n'a pas à attendre la
  // compilation (200 ms par shader sur la console).
  torchlight::natif::ShaderSource(h, target, entry, text, ctx.r9.u32);
  if (!g_write) return;
  std::lock_guard<std::mutex> lock(g_file_mutex);
  // Options de compilation D3DX (r9) : 8 = D3DXSHADER_PACKMATRIX_ROWMAJOR, 16 = ...COLUMNMAJOR. Elles varient
  // selon le programme et décident du rangement des matrices : index hlsl/options.txt (« empreinte options »).
  char line[48];
  std::snprintf(line, sizeof(line), "%016llx %u\n", (unsigned long long)h, ctx.r9.u32);
  QueueFile(g_dir / "hlsl" / "options.txt", line, true);
  char name[160];
  std::snprintf(name, sizeof(name), "%016llx_%s_%s.hlsl", (unsigned long long)h, target.c_str(),
                entry.c_str());
  QueueFile(g_dir / "hlsl" / name, text, false);
  TL_LOG("[hlsl] %s (%u octets, lr=%08llX)\n", name, len, (unsigned long long)ctx.lr);
}

}  // namespace

// Compteurs pour le rapport du traceur seulement : en rendu natif seul, un incrément atomique à chaque appel des 78
// méthodes comptées coûtait ~1 % du thread principal sur la console (pas d'instructions LSE sur le Cortex-A57).
void CountSlot(int slot) {
  if (!g_write) return;
  if (On() && slot >= 0 && slot < 128) g_slot_calls[slot].fetch_add(1, std::memory_order_relaxed);
}
}  // namespace torchlight::trace

#define TL_TRACE(addr, id)                                           \
  REX_EXTERN(__imp__sub_##addr);                                     \
  REX_HOOK_RAW(sub_##addr) {                                         \
    torchlight::trace::Enter(torchlight::trace::id, ctx, base);      \
    __imp__sub_##addr(ctx, base);                                    \
  }

// Méthodes qui ne font que transmettre un état au périphérique Direct3D du jeu (vérifié dans Ogre 1.7 : aucun
// membre d'Ogre modifié) : en rendu natif seul, l'original n'est plus appelé pendant une image (le Direct3D du jeu
// ne dessine plus). Nos interceptions lisent les arguments avant.
#define TL_TRACE_D3D(addr, id)                                       \
  REX_EXTERN(__imp__sub_##addr);                                     \
  REX_HOOK_RAW(sub_##addr) {                                         \
    torchlight::trace::Enter(torchlight::trace::id, ctx, base);      \
    if (torchlight::natif::DrawsHandled()) return;                   \
    __imp__sub_##addr(ctx, base);                                    \
  }
#define TL_SLOT_D3D(addr, slot)                              \
  REX_EXTERN(__imp__sub_##addr);                             \
  REX_HOOK_RAW(sub_##addr) {                                 \
    torchlight::trace::CountSlot(slot);                      \
    if (torchlight::natif::DrawsHandled()) return;           \
    __imp__sub_##addr(ctx, base);                            \
  }

// Méthodes de Ogre::D3D9RenderSystem (cases de switch/ogre/D3D9RenderSystem.txt). TL_TRACE_D3D : l'original ne fait
// que transmettre un état au Direct3D du jeu (vérifié dans Ogre 1.7 et dans le portage Xbox : aucune écriture hors
// de sa pile ; profondeur, couleurs, scissor, effacement, déclaration de sommets ajoutés le 2026-10-02).
TL_TRACE(821C8E40, kSetTexture)                // case 39 (surcharge TexturePtr ; 38 = surcharge String)
TL_TRACE_D3D(821C52B0, kSetSceneBlending)          // case 51
TL_TRACE(821AE5A8, kBeginFrame)                // case 55
TL_TRACE(821B1000, kEndFrame)                  // case 58
TL_TRACE(821A4368, kSetViewport)               // case 59
TL_TRACE(821C6CF8, kSetCullingMode)            // case 61
TL_TRACE_D3D(821BE2F8, kSetDepthBufferParams)      // case 63
TL_TRACE_D3D(821CE5A0, kSetVertexDeclaration)      // case 84
TL_TRACE(821C3E78, kSetVertexBufferBinding)    // case 85
TL_TRACE(821C1398, kBindGpuProgram)            // case 90
TL_TRACE_D3D(8219CAF8, kClearFrameBuffer)          // case 107
TL_TRACE(821A7620, kSetRenderTarget)           // case 114
TL_TRACE_D3D(82572E30, kSetSeparateSceneBlending)  // case 52
TL_TRACE_D3D(821C54D8, kSetAlphaReject)            // case 53
TL_TRACE_D3D(821C2F38, kSetDepthCheck)             // case 64
TL_TRACE_D3D(821C4E88, kSetDepthWrite)             // case 65
TL_TRACE_D3D(821C3010, kSetDepthFunction)          // case 66
TL_TRACE_D3D(821C3520, kSetColourWrite)            // case 67
TL_TRACE_D3D(825739A8, kSetScissor)                // case 106

// _render (case 87). La capture et le rendu natif lisent le dessin APRÈS l'original : Ogre 1.7 met à jour ses
// tampons Direct3D paresseusement (unlock les marque périmés, la copie a lieu dans setVertexBufferBinding /
// getD3D9IndexBuffer, appelés par _render). Lus avant, les tampons dynamiques avaient une image de retard.
// Avec --torchlight_natif_seul, seuls les appels de dessin du Direct3D Xbox faits par _render sont neutralisés
// (sub_821CF830 : DrawIndexedVertices, sub_821D0A10 : DrawVertices) : tout le reste d'Ogre s'exécute.
namespace torchlight::trace {
thread_local bool t_skip_d3d_draw = false;
// Portage Xbox de _render : lit une surface du périphérique (device+12944 par sub_821C0A08, sinon device+12940 par
// sub_821E0088) avec AddRef, teste qu'elle n'est pas nulle, puis la relâche (sub_821CEDD8). AddRef et Release
// s'annulent mais passent chacun par un lwarx/stwcx. émulé sous le verrou global (~0,9 % du thread principal sur la
// console). Pendant _render, la lecture rend la surface sans AddRef et note son adresse ; le Release de cette même
// surface est sauté. Tout autre Release passe normalement.
thread_local bool t_in_render = false;
thread_local uint32_t t_unreferenced = 0;
}
REX_EXTERN(__imp__sub_821C4058);
REX_HOOK_RAW(sub_821C4058) {
  const uint32_t op = ctx.r4.u32;
  torchlight::trace::Enter(torchlight::trace::kRender, ctx, base);
  torchlight::trace::t_skip_d3d_draw = torchlight::natif::DrawsHandled();
  torchlight::trace::t_in_render = true;
  __imp__sub_821C4058(ctx, base);
  torchlight::trace::t_in_render = false;
  torchlight::trace::t_unreferenced = 0;
  torchlight::trace::t_skip_d3d_draw = false;
  torchlight::capture::Render(op, base);
  torchlight::natif::Render(op, base);
}
// Lecture de surface sans AddRef pendant _render (voir t_in_render) : (device r3, sortie r4).
#define TL_SURFACE_SANS_ADDREF(addr, offset)                                         \
  REX_EXTERN(__imp__sub_##addr);                                                     \
  REX_HOOK_RAW(sub_##addr) {                                                         \
    using torchlight::trace::Load32;                                                 \
    if (!torchlight::trace::t_in_render || torchlight::trace::t_unreferenced) {      \
      __imp__sub_##addr(ctx, base);                                                  \
      return;                                                                        \
    }                                                                                \
    const uint32_t surface = Load32(base, ctx.r3.u32 + offset);                      \
    torchlight::trace::Store32(base, ctx.r4.u32, surface);                           \
    torchlight::trace::t_unreferenced = surface;                                     \
  }
TL_SURFACE_SANS_ADDREF(821C0A08, 12944)
TL_SURFACE_SANS_ADDREF(821E0088, 12940)
REX_EXTERN(__imp__sub_821CEDD8);
REX_HOOK_RAW(sub_821CEDD8) {
  if (torchlight::trace::t_unreferenced && ctx.r3.u32 == torchlight::trace::t_unreferenced) {
    torchlight::trace::t_unreferenced = 0;  // Release de la surface lue sans AddRef : sauté
    return;
  }
  __imp__sub_821CEDD8(ctx, base);
}
REX_EXTERN(__imp__sub_821CF830);
REX_HOOK_RAW(sub_821CF830) {
  if (torchlight::trace::t_skip_d3d_draw) return;
  __imp__sub_821CF830(ctx, base);
}
REX_EXTERN(__imp__sub_821D0A10);
REX_HOOK_RAW(sub_821D0A10) {
  if (torchlight::trace::t_skip_d3d_draw) return;
  __imp__sub_821D0A10(ctx, base);
}

// Verrouillage des tampons de sommets et d'indices par Ogre (Lock du Direct3D Xbox : sub_821A70D8 sommets,
// sub_8276BA90 indices -> sub_821A6D90 ; r3 = objet Direct3D, adresse en +0x18).
// - Chaque Lock est signalé au rendu natif (BufferLocked) : ses copies de ce tampon sont à refaire.
// - Sans D3DLOCK_NOOVERWRITE, le Direct3D attend que le GPU ait fini avec le tampon (sub_821A5C10, attente sur la
//   fence, boucle sur sub_82774170). En rendu natif seul, le GPU émulé ne dessine plus avec ces tampons (le rendu
//   natif en a copié le contenu au relevé) : attente inutile, sautée. PC, mine : 7 % du thread principal, dont 4 %
//   d'attente du GPU émulé, pour les billboards et particules réécrits à chaque image.
namespace torchlight::trace {
thread_local bool t_lock_no_wait = false;
}
#define TL_LOCK_NO_WAIT(addr)                                                   \
  REX_EXTERN(__imp__sub_##addr);                                                \
  REX_HOOK_RAW(sub_##addr) {                                                    \
    torchlight::natif::BufferLocked(torchlight::trace::Load32(base, ctx.r3.u32 + 0x18)); \
    torchlight::trace::t_lock_no_wait = torchlight::natif::LockWithoutWait();      \
    __imp__sub_##addr(ctx, base);                                               \
    torchlight::trace::t_lock_no_wait = false;                                  \
  }
TL_LOCK_NO_WAIT(821A70D8)
TL_LOCK_NO_WAIT(8276BA90)
// sub_821A5C10(device r3, fence r4, r5, r6, sans_envoi r7) : si le GPU n'a pas atteint la fence et qu'elle est celle
// du tampon de commandes en cours (device+11036), le Direct3D l'envoie d'abord au GPU (sub_821F2910), puis attend.
// Seule l'attente est sautée : sans l'envoi, le GPU émulé restait bloqué (WAIT_REG_MEM) et le jeu avec lui.
REX_EXTERN(sub_821F2910);
REX_EXTERN(__imp__sub_821A5C10);
REX_HOOK_RAW(sub_821A5C10) {
  using torchlight::trace::Load32;
  if (!torchlight::trace::t_lock_no_wait) {
    // Essai contre le blocage au démarrage (désactivé par défaut, --torchlight_envoi_avant_attente) : envoi du
    // tampon de commandes avant d'attendre une fence non atteinte. La vraie cause était une course dans l'émulateur
    // du GPU (réinitialisation de l'anneau, corrigée dans le SDK, voir switch/RENDU_NATIF.md).
    static thread_local bool t_kicking = false;
    const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
    if (fence && !t_kicking && torchlight::natif::KickBeforeWait()) {
      const uint32_t current = Load32(base, device + 11036);
      const uint32_t done = Load32(base, Load32(base, device + 11024));
      if (current - fence < current - done && fence != current && Load32(base, device + 13472) == 0) {
        t_kicking = true;
        PPCContext c = ctx;
        sub_821F2910(c, base);
        t_kicking = false;
      }
    }
    __imp__sub_821A5C10(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
  if (!fence) return;
  const uint32_t current = Load32(base, device + 11036);
  const uint32_t done = Load32(base, Load32(base, device + 11024));  // fence atteinte par le GPU
  if (current - fence >= current - done) return;                      // déjà atteinte
  if ((ctx.r7.u32 & 0xFF) == 0 && fence == current && Load32(base, device + 13472) == 0) {
    torchlight::trace::t_lock_no_wait = false;  // l'envoi peut lui-même attendre (place dans l'anneau)
    PPCContext c = ctx;
    sub_821F2910(c, base);
    torchlight::trace::t_lock_no_wait = true;
  }
}

// bindGpuProgramParameters (case 91). Gardé : copie des paramètres partagés (_copySharedParams, lus ensuite par le
// rendu natif) et mémorisation des paramètres actifs. Avec --torchlight_natif_seul, seules les copies des constantes
// dans le périphérique Direct3D du jeu sont neutralisées : sub_821BFB68 / sub_821C24D8 (flottants, sommets / pixels),
// sub_8276D9F0 / sub_8276D998 (entiers).
namespace torchlight::trace {
thread_local bool t_skip_d3d_consts = false;
}
//
// Avec --torchlight_ogre_natif (défaut), en rendu natif seul, le parcours de toutes les constantes (qui ne faisait
// plus que des copies neutralisées, ~2 % du thread principal sur la console) est sauté : seule la partie utile est
// faite, comme dans l'original (bindGpuProgramParameters(this r3, type r4, SharedPtr<GpuProgramParameters> par
// valeur r5 : vtable, pRep +4, pUseCount +8 ; masque r6)) :
// - masque & 1 (GPV_GLOBAL) : _copySharedParams, sub_82467600 sur chaque jeu partagé ([pRep+104], [pRep+108], pas 48) ;
// - type 0 (sommets) / 1 (pixels) : paramètres actifs, this+60 / this+92 = argument (sub_821C25B0, affectation) ;
// - destruction de l'argument par valeur (convention MSVC : l'appelé la fait) : vtable de base 0x8200091C, compteur
//   décrémenté. Chemin court seulement si le compteur dépasse 1 (pas de destruction de l'objet, donc pas d'appel
//   virtuel) ; masque 8 (numéro d'itération de passe) et autres cas : l'original.
namespace torchlight::ogre {
int Mode();
}
REX_EXTERN(sub_82467600);  // GpuProgramParameters::_copySharedParams (un jeu partagé)
REX_EXTERN(sub_821C25B0);  // SharedPtr<GpuProgramParameters>::operator=
REX_EXTERN(__imp__sub_821C2118);
REX_HOOK_RAW(sub_821C2118) {
  using torchlight::trace::Load32;
  using torchlight::trace::Store32;
  torchlight::trace::Enter(torchlight::trace::kBindGpuProgramParameters, ctx, base);
  const bool natif = torchlight::natif::DrawsHandled();
  const uint32_t params = ctx.r5.u32, mask = ctx.r6.u32 & 0xFFFF, type = ctx.r4.u32;
  if (natif && torchlight::ogre::Mode() == 1 && mask != 8 && params) {
    const uint32_t use = Load32(base, params + 8);
    if (use && Load32(base, use) > 1) {
      const uint32_t p = Load32(base, params + 4);
      if (mask & 1) {
        for (uint32_t shared = Load32(base, p + 104); shared != Load32(base, p + 108); shared += 48) {
          PPCContext c = ctx;
          c.r3.u64 = shared;
          sub_82467600(c, base);
        }
      }
      if (type <= 1) {
        PPCContext c = ctx;
        c.r3.u64 = ctx.r3.u32 + (type == 1 ? 92 : 60);
        c.r4.u64 = params;
        sub_821C25B0(c, base);
      }
      Store32(base, params, 0x8200091Cu);
      Store32(base, use, Load32(base, use) - 1);
      return;
    }
  }
  torchlight::trace::t_skip_d3d_consts = natif;
  __imp__sub_821C2118(ctx, base);
  torchlight::trace::t_skip_d3d_consts = false;
}
#define TL_D3D_CONSTS(addr)                                  \
  REX_EXTERN(__imp__sub_##addr);                             \
  REX_HOOK_RAW(sub_##addr) {                                 \
    if (torchlight::trace::t_skip_d3d_consts) return;        \
    __imp__sub_##addr(ctx, base);                            \
  }
TL_D3D_CONSTS(821BFB68)
TL_D3D_CONSTS(821C24D8)
TL_D3D_CONSTS(8276D9F0)
TL_D3D_CONSTS(8276D998)

// D3DXCompileShader(source r3, longueur r4, defines r5, include r6, entrée r7, profil r8, options r9, ppShader r10,
// ppErrors, ppConstantTable et un 11e argument propre à la Xbox sur la pile : r1+84, +92, +100), appelé par
// D3D9HLSLProgram::loadFromSource (sub_82587FB0).
// Cache (natif/d3dx_cache.h) : le compilateur (sub_827A73F0) tire la table de constantes d'un code intermédiaire
// (sub_82778058, comme D3DXGetShaderConstantTable), puis le convertit en microcode (sub_8278D688). Lors d'une vraie
// compilation, on relève ce code intermédiaire ; au lancement suivant, le microcode est rendu dans un tampon créé par
// le jeu (D3DXCreateBuffer, sub_82776E20 : taille en +8, données en +12) et la table reconstruite par sub_82778058.
// En rendu natif seul avec --torchlight_natif_d3dx_allege (expérimental, sans cache), le compilateur reçoit une
// source allégée (natif::MakeStubSource). Temps passé dans le compilateur : bilan dans le journal.
REX_EXTERN(sub_82776E20);  // D3DXCreateBuffer(taille, ppBuffer)
REX_EXTERN(sub_82778058);  // table de constantes (code, ppConstantTable)
namespace torchlight::trace {
double g_d3dx_ms = 0;
uint32_t g_d3dx_calls = 0, g_d3dx_stubs = 0, g_d3dx_cached = 0;
// Relevé pendant une vraie compilation, sur le thread qui compile.
struct D3dxCapture {
  bool on = false;
  uint32_t conversions = 0, tables = 0;
  uint32_t conv_ptr = 0, table_ptr = 0;
  std::string conv;  // code intermédiaire donné à la conversion en microcode
};
thread_local D3dxCapture t_d3dx;

uint64_t D3dxKey(const PPCContext& ctx, const uint8_t* base) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  auto mix = [&h](const void* data, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
  };
  const uint32_t version = 1;
  mix(&version, 4);
  mix(base + ctx.r3.u32, ctx.r4.u32);
  for (uint32_t d = ctx.r5.u32; d && Load32(base, d); d += 8) {  // D3DXMACRO { Name, Definition }, fin : Name nul
    const std::string name = LoadCString(base, Load32(base, d), 4096);
    const std::string def = LoadCString(base, Load32(base, d + 4), 4096);
    mix(name.c_str(), name.size() + 1);
    mix(def.c_str(), def.size() + 1);
  }
  const std::string entry = LoadCString(base, ctx.r7.u32), profile = LoadCString(base, ctx.r8.u32);
  mix(entry.c_str(), entry.size() + 1);
  mix(profile.c_str(), profile.size() + 1);
  mix(&ctx.r9.u32, 4);
  return h;
}

// Résultat du cache rendu au jeu comme par le compilateur. Faux (rien de modifié) si impossible.
bool D3dxFromCache(PPCContext& ctx, uint8_t* base, const torchlight::natif::d3dx::Result& r) {
  const uint32_t pp_shader = ctx.r10.u32;
  const uint32_t pp_errors = Load32(base, ctx.r1.u32 + 84);
  const uint32_t pp_table = Load32(base, ctx.r1.u32 + 92);
  auto* mem = rex::system::kernel_memory();
  if (!mem || !pp_shader || (pp_table && r.intermediate.empty())) return false;
  const uint32_t slot = mem->SystemHeapAlloc(8);
  if (!slot) return false;
  PPCContext c = ctx;
  if (pp_table) {
    const uint32_t code = mem->SystemHeapAlloc(uint32_t(r.intermediate.size()));
    if (!code) {
      mem->SystemHeapFree(slot);
      return false;
    }
    std::memcpy(base + code, r.intermediate.data(), r.intermediate.size());
    Store32(base, slot, 0);
    c.r3.u64 = code;
    c.r4.u64 = slot;
    sub_82778058(c, base);
    mem->SystemHeapFree(code);
    if (int32_t(c.r3.u32) < 0 || !Load32(base, slot)) {
      mem->SystemHeapFree(slot);
      return false;
    }
    Store32(base, pp_table, Load32(base, slot));
  }
  c = ctx;
  Store32(base, slot, 0);
  c.r3.u64 = uint32_t(r.code.size());
  c.r4.u64 = slot;
  sub_82776E20(c, base);
  const uint32_t buffer = Load32(base, slot);
  mem->SystemHeapFree(slot);
  if (int32_t(c.r3.u32) < 0 || !buffer) {
    ctx.r3.u64 = c.r3.u32 ? c.r3.u32 : 0x8007000Eu;  // E_OUTOFMEMORY
    return true;  // la table est déjà rendue : erreur comme le compilateur sans mémoire
  }
  std::memcpy(base + Load32(base, buffer + 12), r.code.data(), r.code.size());
  Store32(base, pp_shader, buffer);
  if (pp_errors) Store32(base, pp_errors, 0);
  ctx.r3.u64 = 0;  // S_OK
  return true;
}

// Après une vraie compilation réussie : résultat ajouté au cache (écriture du fichier en arrière-plan).
void D3dxToCache(uint64_t key, uint32_t pp_shader, uint32_t pp_table, const uint8_t* base) {
  const D3dxCapture& cap = t_d3dx;
  const uint32_t buffer = pp_shader ? Load32(base, pp_shader) : 0;
  if (!buffer || cap.conversions > 1 || cap.tables > 1) return;
  torchlight::natif::d3dx::Result r;
  const uint32_t data = Load32(base, buffer + 12);
  r.code.assign(reinterpret_cast<const char*>(base + data), Load32(base, buffer + 8));
  if (pp_table && Load32(base, pp_table)) {
    if (cap.tables != 1) return;
    if (cap.conversions == 1 && cap.table_ptr == cap.conv_ptr)
      r.intermediate = cap.conv;
    else if (cap.conversions == 0 && cap.table_ptr == data)
      r.intermediate = r.code;  // pas de conversion : la table vient du microcode lui-même
    else
      return;  // cas inconnu : pas de cache
  }
  std::string block = torchlight::natif::d3dx::Add(key, std::move(r));
  if (!block.empty())
    torchlight::natif::RunInBackground([block = std::move(block)] { torchlight::natif::d3dx::Append(block); });
}
}  // namespace torchlight::trace

// Conversion du code intermédiaire en microcode (r4 : code, r5 : taille) et construction de la table de constantes
// (r3 : code) : relevées pendant une vraie compilation.
REX_EXTERN(__imp__sub_8278D688);
REX_HOOK_RAW(sub_8278D688) {
  auto& cap = torchlight::trace::t_d3dx;
  if (cap.on && cap.conversions++ == 0) {
    cap.conv_ptr = ctx.r4.u32;
    cap.conv.assign(reinterpret_cast<const char*>(base + ctx.r4.u32), ctx.r5.u32);
  }
  __imp__sub_8278D688(ctx, base);
}
REX_EXTERN(__imp__sub_82778058);
REX_HOOK_RAW(sub_82778058) {
  auto& cap = torchlight::trace::t_d3dx;
  if (cap.on && cap.tables++ == 0) cap.table_ptr = ctx.r3.u32;
  __imp__sub_82778058(ctx, base);
}

REX_EXTERN(__imp__sub_82777F10);
REX_HOOK_RAW(sub_82777F10) {
  using namespace torchlight::trace;
  DumpHlsl(ctx, base);
  const auto t0 = std::chrono::steady_clock::now();
  bool done = false;
  if (torchlight::natif::StubXboxShaders() && ctx.r3.u32 && ctx.r4.u32 && ctx.r4.u32 < 4u * 1024 * 1024) {
    const std::string stub =
        torchlight::natif::MakeStubSource(std::string(reinterpret_cast<const char*>(base + ctx.r3.u32), ctx.r4.u32));
    auto* mem = rex::system::kernel_memory();
    const uint32_t addr = (!stub.empty() && mem) ? mem->SystemHeapAlloc(uint32_t(stub.size() + 1)) : 0;
    if (addr) {
      std::memcpy(base + addr, stub.data(), stub.size());
      base[addr + stub.size()] = 0;
      PPCContext saved = ctx;  // arguments d'origine, pour recompiler si la version allégée échoue
      ctx.r3.u64 = addr;
      ctx.r4.u64 = uint32_t(stub.size());
      __imp__sub_82777F10(ctx, base);
      mem->SystemHeapFree(addr);
      if (int32_t(ctx.r3.u32) >= 0) {
        done = true;
        g_d3dx_stubs++;
      } else {
        REXLOG_WARN("[natif] source allégée refusée par D3DXCompileShader ({:08X}) : source d'origine",
                    ctx.r3.u32);
        ctx = saved;
      }
    }
  }
  // Cache : seulement pour les compilations ordinaires (11e argument sans demande d'informations de débogage).
  const uint32_t extra = Load32(base, ctx.r1.u32 + 100);
  const bool cacheable = !done && torchlight::natif::d3dx::Active() && ctx.r3.u32 && ctx.r4.u32 &&
                         ctx.r4.u32 < 4u * 1024 * 1024 &&
                         !(extra && ((Load32(base, extra) & 2) || Load32(base, extra + 20)));
  uint64_t key = 0;
  if (cacheable) {
    key = D3dxKey(ctx, base);
    torchlight::natif::d3dx::Result r;
    if (torchlight::natif::d3dx::Find(key, r) && D3dxFromCache(ctx, base, r)) {
      done = true;
      g_d3dx_cached++;
    }
  }
  if (!done) {
    const uint32_t pp_shader = ctx.r10.u32, pp_table = Load32(base, ctx.r1.u32 + 92);
    t_d3dx = {};
    t_d3dx.on = cacheable;
    __imp__sub_82777F10(ctx, base);
    t_d3dx.on = false;
    if (cacheable && ctx.r3.u32 == 0) D3dxToCache(key, pp_shader, pp_table, base);
    t_d3dx.conv.clear();
    t_d3dx.conv.shrink_to_fit();
  }
  g_d3dx_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if ((++g_d3dx_calls % 25) == 0)
    REXLOG_INFO("[natif] D3DXCompileShader : {} appels ({} allégés, {} du cache), {:.0f} ms au total, {:.1f} ms en "
                "moyenne ; cache : {}",
                g_d3dx_calls, g_d3dx_stubs, g_d3dx_cached, g_d3dx_ms, g_d3dx_ms / g_d3dx_calls,
                torchlight::natif::d3dx::Summary());
}

// D3D9HLSLProgram::loadFromSource (sub_82587FB0) : this = objet HLSL, compile via D3DXCompileShader.
REX_EXTERN(__imp__sub_82587FB0);
REX_HOOK_RAW(sub_82587FB0) {
  torchlight::trace::t_hlsl_object = ctx.r3.u32;
  __imp__sub_82587FB0(ctx, base);
  torchlight::trace::t_hlsl_object = 0;
}

// Compteur de toutes les autres méthodes de D3D9RenderSystem (généré depuis switch/ogre/D3D9RenderSystem.txt) :
// savoir lesquelles le jeu appelle vraiment, et à quelle fréquence. Une fonction partagée par plusieurs cases
// est comptée sous la première.
#define TL_SLOT(addr, slot)                                  \
  REX_EXTERN(__imp__sub_##addr);                             \
  REX_HOOK_RAW(sub_##addr) {                                \
    torchlight::trace::CountSlot(slot);                      \
    __imp__sub_##addr(ctx, base);                            \
  }
TL_SLOT(8256E4E8, 0)  // ~RenderSystem
TL_SLOT(8256E738, 1)  // getName
TL_SLOT(8256FE98, 2)  // getConfigOptions
TL_SLOT(8256F510, 3)  // setConfigOption
TL_SLOT(82573BD0, 4)  // createHardwareOcclusionQuery
TL_SLOT(824C3FA8, 5)  // destroyHardwareOcclusionQuery
TL_SLOT(8256FB90, 6)  // validateConfigOptions
TL_SLOT(8256FEA0, 7)  // _initialise
TL_SLOT(82574168, 8)  // createRenderSystemCapabilities
TL_SLOT(824C34D0, 9)  // useCustomRenderSystemCapabilities
TL_SLOT(82570BC0, 10)  // reinitialise
TL_SLOT(82570C70, 11)  // shutdown
// case 12 (setAmbientLight) : sub_825BC790, fonction partagée avec le reste du jeu (pas de compteur)
TL_SLOT(82570E40, 15)  // _createRenderWindow
TL_SLOT(825711E0, 16)  // _createRenderWindows
TL_SLOT(82572788, 17)  // createMultiRenderTarget
TL_SLOT(824C3778, 18)  // destroyRenderWindow
TL_SLOT(82572858, 20)  // destroyRenderTarget
TL_SLOT(824C37D0, 21)  // attachRenderTarget
TL_SLOT(824C38D8, 22)  // getRenderTarget
TL_SLOT(824C3928, 23)  // detachRenderTarget
TL_SLOT(824C2F80, 24)  // getRenderTargetIterator
TL_SLOT(82572938, 25)  // getErrorDescription
TL_SLOT(82572A50, 26)  // _useLights
// case 27 (areFixedFunctionLightsInViewSpace) : sub_828AD748, fonction partagée avec le reste du jeu (pas de compteur)
TL_SLOT(824C3CB8, 29)  // _setWorldMatrices
TL_SLOT(821C93A0, 32)  // _setTextureUnitSettings
TL_SLOT(821D2530, 36)  // _setPointSpritesEnabled
TL_SLOT_D3D(821C56A0, 37)  // _setPointParameters
TL_SLOT(824C39D8, 38)  // _setTexture
TL_SLOT(821CA710, 40)  // _setVertexTexture
TL_SLOT(821CAD88, 41)  // _setTextureCoordSet
TL_SLOT(821CB0F0, 42)  // _setTextureCoordCalculation
TL_SLOT_D3D(821CA470, 44)  // _setTextureUnitFiltering
TL_SLOT_D3D(821CA3F0, 45)  // _setTextureUnitFiltering
TL_SLOT_D3D(821CAD08, 46)  // _setTextureLayerAnisotropy
TL_SLOT_D3D(821CA880, 49)  // _setTextureMipmapBias
TL_SLOT_D3D(821CA930, 50)  // _setTextureMatrix (portage Xbox : calcule la matrice sans la transmettre ni la garder)
TL_SLOT(8219B038, 54)  // _setTextureProjectionRelativeTo
TL_SLOT(82573538, 56)  // _pauseFrame
TL_SLOT(82573688, 57)  // _resumeFrame
// case 60 (_getViewport) : sub_821910A8, fonction partagée avec le reste du jeu (pas de compteur)
TL_SLOT(82204268, 62)  // _getCullingMode
TL_SLOT(821F28F8, 70)  // _beginGeometryCount
// case 71 (_getFaceCount) : sub_821F1790, fonction partagée avec le reste du jeu (pas de compteur)
TL_SLOT(8219AE58, 72)  // _getBatchCount
TL_SLOT(824C3CB0, 73)  // _getVertexCount
TL_SLOT(821DB560, 74)  // convertColourValue
TL_SLOT(8257F850, 75)  // getColourVertexElementType
TL_SLOT(8219AD88, 76)  // _convertProjectionMatrix
TL_SLOT(82573AB0, 77)  // _makeProjectionMatrix
TL_SLOT(82572970, 78)  // _makeProjectionMatrix
TL_SLOT(821ABAE8, 79)  // _makeOrthoMatrix
TL_SLOT(82573C80, 80)  // _applyObliqueDepthProjection
TL_SLOT(821C5228, 81)  // _setPolygonMode
TL_SLOT(825730A0, 82)  // setStencilCheckEnabled
TL_SLOT(825730F8, 83)  // setStencilBufferParams
TL_SLOT(824C2F98, 88)  // getDriverVersion
TL_SLOT(824C40E8, 89)  // (méthode absente d'Ogre 1.7.0 : ajout To
TL_SLOT(82573730, 92)  // bindGpuProgramPassIterationParameters
TL_SLOT(821BF120, 94)  // isGpuProgramBound
TL_SLOT(824C3D50, 95)  // setClipPlanes
TL_SLOT(824C3D10, 96)  // addClipPlane
TL_SLOT(824C3CD8, 97)  // addClipPlane
TL_SLOT(824C3DA0, 98)  // resetClipPlanes
TL_SLOT(824C3448, 99)  // _initRenderTargets
TL_SLOT(824C3DE8, 100)  // _notifyCameraRemoved
TL_SLOT(821EE2F8, 102)  // _swapAllRenderTargetBuffers
TL_SLOT(824C3CD0, 103)  // getInvertVertexWinding
TL_SLOT(8219BEB0, 104)  // setInvertVertexWinding
TL_SLOT(82573C70, 108)  // getHorizontalTexelOffset
TL_SLOT(82455DF8, 110)  // getMinimumDepthInputValue
TL_SLOT(82573D48, 111)  // getMaximumDepthInputValue
TL_SLOT(821CEF38, 112)  // setCurrentPassIterationCount
TL_SLOT(821CEF20, 113)  // setDeriveDepthBias
TL_SLOT(824C3E58, 115)  // addListener
TL_SLOT(824C3ED0, 116)  // removeListener
TL_SLOT(824C2FA0, 117)  // getRenderSystemEvents
// case 122 (getDisplayMonitorCount) : sub_828AD7C0, fonction partagée avec le reste du jeu (pas de compteur)
TL_SLOT(824C3F48, 123)  // fireEvent
TL_SLOT(82573870, 124)  // setClipPlanesImpl
TL_SLOT(82572528, 125)  // initialiseFromRenderSystemCapabilities
TL_SLOT(8256E848, 126)  // (propre à D3D9RenderSystem)

// _updateAllRenderTargets (case 101) : une image complète du jeu, toutes cibles comprises.
REX_EXTERN(__imp__sub_821EE1C0);
REX_HOOK_RAW(sub_821EE1C0) {
  torchlight::trace::CountSlot(101);
  torchlight::trace::On();  // démarrage du traceur (et donc des interceptions) dès la première image
  torchlight::capture::BeginGameFrame();
  torchlight::natif::BeginGameFrame();
  __imp__sub_821EE1C0(ctx, base);
  torchlight::natif::EndGameFrame();
  torchlight::capture::EndGameFrame();
}

// unbindGpuProgram (case 93) : programme délié -> pipeline fixe émulé par le portage Xbox d'Ogre.
REX_EXTERN(__imp__sub_821BE470);
REX_HOOK_RAW(sub_821BE470) {
  torchlight::trace::CountSlot(93);
  torchlight::capture::UnbindProgram(ctx.r4.u32);
  torchlight::natif::UnbindProgram(ctx.r4.u32);
  __imp__sub_821BE470(ctx, base);
}

// Pipeline fixe (dessins sans programme) : matrices, modes de combinaison des textures, unités désactivées.
namespace torchlight::trace {
inline void MatrixArg(int kind, PPCContext& ctx, uint8_t* base) {
  float m[16];
  for (int k = 0; k < 16; k++) {
    uint32_t w;
    std::memcpy(&w, base + ctx.r4.u32 + k * 4, 4);
    w = __builtin_bswap32(w);
    std::memcpy(&m[k], &w, 4);
  }
  torchlight::capture::SetMatrix(kind, m);
  torchlight::natif::SetMatrix(kind, m);
}
}  // namespace torchlight::trace
#define TL_MATRIX(addr, slot, kind)                              \
  REX_EXTERN(__imp__sub_##addr);                                 \
  REX_HOOK_RAW(sub_##addr) {                                     \
    torchlight::trace::CountSlot(slot);                          \
    if (ctx.r4.u32) torchlight::trace::MatrixArg(kind, ctx, base); \
    __imp__sub_##addr(ctx, base);                                \
  }
TL_MATRIX(821DD450, 28, 0)  // _setWorldMatrix
TL_MATRIX(821BAC40, 30, 1)  // _setViewMatrix
TL_MATRIX(821BDC50, 31, 2)  // _setProjectionMatrix

// _setTextureAddressingMode(unit, const UVWAddressingMode& {u, v, w}) (case 47) et
// _setTextureBorderColour(unit, const ColourValue&) (case 48).
REX_EXTERN(__imp__sub_821CA1A8);
REX_HOOK_RAW(sub_821CA1A8) {
  torchlight::trace::CountSlot(47);
  if (ctx.r5.u32) {
    uint32_t w[3];
    for (int k = 0; k < 3; k++) {
      std::memcpy(&w[k], base + ctx.r5.u32 + k * 4, 4);
      w[k] = __builtin_bswap32(w[k]);
    }
    torchlight::natif::SetTextureAddressing(ctx.r4.u32, w[0], w[1], w[2]);
  }
  if (torchlight::natif::DrawsHandled()) return;  // état Direct3D seulement
  __imp__sub_821CA1A8(ctx, base);
}
REX_EXTERN(__imp__sub_82572D58);
REX_HOOK_RAW(sub_82572D58) {
  torchlight::trace::CountSlot(48);
  if (ctx.r5.u32) {
    float c[4];
    for (int k = 0; k < 4; k++) {
      uint32_t w;
      std::memcpy(&w, base + ctx.r5.u32 + k * 4, 4);
      w = __builtin_bswap32(w);
      std::memcpy(&c[k], &w, 4);
    }
    torchlight::natif::SetTextureBorder(ctx.r4.u32, c);
  }
  if (torchlight::natif::DrawsHandled()) return;  // état Direct3D seulement
  __imp__sub_82572D58(ctx, base);
}

// _setDepthBias(constantBias f1, slopeScaleBias f2) (case 68). Le portage Xbox d'Ogre envoie à Direct3D
// constantBias * K (K, flottant à 0x820AA720 dans l'exécutable) et -slopeScaleBias : mêmes valeurs ici.
REX_EXTERN(__imp__sub_821D14B8);
REX_HOOK_RAW(sub_821D14B8) {
  torchlight::trace::CountSlot(68);
  static const float k = [&] {
    uint32_t w;
    std::memcpy(&w, base + 0x820AA720u, 4);
    w = __builtin_bswap32(w);
    float f;
    std::memcpy(&f, &w, 4);
    REXLOG_INFO("[natif] _setDepthBias : facteur Direct3D du portage Xbox = {:g}", f);
    return f;
  }();
  torchlight::natif::SetDepthBias(float(ctx.f1.f64) * k, -float(ctx.f2.f64));
  if (torchlight::natif::DrawsHandled()) return;  // état Direct3D seulement
  __imp__sub_821D14B8(ctx, base);
}

REX_EXTERN(__imp__sub_821DC8D8);
REX_HOOK_RAW(sub_821DC8D8) {  // _disableTextureUnit (case 33)
  torchlight::trace::CountSlot(33);
  torchlight::capture::DisableTextureUnit(ctx.r4.u32);
  torchlight::natif::DisableTextureUnit(ctx.r4.u32);
  __imp__sub_821DC8D8(ctx, base);
}
REX_EXTERN(__imp__sub_821BF850);
REX_HOOK_RAW(sub_821BF850) {  // _disableTextureUnitsFrom (case 34)
  torchlight::trace::CountSlot(34);
  torchlight::capture::DisableTextureUnitsFrom(ctx.r4.u32);
  torchlight::natif::DisableTextureUnitsFrom(ctx.r4.u32);
  __imp__sub_821BF850(ctx, base);
}
REX_EXTERN(__imp__sub_821C9100);
REX_HOOK_RAW(sub_821C9100) {  // _setTextureBlendMode(unit, const LayerBlendModeEx&) (case 43)
  torchlight::trace::CountSlot(43);
  if (ctx.r5.u32) {
    uint32_t w[15];
    for (int k = 0; k < 15; k++) {
      std::memcpy(&w[k], base + ctx.r5.u32 + k * 4, 4);
      w[k] = __builtin_bswap32(w[k]);
    }
    torchlight::capture::SetTextureBlendMode(ctx.r4.u32, w);
    torchlight::natif::SetTextureBlendMode(ctx.r4.u32, w);
  }
  if (torchlight::natif::DrawsHandled()) return;  // état Direct3D seulement
  __imp__sub_821C9100(ctx, base);
}
