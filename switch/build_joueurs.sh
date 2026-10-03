#!/bin/bash
# Build distribuée aux joueurs : mêmes options que la build de test (LTO, PGO, pile sans volatile) sans les outils de
# développement (TORCHLIGHT_OUTILS=OFF : manette de script, capture d'écran), dans switch/out-joueurs (compilation
# complète la première fois, ~1 h). Le profil PGO (pgo/) vaut pour le code généré actuel.
# Résultat : dist/torchlight-nx-<version>.zip, à extraire à la racine de la carte SD :
#   switch/torchlight/torchlight.nro    (le joueur y ajoute son paquet XBLA)
# avec README.md, LICENSE et THIRD_PARTY_NOTICES.md dans le même dossier (pas à la racine de la carte). Garde l'ELF non strippé dans switch/elf/.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$HERE/out-joueurs"
OUT="$OUT" CMAKE_OPTIONS="-DTORCHLIGHT_LTO=ON -DTORCHLIGHT_PILE_NON_VOLATILE=ON -DTORCHLIGHT_PGO=utiliser \
-DTORCHLIGHT_OUTILS=OFF" "$HERE/build_switch.sh"

VERSION=$(sed -n 's/^set(TORCHLIGHT_VERSION "\(.*\)")$/\1/p' "$HERE/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "TORCHLIGHT_VERSION introuvable dans switch/CMakeLists.txt" >&2; exit 1; }
mkdir -p "$HERE/elf"
cp "$OUT/torchlight" "$HERE/elf/torchlight-joueurs-$VERSION-$(date +%Y%m%d-%H%M).elf"

PAQUET="$(mktemp -d)"
trap 'rm -rf "$PAQUET"' EXIT
mkdir -p "$PAQUET/switch/torchlight"
cp "$OUT/torchlight.nro" "$PAQUET/switch/torchlight/"
cp "$ROOT/README.md" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$PAQUET/switch/torchlight/"
mkdir -p "$ROOT/dist"
ZIP="$ROOT/dist/torchlight-nx-$VERSION.zip"
rm -f "$ZIP"
(cd "$PAQUET" && zip -qr "$ZIP" .)
echo "$ZIP :"
unzip -l "$ZIP"
