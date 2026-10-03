#!/bin/bash
# Assemble le dossier SDK Mesa NVK (portlibs/switch) utilisé par build_switch.sh,
# à partir de ~/workspace/mesa-switch/builddir-switch (build-switch.sh).
# L'index des archives est refait avec le ranlib de devkitA64 : celui produit par
# le ar de Debian lors de la fusion ne contient aucun symbole Rust (NAK, nil, std),
# d'où des « undefined reference to core::panicking::... » au lien du .nro.
set -e
WS="$(cd "$(dirname "$0")/../.." && pwd)"
MESA="$WS/mesa-switch"
SDK="$MESA/nvk-sdk/opt/devkitpro/portlibs/switch"
mkdir -p "$SDK/lib" "$SDK/include"
cp "$MESA/builddir-switch/src/nouveau/vulkan/libvulkan.a" \
   "$MESA/builddir-switch/src/nouveau/vulkan/libnvk.a" \
   "$MESA/builddir-switch/src/nouveau/compiler/libnak_rs.a" "$SDK/lib/"
cp -r "$MESA/include/vulkan" "$MESA/include/vk_video" "$SDK/include/"

RUN="docker run --rm --user $(id -u):$(id -g) -v '$SDK:$SDK' devkitpro/devkita64 bash -c 'for f in $SDK/lib/*.a; do /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ranlib \$f; done'"
if docker info >/dev/null 2>&1; then eval "$RUN"; else sg docker -c "$RUN"; fi

# Contrôles : aucun objet x86-64, symboles Rust présents dans l'index.
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && ar x "$SDK/lib/libvulkan.a" 2>/dev/null && ! file ./* | grep -q x86-64) \
    || { echo "ERREUR : objets x86-64 dans libvulkan.a (MESA_SWITCH_RUST_TARGET ?)" >&2; exit 1; }
llvm-nm-20 --print-armap "$SDK/lib/libvulkan.a" 2>/dev/null | grep -q '4core9panicking9panic_fmt' \
    || { echo "ERREUR : symboles Rust absents de l'index de libvulkan.a" >&2; exit 1; }
echo "SDK NVK prêt : $SDK"
