// torchlight - compilation HLSL -> SPIR-V dans le jeu (voir hlsl_compiler.h).
//
// Options reprises de switch/shaders/build_lib.sh (glslangValidator -D -V -e main --hlsl-dx9-compatible
// --hlsl-iomap --resource-set-binding <0|1> --shift-sampler-binding 16 --shift-texture-binding 16
// --auto-map-locations), légalisation par les passes SPIRV-Tools de glslang (au lieu de spirv-opt --legalize-hlsl)
// et validation.

#include "hlsl_compiler.h"

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>

#include "ResourceLimits.h"  // glslang/StandAlone : limites par défaut (DefaultTBuiltInResource)
#include "hlsl_prep.h"
#include "shader_pack.h"

namespace torchlight::natif {
namespace fs = std::filesystem;
namespace {

bool ReadFile(const fs::path& p, std::string& out) {
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[16384];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

// Création du dossier et de ses parents avec mkdir : std::filesystem est peu fiable sur la console.
bool MakeDirs(const fs::path& dir) {
  const std::string path = dir.string();
  for (size_t i = 1; i <= path.size(); i++) {
    if (i < path.size() && path[i] != '/') continue;
    const std::string part = path.substr(0, i);
    if (part.empty() || part.back() == ':') continue;  // « sdmc: »
    if (::mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) {
      struct stat st;
      if (::stat(part.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }
  }
  return true;
}

// #include "FFPLib_*.hlsl" : fichiers de rtshaderlib déjà corrigés, en mémoire.
class Includer : public glslang::TShader::Includer {
 public:
  explicit Includer(const std::map<std::string, std::string>& files) : files_(files) {}
  IncludeResult* includeLocal(const char* name, const char*, size_t) override { return Find(name); }
  IncludeResult* includeSystem(const char* name, const char*, size_t) override { return Find(name); }
  void releaseInclude(IncludeResult* r) override { delete r; }

 private:
  IncludeResult* Find(const char* name) {
    auto it = files_.find(name);
    if (it == files_.end()) return nullptr;
    return new IncludeResult(it->first, it->second.data(), it->second.size(), nullptr);
  }
  const std::map<std::string, std::string>& files_;
};

// Même format que glslangValidator -q (TObjectReflection::dump), relu par le rendu natif.
void DumpObject(std::string& out, const glslang::TObjectReflection& o) {
  char line[512];
  std::snprintf(line, sizeof(line), "%s: offset %d, type %x, size %d, index %d, binding %d, stages %d", o.name.c_str(),
                o.offset, o.glDefineType, o.size, o.index, o.getBinding(), int(o.stages));
  out += line;
  if (o.counterIndex != -1) out += ", counter " + std::to_string(o.counterIndex);
  if (o.numMembers != -1) out += ", numMembers " + std::to_string(o.numMembers);
  if (o.arrayStride != 0) out += ", arrayStride " + std::to_string(o.arrayStride);
  if (o.topLevelArrayStride != 0) out += ", topLevelArrayStride " + std::to_string(o.topLevelArrayStride);
  out += "\n";
}

class Compiler final : public HlslCompiler {
 public:
  Compiler(fs::path cache) : cache_(std::move(cache)) {}

  bool Init(const fs::path& rtshaderlib) {
    if (!MakeDirs(cache_)) return false;
    std::error_code ec;
    // Fichiers de la bibliothèque d'Ogre : noms connus (lister un dossier échoue sur la console).
    static const char* kFiles[] = {"FFPLib_Common.hlsl",   "FFPLib_Fog.hlsl",      "FFPLib_Lighting.hlsl",
                                   "FFPLib_Texturing.hlsl", "FFPLib_Transform.hlsl", "SGXLib_IntegratedPSSM.hlsl",
                                   "SGXLib_NormalMapLighting.hlsl", "SGXLib_PerPixelLighting.hlsl",
                                   "SampleLib_ReflectionMap.hlsl"};
    for (const char* name : kFiles) {
      std::string text;
      if (ReadFile(rtshaderlib / name, text)) lib_[name] = PrepareHlslLibraryFile(text);
    }
    // Et ceux qu'une liste du dossier trouverait en plus (PC).
    for (const auto& e : fs::directory_iterator(rtshaderlib, ec)) {
      const std::string name = e.path().filename().string();
      std::string text;
      if (e.path().extension() == ".hlsl" && !lib_.count(name) && ReadFile(e.path(), text))
        lib_[name] = PrepareHlslLibraryFile(text);
    }
    if (lib_.empty()) return false;
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
    return true;
  }

  bool InCache(const std::string& name) const override {
    return pack::Valid((cache_ / (name + ".tlsh")).string());
  }

  bool Compile(const std::string& name, const std::string& source, bool vertex, int64_t d3dx_flags,
               std::string& error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (InCache(name)) return true;
    std::string io;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string prepared = PrepareHlslSource(source, vertex, d3dx_flags, io);
    last_timing_ = "préparation " + Ms(t0);
    return CompilePrepared(name, prepared, io, vertex, error);
  }

  bool CompileRaw(const std::string& name, const std::string& source, const std::string& io, bool vertex,
                  std::string& error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (InCache(name)) return true;
    last_timing_.clear();
    return CompilePrepared(name, source, io, vertex, error);
  }

 private:
  bool CompilePrepared(const std::string& name, const std::string& prepared, const std::string& io, bool vertex,
                       std::string& error) {
    auto t = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
      last_timing_ += std::string(last_timing_.empty() ? "" : ", ") + what + " " + Ms(t);
      t = std::chrono::steady_clock::now();
    };

    const EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(stage);
    const char* text = prepared.c_str();
    const int length = int(prepared.size());
    const char* file_name = name.c_str();
    shader.setStringsWithLengthsAndNames(&text, &length, &file_name, 1);
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setHlslIoMapping(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResSampler, 16);
    shader.setShiftBinding(glslang::EResTexture, 16);
    shader.setResourceSetBinding({vertex ? "0" : "1"});
    const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl |
                                             EShMsgHlslDX9Compatible);
    Includer includer(lib_);
    if (!shader.parse(&glslang::DefaultTBuiltInResource, 100, false, messages, includer)) {
      error = shader.getInfoLog();
      return false;
    }
    lap("analyse");
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
      error = program.getInfoLog();
      return false;
    }
    program.buildReflection();
    lap("liens");

    std::vector<uint32_t> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.disableOptimizer = false;  // légalisation du SPIR-V issu de HLSL (passes SPIRV-Tools)
    options.validate = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &options);
    lap("SPIR-V");
    const std::string messages_text = logger.getAllMessages();
    if (spirv.empty() || messages_text.find("error") != std::string::npos) {
      error = messages_text.empty() ? "SPIR-V vide" : messages_text;
      return false;
    }

    std::string refl = name + "\n";
    refl += "Uniform reflection:\n";
    for (int i = 0; i < program.getNumUniformVariables(); i++) DumpObject(refl, program.getUniform(i));
    refl += "\nUniform block reflection:\n";
    for (int i = 0; i < program.getNumUniformBlocks(); i++) DumpObject(refl, program.getUniformBlock(i));
    refl += "\n";

    struct Lap {
      decltype(lap)& f;
      ~Lap() { f("écriture"); }
    } write_lap{lap};
    if (!pack::Write((cache_ / (name + ".tlsh")).string(), spirv, refl, io)) {
      error = "écriture du cache impossible : " + cache_.string();
      return false;
    }
    return true;
  }

  static std::string Ms(std::chrono::steady_clock::time_point t0) {
    char b[24];
    std::snprintf(b, sizeof(b), "%.1f",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return b;
  }

  std::string LastTiming() const override { return last_timing_; }

  fs::path cache_;
  std::string last_timing_;
  std::map<std::string, std::string> lib_;
  std::mutex mutex_;
};

}  // namespace

std::unique_ptr<HlslCompiler> HlslCompiler::Create(const fs::path& rtshaderlib, const fs::path& cache) {
  auto c = std::make_unique<Compiler>(cache);
  if (!c->Init(rtshaderlib)) return nullptr;
  return c;
}

}  // namespace torchlight::natif
