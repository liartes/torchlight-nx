#!/bin/bash
# Construit la bibliothèque de shaders SPIR-V à partir des sources HLSL capturées par le traceur.
# Usage : build_lib.sh <dossier hlsl capturés> <sortie> [glslangValidator]
# Variables : SPIRV_OPT et SPIRV_VAL (SPIRV-Tools) : légalisation HLSL et validation, indispensables
# (sans légalisation, les fonctions qui reçoivent un échantillonneur donnent un SPIR-V invalide).
# Sortie : <hash>_<profil>_main.spv et .refl (réflexion glslang : noms et décalages des constantes du
# bloc $Global, bindings des échantillonneurs).
# Bindings : shader de sommets set 0, shader de pixels set 1 ; $Global = binding 0 ; échantillonneur
# register(sN) = binding 16 + N (N = unité de texture d'Ogre).
set -e
SRC="$1"; OUT="$2"
GV="${3:-${GLSLANG:-glslangValidator}}"
OPT="${SPIRV_OPT:-spirv-opt}"
VAL="${SPIRV_VAL:-spirv-val}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PREP="$OUT/prep"
rm -rf "$PREP"; mkdir -p "$OUT"
python3 "$HERE/prep_hlsl.py" "$SRC" "$ROOT/assets/rtshaderlib" "$PREP"
ok=0; ko=0
for f in "$PREP"/src/*.hlsl; do
  b=$(basename "$f" .hlsl)
  case "$b" in
    *_vs_*) stage=vert; set=0 ;;
    *) stage=frag; set=1 ;;
  esac
  # --hlsl-dx9-compatible : sampler2D de Direct3D 9 = texture + échantillonneur combinés (sinon glslang en
  # fait un simple sampler et tex2D donne un SPIR-V invalide).
  if "$GV" -D -V -S $stage -e main -I"$PREP/lib" --hlsl-dx9-compatible --hlsl-iomap --resource-set-binding $set \
       --shift-sampler-binding 16 --shift-texture-binding 16 --auto-map-locations -q \
       -o "$OUT/$b.brut.spv" "$f" > "$OUT/$b.refl" 2>&1 &&
     "$OPT" --legalize-hlsl "$OUT/$b.brut.spv" -o "$OUT/$b.spv" &&
     "$VAL" --target-env vulkan1.1 "$OUT/$b.spv" >> "$OUT/$b.refl" 2>&1; then
    rm -f "$OUT/$b.brut.spv"
    ok=$((ok+1))
    cp "$PREP/src/$b.io" "$OUT/$b.io"
  else
    ko=$((ko+1)); echo "ÉCHEC : $b"; grep ERROR "$OUT/$b.refl" | head -3
  fi
done
# Émulation du pipeline fixe (dessins sans programme) : shaders écrits à la main, compilés de la même façon.
for f in "$HERE"/ffp/*.hlsl; do
  b=$(basename "$f" .hlsl)
  case "$b" in *_vs_*) stage=vert; set=0 ;; *) stage=frag; set=1 ;; esac
  if "$GV" -D -V -S $stage -e main --hlsl-dx9-compatible --hlsl-iomap --resource-set-binding $set \
       --shift-sampler-binding 16 --shift-texture-binding 16 --auto-map-locations -q \
       -o "$OUT/$b.brut.spv" "$f" > "$OUT/$b.refl" 2>&1 &&
     "$OPT" --legalize-hlsl "$OUT/$b.brut.spv" -o "$OUT/$b.spv" &&
     "$VAL" --target-env vulkan1.1 "$OUT/$b.spv" >> "$OUT/$b.refl" 2>&1; then
    rm -f "$OUT/$b.brut.spv"; cp "$HERE/ffp/$b.io" "$OUT/$b.io"; ok=$((ok+1))
  else
    ko=$((ko+1)); echo "ÉCHEC : $b"; grep -i error "$OUT/$b.refl" | head -3
  fi
done
echo "$ok shaders compilés, $ko échecs"
