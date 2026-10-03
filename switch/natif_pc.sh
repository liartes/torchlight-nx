#!/bin/bash
# Lance Torchlight PC (SDK de nfsmw-nx, switch/out-pc) avec le rendu natif (phase 3, switch/RENDU_NATIF.md).
# Données utilisateur : copie dans switch/out-pc/donnees-test (les vraies sauvegardes ne sont jamais touchées).
# Options supplémentaires passées telles quelles, par exemple :
#   --torchlight_manette="20:a,24:a,28:a,32:a,36:a,40:a,44:a,48:a,52:a,56:a,60:a,64:a,68:a,72:a,76:a,80:a"
#   --torchlight_capture_s=100   (capture d'écran de l'image présentée : trace/capture/emulation.ppm)
set -e
cd "$(dirname "$0")/.."
cmake --build switch/out-pc -j 6 | tail -1
DONNEES=switch/out-pc/donnees-test
if [ ! -d "$DONNEES" ]; then cp -a "$HOME/.local/share/torchlight" "$DONNEES"; fi
if [ -d switch/out-pc/trace ]; then mv switch/out-pc/trace "switch/out-pc/trace.$(date +%Y%m%d-%H%M%S)"; fi
cleanup_shm() { for f in /dev/shm/xenia_memory_*; do [ -e "$f" ] && ! fuser "$f" >/dev/null 2>&1 && rm -f "$f"; done; true; }
cleanup_shm
trap cleanup_shm EXIT
LD_LIBRARY_PATH="$HOME/workspace/nfsmw-nx/sdk/out/linux-amd64" ./switch/out-pc/torchlight \
  --game_data_root="$PWD/assets" --gpu_plugin=xenos --license_mask=1 --torchlight_natif=true \
  --user_data_root="$PWD/$DONNEES" --log_file="$PWD/switch/out-pc/torchlight-natif.log" "$@"
