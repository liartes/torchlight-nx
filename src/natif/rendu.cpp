// torchlight - rendu natif (voir rendu.h). Repris de switch/tools/rendu_hors_ligne (phase 2), validé sur
// des captures : mêmes conventions, mêmes conversions d'états d'Ogre.

#include "rendu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <chrono>

#include "present_spv.h"
#include "shader_pack.h"

namespace torchlight::natif {
namespace fs = std::filesystem;

// --- Fonctions Vulkan ---------------------------------------------------------------------------------

#define TL_VK_INSTANCE_FUNCS(X)              \
  X(vkGetDeviceProcAddr)                     \
  X(vkGetPhysicalDeviceMemoryProperties)     \
  X(vkGetPhysicalDeviceProperties)           \
  X(vkGetPhysicalDeviceFormatProperties)

#define TL_VK_DEVICE_FUNCS(X)     \
  X(vkGetDeviceQueue)             \
  X(vkQueueSubmit)                \
  X(vkQueueWaitIdle)              \
  X(vkDeviceWaitIdle)             \
  X(vkCreateBuffer)               \
  X(vkDestroyBuffer)              \
  X(vkGetBufferMemoryRequirements) \
  X(vkAllocateMemory)             \
  X(vkFreeMemory)                 \
  X(vkBindBufferMemory)           \
  X(vkMapMemory)                  \
  X(vkCreateImage)                \
  X(vkDestroyImage)               \
  X(vkGetImageMemoryRequirements) \
  X(vkBindImageMemory)            \
  X(vkCreateImageView)            \
  X(vkDestroyImageView)           \
  X(vkCreateRenderPass)           \
  X(vkCreateFramebuffer)          \
  X(vkDestroyFramebuffer)         \
  X(vkCreateDescriptorSetLayout)  \
  X(vkCreatePipelineLayout)       \
  X(vkCreateDescriptorPool)       \
  X(vkResetDescriptorPool)        \
  X(vkAllocateDescriptorSets)     \
  X(vkUpdateDescriptorSets)       \
  X(vkCreateSampler)              \
  X(vkCreateShaderModule)         \
  X(vkCreatePipelineCache)        \
  X(vkGetPipelineCacheData)       \
  X(vkCreateGraphicsPipelines)    \
  X(vkCreateCommandPool)          \
  X(vkAllocateCommandBuffers)     \
  X(vkBeginCommandBuffer)         \
  X(vkEndCommandBuffer)           \
  X(vkCreateFence)                \
  X(vkWaitForFences)              \
  X(vkResetFences)                \
  X(vkCmdPipelineBarrier)         \
  X(vkCmdBeginRenderPass)         \
  X(vkCmdEndRenderPass)           \
  X(vkCmdClearAttachments)        \
  X(vkCmdSetViewport)             \
  X(vkCmdSetScissor)              \
  X(vkCmdSetDepthBias)            \
  X(vkCmdBindPipeline)            \
  X(vkCmdBindDescriptorSets)      \
  X(vkCmdBindVertexBuffers)       \
  X(vkCmdBindIndexBuffer)         \
  X(vkCmdDraw)                    \
  X(vkCmdDrawIndexed)             \
  X(vkCmdCopyBufferToImage)       \
  X(vkCmdCopyImageToBuffer)

struct Vk {
#define TL_DECL(name) PFN_##name name = nullptr;
  TL_VK_INSTANCE_FUNCS(TL_DECL)
  TL_VK_DEVICE_FUNCS(TL_DECL)
#undef TL_DECL

  bool Load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkDevice device) {
    bool ok = true;
#define TL_LOAD_I(name)                                                              \
  name = reinterpret_cast<PFN_##name>(gipa(instance, #name));                       \
  ok = ok && name;
    TL_VK_INSTANCE_FUNCS(TL_LOAD_I)
#undef TL_LOAD_I
    if (!ok) return false;
#define TL_LOAD_D(name)                                                              \
  name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));          \
  ok = ok && name;
    TL_VK_DEVICE_FUNCS(TL_LOAD_D)
#undef TL_LOAD_D
    return ok;
  }
};

// --- Contexte autonome (outil hors ligne) ----------------------------------------------------------------

bool CreateStandaloneContext(PFN_vkGetInstanceProcAddr gipa, const char* prefer, Context& out) {
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!create_instance) return false;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "torchlight_natif";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance;
  if (create_instance(&ici, nullptr, &instance) != VK_SUCCESS) return false;
  auto enum_devices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
  auto get_props =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  auto get_qf = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  auto get_features =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance, "vkGetPhysicalDeviceFeatures"));
  auto create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
  uint32_t n = 0;
  enum_devices(instance, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  enum_devices(instance, &n, devs.data());
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  for (VkPhysicalDevice d : devs) {
    VkPhysicalDeviceProperties p;
    get_props(d, &p);
    if (!physical || (prefer && std::strstr(p.deviceName, prefer))) physical = d;
  }
  if (!physical) return false;
  VkPhysicalDeviceProperties props;
  get_props(physical, &props);
  std::printf("GPU : %s\n", props.deviceName);
  uint32_t qn = 0;
  get_qf(physical, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  get_qf(physical, &qn, qf.data());
  uint32_t family = 0;
  for (uint32_t i = 0; i < qn; i++)
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      family = i;
      break;
    }
  VkPhysicalDeviceFeatures supported{}, features{};
  get_features(physical, &supported);
  features.textureCompressionBC = supported.textureCompressionBC;
  features.depthClamp = supported.depthClamp;
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &features;
  VkDevice device;
  if (create_device(physical, &dci, nullptr, &device) != VK_SUCCESS) return false;
  out.get_instance_proc_addr = gipa;
  out.instance = instance;
  out.physical = physical;
  out.device = device;
  out.queue_family = family;
  return true;
}

namespace {

// --- Bibliothèque de shaders (SPIR-V + réflexion glslang + emplacements) ------------------------------

struct Uniform {
  std::string name;
  int offset;
  uint32_t gl_type;
  int array_size;
  int array_stride;
  int binding;
};
struct Shader {
  std::vector<uint32_t> spirv;
  std::vector<Uniform> uniforms;  // membres de $Global (offset >= 0) et échantillonneurs (binding >= 0)
  int global_size = 0;
  std::vector<std::pair<uint32_t, std::string>> inputs;  // emplacement, sémantique (shader de sommets)
  std::map<uint32_t, std::string> samplers;                // unité -> 1D, 2D, 3D, CUBE
  VkShaderModule module = VK_NULL_HANDLE;
};

// Lecture d'un fichier entier avec stdio : sur Switch, une erreur de lecture de la carte SD fait lever une
// exception à std::ifstream (basic_filebuf::underflow), qui arrêtait le jeu. Ici : trois essais, jamais
// d'exception, vecteur vide en cas d'échec.
std::vector<uint8_t> ReadBinary(const fs::path& p) {
  for (int attempt = 0; attempt < 3; attempt++) {
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return {};
    std::vector<uint8_t> data;
    uint8_t buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (!failed) return data;
  }
  return {};
}

std::string ReadText(const fs::path& p) {
  const auto data = ReadBinary(p);
  return std::string(data.begin(), data.end());
}

bool FileExists(const fs::path& p) {
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}

int FieldInt(const std::string& line, const std::string& key, int def = -1) {
  const size_t p = line.find(key + " ");
  if (p == std::string::npos) return def;
  return int(std::strtol(line.c_str() + p + key.size() + 1, nullptr, 0));
}
uint32_t FieldHex(const std::string& line, const std::string& key) {
  const size_t p = line.find(key + " ");
  if (p == std::string::npos) return 0;
  return uint32_t(std::strtoul(line.c_str() + p + key.size() + 1, nullptr, 16));
}

// <nom>.tlsh (cache construit par le jeu, shader_pack.h) ou <nom>.spv + .refl + .io (build_lib.sh).
std::optional<Shader> LoadShader(const fs::path& path) {
  Shader s;
  std::string refl_text, io_text;
  if (path.extension() == ".tlsh") {
    if (!pack::Read(path.string(), s.spirv, refl_text, io_text) || s.spirv.size() < 5) return std::nullopt;
  } else {
    auto bytes = ReadBinary(path);
    if (bytes.size() < 20) return std::nullopt;
    s.spirv.resize(bytes.size() / 4);
    std::memcpy(s.spirv.data(), bytes.data(), s.spirv.size() * 4);
    fs::path refl = path, io = path;
    refl.replace_extension(".refl");
    io.replace_extension(".io");
    refl_text = ReadText(refl);
    io_text = ReadText(io);
  }
  std::istringstream in(refl_text);
  std::string line, section;
  while (std::getline(in, line)) {
    if (line.find("reflection:") != std::string::npos) {
      section = line;
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = line.substr(0, colon);
    if (section.rfind("Uniform reflection", 0) == 0) {
      Uniform u;
      u.name = name;
      u.offset = FieldInt(line, "offset");
      u.gl_type = FieldHex(line, "type");
      u.array_size = FieldInt(line, "size", 1);
      u.array_stride = FieldInt(line, "arrayStride", 0);
      u.binding = FieldInt(line, "binding");
      s.uniforms.push_back(u);
    } else if (section.rfind("Uniform block reflection", 0) == 0 && name == "$Global") {
      s.global_size = FieldInt(line, "size", 0);
    }
  }
  std::istringstream iin(io_text);
  std::string dir, sem;
  uint32_t loc;
  while (iin >> dir >> loc >> sem) {
    if (dir == "in") s.inputs.push_back({loc, sem});
    if (dir == "sampler") s.samplers[loc] = sem;
  }
  return s;
}

// Taille en octets d'un registre (colonne GLSL) selon le type GL de la réflexion, et nombre de registres.
void RegisterShape(uint32_t gl_type, uint32_t& reg_bytes, uint32_t& regs) {
  switch (gl_type) {
    case 0x1406: reg_bytes = 4; regs = 1; break;   // float
    case 0x8B50: reg_bytes = 8; regs = 1; break;   // vec2
    case 0x8B51: reg_bytes = 12; regs = 1; break;  // vec3
    case 0x8B52: reg_bytes = 16; regs = 1; break;  // vec4
    case 0x8B5A: reg_bytes = 8; regs = 2; break;   // mat2
    case 0x8B5B: reg_bytes = 12; regs = 3; break;  // mat3
    case 0x8B5C: reg_bytes = 16; regs = 4; break;  // mat4
    case 0x8B65: reg_bytes = 12; regs = 2; break;  // mat2x3
    case 0x8B66: reg_bytes = 16; regs = 2; break;  // mat2x4
    case 0x8B67: reg_bytes = 8; regs = 3; break;   // mat3x2
    case 0x8B68: reg_bytes = 16; regs = 3; break;  // mat3x4
    case 0x8B69: reg_bytes = 8; regs = 4; break;   // mat4x2
    case 0x8B6A: reg_bytes = 12; regs = 4; break;  // mat4x3
    case 0x1404: reg_bytes = 4; regs = 1; break;   // int
    case 0x8B56: reg_bytes = 4; regs = 1; break;   // bool
    default: reg_bytes = 16; regs = 1; break;
  }
}

// Remplit le bloc $Global d'un shader à partir des constantes nommées d'Ogre.
void BuildGlobals(const Shader& sh, const Constants& c, std::map<std::string, uint32_t>& missing,
                  std::vector<uint8_t>& buf, const float alpha_test[4] = nullptr) {
  buf.assign(std::max(sh.global_size, 16), 0);
  for (const Uniform& u : sh.uniforms) {
    if (u.offset < 0) continue;
    if (u.name == "torchlight_alpha_test") {  // ajouté par prep_hlsl.py : (seuil / 255, fonction, 0, 0)
      const float def[4] = {0.0f, 1.0f, 0.0f, 0.0f};
      if (size_t(u.offset) + 16 <= buf.size()) std::memcpy(&buf[u.offset], alpha_test ? alpha_test : def, 16);
      continue;
    }
    const ConstantNames& names = c.Names();
    auto it = names.find(u.name);
    if (it == names.end()) {
      missing[u.name]++;
      continue;
    }
    const OgreConst& oc = it->second;
    uint32_t reg_bytes, regs;
    RegisterShape(u.gl_type, reg_bytes, regs);
    const uint32_t ogre_regs = std::max(1u, oc.elem_size / 4);
    const uint32_t elements = std::min<uint32_t>(oc.array_size, std::max(1, u.array_size));
    const uint32_t stride = u.array_stride > 0 ? uint32_t(u.array_stride) : regs * 16;
    for (uint32_t e = 0; e < elements; e++) {
      for (uint32_t r = 0; r < std::min(regs, ogre_regs); r++) {
        const size_t src = oc.phys + (size_t(e) * ogre_regs + r) * 4;
        const size_t dst = size_t(u.offset) + size_t(e) * stride + size_t(r) * 16;
        if (src + reg_bytes / 4 > c.floats.size() || dst + reg_bytes > buf.size()) continue;
        std::memcpy(&buf[dst], &c.floats[src], reg_bytes);
      }
    }
  }
}

// Constantes des shaders d'émulation du pipeline fixe (switch/shaders/ffp), présentées comme des constantes
// nommées d'Ogre (registres de 4 flottants) pour réutiliser BuildGlobals.
void FfpConstants(const DrawDesc& d, Constants& vs, Constants& ps) {
  // tl_wvp = projection * vue * monde (Matrix4 d'Ogre : par lignes, vecteur colonne).
  float pv[16], m[16];
  auto mul = [](const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++) {
        float acc = 0;
        for (int k = 0; k < 4; k++) acc += a[i * 4 + k] * b[k * 4 + j];
        out[i * 4 + j] = acc;
      }
  };
  mul(d.matrix[2], d.matrix[1], pv);
  mul(pv, d.matrix[0], m);
  vs.floats.assign(m, m + 16);
  vs.names["tl_wvp"] = {19, 0, 16, 1};
  // Étages : un étage n'est actif que si son unité a une texture (sinon Ogre l'a désactivé).
  ps.floats.assign(40, 0.0f);
  for (uint32_t i = 0; i < 2; i++) {
    const bool enabled =
        std::any_of(d.textures.begin(), d.textures.end(), [&](const TextureRef& t) { return t.unit == i; });
    const FfpStage& c = d.stage[i][0];
    const FfpStage& a = d.stage[i][1];
    float* op_c = &ps.floats[0 + i * 4];
    float* op_a = &ps.floats[8 + i * 4];
    op_c[0] = float(c.set ? c.op : 2);
    op_c[1] = float(c.set ? c.src1 : 1);
    op_c[2] = float(c.set ? c.src2 : 0);
    op_c[3] = enabled ? 1.0f : 0.0f;
    op_a[0] = float(a.set ? a.op : 2);
    op_a[1] = float(a.set ? a.src1 : 1);
    op_a[2] = float(a.set ? a.src2 : 0);
    op_a[3] = op_c[3];
    std::memcpy(&ps.floats[16 + i * 4], &c.arg[0], 16);  // colourArg1
    std::memcpy(&ps.floats[24 + i * 4], &c.arg[4], 16);  // colourArg2
    ps.floats[32 + i * 4 + 0] = a.arg[8];                 // alphaArg1
    ps.floats[32 + i * 4 + 1] = a.arg[9];                 // alphaArg2
    ps.floats[32 + i * 4 + 2] = c.arg[10];                // factor
  }
  ps.names["tl_colour_op"] = {4, 0, 4, 2};
  ps.names["tl_alpha_op"] = {4, 8, 4, 2};
  ps.names["tl_colour_arg1"] = {4, 16, 4, 2};
  ps.names["tl_colour_arg2"] = {4, 24, 4, 2};
  ps.names["tl_alpha_arg"] = {4, 32, 4, 2};
}

// --- Formats et états Ogre -> Vulkan ------------------------------------------------------------------

// VertexElementType d'Ogre : format Vulkan, taille, taille d'un composant à inverser.
struct VFormat {
  VkFormat format;
  uint32_t size, swap;
};
std::optional<VFormat> VertexFormat(uint32_t t) {
  switch (t) {
    case 0: return VFormat{VK_FORMAT_R32_SFLOAT, 4, 4};
    case 1: return VFormat{VK_FORMAT_R32G32_SFLOAT, 8, 4};
    case 2: return VFormat{VK_FORMAT_R32G32B32_SFLOAT, 12, 4};
    case 3: return VFormat{VK_FORMAT_R32G32B32A32_SFLOAT, 16, 4};
    case 4:   // VET_COLOUR : sur Direct3D, ARGB
    case 10:  // VET_COLOUR_ARGB : u32 0xAARRGGBB -> octets B G R A une fois en petit-boutiste
      return VFormat{VK_FORMAT_B8G8R8A8_UNORM, 4, 4};
    case 11: return VFormat{VK_FORMAT_R8G8B8A8_UNORM, 4, 4};  // VET_COLOUR_ABGR
    case 5: return VFormat{VK_FORMAT_R16_SSCALED, 2, 2};
    case 6: return VFormat{VK_FORMAT_R16G16_SSCALED, 4, 2};
    case 7: return VFormat{VK_FORMAT_R16G16B16_SSCALED, 6, 2};
    case 8: return VFormat{VK_FORMAT_R16G16B16A16_SSCALED, 8, 2};
    // VET_UBYTE4 (indices d'os) : le Xenos lit ces 4 octets comme un mot de 32 bits gros-boutiste ; les shaders
    // du portage Xbox d'Ogre en tiennent compte (premier os en composante [3]). Retourner le mot, comme le Xenos.
    case 9: return VFormat{VK_FORMAT_R8G8B8A8_USCALED, 4, 4};
    default: return std::nullopt;
  }
}

// VertexElementSemantic d'Ogre -> emplacement (même table que prep_hlsl.py).
std::optional<uint32_t> SemanticLocation(uint32_t semantic, uint32_t index) {
  switch (semantic) {
    case 1: return 0;          // POSITION
    case 2: return 1;          // BLEND_WEIGHTS
    case 3: return 2;          // BLEND_INDICES
    case 4: return 3;          // NORMAL
    case 5: return 4;          // DIFFUSE (COLOR0)
    case 6: return 5;          // SPECULAR (COLOR1)
    case 7: return 6 + index;  // TEXTURE_COORDINATES
    case 8: return 14;         // BINORMAL
    case 9: return 15;         // TANGENT
    default: return std::nullopt;
  }
}

VkBlendFactor BlendFactor(uint32_t f) {
  static const VkBlendFactor t[] = {VK_BLEND_FACTOR_ONE,
                                    VK_BLEND_FACTOR_ZERO,
                                    VK_BLEND_FACTOR_DST_COLOR,
                                    VK_BLEND_FACTOR_SRC_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                                    VK_BLEND_FACTOR_DST_ALPHA,
                                    VK_BLEND_FACTOR_SRC_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
  return f < 10 ? t[f] : VK_BLEND_FACTOR_ONE;
}
VkBlendOp BlendOp(uint32_t o) {
  static const VkBlendOp t[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
  return o < 5 ? t[o] : VK_BLEND_OP_ADD;
}
VkCompareOp CompareOp(uint32_t f) {
  static const VkCompareOp t[] = {VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_ALWAYS,    VK_COMPARE_OP_LESS,
                                  VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL,     VK_COMPARE_OP_NOT_EQUAL,
                                  VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
  return f < 8 ? t[f] : VK_COMPARE_OP_LESS_OR_EQUAL;
}
std::optional<VkPrimitiveTopology> Topology(uint32_t t) {
  switch (t) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return std::nullopt;
  }
}
// Format Xenos (données détilées et dans l'ordre de l'hôte).
std::optional<VkFormat> TextureFormat(uint32_t f) {
  switch (f) {
    case 18: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  // k_DXT1
    case 19: return VK_FORMAT_BC2_UNORM_BLOCK;       // k_DXT2_3
    case 20: return VK_FORMAT_BC3_UNORM_BLOCK;       // k_DXT4_5
    case 6: return VK_FORMAT_B8G8R8A8_UNORM;         // k_8_8_8_8 (ARGB big-endian -> BGRA)
    case 2: return VK_FORMAT_R8_UNORM;               // k_8
    case 10: return VK_FORMAT_R8G8_UNORM;            // k_8_8
    default: return std::nullopt;
  }
}

uint64_t Fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

std::string HashName(uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
  return b;
}

// --- Rendu --------------------------------------------------------------------------------------------

constexpr int kSlots = 2;                         // images en vol
constexpr VkDeviceSize kChunkSize = 64ull << 20;  // tampon d'envoi (sommets, indices, constantes, textures)
constexpr uint32_t kSetsPerPool = 4096;
constexpr uint64_t kTextureIdleFrames = 600;      // texture inutilisée depuis ce nombre d'images : libérée
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  VkDeviceSize size = 0, used = 0;
};
struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

// Cible de rendu d'Ogre (texture rendue ou fenêtre) : image couleur + profondeur.
struct Target {
  std::string name;
  uint32_t width = 0, height = 0;
  Image color, depth;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool depth_ready = false;
  bool rendered = false;
};

struct TextureEntry {
  Image image;  // image.image nul : texture inutilisable (format...)
  bool cube = false;
  uint64_t last_used = 0;
};

struct Slot {
  VkCommandBuffer pre = VK_NULL_HANDLE;   // envois de textures, avant les passes
  VkCommandBuffer main = VK_NULL_HANDLE;  // passes de rendu
  VkFence fence = VK_NULL_HANDLE;
  bool submitted = false;
  std::vector<Buffer> chunks;
  size_t chunk = 0;
  std::vector<VkDescriptorPool> pools;
  size_t pool = 0;
  std::vector<std::function<void()>> deferred;  // destructions après la fin du GPU
};

class RendererImpl final : public Renderer {
 public:
  RendererImpl(const Context& ctx, fs::path lib, VkFormat output_format)
      : ctx_(ctx), lib_(std::move(lib)), output_format_(output_format) {}

  ~RendererImpl() override {
    if (vk_.vkDeviceWaitIdle && ctx_.device) vk_.vkDeviceWaitIdle(ctx_.device);
  }

  bool Init() {
    if (!vk_.Load(ctx_.get_instance_proc_addr, ctx_.instance, ctx_.device)) {
      Log("rendu natif : fonctions Vulkan introuvables");
      return false;
    }
    dev_ = ctx_.device;
    vk_.vkGetPhysicalDeviceMemoryProperties(ctx_.physical, &mem_);
    vk_.vkGetDeviceQueue(dev_, ctx_.queue_family, 0, &queue_);
    IndexLibrary();

    // Passe des cibles : couleur et profondeur conservées ; les effacements passent par
    // vkCmdClearAttachments (limités au viewport, comme Direct3D).
    VkAttachmentDescription att[2]{};
    att[0].format = kColorFormat;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = kDepthFormat;
    att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    if (!Check(vk_.vkCreateRenderPass(dev_, &rpci, nullptr, &pass_), "vkCreateRenderPass")) return false;

    // Descripteurs : set 0 = $Global du shader de sommets ; set 1 = $Global du shader de pixels + unités 16..31.
    VkDescriptorSetLayoutBinding b0{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo l0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l0.bindingCount = 1;
    l0.pBindings = &b0;
    if (!Check(vk_.vkCreateDescriptorSetLayout(dev_, &l0, nullptr, &set_layout_[0]), "set 0")) return false;
    std::vector<VkDescriptorSetLayoutBinding> b1;
    b1.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (uint32_t u = 0; u < 16; u++)
      b1.push_back({16 + u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo l1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l1.bindingCount = uint32_t(b1.size());
    l1.pBindings = b1.data();
    if (!Check(vk_.vkCreateDescriptorSetLayout(dev_, &l1, nullptr, &set_layout_[1]), "set 1")) return false;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = set_layout_;
    if (!Check(vk_.vkCreatePipelineLayout(dev_, &plci, nullptr, &pipeline_layout_), "pipeline layout"))
      return false;
    // Cache de pipelines du pilote, gardé entre deux lancements (construit sur cette machine) : sur NVK, créer
    // un pipeline sans cache coûte des dizaines de ms. Le pilote ignore de lui-même un cache incompatible.
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    std::vector<uint8_t> cache_data;
    if (!ctx_.pipeline_cache_file.empty()) cache_data = ReadBinary(ctx_.pipeline_cache_file);
    pcci.initialDataSize = cache_data.size();
    pcci.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
    if (vk_.vkCreatePipelineCache(dev_, &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
      pcci.initialDataSize = 0;
      pcci.pInitialData = nullptr;
      vk_.vkCreatePipelineCache(dev_, &pcci, nullptr, &pipeline_cache_);
    }
    if (!cache_data.empty()) Log("rendu natif : cache de pipelines relu (" + std::to_string(cache_data.size()) + " octets)");

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.0f;
    if (!Check(vk_.vkCreateSampler(dev_, &sci, nullptr, &sampler_), "sampler")) return false;
    // Textures rendues par le jeu : bords bloqués (projection sur le décor, pas de répétition).
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Check(vk_.vkCreateSampler(dev_, &sci, nullptr, &sampler_clamp_), "sampler clamp")) return false;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx_.queue_family;
    if (!Check(vk_.vkCreateCommandPool(dev_, &pci, nullptr, &cmd_pool_), "command pool")) return false;
    for (Slot& s : slots_) {
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = cmd_pool_;
      cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cai.commandBufferCount = 2;
      VkCommandBuffer cbs[2];
      if (!Check(vk_.vkAllocateCommandBuffers(dev_, &cai, cbs), "command buffers")) return false;
      s.pre = cbs[0];
      s.main = cbs[1];
      VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (!Check(vk_.vkCreateFence(dev_, &fci, nullptr, &s.fence), "fence")) return false;
    }

    // Attributs absents de la déclaration : (0, 0, 0, 1) ; couleur de sommet en pipeline fixe : blanc.
    defaults_ = CreateBuffer(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!defaults_.buffer) return false;
    const float defaults[8] = {0, 0, 0, 1, 1, 1, 1, 1};
    std::memcpy(defaults_.mapped, defaults, sizeof(defaults));

    // Textures de repli : blanche (2D) et cubemap noire (pas de reflet plutôt qu'un reflet blanc qui sature).
    white_ = CreateImage(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
    black_cube_ = CreateImage(VK_FORMAT_R8G8B8A8_UNORM, 4, 4,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                              true);
    if (!white_.image || !black_cube_.image) return false;
    pending_fallbacks_ = true;

    if (output_format_ != VK_FORMAT_UNDEFINED && !InitPresent()) return false;
    return true;
  }

  // --- API ---

  void BeginFrame() override {
    if (in_frame_) Submit();
    slot_index_ = (slot_index_ + 1) % kSlots;
    Slot& s = slots_[slot_index_];
    if (s.submitted) {
      vk_.vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX);
      vk_.vkResetFences(dev_, 1, &s.fence);
      s.submitted = false;
    }
    for (auto& f : s.deferred) f();
    s.deferred.clear();
    s.chunk = 0;
    for (Buffer& b : s.chunks) b.used = 0;
    for (VkDescriptorPool p : s.pools) vk_.vkResetDescriptorPool(dev_, p, 0);
    s.pool = 0;
    frame_++;
    EvictTextures();

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(s.pre, &cbbi);
    vk_.vkBeginCommandBuffer(s.main, &cbbi);
    in_frame_ = true;
    in_pass_ = false;
    current_ = nullptr;
    pending_clear_buffers_ = 0;
    viewport_[0] = viewport_[1] = viewport_[2] = viewport_[3] = 0;
    if (pending_fallbacks_) {
      std::vector<uint8_t> px(4 * 4 * 4, 255);
      UploadImage(white_, px.data(), px.size(), 4, 4, 1);
      std::vector<uint8_t> black(4 * 4 * 4 * 6, 0);
      UploadImage(black_cube_, black.data(), black.size(), 4, 4, 6);
      pending_fallbacks_ = false;
    }
  }

  void SetTarget(const std::string& name, uint32_t width, uint32_t height) override {
    if (!in_frame_) return;
    FlushClear();
    EndPass();
    current_ = GetTarget(name, width, height);
  }

  void SetViewport(int32_t x, int32_t y, int32_t w, int32_t h) override {
    if (!in_frame_) return;
    EndPass();
    viewport_[0] = x;
    viewport_[1] = y;
    viewport_[2] = w;
    viewport_[3] = h;
  }

  void Clear(uint32_t buffers, const float colour[4], float depth) override {
    if (!in_frame_) return;
    // Plusieurs effacements avant le dessin : on garde le dernier pour chaque partie.
    if (buffers & 1) std::memcpy(pending_colour_, colour, sizeof(pending_colour_));
    if (buffers & 2) pending_depth_ = depth;
    pending_clear_buffers_ |= buffers;
    EndPass();  // effacement au milieu d'une cible : nouvelle passe
  }

  void EndTarget() override {
    if (!in_frame_) return;
    FlushClear();
    EndPass();
  }

  void Draw(const DrawDesc& d) override {
    if (!in_frame_ || !current_) return;
    if (d.indexed ? !d.icount : !d.vcount) return;  // dessin vide (Direct3D ne dessine rien)
    Shader* vs = ShaderFor(d.ffp, d.vs, 0);
    Shader* ps = ShaderFor(d.ffp, d.ps, 1);
    if (!vs || !ps) return Skip("shader absent de la bibliothèque");
    auto topo = Topology(d.type);
    if (!topo) return Skip("topologie " + std::to_string(d.type));
    Slot& s = slots_[slot_index_];

    using Clock = std::chrono::steady_clock;
    auto lap = [t = Clock::now()](double& acc) mutable {
      const auto now = Clock::now();
      acc += std::chrono::duration<double, std::milli>(now - t).count();
      t = now;
    };
    double ignore = 0;
    lap(ignore);
    // Indices d'abord : seule la plage du dessin, convertie dans un tampon en cache (lire la mémoire du GPU,
    // souvent non cachée, est très lent), et l'indice maximal donne la plage de sommets utile.
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize oib = 0;
    uint32_t vertex_count = d.vcount;
    if (d.indexed) {
      const size_t isize = d.index.index_size == 2 ? 2 : 4;
      const size_t first = size_t(d.istart) * isize, bytes = size_t(d.icount) * isize;
      if (!d.index.data || !bytes || first + bytes > d.index.size)
        return Skip("plage d'indices hors du tampon (début " + std::to_string(d.istart) + ", nombre " +
                    std::to_string(d.icount) + ", taille " + std::to_string(isize) + ", tampon " +
                    std::to_string(d.index.size) + " octets)");
      scratch_.resize(bytes);
      uint32_t max_index = 0;
      if (isize == 2) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(d.index.data + first);
        uint16_t* dst = reinterpret_cast<uint16_t*>(scratch_.data());
        for (size_t k = 0; k < d.icount; k++) {
          const uint16_t v = __builtin_bswap16(src[k]);
          dst[k] = v;
          max_index = std::max<uint32_t>(max_index, v);
        }
      } else {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(d.index.data + first);
        uint32_t* dst = reinterpret_cast<uint32_t*>(scratch_.data());
        for (size_t k = 0; k < d.icount; k++) {
          const uint32_t v = __builtin_bswap32(src[k]);
          dst[k] = v;
          max_index = std::max(max_index, v);
        }
      }
      if (!Push(scratch_.data(), bytes, 16, ib, oib)) return Skip("tampon d'envoi plein");
      vertex_count = max_index + 1;
    }

    // Sommets : plage [vstart, vstart + vertex_count) de chaque tampon, convertie dans l'ordre des octets de
    // l'hôte (en cache), puis copiée d'un bloc. Les indices sont relatifs à vstart (BaseVertexIndex de D3D9).
    bindings_.clear();
    attributes_.clear();
    vk_buffers_.clear();
    vk_offsets_.clear();
    provided_.clear();
    for (const VertexStream& vb : d.streams) {
      if (!vb.data || !vb.size || !vb.stride) return Skip("tampon de sommets vide");
      const size_t first = size_t(d.vstart) * vb.stride, bytes = size_t(vertex_count) * vb.stride;
      if (!bytes || first + bytes > vb.size)
        return Skip("plage de sommets hors du tampon (début " + std::to_string(d.vstart) + ", nombre " +
                    std::to_string(vertex_count) + ", pas " + std::to_string(vb.stride) + ", tampon " +
                    std::to_string(vb.size) + " octets)");
      // Plan par mot de 32 bits du sommet : 0 copie, 1 inversion 32 bits, 2 inversion de deux 16 bits.
      // Écriture directe et séquentielle dans le tampon d'envoi (mémoire du GPU : jamais relue).
      bool aligned = vb.stride % 4 == 0;
      plan_.assign(vb.stride / 4, 0);
      for (const VertexElement& e : d.elems) {
        if (e.source != vb.source) continue;
        auto f = VertexFormat(e.type);
        if (!f) return Skip("type de sommet " + std::to_string(e.type));
        if (e.offset + f->size > vb.stride || f->swap <= 1) continue;
        for (uint32_t c = 0; c + f->swap <= f->size; c += f->swap) {
          const uint32_t off = e.offset + c;
          if (f->swap == 4 && off % 4 == 0)
            plan_[off / 4] = 1;
          else if (f->swap == 2 && aligned)
            plan_[off / 4] = 2;
          else
            aligned = false;
        }
      }
      if (!aligned) return Skip("sommet non aligné sur 4 octets");
      VkBuffer buffer;
      VkDeviceSize offset;
      uint8_t* out = Push(nullptr, bytes, 16, buffer, offset);
      if (!out) return Skip("tampon d'envoi plein");
      const uint8_t* in = vb.data + first;
      const size_t words = bytes / 4;
      if (std::all_of(plan_.begin(), plan_.end(), [](uint8_t k) { return k == 1; })) {
        for (size_t i = 0; i < words; i++) {
          uint32_t w;
          std::memcpy(&w, in + i * 4, 4);
          w = __builtin_bswap32(w);
          std::memcpy(out + i * 4, &w, 4);
        }
      } else {
        const size_t per_vertex = plan_.size();
        const uint8_t* plan = plan_.data();
        for (size_t v = 0; v < vertex_count; v++) {
          for (size_t k = 0; k < per_vertex; k++) {
            const size_t i = (v * per_vertex + k) * 4;
            uint32_t w;
            std::memcpy(&w, in + i, 4);
            if (plan[k] == 1)
              w = __builtin_bswap32(w);
            else if (plan[k] == 2)
              w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
            std::memcpy(out + i, &w, 4);
          }
        }
      }
      const uint32_t binding = uint32_t(bindings_.size());
      bindings_.push_back({binding, vb.stride, VK_VERTEX_INPUT_RATE_VERTEX});
      for (const VertexElement& e : d.elems) {
        if (e.source != vb.source) continue;
        auto loc = SemanticLocation(e.semantic, e.index);
        auto f = VertexFormat(e.type);
        if (!loc || !f) continue;
        attributes_.push_back({*loc, binding, f->format, e.offset});
        provided_.push_back(*loc);
      }
      vk_buffers_.push_back(buffer);
      vk_offsets_.push_back(offset);
    }
    // Attributs lus par le shader mais absents de la déclaration : (0, 0, 0, 1) ; en pipeline fixe, la couleur
    // du sommet (emplacement 4) vaut blanc, comme sous Direct3D 9.
    int default_binding = -1, white_binding = -1;
    for (const auto& [loc, sem] : vs->inputs) {
      if (std::find(provided_.begin(), provided_.end(), loc) != provided_.end()) continue;
      int& b = (d.ffp && loc == 4) ? white_binding : default_binding;
      if (b < 0) {
        b = int(bindings_.size());
        bindings_.push_back({uint32_t(b), 0, VK_VERTEX_INPUT_RATE_INSTANCE});
        vk_buffers_.push_back(defaults_.buffer);
        vk_offsets_.push_back(&b == &white_binding ? 16 : 0);
      }
      attributes_.push_back({loc, uint32_t(b), VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    attributes_.erase(std::remove_if(attributes_.begin(), attributes_.end(),
                                     [&](const VkVertexInputAttributeDescription& a) {
                                       return std::none_of(vs->inputs.begin(), vs->inputs.end(),
                                                           [&](const auto& in) { return in.first == a.location; });
                                     }),
                      attributes_.end());

    lap(stats_.ms_vertices);
    VkPipeline pipeline = GetPipeline(d, *vs, *ps, *topo);
    if (!pipeline) return Skip("création du pipeline");

    // Constantes.
    Constants ffp_vs, ffp_ps;
    const Constants* cvs = d.consts[0];
    const Constants* cps = d.consts[1];
    if (d.ffp) {
      FfpConstants(d, ffp_vs, ffp_ps);
      cvs = &ffp_vs;
      cps = &ffp_ps;
    }
    static const Constants kEmpty;
    BuildGlobals(*vs, cvs ? *cvs : kEmpty, stats_.missing_consts, globals_[0]);
    const float alpha_test[4] = {float(d.alpha_value) / 255.0f, float(d.alpha_func), 0.0f, 0.0f};
    BuildGlobals(*ps, cps ? *cps : kEmpty, stats_.missing_consts, globals_[1], alpha_test);
    VkBuffer ubuf[2];
    VkDeviceSize uoff[2];
    for (int k = 0; k < 2; k++)
      if (!Push(globals_[k].data(), globals_[k].size(), 256, ubuf[k], uoff[k])) return Skip("tampon d'envoi plein");

    lap(stats_.ms_constants);
    // Textures : une cible déjà rendue remplace le contenu de la mémoire du jeu (périmé).
    std::array<VkDescriptorImageInfo, 16> ii;
    for (uint32_t u = 0; u < 16; u++) {
      auto st = ps->samplers.find(u);
      const bool cube_slot = st != ps->samplers.end() && st->second == "CUBE";
      ii[u] = {sampler_, cube_slot ? black_cube_.view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    for (const TextureRef& t : d.textures) {
      if (t.unit >= 16) continue;
      const VkSampler chosen = t.address[0] >= 0 ? SamplerFor(t) : VK_NULL_HANDLE;
      if (Target* rt = RenderedTexture(t.name); rt && rt != current_) {
        if (rt->color_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
          EndPass();
          Barrier(s.main, rt->color.image, VK_IMAGE_ASPECT_COLOR_BIT, rt->color_layout,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
          rt->color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        ii[t.unit] = {chosen ? chosen : sampler_clamp_, rt->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        stats_.substitutions[t.name + " -> " + rt->name]++;
        continue;
      }
      auto st = ps->samplers.find(t.unit);
      const bool cube_slot = st != ps->samplers.end() && st->second == "CUBE";
      if (t.cube != cube_slot) continue;
      if (const TextureEntry* te = GetTexture(t)) {
        ii[t.unit].imageView = te->image.view;
        if (chosen) ii[t.unit].sampler = chosen;
      }
    }
    BeginPass();
    lap(stats_.ms_textures);

    VkDescriptorSet sets[2];
    if (!AllocateSets(set_layout_, 2, sets)) return Skip("descripteurs");
    VkDescriptorBufferInfo bi[2] = {{ubuf[0], uoff[0], globals_[0].size()}, {ubuf[1], uoff[1], globals_[1].size()}};
    VkWriteDescriptorSet w[3]{};
    for (int k = 0; k < 2; k++) {
      w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[k].dstSet = sets[k];
      w[k].dstBinding = 0;
      w[k].descriptorCount = 1;
      w[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[k].pBufferInfo = &bi[k];
    }
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = sets[1];
    w[2].dstBinding = 16;
    w[2].descriptorCount = 16;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[2].pImageInfo = ii.data();
    vk_.vkUpdateDescriptorSets(dev_, 3, w, 0, nullptr);
    lap(stats_.ms_descriptors);

    VkCommandBuffer cmd = s.main;
    vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    // Décalage absolu de Direct3D -> unités de profondeur de Vulkan : en D32_SFLOAT, l'unité vaut 2^-24 pour une
    // profondeur dans [0,5 ; 1[, soit celle d'une profondeur de 24 bits (Xenos, D24S8).
    vk_.vkCmdSetDepthBias(cmd, d.depth_bias * 16777216.0f, 0.0f, d.depth_slope_bias);
    vk_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 2, sets, 0, nullptr);
    if (!vk_buffers_.empty())
      vk_.vkCmdBindVertexBuffers(cmd, 0, uint32_t(vk_buffers_.size()), vk_buffers_.data(), vk_offsets_.data());
    if (d.indexed) {
      vk_.vkCmdBindIndexBuffer(cmd, ib, oib, d.index.index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
      vk_.vkCmdDrawIndexed(cmd, d.icount, 1, 0, 0, 0);
    } else {
      vk_.vkCmdDraw(cmd, d.vcount, 1, 0, 0);
    }
    stats_.drawn++;
  }

  void Present(VkImage dst, VkImageView dst_view, uint32_t width, uint32_t height,
               bool dst_written_before) override {
    if (!in_frame_ || !present_pass_) return;
    FlushClear();
    EndPass();
    Target* t = FinalTarget();
    if (!t || !t->color.image) return;
    Slot& s = slots_[slot_index_];
    VkCommandBuffer cmd = s.main;
    if (t->color_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      Barrier(cmd, t->color.image, VK_IMAGE_ASPECT_COLOR_BIT, t->color_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      t->color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // Framebuffer de l'image de sortie (le presenter en alterne quelques-unes).
    VkFramebuffer fb = VK_NULL_HANDLE;
    for (auto& [view, f] : present_framebuffers_)
      if (view == dst_view) fb = f.framebuffer;
    if (!fb) {
      VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fci.renderPass = present_pass_;
      fci.attachmentCount = 1;
      fci.pAttachments = &dst_view;
      fci.width = width;
      fci.height = height;
      fci.layers = 1;
      if (!Check(vk_.vkCreateFramebuffer(dev_, &fci, nullptr, &fb), "framebuffer de sortie")) return;
      if (present_framebuffers_.size() >= 8) {
        // Images du presenter recréées : les anciens framebuffers ne servent plus.
        VkFramebuffer old = present_framebuffers_.front().second.framebuffer;
        s.deferred.push_back([this, old] { vk_.vkDestroyFramebuffer(dev_, old, nullptr); });
        present_framebuffers_.erase(present_framebuffers_.begin());
      }
      present_framebuffers_.push_back({dst_view, {fb, width, height}});
    }
    Barrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            dst_written_before ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            dst_written_before ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkDescriptorSet set;
    if (!AllocateSets(&present_set_layout_, 1, &set)) return;
    VkDescriptorImageInfo ii{sampler_clamp_, t->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vk_.vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = present_pass_;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {width, height}};
    vk_.vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, float(width), float(height), 0, 1};
    VkRect2D sc{{0, 0}, {width, height}};
    vk_.vkCmdSetViewport(cmd, 0, 1, &vp);
    vk_.vkCmdSetScissor(cmd, 0, 1, &sc);
    vk_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
    vk_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1, &set, 0, nullptr);
    vk_.vkCmdDraw(cmd, 3, 1, 0, 0);
    vk_.vkCmdEndRenderPass(cmd);
    Barrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  }

  void Submit() override {
    if (!in_frame_) return;
    FlushClear();
    EndPass();
    Slot& s = slots_[slot_index_];
    vk_.vkEndCommandBuffer(s.pre);
    vk_.vkEndCommandBuffer(s.main);
    VkCommandBuffer cbs[2] = {s.pre, s.main};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    const VkResult r = ctx_.submit ? ctx_.submit(si, s.fence) : vk_.vkQueueSubmit(queue_, 1, &si, s.fence);
    Check(r, "vkQueueSubmit");
    s.submitted = r == VK_SUCCESS;
    in_frame_ = false;
  }

  bool ReadTarget(const std::string& name, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) override {
    Target* t = name.empty() ? FinalTarget() : (targets_.count(name) ? &targets_[name] : nullptr);
    if (!t || !t->color.image) return false;
    vk_.vkDeviceWaitIdle(dev_);
    w = t->width;
    h = t->height;
    Buffer b = CreateBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!b.buffer) return false;
    Slot& s = slots_[slot_index_];
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.vkBeginCommandBuffer(s.pre, &cbbi);
    Barrier(s.pre, t->color.image, VK_IMAGE_ASPECT_COLOR_BIT, t->color_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    t->color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vk_.vkCmdCopyImageToBuffer(s.pre, t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &region);
    vk_.vkEndCommandBuffer(s.pre);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.pre;
    if (!Check(vk_.vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit")) return false;
    vk_.vkQueueWaitIdle(queue_);
    rgba.assign(b.mapped, b.mapped + size_t(w) * h * 4);
    return true;
  }

  std::vector<std::string> RenderedTargets() const override {
    std::vector<std::string> out;
    for (const auto& [name, t] : targets_)
      if (t.rendered) out.push_back(name);
    return out;
  }

  bool SavePipelineCache() override {
    if (ctx_.pipeline_cache_file.empty() || !pipeline_cache_ || pipelines_since_save_ == 0) return false;
    size_t size = 0;
    if (vk_.vkGetPipelineCacheData(dev_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return false;
    std::vector<uint8_t> data(size);
    if (vk_.vkGetPipelineCacheData(dev_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return false;
    const std::string path = ctx_.pipeline_cache_file.string(), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, size, f) == size;
    if (std::fclose(f) != 0 || !ok) {
      std::remove(tmp.c_str());
      return false;
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    pipelines_since_save_ = 0;
    return true;
  }

  const Stats& stats() const override { return stats_; }
  void ResetStats() override { stats_ = Stats(); }

 private:
  void Log(const std::string& s) {
    if (ctx_.log)
      ctx_.log(s);
    else
      std::fprintf(stderr, "%s\n", s.c_str());
  }
  bool Check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    Log(std::string("rendu natif : ") + what + " -> " + std::to_string(int(r)));
    return false;
  }
  void Skip(const std::string& why) { stats_.skipped[why]++; }

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mem_.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
  }

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (!Check(vk_.vkCreateBuffer(dev_, &ci, nullptr, &b.buffer), "vkCreateBuffer")) return Buffer();
    VkMemoryRequirements req;
    vk_.vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || !Check(vk_.vkAllocateMemory(dev_, &ai, nullptr, &b.memory), "mémoire")) {
      vk_.vkDestroyBuffer(dev_, b.buffer, nullptr);
      return Buffer();
    }
    vk_.vkBindBufferMemory(dev_, b.buffer, b.memory, 0);
    vk_.vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.mapped));
    return b;
  }

  void DestroyBuffer(Buffer& b) {
    if (b.buffer) vk_.vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (b.memory) vk_.vkFreeMemory(dev_, b.memory, nullptr);
    b = Buffer();
  }

  // Sous-allocation linéaire dans les tampons d'envoi de l'image en cours. Renvoie l'adresse écrite.
  uint8_t* Push(const void* data, size_t size, VkDeviceSize align, VkBuffer& buffer, VkDeviceSize& offset) {
    Slot& s = slots_[slot_index_];
    for (;;) {
      if (s.chunk >= s.chunks.size()) {
        Buffer b = CreateBuffer(std::max<VkDeviceSize>(kChunkSize, size + align),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!b.buffer) return nullptr;
        s.chunks.push_back(b);
      }
      Buffer& b = s.chunks[s.chunk];
      const VkDeviceSize off = (b.used + align - 1) / align * align;
      if (off + size <= b.size) {
        if (data) std::memcpy(b.mapped + off, data, size);
        b.used = off + size;
        buffer = b.buffer;
        offset = off;
        stats_.upload_bytes += size;
        return b.mapped + off;
      }
      s.chunk++;
    }
  }

  Image CreateImage(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    bool cube = false) {
    Image im;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = cube ? 6 : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vk_.vkCreateImage(dev_, &ci, nullptr, &im.image), "vkCreateImage")) return Image();
    VkMemoryRequirements req;
    vk_.vkGetImageMemoryRequirements(dev_, im.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || !Check(vk_.vkAllocateMemory(dev_, &ai, nullptr, &im.memory), "mémoire")) {
      vk_.vkDestroyImage(dev_, im.image, nullptr);
      return Image();
    }
    vk_.vkBindImageMemory(dev_, im.image, im.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, cube ? 6u : 1u};
    vk_.vkCreateImageView(dev_, &vi, nullptr, &im.view);
    return im;
  }

  void DestroyImage(Image& im) {
    if (im.view) vk_.vkDestroyImageView(dev_, im.view, nullptr);
    if (im.image) vk_.vkDestroyImage(dev_, im.image, nullptr);
    if (im.memory) vk_.vkFreeMemory(dev_, im.memory, nullptr);
    im = Image();
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
               VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
               VkPipelineStageFlags dst_stage, uint32_t layers = 1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    vk_.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  }

  // Envoi d'une texture dans le tampon de commandes « pre » (avant toutes les passes de l'image).
  void UploadImage(Image& im, const void* data, size_t size, uint32_t w, uint32_t h, uint32_t layers) {
    Slot& s = slots_[slot_index_];
    VkBuffer buffer;
    VkDeviceSize off;
    if (!Push(data, size, 16, buffer, off)) return;
    Barrier(s.pre, im.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, layers);
    VkBufferImageCopy region{};
    region.bufferOffset = off;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.imageExtent = {w, h, 1};
    vk_.vkCmdCopyBufferToImage(s.pre, buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(s.pre, im.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, layers);
  }

  const TextureEntry* GetTexture(const TextureRef& t) {
    auto it = textures_.find(t.key);
    if (it != textures_.end()) {
      it->second.last_used = frame_;
      return it->second.image.image ? &it->second : nullptr;
    }
    TextureEntry& e = textures_[t.key];
    e.last_used = frame_;
    e.cube = t.cube;
    auto fmt = TextureFormat(t.format);
    if (!fmt || !t.load || !t.width || !t.height) {
      stats_.skipped["texture de format " + std::to_string(t.format)]++;
      return nullptr;
    }
    std::vector<uint8_t>& data = texture_data_;
    data.clear();
    if (!(*t.load)(data) || data.empty()) return nullptr;
    e.image = CreateImage(*fmt, t.width, t.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, t.cube);
    if (!e.image.image) return nullptr;
    UploadImage(e.image, data.data(), data.size(), t.width, t.height, t.cube ? 6 : 1);
    stats_.textures_loaded++;
    return &e;
  }

  void EvictTextures() {
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (frame_ - it->second.last_used > kTextureIdleFrames) {
        DestroyImage(it->second.image);  // inutilisée depuis bien plus que kSlots images
        it = textures_.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool AllocateSets(const VkDescriptorSetLayout* layouts, uint32_t n, VkDescriptorSet* out) {
    Slot& s = slots_[slot_index_];
    for (;;) {
      if (s.pool >= s.pools.size()) {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSetsPerPool},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * kSetsPerPool}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 2 * kSetsPerPool;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = ps;
        VkDescriptorPool p;
        if (!Check(vk_.vkCreateDescriptorPool(dev_, &dpci, nullptr, &p), "pool de descripteurs")) return false;
        s.pools.push_back(p);
      }
      VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dsai.descriptorPool = s.pools[s.pool];
      dsai.descriptorSetCount = n;
      dsai.pSetLayouts = layouts;
      if (vk_.vkAllocateDescriptorSets(dev_, &dsai, out) == VK_SUCCESS) return true;
      s.pool++;
    }
  }

  void IndexLibrary() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(lib_, ec)) {
      if (e.path().extension() != ".spv" && e.path().extension() != ".tlsh") continue;
      // <empreinte>_<vs|ps>_<profil>_main.tlsh (ou .spv)
      const std::string n = e.path().filename().string();
      const size_t a = n.find('_');
      if (a == std::string::npos || n.size() < a + 3) continue;
      library_[n.substr(0, a + 3)] = e.path();
    }
    // Sur Switch, lister le dossier peut échouer : GetShader construit alors le chemin directement.
    if (library_.empty() && !FileExists(lib_ / "ffp_vs_3_0_main.tlsh") && !FileExists(lib_ / "ffp_vs_3_0_main.spv"))
      Log("rendu natif : bibliothèque de shaders vide ou absente : " + lib_.string());
    else
      Log("rendu natif : bibliothèque de shaders " + lib_.string() + " (" + std::to_string(library_.size()) +
          " shaders listés)");
  }

  // Échantillonneur selon l'adressage et la couleur de bordure d'Ogre (ombres projetées : bordure blanche, sinon
  // les pixels du bord de la carte d'ombres s'étirent à l'infini).
  VkSampler SamplerFor(const TextureRef& t) {
    const uint32_t key = uint32_t(t.address[0] & 3) | uint32_t(t.address[1] & 3) << 2 |
                         uint32_t(t.address[2] & 3) << 4 | uint32_t(t.border & 3) << 6;
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    static const VkSamplerAddressMode kModes[4] = {VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                   VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    static const VkBorderColor kBorders[3] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                              VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                                              VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = kModes[t.address[0] & 3];
    sci.addressModeV = kModes[t.address[1] & 3];
    sci.addressModeW = kModes[t.address[2] & 3];
    sci.borderColor = kBorders[std::min<uint32_t>(t.border, 2)];
    sci.maxLod = 0.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    if (!Check(vk_.vkCreateSampler(dev_, &sci, nullptr, &sampler), "vkCreateSampler")) sampler = VK_NULL_HANDLE;
    samplers_[key] = sampler;
    return sampler;
  }

  // Shader d'un dessin, par empreinte (0 = pipeline fixe émulé) ; nul s'il manque à la bibliothèque.
  Shader* ShaderFor(bool ffp, uint64_t hash, int stage) {
    const uint64_t key = (ffp ? 0 : hash) * 2 + uint64_t(stage);
    // Shaders compilés en arrière-plan : un shader absent est recherché à nouveau quand le compilateur en a
    // ajouté (compteur Context::shader_generation), au lieu d'être marqué absent pour toujours.
    const uint32_t generation = ctx_.shader_generation ? ctx_.shader_generation->load(std::memory_order_acquire) : 0;
    if (generation != shader_generation_seen_) {
      shader_generation_seen_ = generation;
      for (auto& by_hash : shader_by_hash_)
        for (auto it = by_hash.begin(); it != by_hash.end();)
          it = it->second ? std::next(it) : by_hash.erase(it);
      for (auto it = shaders_.begin(); it != shaders_.end();) it = it->second.module ? std::next(it) : shaders_.erase(it);
    }
    auto it = shader_by_hash_[stage].find(key);
    if (it != shader_by_hash_[stage].end()) return it->second;
    Shader* s = GetShader(ffp ? "ffp" : HashName(hash), stage ? "ps" : "vs");
    shader_by_hash_[stage][key] = s;
    return s;
  }

  Shader* GetShader(const std::string& hash, const char* stage) {
    const std::string key = hash + "_" + stage;
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return it->second.module ? &it->second : nullptr;
    Shader& s = shaders_[key];
    fs::path path;
    if (auto lib = library_.find(key); lib != library_.end()) {
      path = lib->second;
    } else {
      // Noms produits par build_lib.sh : profils vs_3_0 et ps_3_x, entrée main.
      const std::string base = key + (stage[0] == 'v' ? "_3_0_main" : "_3_x_main");
      path = lib_ / (base + ".tlsh");
      if (!FileExists(path)) path = lib_ / (base + ".spv");
      if (!FileExists(path)) {
        stats_.missing_shaders[key]++;
        return nullptr;
      }
    }
    if (auto loaded = LoadShader(path)) {
      s = std::move(*loaded);
      VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      smci.codeSize = s.spirv.size() * 4;
      smci.pCode = s.spirv.data();
      if (!Check(vk_.vkCreateShaderModule(dev_, &smci, nullptr, &s.module), "vkCreateShaderModule"))
        s.module = VK_NULL_HANDLE;
    }
    return s.module ? &s : nullptr;
  }

  VkPipeline GetPipeline(const DrawDesc& d, const Shader& vs, const Shader& ps, VkPrimitiveTopology topo) {
    // Clé : shaders, topologie, sommets, états.
    uint64_t key = Fnv(&vs.module, sizeof(vs.module));
    key = Fnv(&ps.module, sizeof(ps.module), key);
    key = Fnv(&topo, sizeof(topo), key);
    for (const auto& b : bindings_) key = Fnv(&b, sizeof(b), key);
    for (const auto& a : attributes_) key = Fnv(&a, sizeof(a), key);
    const uint32_t states[] = {d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.blend[4], d.blend[5], d.cull,
                               uint32_t(d.depth_check), uint32_t(d.depth_write), d.depth_func,
                               uint32_t(d.mask[0]) | uint32_t(d.mask[1]) << 1 | uint32_t(d.mask[2]) << 2 |
                                   uint32_t(d.mask[3]) << 3};
    key = Fnv(states, sizeof(states), key);
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 vs.module, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 ps.module, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(bindings_.size());
    vi.pVertexBindingDescriptions = bindings_.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attributes_.size());
    vi.pVertexAttributeDescriptions = attributes_.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    const VkDynamicState dyn_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                          VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 3;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.depthBiasEnable = VK_TRUE;  // valeurs dynamiques (vkCmdSetDepthBias), nulles le plus souvent
    // L'interface (CEGUI) a des sommets en z = -1 : sans découpage en profondeur côté Xbox (D3DRS_CLIPPING),
    // elle s'affiche ; en Vulkan, la profondeur bloquée désactive aussi ce découpage.
    rs.depthClampEnable = VK_TRUE;
    // Ogre : CULL_NONE = 1, CULL_CLOCKWISE = 2, CULL_ANTICLOCKWISE = 3 (sens à l'écran, comme Direct3D).
    rs.cullMode = d.cull == 1 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = d.cull == 3 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_check;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = CompareOp(d.depth_func);
    VkPipelineColorBlendAttachmentState cb{};
    cb.srcColorBlendFactor = BlendFactor(d.blend[0]);
    cb.dstColorBlendFactor = BlendFactor(d.blend[1]);
    cb.colorBlendOp = BlendOp(d.blend[2]);
    cb.srcAlphaBlendFactor = BlendFactor(d.blend[3]);
    cb.dstAlphaBlendFactor = BlendFactor(d.blend[4]);
    cb.alphaBlendOp = BlendOp(d.blend[5]);
    cb.blendEnable = !(d.blend[0] == 0 && d.blend[1] == 1 && d.blend[2] == 0 && d.blend[3] == 0 &&
                       d.blend[4] == 1 && d.blend[5] == 0);
    cb.colorWriteMask = (d.mask[0] ? VK_COLOR_COMPONENT_R_BIT : 0) | (d.mask[1] ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        (d.mask[2] ? VK_COLOR_COMPONENT_B_BIT : 0) | (d.mask[3] ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipeline_layout_;
    gpci.renderPass = pass_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto t0 = std::chrono::steady_clock::now();
    if (!Check(vk_.vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &gpci, nullptr, &pipeline),
               "vkCreateGraphicsPipelines"))
      pipeline = VK_NULL_HANDLE;
    stats_.ms_pipelines +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    pipelines_[key] = pipeline;
    stats_.pipelines_created++;
    pipelines_since_save_++;
    return pipeline;
  }

  Target* GetTarget(const std::string& name, uint32_t width, uint32_t height) {
    if (!width || !height) {
      width = 1280;
      height = 720;
    }
    Target& t = targets_[name];
    if (t.framebuffer && t.width == width && t.height == height) return &t;
    if (t.framebuffer) {
      // Cible redimensionnée : l'ancienne est détruite quand le GPU ne l'utilise plus.
      Target old = t;
      slots_[slot_index_].deferred.push_back([this, old]() mutable {
        vk_.vkDestroyFramebuffer(dev_, old.framebuffer, nullptr);
        DestroyImage(old.color);
        DestroyImage(old.depth);
      });
      t = Target();
    }
    t.name = name;
    t.width = width;
    t.height = height;
    t.color = CreateImage(kColorFormat, width, height,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
    t.depth = CreateImage(kDepthFormat, width, height, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!t.color.image || !t.depth.image) return nullptr;
    VkImageView views[2] = {t.color.view, t.depth.view};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass_;
    fci.attachmentCount = 2;
    fci.pAttachments = views;
    fci.width = width;
    fci.height = height;
    fci.layers = 1;
    if (!Check(vk_.vkCreateFramebuffer(dev_, &fci, nullptr, &t.framebuffer), "framebuffer")) return nullptr;
    Log("rendu natif : cible " + name + " " + std::to_string(width) + "x" + std::to_string(height));
    return &t;
  }

  // Texture d'Ogre rendue par le jeu : nom de cible « rtt/<adresse>/<nom de texture> ».
  Target* RenderedTexture(const std::string& tex_name) {
    if (tex_name.empty()) return nullptr;
    std::string_view name = tex_name;
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    if (name.empty()) return nullptr;
    for (auto& [tname, t] : targets_) {
      if (!t.rendered || tname.size() <= name.size() + 1) continue;
      if (tname[tname.size() - name.size() - 1] == '/' &&
          std::string_view(tname).substr(tname.size() - name.size()) == name)
        return &t;
    }
    return nullptr;
  }

  Target* FinalTarget() {
    auto it = targets_.find("Game");
    if (it != targets_.end() && it->second.rendered) return &it->second;
    return current_;
  }

  void BeginPass() {
    if (!current_ || in_pass_) return;
    Target& t = *current_;
    VkCommandBuffer cmd = slots_[slot_index_].main;
    if (t.color_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
      Barrier(cmd, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, t.color_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      t.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (!t.depth_ready) {
      Barrier(cmd, t.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
      t.depth_ready = true;
    }
    // Rectangle du viewport d'Ogre, borné à la cible (0 = toute la cible).
    int32_t vx = viewport_[0], vy = viewport_[1], vw = viewport_[2], vh = viewport_[3];
    if (vw <= 0 || vh <= 0) {
      vx = vy = 0;
      vw = int32_t(t.width);
      vh = int32_t(t.height);
    }
    vx = std::clamp(vx, 0, int32_t(t.width) - 1);
    vy = std::clamp(vy, 0, int32_t(t.height) - 1);
    vw = std::min(vw, int32_t(t.width) - vx);
    vh = std::min(vh, int32_t(t.height) - vy);
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = pass_;
    rpbi.framebuffer = t.framebuffer;
    rpbi.renderArea = {{0, 0}, {t.width, t.height}};
    vk_.vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    // Effacement limité au viewport, comme Direct3D.
    VkClearAttachment ca[2];
    uint32_t nca = 0;
    if (pending_clear_buffers_ & 1) {
      ca[nca] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
      ca[nca].clearValue.color = {{pending_colour_[0], pending_colour_[1], pending_colour_[2], pending_colour_[3]}};
      nca++;
    }
    if (pending_clear_buffers_ & 2) {
      ca[nca] = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
      ca[nca].clearValue.depthStencil = {pending_depth_, 0};
      nca++;
    }
    pending_clear_buffers_ = 0;
    if (nca) {
      VkClearRect cr{{{vx, vy}, {uint32_t(vw), uint32_t(vh)}}, 0, 1};
      vk_.vkCmdClearAttachments(cmd, nca, ca, 1, &cr);
    }
    // Viewport inversé : même orientation que Direct3D (y vers le haut en NDC), y compris pour les textures
    // rendues, que le jeu échantillonne ensuite avec des coordonnées Direct3D.
    // Règle de Direct3D 9 : centre des pixels aux coordonnées entières (Vulkan : + 0,5). Le jeu (CEGUI, Ogre)
    // compense avec -0,5 texel ; sans ce décalage d'un demi-pixel, le texte tombe à cheval sur deux pixels.
    VkViewport viewport{float(vx) + 0.5f, float(vy + vh) + 0.5f, float(vw), -float(vh), 0.0f, 1.0f};
    VkRect2D scissor{{vx, vy}, {uint32_t(vw), uint32_t(vh)}};
    vk_.vkCmdSetViewport(cmd, 0, 1, &viewport);
    vk_.vkCmdSetScissor(cmd, 0, 1, &scissor);
    in_pass_ = true;
  }

  void EndPass() {
    if (!in_pass_) return;
    vk_.vkCmdEndRenderPass(slots_[slot_index_].main);
    in_pass_ = false;
    if (current_) current_->rendered = true;
  }

  // Effacement demandé sans dessin derrière (cible effacée puis laissée) : appliqué quand même.
  void FlushClear() {
    if (!pending_clear_buffers_ || !current_ || in_pass_) return;
    BeginPass();
    EndPass();
  }

  bool InitPresent() {
    VkAttachmentDescription att{};
    att.format = output_format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    if (!Check(vk_.vkCreateRenderPass(dev_, &rpci, nullptr, &present_pass_), "passe de sortie")) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   nullptr};
    VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l.bindingCount = 1;
    l.pBindings = &b;
    if (!Check(vk_.vkCreateDescriptorSetLayout(dev_, &l, nullptr, &present_set_layout_), "set de sortie"))
      return false;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &present_set_layout_;
    if (!Check(vk_.vkCreatePipelineLayout(dev_, &plci, nullptr, &present_layout_), "layout de sortie")) return false;
    VkShaderModule modules[2];
    const struct {
      const uint32_t* code;
      size_t size;
    } code[2] = {{kPresentVs, sizeof(kPresentVs)}, {kPresentPs, sizeof(kPresentPs)}};
    for (int k = 0; k < 2; k++) {
      VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      smci.codeSize = code[k].size;
      smci.pCode = code[k].code;
      if (!Check(vk_.vkCreateShaderModule(dev_, &smci, nullptr, &modules[k]), "shader de sortie")) return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 modules[0], "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 modules[1], "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyn;
    gpci.layout = present_layout_;
    gpci.renderPass = present_pass_;
    return Check(vk_.vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &gpci, nullptr, &present_pipeline_),
                 "pipeline de sortie");
  }

  Context ctx_;
  fs::path lib_;
  VkFormat output_format_;
  Vk vk_;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem_{};
  VkRenderPass pass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_[2] = {};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE, sampler_clamp_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  std::array<Slot, kSlots> slots_;
  int slot_index_ = 0;
  uint64_t frame_ = 0;
  bool in_frame_ = false, in_pass_ = false;
  Buffer defaults_;
  Image white_, black_cube_;
  bool pending_fallbacks_ = false;

  std::unordered_map<std::string, fs::path> library_;
  std::unordered_map<std::string, Shader> shaders_;
  std::unordered_map<uint64_t, Shader*> shader_by_hash_[2];
  uint32_t shader_generation_seen_ = 0;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  std::unordered_map<uint32_t, VkSampler> samplers_;
  uint32_t pipelines_since_save_ = 0;
  std::unordered_map<uint64_t, TextureEntry> textures_;
  std::map<std::string, Target> targets_;
  Target* current_ = nullptr;
  int32_t viewport_[4] = {};
  uint32_t pending_clear_buffers_ = 0;
  float pending_colour_[4] = {};
  float pending_depth_ = 1.0f;

  // Sortie (Present).
  VkRenderPass present_pass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  struct PresentFramebuffer {
    VkFramebuffer framebuffer;
    uint32_t width, height;
  };
  std::vector<std::pair<VkImageView, PresentFramebuffer>> present_framebuffers_;

  // Tampons réutilisés d'un dessin à l'autre.
  std::vector<VkVertexInputBindingDescription> bindings_;
  std::vector<VkVertexInputAttributeDescription> attributes_;
  std::vector<VkBuffer> vk_buffers_;
  std::vector<VkDeviceSize> vk_offsets_;
  std::vector<uint32_t> provided_;
  std::vector<uint8_t> globals_[2];
  std::vector<uint8_t> texture_data_;
  std::vector<uint8_t> scratch_;
  std::vector<uint8_t> plan_;

  Stats stats_;
};

}  // namespace

std::unique_ptr<Renderer> Renderer::Create(const Context& ctx, const fs::path& shader_lib, VkFormat output_format) {
  auto r = std::make_unique<RendererImpl>(ctx, shader_lib, output_format);
  if (!r->Init()) return nullptr;
  return r;
}

}  // namespace torchlight::natif
