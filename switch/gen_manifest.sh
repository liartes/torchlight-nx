#!/bin/bash
# Produit switch/torchlight_manifest.toml à partir du manifeste PC (seule source des
# fonctions) en adaptant les chemins, pour que le rexglue de nfsmw-nx génère dans
# switch/generated sans toucher au build PC. À relancer après chaque modification
# de ../torchlight_manifest.toml.
set -e
cd "$(dirname "$0")"
{
    echo "# Fichier produit par gen_manifest.sh depuis ../torchlight_manifest.toml : ne pas modifier à la main."
    sed -e 's|^game_root = "assets"|game_root = "../assets"|' \
        -e 's|^file_path = "assets/|file_path = "../assets/|' \
        ../torchlight_manifest.toml
} > torchlight_manifest.toml
grep -q '^game_root = "../assets"' torchlight_manifest.toml
grep -q '^file_path = "../assets/default.xex"' torchlight_manifest.toml

# Options de génération du code (Switch seulement ; méthode de nfsmw-nx, docs/toolchain.md « Registers as C++
# locals ») : registres PowerPC en variables locales C++ au lieu de champs de ctx en mémoire. Placées dans
# [entrypoint] (lu comme configuration du générateur ; une clé ailleurs serait ignorée sans erreur). OPTIONS_CODEGEN
# permet de les choisir pour un essai (vide = aucune).
# non_argument_as_local (2026-11-01, d'après renut-nx) : r0, r2, r11, r12, f0, v32-v63 en variables locales ; les
# fonctions de non_arguments_gardes.txt (switch/tools/audit_non_arguments.py) les gardent dans ctx et leurs appelants
# les leur passent (générateur du SDK modifié, switch/patches/nfsmw-sdk.patch).
OPTIONS_CODEGEN="${OPTIONS_CODEGEN-cr_as_local xer_as_local ctr_as_local non_volatile_as_local non_argument_as_local}"
# Morceaux de fonctions qui reçoivent des registres de leur voisine (lus avant d'être écrits, voir
# tools/registres_lus_avant.py) : share_registers (la voisine passe ses variables par ctx). Fusionné dans la ligne
# de l'adresse si elle existe déjà (une entrée du manifeste remplace toute entrée d'un fichier inclus).
python3 - "$OPTIONS_CODEGEN" <<'PY'
import re, sys
options = sys.argv[1].split()
lignes = open('torchlight_manifest.toml').read().split('\n')
marques = []
try:
    for l in open('registres_partages.txt'):
        l = l.split('#', 1)[0].strip()
        if l:
            marques.append(int(l, 16))
except FileNotFoundError:
    pass
sortie, dans_fonctions, vues = [], False, set()
for l in lignes:
    if l.strip() == '[entrypoint.functions]':
        dans_fonctions = True
    m = re.match(r'^(0x[0-9A-Fa-f]{8}) = \{(.*)\}(.*)$', l) if dans_fonctions else None
    if m and int(m.group(1), 16) in marques:
        a = int(m.group(1), 16)
        corps = m.group(2).strip()
        corps = (corps + ', ' if corps else '') + 'share_registers = true'
        l = '%s = { %s }%s' % (m.group(1), corps, m.group(3))
        vues.add(a)
    sortie.append(l)
    if l.startswith('includes = ') and options:
        sortie.append('# Options de génération ajoutées par gen_manifest.sh')
        sortie.extend('%s = true' % o for o in options)
        if 'non_argument_as_local' in options:
            gardes = []
            for g in open('non_arguments_gardes.txt'):
                g = g.split('#', 1)[0].strip()
                if g:
                    gardes.append('0x%08X' % int(g, 16))
            sortie.append('non_argument_as_local_keep = [%s]' % ', '.join(gardes))
while sortie and not sortie[-1].strip():
    sortie.pop()
for a in marques:
    if a not in vues:
        sortie.append('0x%08X = { share_registers = true }' % a)
open('torchlight_manifest.toml', 'w').write('\n'.join(sortie) + '\n')
print('options :', ' '.join(options) or '(aucune)', '; share_registers :', len(marques))
PY
