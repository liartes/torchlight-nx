// torchlight - résultats du compilateur HLSL de la Xbox (D3DXCompileShader) gardés d'un lancement à l'autre.
// Chaque compilation coûte 27 ms sur PC (bien plus sur la console) au thread du jeu, à chaque lancement. On garde,
// par source : le microcode final (rendu à Ogre dans un tampon D3DX) et le code intermédiaire dont le compilateur
// tire la table de constantes (sub_82778058, équivalent de D3DXGetShaderConstantTable) : au lancement suivant, la
// table est reconstruite par le jeu lui-même à partir de ce code, identique à celle d'une vraie compilation.
// Un seul fichier, complété en fin (<données>/cache/natif/d3dx.bin), lu en entier au démarrage : sur la carte SD de
// la console, chaque ouverture de fichier coûte cher. Compilé sur la machine qui fait tourner le jeu, jamais
// distribué.

#pragma once

#include <cstdint>
#include <string>

namespace torchlight::natif::d3dx {

struct Result {
  std::string code;          // microcode final (*ppShader)
  std::string intermediate;  // code dont est tirée la table de constantes ; vide si non demandée
};

// Lit le fichier (s'il existe) ; les écritures suivantes le complètent. Sans appel, le cache est inactif.
void Open(const std::string& path);
bool Active();

// Faux si la source n'est pas dans le cache. Utilisable depuis plusieurs threads.
bool Find(uint64_t key, Result& out);

// Mémorise un résultat et retourne le bloc à ajouter au fichier (vide si déjà connu), à écrire avec Append
// (hors du thread du jeu de préférence).
std::string Add(uint64_t key, Result result);
void Append(const std::string& block);

// Bilan pour le journal : entrées lues au démarrage, trouvées, ajoutées.
std::string Summary();

}  // namespace torchlight::natif::d3dx
