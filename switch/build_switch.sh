#!/bin/bash
# Compile torchlight.nro dans le conteneur devkitpro/devkita64.
# Prérequis : code généré dans switch/generated (./gen_manifest.sh puis
# rexglue codegen avec le rexglue de nfsmw-nx), Mesa NVK assemblé (voir CLAUDE.md).
# Usage : ./build_switch.sh [cible ninja...]   (JOBS=n pour limiter le parallélisme ; OUT=dossier et
# CMAKE_OPTIONS="-DTORCHLIGHT_LTO=ON" pour un essai dans un autre dossier)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WS="$(cd "$HERE/../.." && pwd)"   # ~/workspace, monté au même chemin dans le conteneur
NFSMW="$WS/nfsmw-nx"
NVK_SDK="$WS/mesa-switch/nvk-sdk/opt/devkitpro/portlibs/switch"
OUT="${OUT:-$HERE/out}"   # OUT=... : autre dossier de compilation (essais)
mkdir -p "$OUT" && OUT="$(cd "$OUT" && pwd)"   # absolu : la compilation se fait depuis switch/ (dans Docker)
JOBS="${JOBS:-$(nproc)}"

# Icône du .nro : nro-icon.png à la racine du dépôt (versionnée, ce n'est pas une image du jeu ; jamais
# assets/TitleIcon.png, que le .nro distribué ne doit pas contenir). Recadrée au centre en carré et mise en JPEG 256x256
# (format attendu par elf2nro) dans switch/out/icone.jpg. Si elle change, le .nro est refait.
mkdir -p "$OUT"
ICONE_SRC="$HERE/../nro-icon.png"
if [ -f "$ICONE_SRC" ]; then
  python3 - "$ICONE_SRC" "$OUT/icone.jpg.new" <<'PY'
import sys
from PIL import Image
src = Image.open(sys.argv[1]).convert("RGBA")
fond = Image.new("RGBA", src.size, (0, 0, 0, 255))
fond.alpha_composite(src)
w, h = fond.size
c = min(w, h)
fond = fond.crop(((w - c) // 2, (h - c) // 2, (w - c) // 2 + c, (h - c) // 2 + c))
fond.convert("RGB").resize((256, 256), Image.LANCZOS).save(sys.argv[2], "JPEG", quality=95)
PY
  if ! cmp -s "$OUT/icone.jpg.new" "$OUT/icone.jpg"; then
    mv "$OUT/icone.jpg.new" "$OUT/icone.jpg"
    rm -f "$OUT/torchlight"  # nouvelle édition de liens, donc nouveau .nro avec l'icône
  else
    rm -f "$OUT/icone.jpg.new"
  fi
fi

# Constantes de .rdata repliées (switch/tools/constantes_lecture_seule.py) : il faut la copie de l'image du jeu
# (switch/tools/image_jeu.py, donnée dérivée du jeu, jamais versionnée) ; sans elle, étape sautée (code d'origine).
IMAGE_JEU="$HERE/out-pc/image_jeu.bin"
if [ -f "$IMAGE_JEU" ]; then
  python3 "$HERE/tools/constantes_lecture_seule.py" --image "$IMAGE_JEU"
else
  echo "AVERTISSEMENT : $IMAGE_JEU absent (switch/tools/image_jeu.py) : constantes de .rdata non repliées" >&2
fi
# Chargements flottants inversés dans l'unité vectorielle sur la Switch (switch/tools/chargements_flottants.py) :
# APRÈS les constantes (un chargement replié n'est plus un chargement).
python3 "$HERE/tools/chargements_flottants.py"
# Appels indirects marqués, et promus en appels directs gardés d'après switch/appels_indirects_promus.txt
# (switch/tools/appels_indirects.py) : AVANT les appels directs, qui rendent ensuite directs les appels promus.
python3 "$HERE/tools/appels_indirects.py" --promouvoir "$HERE/appels_indirects_promus.txt"
# Registres en variables locales dans les fonctions feuilles (switch/tools/registres_locaux_feuilles.py --toutes) :
# après les étapes qui lisent ctx.<registre> (constantes, flottants, appels indirects).
python3 "$HERE/tools/registres_locaux_feuilles.py" --toutes
# Registres en variables locales le temps des boucles sans appel des autres fonctions
# (switch/tools/registres_locaux_boucles.py) : après les feuilles, avant les appels directs.
python3 "$HERE/tools/registres_locaux_boucles.py"
# Appels directs entre fonctions recompilées sans interception (switch/tools/appels_directs.py) : à refaire après
# chaque génération du code ; sans effet (aucun fichier touché) si c'est déjà fait.
python3 "$HERE/tools/appels_directs.py"
# Accès à la pile sans volatile (switch/tools/pile_non_volatile.py) : idem, à refaire après chaque génération.
python3 "$HERE/tools/pile_non_volatile.py"

DOCKER=docker
if ! docker info >/dev/null 2>&1; then DOCKER="sg docker -c"; fi

CMD="cd '$HERE' && \
cmake -S . -B '$OUT' -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE='$NFSMW/tools/switch/cmake/switch-devkitA64.cmake' \
  -DREXSDK_DIR='$NFSMW/sdk' \
  -DREXGLUE_SWITCH_NVK_SDK='$NVK_SDK' \
  -DGIT_EXECUTABLE=/bin/false $CMAKE_OPTIONS && \
cmake --build '$OUT' -j $JOBS $*"

RUN="docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v '$WS:$WS' devkitpro/devkita64 bash -c \"$CMD\""
if [ "$DOCKER" = docker ]; then eval "$RUN"; else sg docker -c "$RUN"; fi

# Ordre des fonctions (switch/ordre_fonctions.ld) : le code de démarrage de libnx doit rester à l'adresse 0, sinon
# le .nro ne démarre pas (piège de nfsmw-nx). Vérifié à chaque compilation.
if [ -f "$OUT/torchlight" ]; then
  START=$(llvm-nm-20 "$OUT/torchlight" 2>/dev/null | awk '$3 == "_start" {print $1}')
  if [ -n "$START" ] && [ "$((16#$START))" != 0 ]; then
    echo "ERREUR : _start à 0x$START au lieu de 0 (ordre des fonctions) : ce .nro ne démarrerait pas" >&2
    rm -f "$OUT/torchlight.nro"
    exit 1
  fi
fi
