#!/bin/sh
# Dump de la zone de code invitée (0x82000000-0x83030000) dans <dossier>/code.bin.
# Lance le jeu sous gdb et s'arrête au premier NtCreateFile (le code est alors chargé).
OUT="${1:?usage: tools/dump_code.sh <dossier>}"
cd "$HOME/workspace/torchlight"
LD_LIBRARY_PATH="$HOME/rexglue-sdk-0.10.0-linux-amd64/linux-amd64/lib" timeout 170 gdb -q -batch \
  -ex 'set debuginfod enabled off' -ex 'handle SIGSEGV nostop noprint pass' -ex 'handle SIG35 nostop noprint pass' \
  -ex 'set breakpoint pending on' -ex 'break rex::kernel::xboxkrnl::NtCreateFile_entry' -ex run \
  -ex "dump binary memory $OUT/code.bin 0x182000000 0x183030000" -ex kill \
  --args ./out/build/linux-amd64-debug/torchlight --game_data_root="$PWD/assets" --gpu_plugin=xenos --log_file="$OUT/dump.log"
