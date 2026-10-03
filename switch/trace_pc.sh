#!/bin/bash
# Lance Torchlight PC (SDK de nfsmw-nx, switch/out-pc) avec le traceur du rendu.
# Résultat dans switch/out-pc/trace/ : rendu.log (compteurs toutes les 10 s) et hlsl/ (sources des shaders).
# Une trace précédente est gardée dans trace.<date>/.
set -e
cd "$(dirname "$0")/.."
cmake --build switch/out-pc -j 6 | tail -1
if [ -d switch/out-pc/trace ]; then mv switch/out-pc/trace "switch/out-pc/trace.$(date +%Y%m%d-%H%M%S)"; fi
# Le runtime place la mémoire du jeu dans /dev/shm/xenia_memory_* et ne la libère pas quand il se termine
# en « hard-exit » : sans nettoyage, /dev/shm se remplit et le lancement suivant meurt en « Erreur du bus ».
cleanup_shm() { for f in /dev/shm/xenia_memory_*; do [ -e "$f" ] && ! fuser "$f" >/dev/null 2>&1 && rm -f "$f"; done; true; }
cleanup_shm
trap cleanup_shm EXIT
LD_LIBRARY_PATH="$HOME/workspace/nfsmw-nx/sdk/out/linux-amd64" ./switch/out-pc/torchlight \
  --game_data_root="$PWD/assets" --gpu_plugin=xenos --license_mask=1 --torchlight_trace=true \
  --log_file="$PWD/switch/out-pc/torchlight-trace.log" "$@"
echo "Trace : switch/out-pc/trace/ ($(ls switch/out-pc/trace/hlsl | wc -l) shaders HLSL)"
