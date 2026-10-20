#!/bin/bash
# Échanges avec la Switch connectée en MTP (DBI, montée par GVFS) : envoi du .nro, profileur, journaux, cache.
#
# Usage : switch/console.sh <commande>
#   etat               ce qu'il y a sur la console (.nro, réglages, profileur, journaux, cache)
#   envoyer            copie switch/out/sd/switch/torchlight/{torchlight.nro,torchlight.toml} sur la carte
#   pilas-on|pilas-off crée / supprime logs/rex/perfil_pilas.flag (relevé des piles par le profileur du runtime)
#   journaux [--vider] copie logs/ (et logs/rex/) de la console dans switch-logs/ (vidé d'abord) ;
#                      --vider efface ensuite les journaux sur la console (le drapeau du profileur est gardé)
#   vider-cache        supprime torchlight/cache/natif sur la carte (shaders et pipelines : essai à froid)
#   pgo                copie pgo/ de la console (profils .gcda de la build instrumentée) dans pgo/ du dépôt
#                      (vidé d'abord ; jamais versionné : données dérivées du jeu)
#   pgo-vider          supprime pgo/ sur la console (avant une nouvelle session de profil)
#   installation <paquet>
#                      essai de l'installation depuis le paquet XBLA comme un joueur : VIDE sdmc:/switch/torchlight
#                      (fichiers du jeu, sauvegardes, cache des shaders, journaux) puis y copie seulement le .nro
#                      et le paquet (torchlight.toml est facultatif) ; demande confirmation
#
# Lecture par le point de montage GVFS, écritures et suppressions par gio (plus fiable en MTP que cp/rm).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# Dossier sdmc:/switch/torchlight vu par GVFS : premier stockage « SD Card » d'un appareil MTP qui le contient.
trouver() {
  local d s
  for d in /run/user/"$(id -u)"/gvfs/mtp:host=*; do
    [ -d "$d" ] || continue
    for s in "$d"/*SD\ Card; do
      if [ -d "$s/switch/torchlight" ]; then
        T="$s/switch/torchlight"
        return 0
      fi
    done
  done
  echo "Switch introuvable en MTP : lancer DBI en mode MTP et brancher la console (dossier switch/torchlight)." >&2
  exit 1
}

# Existence d'un fichier sur la console, par gio (le point de montage FUSE peut montrer un état périmé un instant).
existe() { gio info "$1" >/dev/null 2>&1; }

# Envoi d'un fichier local vers un chemin de la console (remplace s'il existe).
envoyer_fichier() {
  local src="$1" dst="$2"
  gio remove "$dst" 2>/dev/null || true
  gio copy "$src" "$dst"
}

# Suppression récursive d'un dossier de la console (fichiers puis dossiers, du plus profond au moins profond).
supprimer_dossier() {
  local dir="$1"
  [ -d "$dir" ] || return 0
  find "$dir" -depth -mindepth 1 -print0 | while IFS= read -r -d '' p; do gio remove "$p"; done
  gio remove "$dir"
}

cmd="${1:-etat}"
trouver
case "$cmd" in
  etat)
    echo "Console : $T"
    for f in torchlight.nro torchlight.toml; do
      if [ -f "$T/$f" ]; then echo "  $f : $(du -h "$T/$f" | cut -f1)"; else echo "  $f : absent"; fi
    done
    if existe "$T/logs/rex/perfil_pilas.flag"; then echo "  profileur : relevé des piles ACTIF"; else echo "  profileur : relevé des piles inactif"; fi
    echo "  journaux : $(ls "$T/logs" 2>/dev/null | grep -c '\.log$' || true) fichiers, rex/ : $(ls "$T/logs/rex" 2>/dev/null | tr '\n' ' ')"
    if [ -d "$T/torchlight/cache/natif" ]; then
      echo "  cache natif : $(ls "$T/torchlight/cache/natif" | grep -c '\.tlsh$' || true) shaders$( [ -f "$T/torchlight/cache/natif/pipelines.bin" ] && echo ', pipelines.bin')"
    else
      echo "  cache natif : vide"
    fi
    ;;
  installation)
    PAQUET="${2:?usage : console.sh installation <paquet XBLA>}"
    [ -f "$PAQUET" ] || { echo "Paquet introuvable : $PAQUET" >&2; exit 1; }
    SRC="$HERE/out/sd/switch/torchlight"
    [ -f "$SRC/torchlight.nro" ] || { echo "Pas de $SRC/torchlight.nro : lancer switch/carte_sd.sh" >&2; exit 1; }
    echo "Tout le contenu de $T va être supprimé (fichiers du jeu, sauvegardes, cache, journaux)."
    read -r -p "Continuer ? (oui/non) " reponse
    [ "$reponse" = "oui" ] || { echo "annulé"; exit 1; }
    find "$T" -mindepth 1 -maxdepth 1 -print0 | while IFS= read -r -d '' p; do
      if [ -d "$p" ]; then supprimer_dossier "$p"; else gio remove "$p"; fi
    done
    for f in "$SRC/torchlight.nro" "$PAQUET"; do
      echo "envoi de $(basename "$f") ($(du -h "$f" | cut -f1))..."
      gio copy "$f" "$T/$(basename "$f")"
    done
    echo "fait : lancer torchlight.nro (title override), puis console.sh journaux"
    ;;
  envoyer)
    SRC="$HERE/out/sd/switch/torchlight"
    [ -f "$SRC/torchlight.nro" ] || { echo "Pas de $SRC/torchlight.nro : lancer switch/carte_sd.sh" >&2; exit 1; }
    for f in torchlight.nro torchlight.toml; do
      echo "envoi de $f ($(du -h "$SRC/$f" | cut -f1))..."
      envoyer_fichier "$SRC/$f" "$T/$f"
    done
    echo "fait"
    ;;
  pilas-on)
    vide="$(mktemp)"
    gio mkdir -p "$T/logs/rex" 2>/dev/null || true
    envoyer_fichier "$vide" "$T/logs/rex/perfil_pilas.flag"
    rm -f "$vide"
    echo "relevé des piles activé (penser à pilas-off ensuite : il ralentit le jeu)"
    ;;
  pilas-off)
    gio remove "$T/logs/rex/perfil_pilas.flag" 2>/dev/null && echo "relevé des piles désactivé" || echo "déjà inactif"
    ;;
  journaux)
    DEST="$ROOT/switch-logs"
    mkdir -p "$DEST"
    # Échantillons bruts du profileur gardés (ordre des fonctions : switch/tools/ordre_fonctions.py en a besoin avec
    # l'ELF du .nro testé) : pilas-archive/, jamais versionné.
    find "$DEST" -mindepth 1 -delete
    cp -r "$T/logs/." "$DEST/"
    # Archivé après la copie, et seulement s'il est nouveau : avant, le fichier local précédent était recopié à
    # chaque appel (trois copies identiques du lot 2 le 2026-10-15).
    if [ -s "$DEST/rex/rex_pilas.txt" ]; then
      mkdir -p "$ROOT/pilas-archive"
      somme="$(md5sum < "$DEST/rex/rex_pilas.txt" | cut -d' ' -f1)"
      if ! md5sum "$ROOT"/pilas-archive/*.txt 2>/dev/null | grep -q "^$somme "; then
        cp "$DEST/rex/rex_pilas.txt" "$ROOT/pilas-archive/rex_pilas-$(date +%Y%m%d-%H%M).txt"
        echo "relevé des piles archivé dans pilas-archive/"
      fi
    fi
    echo "copié dans $DEST :"
    (cd "$DEST" && find . -type f | sort | sed 's/^/  /')
    if [ "${2:-}" = "--vider" ]; then
      for f in "$T/logs"/*.log "$T/logs/rex"/*; do
        [ -e "$f" ] || continue
        [ "$(basename "$f")" = "perfil_pilas.flag" ] && continue
        gio remove "$f"
      done
      echo "journaux effacés sur la console"
    fi
    ;;
  pgo)
    [ -d "$T/pgo" ] || { echo "Pas de pgo/ sur la console : build instrumentée lancée plus de 3 min ?" >&2; exit 1; }
    DEST="$ROOT/pgo"
    mkdir -p "$DEST"
    find "$DEST" -mindepth 1 -delete
    cp -r "$T/pgo/." "$DEST/"
    echo "copié dans $DEST : $(find "$DEST" -name '*.gcda' | wc -l) fichiers .gcda"
    [ -f "$DEST/vidages.txt" ] && cat "$DEST/vidages.txt"
    ;;
  pgo-vider)
    supprimer_dossier "$T/pgo"
    echo "profils PGO supprimés sur la console"
    ;;
  vider-cache)
    supprimer_dossier "$T/torchlight/cache/natif"
    echo "cache natif supprimé (le prochain lancement recompile shaders et pipelines)"
    ;;
  *)
    sed -n '2,16p' "$0"
    exit 2
    ;;
esac
