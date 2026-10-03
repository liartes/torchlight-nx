#!/usr/bin/env python3
"""Appels directs entre fonctions recompilées sans interception (portage de tools/llamadas_directas.py de nfsmw-nx).

Avec GCC (Switch) comme avec Clang, chaque fonction recompilée sub_X est un alias faible de __imp__sub_X
(DEFINE_REX_FUNC), pour qu'un REX_HOOK_RAW(sub_X) de l'application la remplace à l'édition des liens. Le code
généré appelle toujours sub_X, et le compilateur ne peut jamais intégrer (inline) un appel à travers un alias faible.
Cet outil remplace `sub_X(ctx, base);` par `__imp__sub_X(ctx, base);` quand sub_X n'a pas d'interception : c'est
la même fonction, mais le compilateur peut maintenant l'intégrer à son appelant. nfsmw-nx : avec le LTO et le PGO,
de 30,3 à ~34 images/s.

Même chose pour les fonctions d'aide des prologues et épilogues PowerPC (__savegprlr_N, __restgprlr_N,
__savefpr_N, __restfpr_N, __savevmx_N, __restvmx_N : quelques écritures, ~44 000 appels), jamais interceptées.

Toute adresse 82xxxxxx ou 83xxxxxx citée dans nos sources, le SDK ou les fichiers de configuration compte comme
interceptée (nos interceptions fabriquent leurs noms avec sub_##addr) : un peu plus que nécessaire, sans risque.
Les fichiers d'initialisation et d'enregistrement (table des appels indirects) ne sont pas touchés : les appels
indirects passent toujours par sub_X et voient les interceptions.

À refaire après chaque génération du code (build_switch.sh le fait avant de compiler ; sans effet si déjà fait).

Usage : appels_directs.py [--annuler] [--verifier <dossier d'objets>]
  --verifier : vérifie avec nm que chaque sub_X défini par nos objets compilés (src/) est bien compté comme
               intercepté.
"""
import os
import re
import subprocess
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
SDK = os.path.expanduser('~/workspace/nfsmw-nx/sdk')
SOURCES = [os.path.join(RACINE, 'src'), os.path.join(SDK, 'src'), os.path.join(SDK, 'include'),
           os.path.join(RACINE, 'switch', 'torchlight_manifest.toml'), os.path.join(RACINE, 'switch', 'torchlight.toml'),
           os.path.join(RACINE, 'switch', 'CMakeLists.txt')]
ADRESSE = re.compile(r'(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])')


def interceptees():
    noms = set()
    for racine in SOURCES:
        chemins = [racine] if os.path.isfile(racine) else [
            os.path.join(d, f) for d, _, fs in os.walk(racine) for f in fs
            if not f.endswith(('.a', '.o', '.obj', '.png', '.jpg', '.bin'))]
        for chemin in chemins:
            try:
                texte = open(chemin, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            noms.update('sub_' + m.upper() for m in ADRESSE.findall(texte))
    return noms


def verifier(dossier, avec):
    objets = [os.path.join(d, f) for d, _, fs in os.walk(dossier) for f in fs if f.endswith(('.o', '.obj'))
              and 'generated' not in d]
    manquants = set()
    for o in objets:
        sortie = subprocess.run(['nm', '--defined-only', o], capture_output=True, text=True).stdout
        for nom in re.findall(r'\b(sub_[0-9A-F]{8})\b', sortie):
            if nom not in avec:
                manquants.add(nom)
    if manquants:
        print('ERREUR : interceptions inconnues de l\'outil :', ' '.join(sorted(manquants)))
        return 1
    print(f'vérifié : {len(objets)} objets, toutes leurs fonctions sub_ sont comptées comme interceptées')
    return 0


def main():
    avec = interceptees()
    if '--verifier' in sys.argv:
        return verifier(sys.argv[sys.argv.index('--verifier') + 1], avec)
    annuler = '--annuler' in sys.argv
    appel = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    aide = re.compile(r'(?<![\w])(__(?:save|rest)(?:gprlr|fpr|vmx)_\d+)\(ctx, base\);')
    aide_directe = re.compile(r'__imp__(__(?:save|rest)(?:gprlr|fpr|vmx)_\d+)\(ctx, base\);')
    total = fichiers = 0
    for f in sorted(os.listdir(GEN)):
        if not (f.startswith('torchlight_recomp.') and f.endswith('.cpp')):
            continue
        chemin = os.path.join(GEN, f)
        texte = open(chemin, encoding='utf-8').read()
        if annuler:
            # Rétablit les appels ordinaires, sauf l'appel de l'original par une fonction elle-même (aucun).
            nouveau, n = direct.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), texte)
            nouveau, n2 = aide_directe.subn(lambda m: '%s(ctx, base);' % m.group(1), nouveau)
            n += n2
        else:
            nouveau, n = appel.subn(lambda m: m.group(0) if 'sub_' + m.group(1) in avec
                                    else '__imp__sub_%s(ctx, base);' % m.group(1), texte)
            n -= len(appel.findall(nouveau))
            nouveau, n2 = aide.subn(lambda m: '__imp__%s(ctx, base);' % m.group(1), nouveau)
            n += n2
        if nouveau != texte:
            open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
            fichiers += 1
        total += n
    print('%s : %d appels dans %d fichiers ; %d adresses interceptées respectées' %
          ('annulé' if annuler else 'directs', total, fichiers, len(avec)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
