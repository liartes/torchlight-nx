// torchlight - compilation des shaders HLSL du jeu en SPIR-V, dans le jeu (console comprise).
//
// Remplace la chaîne PC prep_hlsl.py + glslangValidator + spirv-opt + spirv-val (switch/shaders/build_lib.sh),
// avec les mêmes options, et écrit les mêmes fichiers dans le cache : <nom>.spv, <nom>.refl (réflexion glslang,
// même format) et <nom>.io. Aucun shader compilé n'est distribué : chaque machine construit le sien à partir des
// sources que le jeu génère (RT Shader System d'Ogre) et de son assets/rtshaderlib.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace torchlight::natif {

class HlslCompiler {
 public:
  // rtshaderlib : dossier de la bibliothèque HLSL d'Ogre (fichiers du jeu). cache : dossier de sortie.
  static std::unique_ptr<HlslCompiler> Create(const std::filesystem::path& rtshaderlib,
                                              const std::filesystem::path& cache);
  virtual ~HlslCompiler() = default;

  // Compile si <name>.spv n'est pas déjà dans le cache. name : <empreinte>_<profil>_<entrée>, comme les sources
  // relevées par le traceur. d3dx_flags : options de D3DXCompileShader (-1 si inconnues). Utilisable depuis
  // plusieurs threads (sérialisé). Renvoie faux en cas d'échec (error décrit l'erreur).
  virtual bool Compile(const std::string& name, const std::string& source, bool vertex, int64_t d3dx_flags,
                       std::string& error) = 0;

  // Source déjà prête pour glslang (shaders écrits pour le rendu natif, switch/shaders/ffp), avec son .io.
  virtual bool CompileRaw(const std::string& name, const std::string& source, const std::string& io, bool vertex,
                          std::string& error) = 0;

  virtual bool InCache(const std::string& name) const = 0;

  // Répartition du temps de la dernière compilation (préparation, analyse, liens, SPIR-V, écriture), en ms.
  virtual std::string LastTiming() const = 0;
};

}  // namespace torchlight::natif
