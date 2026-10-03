#!/bin/sh
set -e
cd "$HOME/workspace/torchlight"
cmake --build --preset linux-amd64-release --target torchlight_codegen
cmake --preset linux-amd64-release > /dev/null
cmake --build --preset linux-amd64-release
rm -f torchlight.log
LD_LIBRARY_PATH="$HOME/rexglue-sdk-0.10.0-linux-amd64/linux-amd64/lib" \
  ./out/build/linux-amd64-release/torchlight --game_data_root="$PWD/assets" --gpu_plugin=xenos --license_mask=1 --log_file=torchlight.log || true
grep FATAL torchlight.log || echo "Pas de FATAL dans le log"
