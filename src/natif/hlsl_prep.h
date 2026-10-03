// torchlight - préparation des shaders HLSL (Direct3D 9) du jeu pour glslang (HLSL -> SPIR-V).
//
// Portage C++ de switch/shaders/prep_hlsl.py (mêmes corrections, même résultat octet pour octet), pour compiler
// les shaders sur la machine qui fait tourner le jeu, console comprise : aucun shader compilé n'est distribué.
// Corrections : fins de ligne, surcharges FFP_SampleTexture par type d'échantillonneur, emplacements Vulkan tirés
// des sémantiques, sémantiques système, test alpha d'Ogre, constantes globales écrites par le shader, rangement
// des matrices (option D3DX). Détails dans prep_hlsl.py.

#pragma once

#include <cstdint>
#include <string>

namespace torchlight::natif {

// Fichier de assets/rtshaderlib corrigé (inclus par les shaders du RT Shader System).
std::string PrepareHlslLibraryFile(const std::string& text);

// Shader du jeu corrigé. vertex : shader de sommets. d3dx_flags : options de D3DXCompileShader (8 = matrices par
// lignes, 16 = par colonnes), ou -1 si inconnues. io reçoit le contenu du fichier .io (emplacements, types des
// échantillonneurs).
std::string PrepareHlslSource(const std::string& text, bool vertex, int64_t d3dx_flags, std::string& io);

// Source allégée pour le compilateur HLSL de la Xbox (D3DXCompileShader), dont le microcode ne sert plus en rendu
// natif seul : mêmes déclarations et même signature de main(), corps réduit à une référence par constante globale
// (pour qu'elle reste dans la table de constantes, que lit Ogre) et à l'écriture des sorties. La table ne dépend
// que des déclarations (noms, types, tailles des tableaux) : Ogre obtient les mêmes constantes nommées.
// Chaîne vide si la source n'a pas la forme attendue (garder alors l'originale).
std::string MakeStubSource(const std::string& text);

}  // namespace torchlight::natif
