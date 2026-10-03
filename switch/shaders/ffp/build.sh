#!/bin/bash
# Embarque les shaders d'émulation du pipeline fixe (sources HLSL écrites pour le rendu natif) dans
# src/natif/ffp_sources.h (versionné) : le jeu les compile lui-même au démarrage, comme les autres.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../../src/natif/ffp_sources.h"
{
  echo "// Généré par switch/shaders/ffp/build.sh depuis switch/shaders/ffp : ne pas modifier."
  echo "#pragma once"
  for b in ffp_vs_3_0_main ffp_ps_3_x_main; do
    v=$(echo "$b" | sed 's/^ffp_\(..\)_.*/\1/')
    echo "inline constexpr const char* kFfpSource_$v = R\"tl_ffp($(cat "$HERE/$b.hlsl"))tl_ffp\";"
    echo "inline constexpr const char* kFfpIo_$v = R\"tl_ffp($(cat "$HERE/$b.io")"
    echo ")tl_ffp\";"
  done
} > "$OUT"
echo "-> $OUT"
