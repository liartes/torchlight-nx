#!/bin/bash
# Compile les shaders de sortie du rendu natif en tableaux C : src/natif/present_spv.h (versionné).
# Usage : build.sh [glslangValidator]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
GV="${1:-${GLSLANG:-glslangValidator}}"
OUT="$HERE/../../../src/natif/present_spv.h"
TMP="$(mktemp -d)"
"$GV" -V --vn kPresentVs -o "$TMP/vs.h" "$HERE/present.vert" >/dev/null
"$GV" -V --vn kPresentPs -o "$TMP/ps.h" "$HERE/present.frag" >/dev/null
{
  echo "// Généré par switch/shaders/sortie/build.sh depuis present.vert / present.frag : ne pas modifier."
  echo "#pragma once"
  echo "#include <cstdint>"
  sed -e 's/^const uint32_t/inline constexpr uint32_t/' -e '/^\/\//d' -e '/#pragma once/d' "$TMP/vs.h" "$TMP/ps.h"
} > "$OUT"
rm -rf "$TMP"
echo "-> $OUT"
