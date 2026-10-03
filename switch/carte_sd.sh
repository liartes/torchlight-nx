#!/bin/bash
# Assemble dans switch/out/sd/ ce qu'il faut copier à la racine de la carte SD (en plus du contenu de assets/,
# déjà présent dans sdmc:/switch/torchlight/) : le .nro et les réglages.
#   switch/out/sd/switch/torchlight/torchlight.nro
#   switch/out/sd/switch/torchlight/torchlight.toml
# Aucun shader : la console compile elle-même ceux du jeu (cache dans torchlight/cache/natif sur la carte).
# Garde aussi l'ELF non strippé du .nro dans switch/elf/ (symboliser rex_crash.log).
# BUILD=<dossier de compilation> pour un autre dossier que switch/out (par exemple switch/out-lto).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/out}"
DEST="$HERE/out/sd/switch/torchlight"
rm -rf "$HERE/out/sd"
mkdir -p "$DEST"
cp "$BUILD/torchlight.nro" "$DEST/"
cp "$HERE/torchlight.toml" "$DEST/"
mkdir -p "$HERE/elf"
ELF="$BUILD/torchlight"
[ -f "$ELF" ] && cp "$ELF" "$HERE/elf/torchlight-$(date +%Y%m%d-%H%M).elf"
echo ".nro de $(du -h "$DEST/torchlight.nro" | cut -f1) -> $HERE/out/sd"
